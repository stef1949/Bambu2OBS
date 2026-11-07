#include "bambu-zip.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EOCD_SIGNATURE 0x06054b50u
#define CENTRAL_SIGNATURE 0x02014b50u
#define LOCAL_SIGNATURE 0x04034b50u
#define EOCD_SIZE 22u
#define CENTRAL_SIZE 46u
#define LOCAL_SIZE 30u
#define MAX_COMMENT 0xFFFFu

#define MAX_BITS 15
#define MAX_LITERAL_CODES 288
#define MAX_DISTANCE_CODES 30

struct huffman {
    uint16_t count[MAX_BITS + 1];
    uint16_t symbol[MAX_LITERAL_CODES];
};

struct inflate_state {
    const uint8_t *input;
    size_t input_length;
    size_t input_position;
    uint32_t bit_buffer;
    int bit_count;
    uint8_t *output;
    size_t capacity;
    size_t written;
    bool failed;
};

static uint32_t read_bits(struct inflate_state *state, int need)
{
    uint32_t value = state->bit_buffer;

    while (state->bit_count < need) {
        if (state->input_position >= state->input_length) {
            state->failed = true;
            return 0;
        }
        value |= (uint32_t)state->input[state->input_position++] << state->bit_count;
        state->bit_count += 8;
    }
    state->bit_buffer = need < 32 ? value >> need : 0;
    state->bit_count -= need;
    return value & ((1u << need) - 1u);
}

static int decode_symbol(struct inflate_state *state, const struct huffman *table)
{
    int code = 0;
    int first = 0;
    int index = 0;

    for (int length = 1; length <= MAX_BITS; length++) {
        int count;
        code |= (int)read_bits(state, 1);
        if (state->failed)
            return -1;
        count = table->count[length];
        if (code - count < first)
            return table->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    state->failed = true;
    return -1;
}

static bool build_table(struct huffman *table, const uint8_t *lengths, int count)
{
    uint16_t offsets[MAX_BITS + 1];
    int left = 1;

    memset(table->count, 0, sizeof(table->count));
    for (int i = 0; i < count; i++)
        table->count[lengths[i]]++;
    if (table->count[0] == count)
        return true;

    for (int length = 1; length <= MAX_BITS; length++) {
        left <<= 1;
        left -= table->count[length];
        if (left < 0)
            return false;
    }

    offsets[1] = 0;
    for (int length = 1; length < MAX_BITS; length++)
        offsets[length + 1] = (uint16_t)(offsets[length] + table->count[length]);
    for (int i = 0; i < count; i++) {
        if (lengths[i] != 0)
            table->symbol[offsets[lengths[i]]++] = (uint16_t)i;
    }
    return true;
}

static bool inflate_codes(struct inflate_state *state, const struct huffman *literals, const struct huffman *distances)
{
    static const uint16_t length_base[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                             31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const uint8_t length_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                             2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const uint16_t distance_base[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,
                                               33,  49,  65,  97,  129, 193,  257,  385,  513,  769,
                                               1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static const uint8_t distance_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                               6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

    for (;;) {
        int symbol = decode_symbol(state, literals);
        if (state->failed)
            return false;
        if (symbol < 256) {
            if (state->written >= state->capacity)
                return false;
            state->output[state->written++] = (uint8_t)symbol;
        } else if (symbol == 256) {
            return true;
        } else {
            size_t length;
            size_t distance;
            int distance_symbol;

            symbol -= 257;
            if (symbol >= 29)
                return false;
            length = length_base[symbol] + read_bits(state, length_extra[symbol]);
            distance_symbol = decode_symbol(state, distances);
            if (state->failed || distance_symbol < 0 || distance_symbol >= MAX_DISTANCE_CODES)
                return false;
            distance = distance_base[distance_symbol] + read_bits(state, distance_extra[distance_symbol]);
            if (state->failed || distance > state->written || length > state->capacity - state->written)
                return false;
            /* Byte-wise on purpose: DEFLATE copies may overlap their own output. */
            for (size_t i = 0; i < length; i++) {
                state->output[state->written] = state->output[state->written - distance];
                state->written++;
            }
        }
    }
}

static bool inflate_stored(struct inflate_state *state)
{
    size_t length;
    size_t complement;

    state->bit_buffer = 0;
    state->bit_count = 0;
    if (state->input_length - state->input_position < 4)
        return false;
    length = state->input[state->input_position] | ((size_t)state->input[state->input_position + 1] << 8);
    complement = state->input[state->input_position + 2] | ((size_t)state->input[state->input_position + 3] << 8);
    state->input_position += 4;
    if (length != (~complement & 0xFFFFu))
        return false;
    if (state->input_length - state->input_position < length || state->capacity - state->written < length)
        return false;
    if (length > 0)
        memcpy(state->output + state->written, state->input + state->input_position, length);
    state->input_position += length;
    state->written += length;
    return true;
}

static bool inflate_fixed(struct inflate_state *state)
{
    struct huffman literals;
    struct huffman distances;
    uint8_t lengths[MAX_LITERAL_CODES];
    int symbol;

    for (symbol = 0; symbol < 144; symbol++)
        lengths[symbol] = 8;
    for (; symbol < 256; symbol++)
        lengths[symbol] = 9;
    for (; symbol < 280; symbol++)
        lengths[symbol] = 7;
    for (; symbol < MAX_LITERAL_CODES; symbol++)
        lengths[symbol] = 8;
    if (!build_table(&literals, lengths, MAX_LITERAL_CODES))
        return false;
    for (symbol = 0; symbol < MAX_DISTANCE_CODES; symbol++)
        lengths[symbol] = 5;
    if (!build_table(&distances, lengths, MAX_DISTANCE_CODES))
        return false;
    return inflate_codes(state, &literals, &distances);
}

static bool inflate_dynamic(struct inflate_state *state)
{
    static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    struct huffman literals;
    struct huffman distances;
    uint8_t lengths[MAX_LITERAL_CODES + MAX_DISTANCE_CODES + 2];
    int literal_count = (int)read_bits(state, 5) + 257;
    int distance_count = (int)read_bits(state, 5) + 1;
    int code_count = (int)read_bits(state, 4) + 4;
    int index;

    if (state->failed || literal_count > 286 || distance_count > MAX_DISTANCE_CODES)
        return false;

    memset(lengths, 0, 19);
    for (index = 0; index < code_count; index++)
        lengths[order[index]] = (uint8_t)read_bits(state, 3);
    if (state->failed || !build_table(&literals, lengths, 19))
        return false;

    index = 0;
    while (index < literal_count + distance_count) {
        int symbol = decode_symbol(state, &literals);
        if (state->failed)
            return false;
        if (symbol < 16) {
            lengths[index++] = (uint8_t)symbol;
        } else {
            uint8_t previous = 0;
            int repeat;
            if (symbol == 16) {
                if (index == 0)
                    return false;
                previous = lengths[index - 1];
                repeat = 3 + (int)read_bits(state, 2);
            } else if (symbol == 17) {
                repeat = 3 + (int)read_bits(state, 3);
            } else {
                repeat = 11 + (int)read_bits(state, 7);
            }
            if (state->failed || index + repeat > literal_count + distance_count)
                return false;
            while (repeat-- > 0)
                lengths[index++] = previous;
        }
    }
    if (lengths[256] == 0)
        return false;

    if (!build_table(&literals, lengths, literal_count))
        return false;
    if (!build_table(&distances, lengths + literal_count, distance_count))
        return false;
    return inflate_codes(state, &literals, &distances);
}

bool bambu_inflate(const uint8_t *input, size_t input_length, uint8_t *output, size_t capacity, size_t *written)
{
    struct inflate_state state;
    uint32_t last;

    if (input == NULL || output == NULL || written == NULL)
        return false;
    memset(&state, 0, sizeof(state));
    state.input = input;
    state.input_length = input_length;
    state.output = output;
    state.capacity = capacity;

    do {
        uint32_t type;
        bool ok;

        last = read_bits(&state, 1);
        type = read_bits(&state, 2);
        if (state.failed)
            return false;
        switch (type) {
        case 0:
            ok = inflate_stored(&state);
            break;
        case 1:
            ok = inflate_fixed(&state);
            break;
        case 2:
            ok = inflate_dynamic(&state);
            break;
        default:
            return false;
        }
        if (!ok)
            return false;
    } while (last == 0);

    *written = state.written;
    return true;
}

uint32_t bambu_crc32(const uint8_t *data, size_t length)
{
    uint32_t crc = 0xFFFFFFFFu;

    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

bool bambu_image_signature_ok(const uint8_t *data, size_t length)
{
    static const uint8_t png[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    static const uint8_t jpeg[3] = {0xFF, 0xD8, 0xFF};

    if (data == NULL)
        return false;
    if (length >= sizeof(png) && memcmp(data, png, sizeof(png)) == 0)
        return true;
    if (length >= sizeof(jpeg) && memcmp(data, jpeg, sizeof(jpeg)) == 0)
        return true;
    return length >= 12 && memcmp(data, "RIFF", 4) == 0 && memcmp(data + 8, "WEBP", 4) == 0;
}

static uint32_t read16(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8);
}

static uint32_t read32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

struct zip_member {
    const uint8_t *name;
    size_t name_length;
    uint32_t method;
    uint32_t crc;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint32_t local_offset;
};

static bool find_central_directory(
    const uint8_t *archive,
    size_t length,
    size_t *directory_offset,
    size_t *directory_size,
    uint32_t *entry_count
)
{
    size_t search_floor;

    if (length < EOCD_SIZE)
        return false;
    search_floor = length > EOCD_SIZE + MAX_COMMENT ? length - EOCD_SIZE - MAX_COMMENT : 0;
    for (size_t position = length - EOCD_SIZE + 1; position-- > search_floor;) {
        if (read32(archive + position) != EOCD_SIGNATURE)
            continue;
        *entry_count = read16(archive + position + 10);
        *directory_size = read32(archive + position + 12);
        *directory_offset = read32(archive + position + 16);
        return *directory_offset <= length && *directory_size <= length - *directory_offset;
    }
    return false;
}

static bool next_member(
    const uint8_t *archive,
    size_t directory_end,
    size_t *cursor,
    struct zip_member *member
)
{
    const uint8_t *entry;
    size_t name_length;
    size_t extra_length;
    size_t comment_length;

    if (directory_end - *cursor < CENTRAL_SIZE)
        return false;
    entry = archive + *cursor;
    if (read32(entry) != CENTRAL_SIGNATURE)
        return false;
    name_length = read16(entry + 28);
    extra_length = read16(entry + 30);
    comment_length = read16(entry + 32);
    if (directory_end - *cursor - CENTRAL_SIZE < name_length + extra_length + comment_length)
        return false;

    member->method = read16(entry + 10);
    member->crc = read32(entry + 16);
    member->compressed_size = read32(entry + 20);
    member->uncompressed_size = read32(entry + 24);
    member->local_offset = read32(entry + 42);
    member->name = entry + CENTRAL_SIZE;
    member->name_length = name_length;
    *cursor += CENTRAL_SIZE + name_length + extra_length + comment_length;
    return true;
}

static bool name_equals(const struct zip_member *member, const char *name)
{
    size_t length = strlen(name);
    return member->name_length == length && memcmp(member->name, name, length) == 0;
}

static bool has_prefix_nocase(const uint8_t *text, size_t length, const char *prefix)
{
    size_t prefix_length = strlen(prefix);
    if (length < prefix_length)
        return false;
    for (size_t i = 0; i < prefix_length; i++) {
        if (tolower(text[i]) != tolower((unsigned char)prefix[i]))
            return false;
    }
    return true;
}

static bool has_suffix_nocase(const uint8_t *text, size_t length, const char *suffix)
{
    size_t suffix_length = strlen(suffix);
    return length >= suffix_length && has_prefix_nocase(text + length - suffix_length, suffix_length, suffix);
}

static bool contains_nocase(const uint8_t *text, size_t length, const char *needle)
{
    size_t needle_length = strlen(needle);
    for (size_t i = 0; i + needle_length <= length; i++) {
        if (has_prefix_nocase(text + i, length - i, needle))
            return true;
    }
    return false;
}

static bool is_metadata_image(const struct zip_member *member)
{
    return has_prefix_nocase(member->name, member->name_length, "metadata/") &&
           (has_suffix_nocase(member->name, member->name_length, ".png") ||
            has_suffix_nocase(member->name, member->name_length, ".jpg") ||
            has_suffix_nocase(member->name, member->name_length, ".jpeg"));
}

static enum bambu_zip_result read_member(
    const uint8_t *archive,
    size_t archive_length,
    const struct zip_member *member,
    uint8_t **image,
    size_t *image_length
)
{
    const uint8_t *local;
    const uint8_t *data;
    size_t data_offset;
    size_t written = 0;
    uint8_t *output;

    if (member->uncompressed_size > BAMBU_PREVIEW_MAX_IMAGE_BYTES)
        return BAMBU_ZIP_TOO_LARGE;
    if (member->uncompressed_size == 0)
        return BAMBU_ZIP_BAD_IMAGE;
    if (member->local_offset > archive_length || archive_length - member->local_offset < LOCAL_SIZE)
        return BAMBU_ZIP_MALFORMED;
    local = archive + member->local_offset;
    if (read32(local) != LOCAL_SIGNATURE)
        return BAMBU_ZIP_MALFORMED;

    data_offset = (size_t)member->local_offset + LOCAL_SIZE + read16(local + 26) + read16(local + 28);
    if (data_offset > archive_length || archive_length - data_offset < member->compressed_size)
        return BAMBU_ZIP_MALFORMED;
    data = archive + data_offset;

    output = malloc(member->uncompressed_size);
    if (output == NULL)
        return BAMBU_ZIP_OUT_OF_MEMORY;

    if (member->method == 0) {
        if (member->compressed_size != member->uncompressed_size) {
            free(output);
            return BAMBU_ZIP_MALFORMED;
        }
        memcpy(output, data, member->uncompressed_size);
        written = member->uncompressed_size;
    } else if (member->method != 8 ||
               !bambu_inflate(data, member->compressed_size, output, member->uncompressed_size, &written)) {
        free(output);
        return BAMBU_ZIP_MALFORMED;
    }

    if (written != member->uncompressed_size || bambu_crc32(output, written) != member->crc) {
        free(output);
        return BAMBU_ZIP_MALFORMED;
    }
    if (!bambu_image_signature_ok(output, written)) {
        free(output);
        return BAMBU_ZIP_BAD_IMAGE;
    }
    *image = output;
    *image_length = written;
    return BAMBU_ZIP_OK;
}

enum bambu_zip_result bambu_3mf_extract_preview(
    const uint8_t *archive,
    size_t archive_length,
    uint8_t **image,
    size_t *image_length,
    char *member_name,
    size_t member_name_capacity
)
{
    static const char *const priority[] = {
        "Metadata/plate_1.png",        "Metadata/plate_1_small.png", "Metadata/plate_no_light_1.png",
        "Metadata/top_1.png",          "Metadata/pick_1.png",
    };
    struct zip_member best;
    struct zip_member member;
    size_t directory_offset;
    size_t directory_size;
    size_t cursor;
    size_t directory_end;
    uint32_t entry_count;
    int best_rank = 1 << 30;
    size_t best_fallback_score = (size_t)-1;
    bool found = false;
    enum bambu_zip_result result;

    if (archive == NULL || image == NULL || image_length == NULL)
        return BAMBU_ZIP_MALFORMED;
    if (archive_length > BAMBU_ZIP_MAX_ARCHIVE_BYTES)
        return BAMBU_ZIP_TOO_LARGE;
    if (!find_central_directory(archive, archive_length, &directory_offset, &directory_size, &entry_count))
        return BAMBU_ZIP_MALFORMED;
    if (entry_count > BAMBU_ZIP_MAX_MEMBERS)
        return BAMBU_ZIP_TOO_MANY_MEMBERS;

    cursor = directory_offset;
    directory_end = directory_offset + directory_size;
    memset(&best, 0, sizeof(best));

    for (uint32_t i = 0; i < entry_count; i++) {
        int rank = -1;

        if (!next_member(archive, directory_end, &cursor, &member))
            return BAMBU_ZIP_MALFORMED;
        for (size_t p = 0; p < sizeof(priority) / sizeof(priority[0]); p++) {
            if (name_equals(&member, priority[p])) {
                rank = (int)p;
                break;
            }
        }
        if (rank >= 0) {
            if (rank < best_rank) {
                best_rank = rank;
                best = member;
                found = true;
            }
        } else if (best_rank >= 1000 && is_metadata_image(&member)) {
            /* Fallback order: plate_1-like names first, then shortest name. */
            size_t score = member.name_length + (contains_nocase(member.name, member.name_length, "plate_1") ? 0 : 4096);
            if (score < best_fallback_score) {
                best_fallback_score = score;
                best_rank = 1000;
                best = member;
                found = true;
            }
        }
    }

    if (!found)
        return BAMBU_ZIP_NO_PREVIEW;
    result = read_member(archive, archive_length, &best, image, image_length);
    if (result == BAMBU_ZIP_OK && member_name != NULL && member_name_capacity > 0)
        snprintf(member_name, member_name_capacity, "%.*s", (int)best.name_length, (const char *)best.name);
    return result;
}
