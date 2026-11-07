#include "bambu-format.h"

#include <stdio.h>

void bambu_format_remaining(int minutes, char *out, size_t capacity)
{
    int days;
    int hours;

    if (minutes < 0)
        minutes = 0;
    days = minutes / (60 * 24);
    hours = (minutes / 60) % 24;
    minutes %= 60;
    if (days > 0)
        snprintf(out, capacity, "%dd %dh", days, hours);
    else if (hours > 0)
        snprintf(out, capacity, "%dh %02dm", hours, minutes);
    else
        snprintf(out, capacity, "%dm", minutes);
}

void bambu_format_duration_seconds(int seconds, char *out, size_t capacity)
{
    int hours;
    int minutes;

    if (seconds < 0)
        seconds = 0;
    hours = seconds / 3600;
    minutes = (seconds / 60) % 60;
    seconds %= 60;
    if (hours > 0)
        snprintf(out, capacity, "%dh %02dm %02ds", hours, minutes, seconds);
    else if (minutes > 0)
        snprintf(out, capacity, "%dm %02ds", minutes, seconds);
    else
        snprintf(out, capacity, "%ds", seconds);
}

void bambu_format_eta(int hour, int minute, int add_minutes, bool use_24_hour, char *out, size_t capacity)
{
    int total;
    int days;
    int display_hour;
    char clock[16];

    if (add_minutes < 0)
        add_minutes = 0;
    total = hour * 60 + minute + add_minutes;
    days = total / (60 * 24);
    hour = (total / 60) % 24;
    minute = total % 60;

    if (use_24_hour) {
        snprintf(clock, sizeof(clock), "%02d:%02d", hour, minute);
    } else {
        display_hour = hour % 12;
        if (display_hour == 0)
            display_hour = 12;
        snprintf(clock, sizeof(clock), "%d:%02d %s", display_hour, minute, hour < 12 ? "AM" : "PM");
    }
    if (days > 0)
        snprintf(out, capacity, "%s +%dd", clock, days);
    else
        snprintf(out, capacity, "%s", clock);
}

void bambu_format_temperature(
    double celsius,
    enum bambu_temperature_unit unit,
    int decimals,
    char *out,
    size_t capacity
)
{
    double value = unit == BAMBU_UNIT_FAHRENHEIT ? celsius * 9.0 / 5.0 + 32.0 : celsius;

    if (decimals < 0)
        decimals = 0;
    if (decimals > 2)
        decimals = 2;
    snprintf(
        out,
        capacity,
        "%.*f\xC2\xB0%c",
        decimals,
        value,
        unit == BAMBU_UNIT_FAHRENHEIT ? 'F' : 'C'
    );
}

void bambu_format_percent(double percent, char *out, size_t capacity)
{
    if (percent < 0.0)
        percent = 0.0;
    if (percent > 100.0)
        percent = 100.0;
    snprintf(out, capacity, "%.0f%%", percent);
}

void bambu_format_weight(double grams, char *out, size_t capacity)
{
    if (grams >= 1000.0)
        snprintf(out, capacity, "%.2f kg", grams / 1000.0);
    else
        snprintf(out, capacity, "%.1f g", grams);
}

void bambu_format_layers(int current, int total, char *out, size_t capacity)
{
    if (current < 0)
        current = 0;
    if (total > 0 && current > total)
        current = total;
    if (total > 0)
        snprintf(out, capacity, "%d / %d", current, total);
    else
        snprintf(out, capacity, "%d / ?", current);
}
