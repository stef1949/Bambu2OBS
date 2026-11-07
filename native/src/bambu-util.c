#include "bambu-util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t bambu_backoff_delay_ms(uint32_t attempt, uint32_t base_ms, uint32_t maximum_ms, double random_unit)
{
    uint64_t ceiling = base_ms;
    double half;

    for (uint32_t i = 0; i < attempt && ceiling < maximum_ms; i++)
        ceiling *= 2;
    if (ceiling > maximum_ms)
        ceiling = maximum_ms;
    if (random_unit < 0.0)
        random_unit = 0.0;
    if (random_unit > 1.0)
        random_unit = 1.0;
    half = (double)ceiling / 2.0;
    return (uint32_t)(half + half * random_unit);
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

bool bambu_parse_hex_color(const char *value, uint32_t *rgb)
{
    uint32_t parsed = 0;
    size_t length;

    if (value == NULL || rgb == NULL)
        return false;
    length = strlen(value);
    if (length != 6 && length != 8)
        return false;
    for (size_t i = 0; i < length; i++) {
        int digit = hex_digit(value[i]);
        if (digit < 0)
            return false;
        if (i < 6)
            parsed = (parsed << 4) | (uint32_t)digit;
    }
    *rgb = parsed;
    return true;
}

bool bambu_normalize_fingerprint(const char *value, char out[BAMBU_FINGERPRINT_CAPACITY])
{
    size_t used = 0;

    if (value == NULL || out == NULL)
        return false;
    for (; *value != '\0'; value++) {
        int digit;
        if (*value == ':' || *value == ' ' || *value == '-')
            continue;
        digit = hex_digit(*value);
        if (digit < 0 || used >= BAMBU_FINGERPRINT_HEX_LENGTH)
            return false;
        out[used++] = "0123456789abcdef"[digit];
    }
    if (used != BAMBU_FINGERPRINT_HEX_LENGTH)
        return false;
    out[used] = '\0';
    return true;
}

static void replace_all(char *text, size_t capacity, const char *needle)
{
    size_t needle_length = strlen(needle);
    size_t replacement_length = strlen(BAMBU_REDACTED);
    char *cursor = text;

    if (needle_length == 0)
        return;
    while ((cursor = strstr(cursor, needle)) != NULL) {
        size_t tail_length = strlen(cursor + needle_length);
        size_t prefix = (size_t)(cursor - text);

        if (prefix + replacement_length + tail_length + 1 > capacity)
            tail_length = capacity > prefix + replacement_length + 1 ? capacity - prefix - replacement_length - 1 : 0;
        if (prefix + replacement_length + 1 > capacity) {
            *cursor = '\0';
            return;
        }
        memmove(cursor + replacement_length, cursor + needle_length, tail_length);
        memcpy(cursor, BAMBU_REDACTED, replacement_length);
        cursor[replacement_length + tail_length] = '\0';
        cursor += replacement_length;
    }
}

static bool token_char(char c)
{
    return isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-' || c == '+' || c == '%';
}

/* Replaces [start, stop) with the redaction marker. Returns the resume point. */
static char *replace_span(char *text, size_t capacity, char *start, char *stop)
{
    size_t replacement_length = strlen(BAMBU_REDACTED);
    size_t prefix = (size_t)(start - text);
    size_t tail_length = strlen(stop);

    if (prefix + replacement_length + 1 > capacity) {
        *start = '\0';
        return start;
    }
    if (prefix + replacement_length + tail_length + 1 > capacity)
        tail_length = capacity - prefix - replacement_length - 1;
    memmove(start + replacement_length, stop, tail_length);
    memcpy(start, BAMBU_REDACTED, replacement_length);
    start[replacement_length + tail_length] = '\0';
    return start + replacement_length;
}

static void redact_bearer(char *text, size_t capacity)
{
    static const char marker[] = "Bearer ";
    char *cursor = text;

    while ((cursor = strstr(cursor, marker)) != NULL) {
        char *start = cursor + sizeof(marker) - 1;
        char *stop = start;
        while (*stop != '\0' && !isspace((unsigned char)*stop) && *stop != '"' && *stop != '\'')
            stop++;
        if (stop == start) {
            cursor = start;
            continue;
        }
        cursor = replace_span(text, capacity, start, stop);
    }
}

static void redact_emails(char *text, size_t capacity)
{
    char *at = text;

    while ((at = strchr(at, '@')) != NULL) {
        char *start = at;
        char *stop = at + 1;
        bool dotted = false;

        while (start > text && token_char(start[-1]))
            start--;
        while (token_char(*stop)) {
            dotted = dotted || *stop == '.';
            stop++;
        }
        if (start == at || !dotted) {
            at++;
            continue;
        }
        at = replace_span(text, capacity, start, stop);
    }
}

static void redact_query_strings(char *text, size_t capacity)
{
    char *scheme = text;

    while ((scheme = strstr(scheme, "://")) != NULL) {
        char *cursor = scheme + 3;
        while (*cursor != '\0' && !isspace((unsigned char)*cursor) && *cursor != '?' && *cursor != '"')
            cursor++;
        scheme += 3;
        if (*cursor == '?') {
            char *stop = cursor;
            while (*stop != '\0' && !isspace((unsigned char)*stop) && *stop != '"')
                stop++;
            scheme = replace_span(text, capacity, cursor, stop);
        }
    }
}

void bambu_redact_text(
    const char *input,
    const char *const *secrets,
    size_t secret_count,
    char *out,
    size_t capacity
)
{
    const char *source = input != NULL ? input : "";
    size_t work_capacity;
    char *work;

    if (out == NULL || capacity == 0)
        return;

    /* Redact at full length and only then truncate. Truncating first could cut a
     * secret so that it no longer matches, leaking its prefix. Each replacement
     * grows the text by at most the marker length per source byte. */
    work_capacity = strlen(source) * (strlen(BAMBU_REDACTED) + 1) + 1;
    work = malloc(work_capacity);
    if (work == NULL) {
        snprintf(out, capacity, "%s", BAMBU_REDACTED);
        return;
    }
    snprintf(work, work_capacity, "%s", source);
    for (size_t i = 0; i < secret_count; i++) {
        if (secrets[i] != NULL)
            replace_all(work, work_capacity, secrets[i]);
    }
    redact_bearer(work, work_capacity);
    redact_query_strings(work, work_capacity);
    redact_emails(work, work_capacity);
    snprintf(out, capacity, "%s", work);
    free(work);
}

void bambu_redact_url(const char *url, char *out, size_t capacity)
{
    const char *scheme_end;
    const char *authority;
    const char *authority_end;
    const char *at;
    const char *stop;

    if (out == NULL || capacity == 0)
        return;
    scheme_end = url != NULL ? strstr(url, "://") : NULL;
    if (scheme_end == NULL || scheme_end == url || scheme_end[3] == '\0') {
        snprintf(out, capacity, "<invalid-url>");
        return;
    }
    authority = scheme_end + 3;
    authority_end = authority + strcspn(authority, "/?#");
    if (authority_end == authority) {
        snprintf(out, capacity, "<invalid-url>");
        return;
    }
    at = authority;
    for (const char *scan = authority; scan < authority_end; scan++) {
        if (*scan == '@')
            at = scan + 1;
    }
    stop = authority_end + strcspn(authority_end, "?#");
    snprintf(
        out,
        capacity,
        "%.*s://%.*s%.*s",
        (int)(scheme_end - url),
        url,
        (int)(authority_end - at),
        at,
        (int)(stop - authority_end),
        authority_end
    );
}

void bambu_redact_serial(const char *serial, char *out, size_t capacity)
{
    size_t length;

    if (out == NULL || capacity == 0)
        return;
    length = serial != NULL ? strlen(serial) : 0;
    if (length == 0)
        snprintf(out, capacity, "<none>");
    else if (length <= 4)
        snprintf(out, capacity, "****");
    else
        snprintf(out, capacity, "****%s", serial + length - 4);
}
