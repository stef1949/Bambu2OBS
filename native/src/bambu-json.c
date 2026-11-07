#include "bambu-json.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct parser {
    const char *cursor;
    const char *end;
};

static void skip_whitespace(struct parser *parser)
{
    while (parser->cursor < parser->end) {
        char c = *parser->cursor;
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            return;
        parser->cursor++;
    }
}

static bool consume_literal(struct parser *parser, const char *literal)
{
    size_t length = strlen(literal);
    if ((size_t)(parser->end - parser->cursor) < length)
        return false;
    if (memcmp(parser->cursor, literal, length) != 0)
        return false;
    parser->cursor += length;
    return true;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static bool parse_hex4(struct parser *parser, uint32_t *out)
{
    uint32_t value = 0;
    if (parser->end - parser->cursor < 4)
        return false;
    for (int i = 0; i < 4; i++) {
        int digit = hex_value(parser->cursor[i]);
        if (digit < 0)
            return false;
        value = (value << 4) | (uint32_t)digit;
    }
    parser->cursor += 4;
    *out = value;
    return true;
}

static size_t encode_utf8(uint32_t codepoint, char *out)
{
    if (codepoint < 0x80) {
        out[0] = (char)codepoint;
        return 1;
    }
    if (codepoint < 0x800) {
        out[0] = (char)(0xC0 | (codepoint >> 6));
        out[1] = (char)(0x80 | (codepoint & 0x3F));
        return 2;
    }
    if (codepoint < 0x10000) {
        out[0] = (char)(0xE0 | (codepoint >> 12));
        out[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        out[2] = (char)(0x80 | (codepoint & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (codepoint >> 18));
    out[1] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
    out[3] = (char)(0x80 | (codepoint & 0x3F));
    return 4;
}

/* An escape never decodes to more bytes than its source text, so a single
 * allocation sized from the raw token always fits. */
static char *parse_string(struct parser *parser)
{
    const char *start;
    const char *scan;
    char *result;
    size_t used = 0;

    if (parser->cursor >= parser->end || *parser->cursor != '"')
        return NULL;
    parser->cursor++;
    start = parser->cursor;

    for (scan = start; scan < parser->end && *scan != '"'; scan++) {
        if (*scan == '\\') {
            scan++;
            if (scan >= parser->end)
                return NULL;
        }
    }
    if (scan >= parser->end)
        return NULL;

    result = malloc((size_t)(scan - start) + 1);
    if (result == NULL)
        return NULL;

    while (parser->cursor < scan) {
        char c = *parser->cursor++;
        if ((unsigned char)c < 0x20) {
            free(result);
            return NULL;
        }
        if (c != '\\') {
            result[used++] = c;
            continue;
        }
        c = *parser->cursor++;
        switch (c) {
        case '"':
        case '\\':
        case '/':
            result[used++] = c;
            break;
        case 'b':
            result[used++] = '\b';
            break;
        case 'f':
            result[used++] = '\f';
            break;
        case 'n':
            result[used++] = '\n';
            break;
        case 'r':
            result[used++] = '\r';
            break;
        case 't':
            result[used++] = '\t';
            break;
        case 'u': {
            uint32_t codepoint;
            uint32_t low;
            if (!parse_hex4(parser, &codepoint)) {
                free(result);
                return NULL;
            }
            if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
                if (scan - parser->cursor >= 6 && parser->cursor[0] == '\\' && parser->cursor[1] == 'u') {
                    parser->cursor += 2;
                    if (!parse_hex4(parser, &low) || low < 0xDC00 || low > 0xDFFF) {
                        free(result);
                        return NULL;
                    }
                    codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
                } else {
                    codepoint = 0xFFFD;
                }
            } else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF) {
                codepoint = 0xFFFD;
            }
            used += encode_utf8(codepoint, result + used);
            break;
        }
        default:
            free(result);
            return NULL;
        }
    }
    parser->cursor = scan + 1;
    result[used] = '\0';
    return result;
}

/* Hand-rolled so a process-wide LC_NUMERIC can never change the result. */
static bool parse_number_text(const char *text, const char *end, double *out, const char **stop)
{
    const char *cursor = text;
    double value = 0.0;
    double scale = 0.1;
    bool negative = false;
    bool digits = false;
    int exponent = 0;

    if (cursor < end && (*cursor == '-' || *cursor == '+')) {
        negative = *cursor == '-';
        cursor++;
    }
    while (cursor < end && *cursor >= '0' && *cursor <= '9') {
        value = value * 10.0 + (*cursor - '0');
        digits = true;
        cursor++;
    }
    if (cursor < end && *cursor == '.') {
        cursor++;
        while (cursor < end && *cursor >= '0' && *cursor <= '9') {
            value += (*cursor - '0') * scale;
            scale *= 0.1;
            digits = true;
            cursor++;
        }
    }
    if (!digits)
        return false;
    if (cursor < end && (*cursor == 'e' || *cursor == 'E')) {
        bool exponent_negative = false;
        bool exponent_digits = false;
        cursor++;
        if (cursor < end && (*cursor == '-' || *cursor == '+')) {
            exponent_negative = *cursor == '-';
            cursor++;
        }
        while (cursor < end && *cursor >= '0' && *cursor <= '9') {
            if (exponent < 10000)
                exponent = exponent * 10 + (*cursor - '0');
            exponent_digits = true;
            cursor++;
        }
        if (!exponent_digits)
            return false;
        value *= pow(10.0, exponent_negative ? -exponent : exponent);
    }
    *out = negative ? -value : value;
    *stop = cursor;
    return true;
}

static struct bambu_json *new_node(enum bambu_json_type type)
{
    struct bambu_json *node = calloc(1, sizeof(*node));
    if (node != NULL)
        node->type = type;
    return node;
}

static struct bambu_json *parse_value(struct parser *parser, int depth);

static struct bambu_json *parse_container(struct parser *parser, int depth, bool object)
{
    struct bambu_json *node = new_node(object ? BAMBU_JSON_OBJECT : BAMBU_JSON_ARRAY);
    struct bambu_json **tail;
    char closer = object ? '}' : ']';

    if (node == NULL)
        return NULL;
    tail = &node->child;
    parser->cursor++;
    skip_whitespace(parser);
    if (parser->cursor < parser->end && *parser->cursor == closer) {
        parser->cursor++;
        return node;
    }

    for (;;) {
        char *key = NULL;
        struct bambu_json *child;

        if (object) {
            skip_whitespace(parser);
            key = parse_string(parser);
            if (key == NULL)
                goto fail;
            skip_whitespace(parser);
            if (parser->cursor >= parser->end || *parser->cursor != ':') {
                free(key);
                goto fail;
            }
            parser->cursor++;
        }

        child = parse_value(parser, depth + 1);
        if (child == NULL) {
            free(key);
            goto fail;
        }
        child->key = key;
        *tail = child;
        tail = &child->next;

        skip_whitespace(parser);
        if (parser->cursor >= parser->end)
            goto fail;
        if (*parser->cursor == ',') {
            parser->cursor++;
            continue;
        }
        if (*parser->cursor == closer) {
            parser->cursor++;
            return node;
        }
        goto fail;
    }

fail:
    bambu_json_free(node);
    return NULL;
}

static struct bambu_json *parse_value(struct parser *parser, int depth)
{
    struct bambu_json *node;

    if (depth > BAMBU_JSON_MAX_DEPTH)
        return NULL;
    skip_whitespace(parser);
    if (parser->cursor >= parser->end)
        return NULL;

    switch (*parser->cursor) {
    case '{':
        return parse_container(parser, depth, true);
    case '[':
        return parse_container(parser, depth, false);
    case '"': {
        char *value = parse_string(parser);
        if (value == NULL)
            return NULL;
        node = new_node(BAMBU_JSON_STRING);
        if (node == NULL) {
            free(value);
            return NULL;
        }
        node->string = value;
        return node;
    }
    case 't':
        if (!consume_literal(parser, "true"))
            return NULL;
        node = new_node(BAMBU_JSON_BOOL);
        if (node != NULL)
            node->boolean = true;
        return node;
    case 'f':
        if (!consume_literal(parser, "false"))
            return NULL;
        return new_node(BAMBU_JSON_BOOL);
    case 'n':
        if (!consume_literal(parser, "null"))
            return NULL;
        return new_node(BAMBU_JSON_NULL);
    default: {
        double value;
        const char *stop;
        if (!parse_number_text(parser->cursor, parser->end, &value, &stop))
            return NULL;
        parser->cursor = stop;
        node = new_node(BAMBU_JSON_NUMBER);
        if (node != NULL)
            node->number = value;
        return node;
    }
    }
}

struct bambu_json *bambu_json_parse(const char *text, size_t length)
{
    struct parser parser;
    struct bambu_json *root;

    if (text == NULL || length == 0 || length > BAMBU_JSON_MAX_BYTES)
        return NULL;
    parser.cursor = text;
    parser.end = text + length;
    root = parse_value(&parser, 0);
    if (root == NULL)
        return NULL;
    skip_whitespace(&parser);
    /* Some firmware NUL-pads payloads; tolerate that one case only. */
    while (parser.cursor < parser.end && *parser.cursor == '\0')
        parser.cursor++;
    if (parser.cursor != parser.end) {
        bambu_json_free(root);
        return NULL;
    }
    return root;
}

void bambu_json_free(struct bambu_json *root)
{
    while (root != NULL) {
        struct bambu_json *next = root->next;
        bambu_json_free(root->child);
        free(root->string);
        free(root->key);
        free(root);
        root = next;
    }
}

const struct bambu_json *bambu_json_get(const struct bambu_json *object, const char *key)
{
    const struct bambu_json *child;
    if (object == NULL || object->type != BAMBU_JSON_OBJECT || key == NULL)
        return NULL;
    for (child = object->child; child != NULL; child = child->next) {
        if (child->key != NULL && strcmp(child->key, key) == 0)
            return child;
    }
    return NULL;
}

bool bambu_json_as_double(const struct bambu_json *node, double *out)
{
    if (node == NULL || out == NULL)
        return false;
    if (node->type == BAMBU_JSON_NUMBER) {
        if (!isfinite(node->number))
            return false;
        *out = node->number;
        return true;
    }
    if (node->type == BAMBU_JSON_STRING && node->string != NULL) {
        const char *end = node->string + strlen(node->string);
        const char *stop;
        double value;
        if (!parse_number_text(node->string, end, &value, &stop) || stop != end || !isfinite(value))
            return false;
        *out = value;
        return true;
    }
    return false;
}

bool bambu_json_as_int(const struct bambu_json *node, int *out)
{
    double value;
    if (out == NULL || !bambu_json_as_double(node, &value))
        return false;
    if (value < -2147483647.0 || value > 2147483647.0)
        return false;
    *out = (int)value;
    return true;
}

const char *bambu_json_as_string(const struct bambu_json *node)
{
    if (node == NULL || node->type != BAMBU_JSON_STRING)
        return NULL;
    return node->string;
}

bool bambu_json_as_text(const struct bambu_json *node, char *out, size_t capacity)
{
    if (node == NULL || out == NULL || capacity == 0)
        return false;
    if (node->type == BAMBU_JSON_STRING) {
        snprintf(out, capacity, "%s", node->string != NULL ? node->string : "");
        return true;
    }
    if (node->type == BAMBU_JSON_NUMBER) {
        if (node->number == floor(node->number) && fabs(node->number) < 9.0e15)
            snprintf(out, capacity, "%.0f", node->number);
        else
            snprintf(out, capacity, "%g", node->number);
        return true;
    }
    return false;
}

static void put_char(char *out, size_t capacity, size_t *needed, char c)
{
    if (out != NULL && *needed + 1 < capacity)
        out[*needed] = c;
    (*needed)++;
}

size_t bambu_json_escape(const char *value, char *out, size_t capacity)
{
    static const char digits[] = "0123456789abcdef";
    const unsigned char *cursor = (const unsigned char *)(value != NULL ? value : "");
    size_t needed = 0;

    put_char(out, capacity, &needed, '"');
    for (; *cursor != '\0'; cursor++) {
        unsigned char c = *cursor;
        if (c == '"' || c == '\\') {
            put_char(out, capacity, &needed, '\\');
            put_char(out, capacity, &needed, (char)c);
        } else if (c < 0x20) {
            put_char(out, capacity, &needed, '\\');
            put_char(out, capacity, &needed, 'u');
            put_char(out, capacity, &needed, '0');
            put_char(out, capacity, &needed, '0');
            put_char(out, capacity, &needed, digits[c >> 4]);
            put_char(out, capacity, &needed, digits[c & 15]);
        } else {
            put_char(out, capacity, &needed, (char)c);
        }
    }
    put_char(out, capacity, &needed, '"');

    if (out != NULL && capacity > 0)
        out[needed < capacity ? needed : capacity - 1] = '\0';
    return needed;
}
