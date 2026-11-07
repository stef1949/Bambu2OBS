/* Renders every preset and state to PNG for visual review and documentation.
 * Usage: bambu-overlay-gallery <output-directory>
 * Each overlay is composited over a split light/dark backdrop, so transparency
 * and legibility on both can be judged from one image. */

#include "bambu-simulation.h"
#include "overlay-render.h"
#include "text-gdi-win.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "png-writer.h"

#define NOW_MS 1000000u
#define STALE_AFTER_MS 30000u

/* A synthetic stand-in for a plate thumbnail. */
static struct bambu_image make_test_preview(void)
{
    struct bambu_image image;
    int size = 256;

    image.width = size;
    image.height = size;
    image.pixels = malloc((size_t)size * (size_t)size * 4);
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            uint8_t *p = image.pixels + ((size_t)y * (size_t)size + (size_t)x) * 4;
            int dx = x - size / 2;
            int dy = y - size / 2;
            int inside = dx * dx + dy * dy < 80 * 80;
            p[0] = (uint8_t)(inside ? 60 + y / 4 : 46);
            p[1] = (uint8_t)(inside ? 150 + x / 4 : 42);
            p[2] = (uint8_t)(inside ? 90 : 38);
            p[3] = 255;
        }
    }
    return image;
}

static void write_composite(const struct bambu_canvas *canvas, const uint8_t *encoded, const char *path)
{
    int margin = 24;
    int width = canvas->width + margin * 2;
    int height = canvas->height + margin * 2;
    uint8_t *out = malloc((size_t)width * (size_t)height * 4);

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            uint8_t *p = out + ((size_t)y * (size_t)width + (size_t)x) * 4;
            float backdrop = x < width / 2 ? 0.92f : 0.18f;
            float channel[3] = {backdrop, backdrop, backdrop};
            int cx = x - margin;
            int cy = y - margin;

            if (cx >= 0 && cy >= 0 && cx < canvas->width && cy < canvas->height) {
                const uint8_t *s = encoded + ((size_t)cy * (size_t)canvas->width + (size_t)cx) * 4;
                float alpha = (float)s[3] / 255.0f;
                /* Same maths the GPU performs: decode, then blend ONE/INVSRCALPHA. */
                for (int i = 0; i < 3; i++) {
                    float linear_bg = canvas->to_linear[(int)(channel[i] * 255.0f)];
                    float linear = canvas->to_linear[s[2 - i]] + linear_bg * (1.0f - alpha);
                    channel[i] = (float)canvas->to_srgb[(int)(linear * 4095.0f)] / 255.0f;
                }
            }
            p[0] = (uint8_t)(channel[0] * 255.0f);
            p[1] = (uint8_t)(channel[1] * 255.0f);
            p[2] = (uint8_t)(channel[2] * 255.0f);
            p[3] = 255;
        }
    }
    png_write_rgba(path, out, width, height);
    free(out);
}

static void render_one(
    const char *directory,
    const char *name,
    struct bambu_overlay_style style,
    enum bambu_simulation_fixture fixture,
    const struct bambu_image *preview,
    const struct bambu_text_backend *text
)
{
    static struct bambu_printer_snapshot snapshot;
    struct bambu_overlay_frame frame;
    struct bambu_canvas canvas;
    char path[1024];
    uint8_t *encoded;

    bambu_simulation_build(fixture, 30, NOW_MS, &snapshot);
    memset(&frame, 0, sizeof(frame));
    frame.snapshot = &snapshot;
    frame.strings = bambu_overlay_default_strings();
    frame.preview = preview;
    frame.visual_state = bambu_overlay_visual_state(&snapshot, NOW_MS, STALE_AFTER_MS);
    frame.displayed_progress = snapshot.progress_percent;
    frame.stale_seconds = (int)((NOW_MS - snapshot.updated_monotonic_ms) / 1000);
    frame.clock_hour = 13;
    frame.clock_minute = 29;

    if (!bambu_canvas_init(&canvas, style.width, style.height))
        exit(2);
    encoded = malloc((size_t)style.width * (size_t)style.height * 4);
    bambu_overlay_render(&canvas, text, &style, &frame);
    bambu_canvas_encode(&canvas, encoded);
    snprintf(path, sizeof(path), "%s/%s.png", directory, name);
    write_composite(&canvas, encoded, path);
    free(encoded);
    bambu_canvas_free(&canvas);
    printf("%s\n", path);
}

int main(int argc, char **argv)
{
    struct bambu_gdi_text *gdi;
    struct bambu_text_backend text;
    struct bambu_overlay_style style;
    struct bambu_image preview;
    const char *directory = argc > 1 ? argv[1] : ".";

    gdi = bambu_gdi_text_create();
    if (gdi == NULL)
        return 2;
    bambu_gdi_text_backend(gdi, &text);
    preview = make_test_preview();

    bambu_overlay_default_style(&style);
    render_one(directory, "dashboard-printing", style, BAMBU_SIM_PRINTING, &preview, &text);
    render_one(directory, "dashboard-placeholder", style, BAMBU_SIM_PRINTING, NULL, &text);
    render_one(directory, "dashboard-paused", style, BAMBU_SIM_PAUSED, &preview, &text);
    render_one(directory, "dashboard-failed", style, BAMBU_SIM_FAILED, &preview, &text);
    render_one(directory, "dashboard-finished", style, BAMBU_SIM_FINISHED, &preview, &text);
    render_one(directory, "dashboard-idle", style, BAMBU_SIM_IDLE, NULL, &text);
    render_one(directory, "dashboard-stale", style, BAMBU_SIM_STALE, &preview, &text);
    render_one(directory, "dashboard-offline", style, BAMBU_SIM_OFFLINE, &preview, &text);
    render_one(directory, "dashboard-auth-failed", style, BAMBU_SIM_AUTH_FAILED, NULL, &text);
    render_one(directory, "dashboard-long-text", style, BAMBU_SIM_LONG_TEXT, &preview, &text);

    style.theme = BAMBU_THEME_LIGHT;
    render_one(directory, "dashboard-light", style, BAMBU_SIM_PRINTING, &preview, &text);
    style.theme = BAMBU_THEME_HIGH_CONTRAST;
    render_one(directory, "dashboard-high-contrast", style, BAMBU_SIM_PRINTING, &preview, &text);

    bambu_overlay_default_style(&style);
    style.ams_mode = BAMBU_AMS_ALL;
    render_one(directory, "dashboard-all-ams", style, BAMBU_SIM_MULTI_AMS, &preview, &text);

    bambu_overlay_default_style(&style);
    style.preset = BAMBU_PRESET_COMPACT;
    bambu_overlay_preset_size(style.preset, &style.width, &style.height);
    render_one(directory, "compact-printing", style, BAMBU_SIM_PRINTING, NULL, &text);
    render_one(directory, "compact-long-text", style, BAMBU_SIM_LONG_TEXT, NULL, &text);

    bambu_overlay_default_style(&style);
    style.preset = BAMBU_PRESET_EXPANDED;
    style.ams_mode = BAMBU_AMS_ALL;
    bambu_overlay_preset_size(style.preset, &style.width, &style.height);
    render_one(directory, "expanded-multi-ams", style, BAMBU_SIM_MULTI_AMS, &preview, &text);

    /* Scaled sources: half size and the documented minimum. */
    bambu_overlay_default_style(&style);
    style.width = 480;
    style.height = 130;
    render_one(directory, "dashboard-half-size", style, BAMBU_SIM_PRINTING, &preview, &text);
    style.width = BAMBU_MIN_WIDTH;
    style.height = BAMBU_MIN_HEIGHT;
    render_one(directory, "dashboard-minimum-size", style, BAMBU_SIM_PRINTING, &preview, &text);

    free(preview.pixels);
    bambu_gdi_text_destroy(gdi);
    return 0;
}
