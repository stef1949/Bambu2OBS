#pragma once

/* Typed printer state, shared by value. Rendering consumes this structure and
 * never sees raw JSON. Every optional value carries a has_* flag so a missing
 * value can be shown as a placeholder instead of a misleading zero. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BAMBU_JOB_NAME_CAPACITY 256
#define BAMBU_STAGE_CAPACITY 96
#define BAMBU_PROFILE_NAME_CAPACITY 256
#define BAMBU_FILE_NAME_CAPACITY 256
#define BAMBU_TASK_ID_CAPACITY 64
#define BAMBU_SPEED_CAPACITY 32
#define BAMBU_MATERIAL_ID_CAPACITY 32
#define BAMBU_MATERIAL_TYPE_CAPACITY 32
#define BAMBU_MATERIAL_NAME_CAPACITY 64

#define BAMBU_MAX_AMS_UNITS 8
#define BAMBU_TRAYS_PER_UNIT 4

enum bambu_connection_state {
    BAMBU_CONNECTION_CONFIGURATION_REQUIRED = 0,
    BAMBU_CONNECTION_CONNECTING,
    BAMBU_CONNECTION_LIVE,
    BAMBU_CONNECTION_RECONNECTING,
    BAMBU_CONNECTION_OFFLINE,
    BAMBU_CONNECTION_AUTHENTICATION_FAILED,
    BAMBU_CONNECTION_ERROR,
};

enum bambu_job_state {
    BAMBU_JOB_UNKNOWN = 0,
    BAMBU_JOB_IDLE,
    BAMBU_JOB_PREPARING,
    BAMBU_JOB_PRINTING,
    BAMBU_JOB_PAUSED,
    BAMBU_JOB_FINISHED,
    BAMBU_JOB_FAILED,
};

struct bambu_ams_tray {
    bool present; /* the printer reported this slot */
    bool loaded;  /* the slot holds identifiable filament */
    bool has_color;
    uint32_t color_rgb; /* 0xRRGGBB */
    char material_id[BAMBU_MATERIAL_ID_CAPACITY];
    char material_type[BAMBU_MATERIAL_TYPE_CAPACITY];
    char material_name[BAMBU_MATERIAL_NAME_CAPACITY];
};

struct bambu_ams_unit {
    bool present;
    int unit_id; /* zero-based, as reported by the printer */
    struct bambu_ams_tray trays[BAMBU_TRAYS_PER_UNIT];
};

struct bambu_printer_snapshot {
    uint64_t revision;
    uint64_t updated_monotonic_ms;
    enum bambu_connection_state connection_state;

    enum bambu_job_state job_state;
    bool has_progress;
    double progress_percent;
    bool has_remaining;
    int remaining_minutes;
    bool has_nozzle_temperature;
    double nozzle_temperature_c;
    bool has_bed_temperature;
    double bed_temperature_c;
    bool has_layer;
    int current_layer;
    int total_layers;
    bool has_fan;
    double fan_percent;
    bool has_speed;
    int speed_level;
    bool has_total_time;
    int total_time_seconds;
    bool has_total_weight;
    double total_weight_grams;

    int ams_unit_count;
    struct bambu_ams_unit ams[BAMBU_MAX_AMS_UNITS];
    bool has_active_tray;
    int active_unit_id;
    int active_tray_index;
    bool external_spool_active;

    bool cloud_job_title; /* job_name came from cloud metadata and outranks MQTT */
    bool has_stage_id;    /* stg_cur seen, so the mc_print_stage fallback is unused */
    uint64_t preview_revision;
    char job_name[BAMBU_JOB_NAME_CAPACITY];
    char print_profile[BAMBU_PROFILE_NAME_CAPACITY];
    char stage[BAMBU_STAGE_CAPACITY];
    char speed_name[BAMBU_SPEED_CAPACITY];
    char task_id[BAMBU_TASK_ID_CAPACITY];
    char subtask_name[BAMBU_FILE_NAME_CAPACITY];
    char gcode_file[BAMBU_FILE_NAME_CAPACITY];
    char print_type[16];
};

/* Retained for simple producers such as connection state changes. */
struct bambu_printer_snapshot_update {
    bool has_connection_state;
    enum bambu_connection_state connection_state;
    bool has_progress;
    double progress_percent;
    bool has_nozzle_temperature;
    double nozzle_temperature_c;
    bool has_bed_temperature;
    double bed_temperature_c;
    bool has_layer;
    int current_layer;
    int total_layers;
    bool has_job_name;
    const char *job_name;
    bool has_stage;
    const char *stage;
    uint64_t updated_monotonic_ms;
};

void bambu_snapshot_init(struct bambu_printer_snapshot *snapshot);
bool bambu_snapshot_apply(
    struct bambu_printer_snapshot *snapshot,
    const struct bambu_printer_snapshot_update *update
);
bool bambu_snapshot_is_stale(
    const struct bambu_printer_snapshot *snapshot,
    uint64_t now_monotonic_ms,
    uint64_t stale_after_ms
);
const char *bambu_connection_state_name(enum bambu_connection_state state);
const char *bambu_job_state_name(enum bambu_job_state state);

/* Bounded, always-terminated copy. Returns true when the destination changed.
 * Truncation never splits a UTF-8 sequence. */
bool bambu_copy_text(char *destination, size_t capacity, const char *value);
