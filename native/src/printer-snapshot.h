#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BAMBU_JOB_NAME_CAPACITY 256
#define BAMBU_STAGE_CAPACITY 96

enum bambu_connection_state {
    BAMBU_CONNECTION_CONFIGURATION_REQUIRED = 0,
    BAMBU_CONNECTION_CONNECTING,
    BAMBU_CONNECTION_LIVE,
    BAMBU_CONNECTION_RECONNECTING,
    BAMBU_CONNECTION_OFFLINE,
    BAMBU_CONNECTION_AUTHENTICATION_FAILED,
    BAMBU_CONNECTION_ERROR,
};

struct bambu_printer_snapshot {
    uint64_t revision;
    uint64_t updated_monotonic_ms;
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
    char job_name[BAMBU_JOB_NAME_CAPACITY];
    char stage[BAMBU_STAGE_CAPACITY];
};

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
