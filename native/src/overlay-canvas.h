#pragma once

/* CPU compositing surface. Frames are composed here on a worker thread and
 * uploaded to OBS as one texture, and only when something changed. Pixels are
 * BGRA, premultiplied, in sRGB space until bambu_canvas_encode() runs. */

#include <stdbool.h>
#include <stdint.h>

struct bambu_color {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;
};

struct bambu_rect {
    float x;
    float y;
    float w;
    float h;
};

/* Reference-counted by the owner; pixels are straight-alpha BGRA. */
struct bambu_image {
    int width;
    int height;
    uint8_t *pixels;
};

struct bambu_canvas {
    int width;
    int height;
    uint8_t *pixels;
    uint8_t *coverage; /* scratch mask for text, width * height bytes */
    float to_linear[256];
    uint8_t to_srgb[4096];
};

bool bambu_canvas_init(struct bambu_canvas *canvas, int width, int height);
void bambu_canvas_free(struct bambu_canvas *canvas);
void bambu_canvas_clear(struct bambu_canvas *canvas);

void bambu_canvas_fill_rounded(
    struct bambu_canvas *canvas,
    struct bambu_rect rect,
    float radius,
    struct bambu_color color
);
void bambu_canvas_stroke_rounded(
    struct bambu_canvas *canvas,
    struct bambu_rect rect,
    float radius,
    float thickness,
    struct bambu_color color
);
void bambu_canvas_fill_triangle(
    struct bambu_canvas *canvas,
    float x0,
    float y0,
    float x1,
    float y1,
    float x2,
    float y2,
    struct bambu_color color
);

/* Blends color through an 8-bit coverage mask placed at x, y. */
void bambu_canvas_blit_mask(
    struct bambu_canvas *canvas,
    const uint8_t *mask,
    int mask_width,
    int mask_height,
    int x,
    int y,
    struct bambu_color color
);

/* Draws image scaled to cover rect, clipped to the rounded rectangle. */
void bambu_canvas_draw_image(
    struct bambu_canvas *canvas,
    const struct bambu_image *image,
    struct bambu_rect rect,
    float radius
);

/* Rewrites the frame into out as colour premultiplied in linear light and then
 * sRGB encoded, which is what OBS expects from an sRGB texture blended with
 * GS_BLEND_ONE / GS_BLEND_INVSRCALPHA. */
void bambu_canvas_encode(const struct bambu_canvas *canvas, uint8_t *out);
