#pragma once

/* Reduces partial MQTT report messages into a snapshot. Fields omitted from a
 * message keep their previous value; malformed fields are ignored the same way. */

#include "printer-snapshot.h"

enum bambu_report_result {
    BAMBU_REPORT_IGNORED = 0, /* valid JSON without a print section */
    BAMBU_REPORT_APPLIED,
    BAMBU_REPORT_INVALID, /* unparsable payload; snapshot untouched */
};

struct bambu_report_events {
    bool job_changed; /* a different job was detected, or the first one seen */
};

enum bambu_report_result bambu_report_apply(
    struct bambu_printer_snapshot *snapshot,
    const char *payload,
    size_t length,
    uint64_t now_monotonic_ms,
    struct bambu_report_events *events
);
