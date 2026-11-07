#pragma once

/* Display formatting. All functions are pure so they can be tested with fixed
 * inputs; stored values remain in Celsius, minutes, and grams. */

#include <stdbool.h>
#include <stddef.h>

enum bambu_temperature_unit {
    BAMBU_UNIT_CELSIUS = 0,
    BAMBU_UNIT_FAHRENHEIT,
};

/* "45m", "1h 23m", or "2d 3h". */
void bambu_format_remaining(int minutes, char *out, size_t capacity);

/* "1h 23m 05s" style duration, without leading zero units. */
void bambu_format_duration_seconds(int seconds, char *out, size_t capacity);

/* Clock time reached after adding minutes to a wall clock time, for example
 * "14:52", "2:52 PM", or "14:52 +1d". */
void bambu_format_eta(int hour, int minute, int add_minutes, bool use_24_hour, char *out, size_t capacity);

/* UTF-8 text such as "219.5°C" or "427°F". */
void bambu_format_temperature(
    double celsius,
    enum bambu_temperature_unit unit,
    int decimals,
    char *out,
    size_t capacity
);

void bambu_format_percent(double percent, char *out, size_t capacity);
void bambu_format_weight(double grams, char *out, size_t capacity);

/* "120 / 350", "120 / ?" when the total is unknown; current is clamped to total. */
void bambu_format_layers(int current, int total, char *out, size_t capacity);
