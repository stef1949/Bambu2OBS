#pragma once

/* Bounded, in-memory reader for the preview images inside .3mf archives. Members
 * are matched by exact name and never touch the filesystem, so traversal names
 * in a hostile archive have no effect. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BAMBU_ZIP_MAX_ARCHIVE_BYTES (256u * 1024u * 1024u)
#define BAMBU_ZIP_MAX_MEMBERS 4096u
#define BAMBU_PREVIEW_MAX_IMAGE_BYTES (10u * 1024u * 1024u)

enum bambu_zip_result {
    BAMBU_ZIP_OK = 0,
    BAMBU_ZIP_NO_PREVIEW,
    BAMBU_ZIP_TOO_LARGE,
    BAMBU_ZIP_TOO_MANY_MEMBERS,
    BAMBU_ZIP_MALFORMED,
    BAMBU_ZIP_BAD_IMAGE,
    BAMBU_ZIP_OUT_OF_MEMORY,
};

/* Raw DEFLATE. Returns false on corrupt input or if output would exceed capacity. */
bool bambu_inflate(const uint8_t *input, size_t input_length, uint8_t *output, size_t capacity, size_t *written);

uint32_t bambu_crc32(const uint8_t *data, size_t length);

/* True for PNG, JPEG, or WebP signatures. */
bool bambu_image_signature_ok(const uint8_t *data, size_t length);

/* Finds and extracts the preferred preview from a .3mf archive. On success
 * *image is malloc'd and owned by the caller. member_name may be NULL. */
enum bambu_zip_result bambu_3mf_extract_preview(
    const uint8_t *archive,
    size_t archive_length,
    uint8_t **image,
    size_t *image_length,
    char *member_name,
    size_t member_name_capacity
);
