#pragma once

/* Small bounded JSON DOM used for MQTT, cloud, and profile payloads.
 * It has no locale dependence and rejects deep or oversized documents. */

#include <stdbool.h>
#include <stddef.h>

#define BAMBU_JSON_MAX_DEPTH 32
#define BAMBU_JSON_MAX_BYTES (8u * 1024u * 1024u)

enum bambu_json_type {
    BAMBU_JSON_NULL = 0,
    BAMBU_JSON_BOOL,
    BAMBU_JSON_NUMBER,
    BAMBU_JSON_STRING,
    BAMBU_JSON_ARRAY,
    BAMBU_JSON_OBJECT,
};

struct bambu_json {
    enum bambu_json_type type;
    bool boolean;
    double number;
    char *string;
    char *key;
    struct bambu_json *child;
    struct bambu_json *next;
};

struct bambu_json *bambu_json_parse(const char *text, size_t length);
void bambu_json_free(struct bambu_json *root);

const struct bambu_json *bambu_json_get(const struct bambu_json *object, const char *key);

/* Accepts JSON numbers and numeric strings, which Bambu firmware mixes freely. */
bool bambu_json_as_double(const struct bambu_json *node, double *out);
bool bambu_json_as_int(const struct bambu_json *node, int *out);

/* Returns a string value, or NULL for any other type. */
const char *bambu_json_as_string(const struct bambu_json *node);

/* Renders a string or number into text. Returns false for other types. */
bool bambu_json_as_text(const struct bambu_json *node, char *out, size_t capacity);

/* Writes a JSON-escaped, quoted copy of value. Returns the bytes needed
 * excluding the terminator; output is truncated safely to capacity. */
size_t bambu_json_escape(const char *value, char *out, size_t capacity);
