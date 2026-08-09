#include "printer-snapshot.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void test_initial_state(void)
{
    struct bambu_printer_snapshot snapshot;
    bambu_snapshot_init(&snapshot);
    assert(snapshot.revision == 0);
    assert(snapshot.connection_state == BAMBU_CONNECTION_CONFIGURATION_REQUIRED);
    assert(bambu_snapshot_is_stale(&snapshot, 1000, 500));
}

static void test_update_and_revision(void)
{
    struct bambu_printer_snapshot snapshot;
    struct bambu_printer_snapshot_update update = {0};
    bambu_snapshot_init(&snapshot);

    update.has_connection_state = true;
    update.connection_state = BAMBU_CONNECTION_LIVE;
    update.has_progress = true;
    update.progress_percent = 42.5;
    update.has_job_name = true;
    update.job_name = "Calibration Cube";
    update.has_stage = true;
    update.stage = "printing";
    update.updated_monotonic_ms = 1000;

    assert(bambu_snapshot_apply(&snapshot, &update));
    assert(snapshot.revision == 1);
    assert(snapshot.progress_percent == 42.5);
    assert(strcmp(snapshot.job_name, "Calibration Cube") == 0);
    assert(!bambu_snapshot_apply(&snapshot, &update));
    assert(snapshot.revision == 1);

    update.updated_monotonic_ms = 1500;
    assert(!bambu_snapshot_apply(&snapshot, &update));
    assert(snapshot.updated_monotonic_ms == 1500);

    memset(&update, 0, sizeof(update));
    update.has_progress = true;
    update.progress_percent = 50.0;
    update.updated_monotonic_ms = 2000;
    assert(bambu_snapshot_apply(&snapshot, &update));
    assert(strcmp(snapshot.job_name, "Calibration Cube") == 0);
    assert(strcmp(snapshot.stage, "printing") == 0);
}

static void test_bounds_and_staleness(void)
{
    struct bambu_printer_snapshot snapshot;
    struct bambu_printer_snapshot_update update = {0};
    bambu_snapshot_init(&snapshot);

    update.has_connection_state = true;
    update.connection_state = BAMBU_CONNECTION_LIVE;
    update.has_progress = true;
    update.progress_percent = 150.0;
    update.has_nozzle_temperature = true;
    update.nozzle_temperature_c = NAN;
    update.has_layer = true;
    update.current_layer = 200;
    update.total_layers = 100;
    update.updated_monotonic_ms = 5000;

    assert(bambu_snapshot_apply(&snapshot, &update));
    assert(snapshot.progress_percent == 100.0);
    assert(snapshot.nozzle_temperature_c == -50.0);
    assert(snapshot.current_layer == 100);
    assert(!bambu_snapshot_is_stale(&snapshot, 5999, 1000));
    assert(bambu_snapshot_is_stale(&snapshot, 6001, 1000));
}

int main(void)
{
    test_initial_state();
    test_update_and_revision();
    test_bounds_and_staleness();
    puts("bambu-overlay-core-tests: ok");
    return 0;
}
