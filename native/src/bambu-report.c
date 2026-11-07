#include "bambu-report.h"

#include "bambu-json.h"
#include "bambu-maps.h"
#include "bambu-preview-select.h"
#include "bambu-util.h"

#include <stdlib.h>
#include <string.h>

#define TRAY_NONE 255
#define TRAY_EXTERNAL 254
#define FAN_MAX_STEP 15.0
#define MAX_REMAINING_MINUTES (60 * 24 * 60)

static double clamp(double value, double minimum, double maximum)
{
    if (value < minimum)
        return minimum;
    if (value > maximum)
        return maximum;
    return value;
}

static bool usable_text(const char *value)
{
    return value != NULL && value[0] != '\0' && strcmp(value, "N/A") != 0 && strcmp(value, "Unknown") != 0;
}

static enum bambu_job_state job_state_from_gcode(const char *value)
{
    static const struct {
        const char *name;
        enum bambu_job_state state;
    } states[] = {
        {"IDLE", BAMBU_JOB_IDLE},         {"INIT", BAMBU_JOB_IDLE},        {"PREPARE", BAMBU_JOB_PREPARING},
        {"SLICING", BAMBU_JOB_PREPARING}, {"RUNNING", BAMBU_JOB_PRINTING}, {"PAUSE", BAMBU_JOB_PAUSED},
        {"FINISH", BAMBU_JOB_FINISHED},   {"FAILED", BAMBU_JOB_FAILED},
    };

    for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
        if (strcmp(states[i].name, value) == 0)
            return states[i].state;
    }
    return BAMBU_JOB_UNKNOWN;
}

/* task_id "0" means "no cloud task" on LAN prints, so it cannot identify a job. */
static bool meaningful_task_id(const char *value)
{
    return usable_text(value) && strcmp(value, "0") != 0;
}

static void detect_job_change(
    struct bambu_printer_snapshot *snapshot,
    const struct bambu_json *print,
    struct bambu_report_events *events
)
{
    char task_id[BAMBU_TASK_ID_CAPACITY] = "";
    const char *subtask = bambu_json_as_string(bambu_json_get(print, "subtask_name"));
    bool has_task = bambu_json_as_text(bambu_json_get(print, "task_id"), task_id, sizeof(task_id)) &&
                    meaningful_task_id(task_id);
    bool has_subtask = usable_text(subtask);
    bool changed = false;
    bool first = false;

    if (has_task) {
        if (snapshot->task_id[0] == '\0')
            first = true;
        else if (strcmp(snapshot->task_id, task_id) != 0)
            changed = true;
    }
    if (has_subtask) {
        if (snapshot->subtask_name[0] == '\0')
            first = first || !has_task;
        else if (strncmp(snapshot->subtask_name, subtask, sizeof(snapshot->subtask_name) - 1) != 0)
            changed = true;
    }

    if (changed) {
        snapshot->has_layer = false;
        snapshot->current_layer = 0;
        snapshot->total_layers = 0;
        snapshot->has_total_time = false;
        snapshot->has_total_weight = false;
        snapshot->cloud_job_title = false;
        snapshot->job_name[0] = '\0';
        snapshot->print_profile[0] = '\0';
        snapshot->subtask_name[0] = '\0';
        snapshot->gcode_file[0] = '\0';
    }
    if (events != NULL && (changed || first))
        events->job_changed = true;
    if (has_task)
        bambu_copy_text(snapshot->task_id, sizeof(snapshot->task_id), task_id);
}

static void apply_job_text(struct bambu_printer_snapshot *snapshot, const struct bambu_json *print)
{
    static const char *const fallbacks[] = {"subtask_name", "task_name", "project_name", "gcode_file"};
    const char *subtask = bambu_json_as_string(bambu_json_get(print, "subtask_name"));
    const char *gcode_file = bambu_json_as_string(bambu_json_get(print, "gcode_file"));
    const char *print_type = bambu_json_as_string(bambu_json_get(print, "print_type"));

    if (usable_text(subtask)) {
        bambu_copy_text(snapshot->subtask_name, sizeof(snapshot->subtask_name), subtask);
        if (!snapshot->cloud_job_title)
            bambu_copy_text(snapshot->print_profile, sizeof(snapshot->print_profile), subtask);
    }
    if (usable_text(gcode_file))
        bambu_copy_text(snapshot->gcode_file, sizeof(snapshot->gcode_file), gcode_file);
    if (print_type != NULL)
        bambu_copy_text(snapshot->print_type, sizeof(snapshot->print_type), print_type);

    if (snapshot->job_name[0] != '\0')
        return;
    for (size_t i = 0; i < sizeof(fallbacks) / sizeof(fallbacks[0]); i++) {
        const char *candidate = bambu_json_as_string(bambu_json_get(print, fallbacks[i]));
        if (usable_text(candidate)) {
            char display[BAMBU_JOB_NAME_CAPACITY];
            bambu_strip_print_suffix(candidate, display, sizeof(display));
            bambu_copy_text(snapshot->job_name, sizeof(snapshot->job_name), display[0] ? display : candidate);
            return;
        }
    }
}

static void apply_stage(struct bambu_printer_snapshot *snapshot, const struct bambu_json *print)
{
    int stage_id;

    if (bambu_json_as_int(bambu_json_get(print, "stg_cur"), &stage_id)) {
        snapshot->has_stage_id = true;
        bambu_stage_name(stage_id, snapshot->stage, sizeof(snapshot->stage));
    } else if (!snapshot->has_stage_id && bambu_json_as_int(bambu_json_get(print, "mc_print_stage"), &stage_id)) {
        bambu_stage_name(stage_id, snapshot->stage, sizeof(snapshot->stage));
    }
}

static void apply_tray(struct bambu_ams_tray *tray, const struct bambu_json *source)
{
    const char *filament_id = bambu_json_as_string(bambu_json_get(source, "tray_info_idx"));
    const char *tray_type = bambu_json_as_string(bambu_json_get(source, "tray_type"));
    const char *color = bambu_json_as_string(bambu_json_get(source, "tray_color"));
    uint32_t rgb;

    tray->present = true;
    tray->loaded = (filament_id != NULL && filament_id[0] != '\0') || (tray_type != NULL && tray_type[0] != '\0');
    if (!tray->loaded) {
        tray->has_color = false;
        tray->material_id[0] = '\0';
        tray->material_type[0] = '\0';
        tray->material_name[0] = '\0';
        return;
    }
    bambu_copy_text(tray->material_id, sizeof(tray->material_id), filament_id);
    bambu_copy_text(tray->material_type, sizeof(tray->material_type), tray_type);
    bambu_filament_name(filament_id, tray_type, tray->material_name, sizeof(tray->material_name));
    if (bambu_parse_hex_color(color, &rgb)) {
        tray->has_color = true;
        tray->color_rgb = rgb;
    }
}

static int compare_units(const void *left, const void *right)
{
    const struct bambu_ams_unit *a = left;
    const struct bambu_ams_unit *b = right;
    return (a->unit_id > b->unit_id) - (a->unit_id < b->unit_id);
}

/* The ams array, when present, lists every attached unit, so the unit list is
 * rebuilt from it. Existing entries are carried over so that a tray entry which
 * omits a colour keeps the last known one. */
static void apply_ams_units(struct bambu_printer_snapshot *snapshot, const struct bambu_json *units)
{
    struct bambu_ams_unit rebuilt[BAMBU_MAX_AMS_UNITS];
    const struct bambu_json *unit;
    int count = 0;
    int position = 0;

    memset(rebuilt, 0, sizeof(rebuilt));
    for (unit = units->child; unit != NULL; unit = unit->next, position++) {
        const struct bambu_json *trays;
        const struct bambu_json *tray;
        struct bambu_ams_unit *target;
        int unit_id = position;
        int tray_position = 0;
        bool duplicate = false;

        if (unit->type != BAMBU_JSON_OBJECT)
            continue;
        if (count >= BAMBU_MAX_AMS_UNITS)
            break;
        bambu_json_as_int(bambu_json_get(unit, "id"), &unit_id);
        for (int i = 0; i < count; i++)
            duplicate = duplicate || rebuilt[i].unit_id == unit_id;
        if (duplicate || unit_id < 0)
            continue;

        target = &rebuilt[count++];
        for (int i = 0; i < snapshot->ams_unit_count; i++) {
            if (snapshot->ams[i].unit_id == unit_id)
                *target = snapshot->ams[i];
        }
        target->present = true;
        target->unit_id = unit_id;

        trays = bambu_json_get(unit, "tray");
        if (trays == NULL || trays->type != BAMBU_JSON_ARRAY)
            continue;
        for (tray = trays->child; tray != NULL; tray = tray->next, tray_position++) {
            int tray_index = tray_position;
            if (tray->type != BAMBU_JSON_OBJECT)
                continue;
            bambu_json_as_int(bambu_json_get(tray, "id"), &tray_index);
            if (tray_index >= 0 && tray_index < BAMBU_TRAYS_PER_UNIT)
                apply_tray(&target->trays[tray_index], tray);
        }
    }

    qsort(rebuilt, (size_t)count, sizeof(rebuilt[0]), compare_units);
    memcpy(snapshot->ams, rebuilt, sizeof(rebuilt));
    snapshot->ams_unit_count = count;
}

static void apply_active_tray(struct bambu_printer_snapshot *snapshot, const struct bambu_json *ams)
{
    int tray_now;

    if (!bambu_json_as_int(bambu_json_get(ams, "tray_now"), &tray_now))
        return;
    snapshot->external_spool_active = tray_now == TRAY_EXTERNAL;
    if (tray_now >= 0 && tray_now < BAMBU_MAX_AMS_UNITS * BAMBU_TRAYS_PER_UNIT) {
        snapshot->has_active_tray = true;
        snapshot->active_unit_id = tray_now / BAMBU_TRAYS_PER_UNIT;
        snapshot->active_tray_index = tray_now % BAMBU_TRAYS_PER_UNIT;
    } else {
        /* TRAY_NONE, TRAY_EXTERNAL, and unknown values all mean no AMS slot. */
        snapshot->has_active_tray = false;
    }
}

static void apply_print(struct bambu_printer_snapshot *snapshot, const struct bambu_json *print)
{
    const char *gcode_state = bambu_json_as_string(bambu_json_get(print, "gcode_state"));
    const struct bambu_json *ams = bambu_json_get(print, "ams");
    double number;
    int integer;

    if (gcode_state != NULL)
        snapshot->job_state = job_state_from_gcode(gcode_state);

    apply_job_text(snapshot, print);
    apply_stage(snapshot, print);

    if (bambu_json_as_double(bambu_json_get(print, "mc_percent"), &number)) {
        snapshot->has_progress = true;
        snapshot->progress_percent = clamp(number, 0.0, 100.0);
    }
    if (bambu_json_as_int(bambu_json_get(print, "mc_remaining_time"), &integer)) {
        snapshot->has_remaining = true;
        snapshot->remaining_minutes = (int)clamp(integer, 0, MAX_REMAINING_MINUTES);
    }
    if (bambu_json_as_double(bambu_json_get(print, "nozzle_temper"), &number)) {
        snapshot->has_nozzle_temperature = true;
        snapshot->nozzle_temperature_c = clamp(number, -50.0, 500.0);
    }
    if (bambu_json_as_double(bambu_json_get(print, "bed_temper"), &number)) {
        snapshot->has_bed_temperature = true;
        snapshot->bed_temperature_c = clamp(number, -50.0, 200.0);
    }
    if (bambu_json_as_double(bambu_json_get(print, "cooling_fan_speed"), &number)) {
        snapshot->has_fan = true;
        snapshot->fan_percent = clamp(number / FAN_MAX_STEP * 100.0, 0.0, 100.0);
    }
    if (bambu_json_as_int(bambu_json_get(print, "spd_lvl"), &integer)) {
        snapshot->has_speed = true;
        snapshot->speed_level = integer;
        bambu_speed_name(integer, snapshot->speed_name, sizeof(snapshot->speed_name));
    }
    if (bambu_json_as_int(bambu_json_get(print, "layer_num"), &integer)) {
        snapshot->has_layer = true;
        snapshot->current_layer = integer < 0 ? 0 : integer;
    }
    if (bambu_json_as_int(bambu_json_get(print, "total_layer_num"), &integer)) {
        snapshot->has_layer = true;
        snapshot->total_layers = integer < 0 ? 0 : integer;
    }

    if (ams != NULL && ams->type == BAMBU_JSON_OBJECT) {
        const struct bambu_json *units = bambu_json_get(ams, "ams");
        if (units != NULL && units->type == BAMBU_JSON_ARRAY)
            apply_ams_units(snapshot, units);
        apply_active_tray(snapshot, ams);
    }
}

enum bambu_report_result bambu_report_apply(
    struct bambu_printer_snapshot *snapshot,
    const char *payload,
    size_t length,
    uint64_t now_monotonic_ms,
    struct bambu_report_events *events
)
{
    struct bambu_printer_snapshot *before;
    struct bambu_json *root;
    const struct bambu_json *print;
    enum bambu_report_result result = BAMBU_REPORT_IGNORED;

    if (snapshot == NULL)
        return BAMBU_REPORT_INVALID;
    if (events != NULL)
        events->job_changed = false;

    root = bambu_json_parse(payload, length);
    if (root == NULL)
        return BAMBU_REPORT_INVALID;

    print = bambu_json_get(root, "print");
    if (print != NULL && print->type == BAMBU_JSON_OBJECT) {
        before = malloc(sizeof(*before));
        if (before != NULL)
            *before = *snapshot;
        detect_job_change(snapshot, print, events);
        apply_print(snapshot, print);
        /* Freshness alone must not bump the revision, or every message would redraw. */
        snapshot->updated_monotonic_ms = now_monotonic_ms;
        if (before != NULL)
            before->updated_monotonic_ms = now_monotonic_ms;
        if (before == NULL || memcmp(before, snapshot, sizeof(*snapshot)) != 0)
            snapshot->revision++;
        free(before);
        result = BAMBU_REPORT_APPLIED;
    }
    bambu_json_free(root);
    return result;
}
