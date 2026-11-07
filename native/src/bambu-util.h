#pragma once

/* OBS-independent helpers: reconnect backoff, colours, certificate fingerprints,
 * and redaction of anything that must not reach logs or diagnostics. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BAMBU_FINGERPRINT_HEX_LENGTH 64
#define BAMBU_FINGERPRINT_CAPACITY (BAMBU_FINGERPRINT_HEX_LENGTH + 1)
#define BAMBU_REDACTED "<redacted>"

/* Exponential backoff with jitter: the delay lies in [ceiling/2, ceiling], where
 * ceiling = min(maximum, base * 2^attempt). random_unit is in [0, 1]. */
uint32_t bambu_backoff_delay_ms(uint32_t attempt, uint32_t base_ms, uint32_t maximum_ms, double random_unit);

/* Accepts RRGGBB or RRGGBBAA; alpha is discarded. */
bool bambu_parse_hex_color(const char *value, uint32_t *rgb);

/* Normalises to 64 lowercase hex characters, ignoring colons, spaces, and case. */
bool bambu_normalize_fingerprint(const char *value, char out[BAMBU_FINGERPRINT_CAPACITY]);

/* Copies input to out, replacing each secret, then anything that looks like a
 * bearer token, an email address, or a URL query string. Bounded by capacity. */
void bambu_redact_text(
    const char *input,
    const char *const *secrets,
    size_t secret_count,
    char *out,
    size_t capacity
);

/* Keeps only scheme, host, and path. Yields "<invalid-url>" for anything else. */
void bambu_redact_url(const char *url, char *out, size_t capacity);

/* Keeps the final four characters of a printer serial number. */
void bambu_redact_serial(const char *serial, char *out, size_t capacity);
