#include "printer-snapshot.h"

#include <math.h>
#include <string.h>

static double clamp_double(double value, double minimum, double maximum)
{
    if (!isfinite(value))
        return minimum;
    if (value < minimum)
        return minimum;
    if (value > maximum)
        return maximum;
    return value;
}

static bool copy_if_changed(char *destination, size_t capacity, const char *value)
{
    const char *source = value != NULL ? value : "";
    size_t source_length;

    if (capacity == 0)
        return false;

    source_length = strlen(source);
    if (source_length >= capacity)
        source_length = capacity - 1;
    if (strlen(destination) == source_length && memcmp(destination, source, source_length) == 0)
        return false;

    memcpy(destination, source, source_length);
    destination[source_length] = '\0';
    return true;
}

void bambu_snapshot_init(struct bambu_printer_snapshot *snapshot)
{
    if (snapshot == NULL)
        return;
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->connection_state = BAMBU_CONNECTION_CONFIGURATION_REQUIRED;
}

bool bambu_snapshot_apply(
    struct bambu_printer_snapshot *snapshot,
    const struct bambu_printer_snapshot_update *update
)
{
    bool changed = false;
    bool observed;
    double value;

    if (snapshot == NULL || update == NULL)
        return false;

    observed = update->has_connection_state || update->has_progress || update->has_nozzle_temperature ||
               update->has_bed_temperature || update->has_layer || update->has_job_name || update->has_stage;

    if (update->has_connection_state && snapshot->connection_state != update->connection_state) {
        snapshot->connection_state = update->connection_state;
        changed = true;
    }

    if (update->has_progress) {
        value = clamp_double(update->progress_percent, 0.0, 100.0);
        if (!snapshot->has_progress || snapshot->progress_percent != value) {
            snapshot->has_progress = true;
            snapshot->progress_percent = value;
            changed = true;
        }
    }

    if (update->has_nozzle_temperature) {
        value = clamp_double(update->nozzle_temperature_c, -50.0, 500.0);
        if (!snapshot->has_nozzle_temperature || snapshot->nozzle_temperature_c != value) {
            snapshot->has_nozzle_temperature = true;
            snapshot->nozzle_temperature_c = value;
            changed = true;
        }
    }

    if (update->has_bed_temperature) {
        value = clamp_double(update->bed_temperature_c, -50.0, 200.0);
        if (!snapshot->has_bed_temperature || snapshot->bed_temperature_c != value) {
            snapshot->has_bed_temperature = true;
            snapshot->bed_temperature_c = value;
            changed = true;
        }
    }

    if (update->has_layer) {
        int current_layer = update->current_layer < 0 ? 0 : update->current_layer;
        int total_layers = update->total_layers < 0 ? 0 : update->total_layers;
        if (total_layers > 0 && current_layer > total_layers)
            current_layer = total_layers;
        if (!snapshot->has_layer || snapshot->current_layer != current_layer ||
            snapshot->total_layers != total_layers) {
            snapshot->has_layer = true;
            snapshot->current_layer = current_layer;
            snapshot->total_layers = total_layers;
            changed = true;
        }
    }

    if (update->has_job_name)
        changed = copy_if_changed(snapshot->job_name, sizeof(snapshot->job_name), update->job_name) || changed;
    if (update->has_stage)
        changed = copy_if_changed(snapshot->stage, sizeof(snapshot->stage), update->stage) || changed;

    if (changed) {
        snapshot->revision++;
    }
    if (observed)
        snapshot->updated_monotonic_ms = update->updated_monotonic_ms;
    return changed;
}

bool bambu_snapshot_is_stale(
    const struct bambu_printer_snapshot *snapshot,
    uint64_t now_monotonic_ms,
    uint64_t stale_after_ms
)
{
    if (snapshot == NULL || snapshot->connection_state != BAMBU_CONNECTION_LIVE)
        return true;
    if (now_monotonic_ms < snapshot->updated_monotonic_ms)
        return false;
    return now_monotonic_ms - snapshot->updated_monotonic_ms > stale_after_ms;
}

const char *bambu_connection_state_name(enum bambu_connection_state state)
{
    switch (state) {
    case BAMBU_CONNECTION_CONFIGURATION_REQUIRED:
        return "configuration_required";
    case BAMBU_CONNECTION_CONNECTING:
        return "connecting";
    case BAMBU_CONNECTION_LIVE:
        return "live";
    case BAMBU_CONNECTION_RECONNECTING:
        return "reconnecting";
    case BAMBU_CONNECTION_OFFLINE:
        return "offline";
    case BAMBU_CONNECTION_AUTHENTICATION_FAILED:
        return "authentication_failed";
    case BAMBU_CONNECTION_ERROR:
        return "error";
    default:
        return "unknown";
    }
}
