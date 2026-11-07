#pragma once

/* Firmware identifier tables. Lookups never fail: an unknown identifier yields a
 * readable fallback, so a new firmware cannot break or mislabel the overlay. */

#include <stdbool.h>
#include <stddef.h>

/* Writes a display name for a stg_cur value. */
void bambu_stage_name(int stage_id, char *out, size_t capacity);

/* Returns true for known levels; false still writes a readable fallback. */
bool bambu_speed_name(int speed_level, char *out, size_t capacity);

/* Writes a display name for a tray_info_idx, falling back to the tray_type
 * reported by the printer and finally to the raw identifier. */
void bambu_filament_name(const char *filament_id, const char *tray_type, char *out, size_t capacity);
