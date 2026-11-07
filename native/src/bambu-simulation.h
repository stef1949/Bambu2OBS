#pragma once

/* Deterministic sample states for design and support work. Fixtures run through
 * the real report reducer, carry no network side effects, and contain no
 * credentials. */

#include "printer-snapshot.h"

enum bambu_simulation_fixture {
    BAMBU_SIM_CYCLE = 0, /* step through every fixture below */
    BAMBU_SIM_PRINTING,
    BAMBU_SIM_PREPARING,
    BAMBU_SIM_PAUSED,
    BAMBU_SIM_FINISHED,
    BAMBU_SIM_FAILED,
    BAMBU_SIM_IDLE,
    BAMBU_SIM_MULTI_AMS,
    BAMBU_SIM_LONG_TEXT,
    BAMBU_SIM_STALE,
    BAMBU_SIM_RECONNECTING,
    BAMBU_SIM_OFFLINE,
    BAMBU_SIM_AUTH_FAILED,
    BAMBU_SIM_FIXTURE_COUNT,
};

#define BAMBU_SIM_CYCLE_SECONDS 6

/* Builds the fixture. elapsed_seconds advances the cycle and the printing
 * progress; stale_age_ms ages the update time for the stale fixture. */
void bambu_simulation_build(
    enum bambu_simulation_fixture fixture,
    uint64_t elapsed_seconds,
    uint64_t now_monotonic_ms,
    struct bambu_printer_snapshot *snapshot
);
