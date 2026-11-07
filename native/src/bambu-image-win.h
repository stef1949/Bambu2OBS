#pragma once

/* Reference-counted preview bitmaps, decoded by Windows Imaging Component. Sources
 * hold references while rendering, so a job change never frees pixels in use. */

#include "overlay-canvas.h"

#include <stddef.h>
#include <stdint.h>

#define BAMBU_IMAGE_MAX_SOURCE_DIMENSION 8192
#define BAMBU_IMAGE_MAX_DIMENSION 512

struct bambu_shared_image;

/* Returns NULL for corrupt, unsupported, or oversized input. Output is scaled
 * down to at most BAMBU_IMAGE_MAX_DIMENSION on the longer side. */
struct bambu_shared_image *bambu_image_decode(const uint8_t *data, size_t length);

struct bambu_shared_image *bambu_image_retain(struct bambu_shared_image *image);
void bambu_image_release(struct bambu_shared_image *image);
const struct bambu_image *bambu_image_pixels(const struct bambu_shared_image *image);
