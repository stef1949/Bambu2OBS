#include "overlay-canvas.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static float srgb_to_linear(float value)
{
    return value <= 0.04045f ? value / 12.92f : powf((value + 0.055f) / 1.055f, 2.4f);
}

static float linear_to_srgb(float value)
{
    return value <= 0.0031308f ? value * 12.92f : 1.055f * powf(value, 1.0f / 2.4f) - 0.055f;
}

bool bambu_canvas_init(struct bambu_canvas *canvas, int width, int height)
{
    size_t pixels;

    if (canvas == NULL || width <= 0 || height <= 0)
        return false;
    memset(canvas, 0, sizeof(*canvas));
    pixels = (size_t)width * (size_t)height;
    canvas->pixels = calloc(pixels, 4);
    canvas->coverage = malloc(pixels);
    if (canvas->pixels == NULL || canvas->coverage == NULL) {
        bambu_canvas_free(canvas);
        return false;
    }
    canvas->width = width;
    canvas->height = height;
    for (int i = 0; i < 256; i++)
        canvas->to_linear[i] = srgb_to_linear((float)i / 255.0f);
    for (int i = 0; i < 4096; i++)
        canvas->to_srgb[i] = (uint8_t)(linear_to_srgb((float)i / 4095.0f) * 255.0f + 0.5f);
    return true;
}

void bambu_canvas_free(struct bambu_canvas *canvas)
{
    if (canvas == NULL)
        return;
    free(canvas->pixels);
    free(canvas->coverage);
    canvas->pixels = NULL;
    canvas->coverage = NULL;
    canvas->width = 0;
    canvas->height = 0;
}

void bambu_canvas_clear(struct bambu_canvas *canvas)
{
    memset(canvas->pixels, 0, (size_t)canvas->width * (size_t)canvas->height * 4);
}

static inline void blend(uint8_t *pixel, struct bambu_color color, unsigned coverage)
{
    unsigned alpha = (color.a * coverage + 127) / 255;
    unsigned inverse = 255 - alpha;

    if (alpha == 0)
        return;
    pixel[0] = (uint8_t)((color.b * alpha + pixel[0] * inverse + 127) / 255);
    pixel[1] = (uint8_t)((color.g * alpha + pixel[1] * inverse + 127) / 255);
    pixel[2] = (uint8_t)((color.r * alpha + pixel[2] * inverse + 127) / 255);
    pixel[3] = (uint8_t)(alpha + (pixel[3] * inverse + 127) / 255);
}

/* Signed distance from a point to a rounded rectangle centred at the origin. */
static inline float rounded_distance(float px, float py, float half_w, float half_h, float radius)
{
    float qx = fabsf(px) - half_w + radius;
    float qy = fabsf(py) - half_h + radius;
    float outside_x = qx > 0.0f ? qx : 0.0f;
    float outside_y = qy > 0.0f ? qy : 0.0f;
    float inside = qx > qy ? qx : qy;

    if (inside > 0.0f)
        inside = 0.0f;
    return sqrtf(outside_x * outside_x + outside_y * outside_y) + inside - radius;
}

static inline float clamp_unit(float value)
{
    if (value < 0.0f)
        return 0.0f;
    if (value > 1.0f)
        return 1.0f;
    return value;
}

static float limit_radius(struct bambu_rect rect, float radius)
{
    float limit = (rect.w < rect.h ? rect.w : rect.h) / 2.0f;
    if (radius < 0.0f)
        return 0.0f;
    return radius > limit ? limit : radius;
}

struct pixel_bounds {
    int x0;
    int y0;
    int x1;
    int y1;
};

static struct pixel_bounds clip_bounds(const struct bambu_canvas *canvas, struct bambu_rect rect)
{
    struct pixel_bounds bounds;

    bounds.x0 = (int)floorf(rect.x) - 1;
    bounds.y0 = (int)floorf(rect.y) - 1;
    bounds.x1 = (int)ceilf(rect.x + rect.w) + 1;
    bounds.y1 = (int)ceilf(rect.y + rect.h) + 1;
    if (bounds.x0 < 0)
        bounds.x0 = 0;
    if (bounds.y0 < 0)
        bounds.y0 = 0;
    if (bounds.x1 > canvas->width)
        bounds.x1 = canvas->width;
    if (bounds.y1 > canvas->height)
        bounds.y1 = canvas->height;
    return bounds;
}

void bambu_canvas_fill_rounded(
    struct bambu_canvas *canvas,
    struct bambu_rect rect,
    float radius,
    struct bambu_color color
)
{
    struct pixel_bounds bounds;
    float half_w = rect.w / 2.0f;
    float half_h = rect.h / 2.0f;
    float center_x = rect.x + half_w;
    float center_y = rect.y + half_h;

    if (rect.w <= 0.0f || rect.h <= 0.0f || color.a == 0)
        return;
    radius = limit_radius(rect, radius);
    bounds = clip_bounds(canvas, rect);

    for (int y = bounds.y0; y < bounds.y1; y++) {
        uint8_t *row = canvas->pixels + ((size_t)y * (size_t)canvas->width + (size_t)bounds.x0) * 4;
        float py = (float)y + 0.5f - center_y;
        for (int x = bounds.x0; x < bounds.x1; x++, row += 4) {
            float distance = rounded_distance((float)x + 0.5f - center_x, py, half_w, half_h, radius);
            float coverage = clamp_unit(0.5f - distance);
            if (coverage > 0.0f)
                blend(row, color, (unsigned)(coverage * 255.0f + 0.5f));
        }
    }
}

void bambu_canvas_stroke_rounded(
    struct bambu_canvas *canvas,
    struct bambu_rect rect,
    float radius,
    float thickness,
    struct bambu_color color
)
{
    struct pixel_bounds bounds;
    float half_w = rect.w / 2.0f;
    float half_h = rect.h / 2.0f;
    float center_x = rect.x + half_w;
    float center_y = rect.y + half_h;

    if (rect.w <= 0.0f || rect.h <= 0.0f || color.a == 0 || thickness <= 0.0f)
        return;
    radius = limit_radius(rect, radius);
    bounds = clip_bounds(canvas, rect);

    for (int y = bounds.y0; y < bounds.y1; y++) {
        uint8_t *row = canvas->pixels + ((size_t)y * (size_t)canvas->width + (size_t)bounds.x0) * 4;
        float py = (float)y + 0.5f - center_y;
        for (int x = bounds.x0; x < bounds.x1; x++, row += 4) {
            float distance = rounded_distance((float)x + 0.5f - center_x, py, half_w, half_h, radius);
            float outer = clamp_unit(0.5f - distance);
            float inner = clamp_unit(0.5f - (distance + thickness));
            float coverage = outer - inner;
            if (coverage > 0.0f)
                blend(row, color, (unsigned)(coverage * 255.0f + 0.5f));
        }
    }
}

static inline float edge_distance(float px, float py, float ax, float ay, float bx, float by)
{
    float dx = bx - ax;
    float dy = by - ay;
    float length = sqrtf(dx * dx + dy * dy);
    if (length <= 0.0f)
        return 0.0f;
    return ((px - ax) * dy - (py - ay) * dx) / length;
}

void bambu_canvas_fill_triangle(
    struct bambu_canvas *canvas,
    float x0,
    float y0,
    float x1,
    float y1,
    float x2,
    float y2,
    struct bambu_color color
)
{
    struct bambu_rect box;
    struct pixel_bounds bounds;
    float min_x = fminf(x0, fminf(x1, x2));
    float min_y = fminf(y0, fminf(y1, y2));
    float max_x = fmaxf(x0, fmaxf(x1, x2));
    float max_y = fmaxf(y0, fmaxf(y1, y2));
    float winding = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);

    /* Normalise winding so a positive edge distance is always outside. */
    if (winding < 0.0f) {
        float swap_x = x1;
        float swap_y = y1;
        x1 = x2;
        y1 = y2;
        x2 = swap_x;
        y2 = swap_y;
    }

    box.x = min_x;
    box.y = min_y;
    box.w = max_x - min_x;
    box.h = max_y - min_y;
    bounds = clip_bounds(canvas, box);

    for (int y = bounds.y0; y < bounds.y1; y++) {
        float py = (float)y + 0.5f;
        for (int x = bounds.x0; x < bounds.x1; x++) {
            float px = (float)x + 0.5f;
            float d0 = edge_distance(px, py, x0, y0, x1, y1);
            float d1 = edge_distance(px, py, x1, y1, x2, y2);
            float d2 = edge_distance(px, py, x2, y2, x0, y0);
            float distance = fmaxf(d0, fmaxf(d1, d2));
            float coverage = clamp_unit(0.5f - distance);
            if (coverage > 0.0f) {
                uint8_t *pixel = canvas->pixels + ((size_t)y * (size_t)canvas->width + (size_t)x) * 4;
                blend(pixel, color, (unsigned)(coverage * 255.0f + 0.5f));
            }
        }
    }
}

void bambu_canvas_blit_mask(
    struct bambu_canvas *canvas,
    const uint8_t *mask,
    int mask_width,
    int mask_height,
    int x,
    int y,
    struct bambu_color color
)
{
    for (int row = 0; row < mask_height; row++) {
        int target_y = y + row;
        if (target_y < 0 || target_y >= canvas->height)
            continue;
        for (int column = 0; column < mask_width; column++) {
            int target_x = x + column;
            uint8_t coverage = mask[(size_t)row * (size_t)mask_width + (size_t)column];
            if (coverage == 0 || target_x < 0 || target_x >= canvas->width)
                continue;
            blend(canvas->pixels + ((size_t)target_y * (size_t)canvas->width + (size_t)target_x) * 4, color, coverage);
        }
    }
}

static inline void sample_bilinear(const struct bambu_image *image, float u, float v, float out[4])
{
    int x0 = (int)floorf(u);
    int y0 = (int)floorf(v);
    float fx = u - (float)x0;
    float fy = v - (float)y0;
    int x1 = x0 + 1;
    int y1 = y0 + 1;

    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 >= image->width)
        x1 = image->width - 1;
    if (y1 >= image->height)
        y1 = image->height - 1;
    if (x0 >= image->width)
        x0 = image->width - 1;
    if (y0 >= image->height)
        y0 = image->height - 1;

    for (int channel = 0; channel < 4; channel++) {
        const uint8_t *row0 = image->pixels + (size_t)y0 * (size_t)image->width * 4;
        const uint8_t *row1 = image->pixels + (size_t)y1 * (size_t)image->width * 4;
        float top = row0[x0 * 4 + channel] * (1.0f - fx) + row0[x1 * 4 + channel] * fx;
        float bottom = row1[x0 * 4 + channel] * (1.0f - fx) + row1[x1 * 4 + channel] * fx;
        out[channel] = top * (1.0f - fy) + bottom * fy;
    }
}

void bambu_canvas_draw_image(
    struct bambu_canvas *canvas,
    const struct bambu_image *image,
    struct bambu_rect rect,
    float radius
)
{
    struct pixel_bounds bounds;
    float half_w = rect.w / 2.0f;
    float half_h = rect.h / 2.0f;
    float center_x = rect.x + half_w;
    float center_y = rect.y + half_h;
    float scale;
    float offset_u;
    float offset_v;

    if (image == NULL || image->pixels == NULL || image->width <= 0 || image->height <= 0)
        return;
    if (rect.w <= 0.0f || rect.h <= 0.0f)
        return;

    /* Cover: scale so the shorter side fills the rectangle, then centre. */
    scale = fmaxf(rect.w / (float)image->width, rect.h / (float)image->height);
    offset_u = ((float)image->width - rect.w / scale) / 2.0f;
    offset_v = ((float)image->height - rect.h / scale) / 2.0f;
    radius = limit_radius(rect, radius);
    bounds = clip_bounds(canvas, rect);

    for (int y = bounds.y0; y < bounds.y1; y++) {
        float py = (float)y + 0.5f;
        for (int x = bounds.x0; x < bounds.x1; x++) {
            float px = (float)x + 0.5f;
            float distance = rounded_distance(px - center_x, py - center_y, half_w, half_h, radius);
            float coverage = clamp_unit(0.5f - distance);
            float sample[4];
            struct bambu_color color;

            if (coverage <= 0.0f)
                continue;
            sample_bilinear(
                image,
                offset_u + (px - rect.x) / scale - 0.5f,
                offset_v + (py - rect.y) / scale - 0.5f,
                sample
            );
            color.b = (uint8_t)sample[0];
            color.g = (uint8_t)sample[1];
            color.r = (uint8_t)sample[2];
            color.a = (uint8_t)sample[3];
            blend(
                canvas->pixels + ((size_t)y * (size_t)canvas->width + (size_t)x) * 4,
                color,
                (unsigned)(coverage * 255.0f + 0.5f)
            );
        }
    }
}

void bambu_canvas_encode(const struct bambu_canvas *canvas, uint8_t *out)
{
    size_t count = (size_t)canvas->width * (size_t)canvas->height;

    for (size_t i = 0; i < count; i++) {
        const uint8_t *source = canvas->pixels + i * 4;
        uint8_t *target = out + i * 4;
        unsigned alpha = source[3];

        if (alpha == 0) {
            memset(target, 0, 4);
        } else if (alpha == 255) {
            memcpy(target, source, 4);
        } else {
            float coverage = (float)alpha / 255.0f;
            for (int channel = 0; channel < 3; channel++) {
                unsigned straight = (source[channel] * 255u + alpha / 2) / alpha;
                float linear;
                if (straight > 255)
                    straight = 255;
                linear = canvas->to_linear[straight] * coverage;
                target[channel] = canvas->to_srgb[(int)(linear * 4095.0f + 0.5f)];
            }
            target[3] = (uint8_t)alpha;
        }
    }
}
