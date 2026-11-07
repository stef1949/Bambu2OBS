#include "bambu-simulation.h"

#include "bambu-report.h"

#include <stdio.h>
#include <string.h>

#define SIM_STALE_AGE_MS 95000u

static const char ams_one[] =
    "\"ams\":{\"tray_now\":\"1\",\"ams\":[{\"id\":\"0\",\"tray\":["
    "{\"id\":\"0\",\"tray_info_idx\":\"GFA00\",\"tray_type\":\"PLA\",\"tray_color\":\"F5F5F5FF\"},"
    "{\"id\":\"1\",\"tray_info_idx\":\"GFA01\",\"tray_type\":\"PLA\",\"tray_color\":\"1F7AE0FF\"},"
    "{\"id\":\"2\",\"tray_info_idx\":\"GFG00\",\"tray_type\":\"PETG\",\"tray_color\":\"161616FF\"},"
    "{\"id\":\"3\"}]}]}";

static const char ams_two[] =
    "\"ams\":{\"tray_now\":\"6\",\"ams\":[{\"id\":\"0\",\"tray\":["
    "{\"id\":\"0\",\"tray_info_idx\":\"GFA00\",\"tray_type\":\"PLA\",\"tray_color\":\"F5F5F5FF\"},"
    "{\"id\":\"1\",\"tray_info_idx\":\"GFA01\",\"tray_type\":\"PLA\",\"tray_color\":\"1F7AE0FF\"},"
    "{\"id\":\"2\",\"tray_info_idx\":\"GFG00\",\"tray_type\":\"PETG\",\"tray_color\":\"161616FF\"},"
    "{\"id\":\"3\",\"tray_info_idx\":\"GFA05\",\"tray_type\":\"PLA\",\"tray_color\":\"C9A227FF\"}]},"
    "{\"id\":\"1\",\"tray\":["
    "{\"id\":\"0\",\"tray_info_idx\":\"GFB00\",\"tray_type\":\"ABS\",\"tray_color\":\"D7263DFF\"},"
    "{\"id\":\"1\",\"tray_info_idx\":\"GFU01\",\"tray_type\":\"TPU\",\"tray_color\":\"2EC4B6FF\"},"
    "{\"id\":\"2\",\"tray_info_idx\":\"GFN03\",\"tray_type\":\"PA-CF\",\"tray_color\":\"3A3A3AFF\"},"
    "{\"id\":\"3\"}]}]}";

static void apply_report(
    struct bambu_printer_snapshot *snapshot,
    const char *job,
    const char *state,
    int stage,
    int percent,
    int remaining,
    int nozzle,
    int bed,
    const char *ams,
    uint64_t now
)
{
    char payload[4096];
    int layer = percent * 412 / 100;
    int length = snprintf(
        payload,
        sizeof(payload),
        "{\"print\":{\"gcode_state\":\"%s\",\"subtask_name\":\"%s\",\"task_id\":\"1001\",\"print_type\":\"local\","
        "\"mc_percent\":%d,\"mc_remaining_time\":%d,\"layer_num\":%d,\"total_layer_num\":412,"
        "\"nozzle_temper\":%d,\"bed_temper\":%d,\"cooling_fan_speed\":\"10\",\"spd_lvl\":2,\"stg_cur\":%d,%s}}",
        state,
        job,
        percent,
        remaining,
        layer,
        nozzle,
        bed,
        stage,
        ams
    );

    if (length > 0 && (size_t)length < sizeof(payload))
        bambu_report_apply(snapshot, payload, (size_t)length, now, NULL);
}

void bambu_simulation_build(
    enum bambu_simulation_fixture fixture,
    uint64_t elapsed_seconds,
    uint64_t now_monotonic_ms,
    struct bambu_printer_snapshot *snapshot
)
{
    static const char job[] = "Articulated Dragon";
    int percent = 12 + (int)(elapsed_seconds % 80);

    if (fixture == BAMBU_SIM_CYCLE) {
        uint64_t step = elapsed_seconds / BAMBU_SIM_CYCLE_SECONDS;
        fixture = (enum bambu_simulation_fixture)(BAMBU_SIM_PRINTING + step % (BAMBU_SIM_FIXTURE_COUNT - 1));
    }

    bambu_snapshot_init(snapshot);
    snapshot->connection_state = BAMBU_CONNECTION_LIVE;

    switch (fixture) {
    case BAMBU_SIM_PREPARING:
        apply_report(snapshot, job, "PREPARE", 1, 0, 214, 150, 55, ams_one, now_monotonic_ms);
        break;
    case BAMBU_SIM_PAUSED:
        apply_report(snapshot, job, "PAUSE", 16, 57, 92, 180, 60, ams_one, now_monotonic_ms);
        break;
    case BAMBU_SIM_FINISHED:
        apply_report(snapshot, job, "FINISH", 255, 100, 0, 38, 30, ams_one, now_monotonic_ms);
        break;
    case BAMBU_SIM_FAILED:
        apply_report(snapshot, job, "FAILED", 255, 64, 77, 45, 34, ams_one, now_monotonic_ms);
        break;
    case BAMBU_SIM_IDLE:
        bambu_report_apply(
            snapshot,
            "{\"print\":{\"gcode_state\":\"IDLE\",\"stg_cur\":255,\"nozzle_temper\":27,\"bed_temper\":24}}",
            82,
            now_monotonic_ms,
            NULL
        );
        break;
    case BAMBU_SIM_MULTI_AMS:
        apply_report(snapshot, job, "RUNNING", 4, 38, 141, 220, 60, ams_two, now_monotonic_ms);
        break;
    case BAMBU_SIM_LONG_TEXT:
        apply_report(
            snapshot,
            "Extremely long project file name that will never fit on the panel at any reasonable width v12 final FINAL",
            "RUNNING",
            0,
            71,
            6000,
            220,
            60,
            ams_two,
            now_monotonic_ms
        );
        break;
    case BAMBU_SIM_STALE:
        apply_report(snapshot, job, "RUNNING", 0, 42, 124, 219, 60, ams_one, 0);
        snapshot->updated_monotonic_ms = now_monotonic_ms > SIM_STALE_AGE_MS ? now_monotonic_ms - SIM_STALE_AGE_MS : 0;
        break;
    case BAMBU_SIM_RECONNECTING:
        apply_report(snapshot, job, "RUNNING", 0, 42, 124, 219, 60, ams_one, now_monotonic_ms);
        snapshot->connection_state = BAMBU_CONNECTION_RECONNECTING;
        break;
    case BAMBU_SIM_OFFLINE:
        apply_report(snapshot, job, "RUNNING", 0, 42, 124, 219, 60, ams_one, now_monotonic_ms);
        snapshot->connection_state = BAMBU_CONNECTION_OFFLINE;
        break;
    case BAMBU_SIM_AUTH_FAILED:
        snapshot->connection_state = BAMBU_CONNECTION_AUTHENTICATION_FAILED;
        break;
    case BAMBU_SIM_PRINTING:
    default:
        apply_report(
            snapshot,
            job,
            "RUNNING",
            0,
            percent,
            214 - percent * 2,
            219,
            60,
            ams_one,
            now_monotonic_ms
        );
        break;
    }

    if (snapshot->job_name[0] != '\0') {
        bambu_copy_text(snapshot->print_profile, sizeof(snapshot->print_profile), "0.20mm Standard @BBL X1C");
        snapshot->has_total_time = true;
        snapshot->total_time_seconds = 15420;
        snapshot->has_total_weight = true;
        snapshot->total_weight_grams = 187.4;
    }
}
