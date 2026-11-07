/* Layout, visual state, and compositing tests. A block rasteriser stands in for
 * GDI so the results are identical on every machine. */

#include "bambu-simulation.h"
#include "overlay-render.h"
#include "test-framework.h"

#include <math.h>

int bambu_test_failures = 0;

static int text_calls = 0;
static const char *last_long_text = NULL;
static float smallest_text = 1.0e9f;
static const char *wanted_text = NULL;
static bool wanted_text_seen = false;

static void fake_rasterize(
    void *context,
    const char *utf8,
    float size_px,
    bool bold,
    enum bambu_text_align align,
    uint8_t *mask,
    int width,
    int height
)
{
    (void)context;
    (void)bold;
    (void)align;
    text_calls++;
    if (size_px < smallest_text)
        smallest_text = size_px;
    if (wanted_text != NULL && strcmp(utf8, wanted_text) == 0)
        wanted_text_seen = true;
    if (strlen(utf8) > 80)
        last_long_text = utf8;
    memset(mask, 255, (size_t)width * (size_t)height / 2);
}

static float fake_measure(void *context, const char *utf8, float size_px, bool bold)
{
    (void)context;
    (void)bold;
    return (float)strlen(utf8) * size_px * 0.5f;
}

static const struct bambu_text_backend fake_text = {NULL, fake_rasterize, fake_measure};

static bool inside(struct bambu_rect inner, struct bambu_rect outer)
{
    const float slack = 0.51f;
    return inner.x >= outer.x - slack && inner.y >= outer.y - slack &&
           inner.x + inner.w <= outer.x + outer.w + slack && inner.y + inner.h <= outer.y + outer.h + slack;
}

static bool overlaps(struct bambu_rect a, struct bambu_rect b)
{
    if (a.w <= 0.0f || a.h <= 0.0f || b.w <= 0.0f || b.h <= 0.0f)
        return false;
    return a.x + 0.5f < b.x + b.w && b.x + 0.5f < a.x + a.w && a.y + 0.5f < b.y + b.h && b.y + 0.5f < a.y + a.h;
}

static void check_geometry(const struct bambu_overlay_style *style)
{
    struct bambu_overlay_geometry g;

    bambu_overlay_layout(style, &g);
    CHECK(g.unit > 0.0f);
    CHECK(inside(g.job, g.panel));
    CHECK(inside(g.track, g.panel));
    CHECK(inside(g.percent, g.panel));
    CHECK(inside(g.details, g.panel));
    CHECK(g.track.w > 0.0f);
    if (g.has_badge) {
        CHECK(inside(g.badge, g.panel));
        CHECK(!overlaps(g.badge, g.job));
    }
    if (g.has_preview) {
        CHECK(inside(g.preview, g.panel));
        CHECK(fabsf(g.preview.w - g.preview.h) < 0.01f);
        CHECK(!overlaps(g.preview, g.job));
        CHECK(!overlaps(g.preview, g.track));
    }
    if (g.has_metrics) {
        CHECK(inside(g.metrics, g.panel));
        CHECK(!overlaps(g.metrics, g.job));
        CHECK(!overlaps(g.metrics, g.track));
    }
    if (g.has_ams) {
        CHECK(inside(g.ams, g.panel));
        CHECK(!overlaps(g.ams, g.track));
        CHECK(!overlaps(g.ams, g.details));
        if (g.has_metrics)
            CHECK(!overlaps(g.ams, g.metrics));
    }
    if (g.has_totals) {
        CHECK(inside(g.totals, g.panel));
        CHECK(!overlaps(g.totals, g.details));
        if (g.has_ams)
            CHECK(!overlaps(g.totals, g.ams));
    }
}

static void test_geometry(void)
{
    static const enum bambu_overlay_preset presets[] = {
        BAMBU_PRESET_COMPACT, BAMBU_PRESET_DASHBOARD, BAMBU_PRESET_EXPANDED
    };

    for (size_t p = 0; p < 3; p++) {
        struct bambu_overlay_style style;
        int width;
        int height;
        const int sizes[][2] = {{0, 0}, {1280, 346}, {1920, 520}, {2560, 693}, {480, 130}, {320, 90}, {3840, 2160}};

        bambu_overlay_default_style(&style);
        style.preset = presets[p];
        bambu_overlay_preset_size(presets[p], &width, &height);

        for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
            style.width = sizes[s][0] != 0 ? sizes[s][0] : width;
            style.height = sizes[s][1] != 0 ? sizes[s][1] : height;
            check_geometry(&style);
        }

        /* Every section toggle combination still lays out safely. */
        style.width = width;
        style.height = height;
        for (int mask = 0; mask < 32; mask++) {
            style.show_preview = (mask & 1) != 0;
            style.show_ams = (mask & 2) != 0;
            style.show_temperatures = (mask & 4) != 0;
            style.show_fan = style.show_speed = (mask & 8) != 0;
            style.show_badge = (mask & 16) != 0;
            check_geometry(&style);
        }
    }
}

/* The layout is a function of style alone, so data arriving cannot move anything. */
static void test_no_layout_shift(void)
{
    struct bambu_overlay_style style;
    struct bambu_overlay_geometry before;
    struct bambu_overlay_geometry after;

    bambu_overlay_default_style(&style);
    bambu_overlay_layout(&style, &before);
    bambu_overlay_layout(&style, &after);
    CHECK(memcmp(&before, &after, sizeof(before)) == 0);
}

static void test_visual_state(void)
{
    struct bambu_printer_snapshot s;

    CHECK(bambu_overlay_visual_state(NULL, 0, 1000) == BAMBU_VISUAL_CONFIG_REQUIRED);
    bambu_snapshot_init(&s);
    CHECK(bambu_overlay_visual_state(&s, 0, 1000) == BAMBU_VISUAL_CONFIG_REQUIRED);

    s.connection_state = BAMBU_CONNECTION_LIVE;
    s.updated_monotonic_ms = 5000;
    CHECK(bambu_overlay_visual_state(&s, 5500, 1000) == BAMBU_VISUAL_LIVE);
    CHECK(bambu_overlay_visual_state(&s, 6000, 1000) == BAMBU_VISUAL_LIVE);
    CHECK(bambu_overlay_visual_state(&s, 6001, 1000) == BAMBU_VISUAL_STALE);

    s.connection_state = BAMBU_CONNECTION_CONNECTING;
    CHECK(bambu_overlay_visual_state(&s, 5500, 1000) == BAMBU_VISUAL_CONNECTING);
    s.connection_state = BAMBU_CONNECTION_RECONNECTING;
    CHECK(bambu_overlay_visual_state(&s, 5500, 1000) == BAMBU_VISUAL_RECONNECTING);
    s.connection_state = BAMBU_CONNECTION_OFFLINE;
    CHECK(bambu_overlay_visual_state(&s, 5500, 1000) == BAMBU_VISUAL_OFFLINE);
    s.connection_state = BAMBU_CONNECTION_AUTHENTICATION_FAILED;
    CHECK(bambu_overlay_visual_state(&s, 5500, 1000) == BAMBU_VISUAL_AUTH_FAILED);
    s.connection_state = BAMBU_CONNECTION_ERROR;
    CHECK(bambu_overlay_visual_state(&s, 5500, 1000) == BAMBU_VISUAL_ERROR);
}

static void test_motion(void)
{
    double displayed = 10.0;

    /* Monotonic, no overshoot, and settles exactly on the real value. */
    for (int i = 0; i < 200; i++) {
        double next = bambu_overlay_animate_progress(displayed, 60.0, 0.033, false);
        CHECK(next >= displayed);
        CHECK(next <= 60.0);
        displayed = next;
    }
    CHECK(displayed == 60.0);

    CHECK(bambu_overlay_animate_progress(10.0, 60.0, 0.033, true) == 60.0);
    CHECK(bambu_overlay_animate_progress(10.0, 60.0, 0.0, false) == 10.0);
    CHECK(bambu_overlay_animate_progress(10.0, 60.0, 5.0, false) == 60.0);
    CHECK(bambu_overlay_animate_progress(60.0, 60.0, 0.033, false) == 60.0);
}

static uint8_t *pixel(struct bambu_canvas *canvas, int x, int y)
{
    return canvas->pixels + ((size_t)y * (size_t)canvas->width + (size_t)x) * 4;
}

static void test_canvas(void)
{
    struct bambu_canvas canvas;
    struct bambu_color white = {255, 255, 255, 255};
    struct bambu_color half = {255, 0, 0, 128};
    struct bambu_rect rect = {10.0f, 10.0f, 40.0f, 40.0f};
    uint8_t *encoded;

    CHECK(!bambu_canvas_init(&canvas, 0, 10));
    CHECK(bambu_canvas_init(&canvas, 64, 64));

    bambu_canvas_fill_rounded(&canvas, rect, 12.0f, white);
    CHECK(pixel(&canvas, 30, 30)[3] == 255);
    CHECK(pixel(&canvas, 2, 2)[3] == 0);
    CHECK(pixel(&canvas, 10, 10)[3] < 255); /* corner is rounded off */

    /* Drawing outside the canvas is clipped, never written. */
    {
        struct bambu_rect outside = {-100.0f, -100.0f, 1000.0f, 1000.0f};
        bambu_canvas_fill_rounded(&canvas, outside, 0.0f, white);
        bambu_canvas_stroke_rounded(&canvas, outside, 4.0f, 2.0f, white);
        bambu_canvas_fill_triangle(&canvas, -50.0f, -50.0f, 500.0f, 0.0f, 0.0f, 500.0f, white);
    }

    bambu_canvas_clear(&canvas);
    bambu_canvas_fill_rounded(&canvas, rect, 0.0f, half);
    CHECK(pixel(&canvas, 30, 30)[3] == 128);
    CHECK(pixel(&canvas, 30, 30)[2] == 128); /* premultiplied */

    /* Encoding must leave fully transparent pixels empty and keep alpha exact. */
    encoded = malloc((size_t)canvas.width * (size_t)canvas.height * 4);
    bambu_canvas_encode(&canvas, encoded);
    CHECK(encoded[3] == 0 && encoded[0] == 0);
    {
        const uint8_t *p = encoded + ((size_t)30 * 64 + 30) * 4;
        CHECK(p[3] == 128);
        /* Half alpha in linear light encodes to about 0.73 in sRGB, not 0.5. */
        CHECK(p[2] > 180 && p[2] < 195);
        CHECK(p[0] == 0 && p[1] == 0);
    }
    free(encoded);
    bambu_canvas_free(&canvas);
}

static void test_image_draw(void)
{
    struct bambu_canvas canvas;
    struct bambu_rect rect = {0.0f, 0.0f, 32.0f, 32.0f};
    struct bambu_image image;
    uint8_t pixels[4 * 4] = {0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255};

    image.width = 2;
    image.height = 2;
    image.pixels = pixels;
    CHECK(bambu_canvas_init(&canvas, 32, 32));
    bambu_canvas_draw_image(&canvas, &image, rect, 0.0f);
    CHECK(pixel(&canvas, 16, 16)[2] == 255);
    CHECK(pixel(&canvas, 16, 16)[3] == 255);
    bambu_canvas_draw_image(&canvas, NULL, rect, 0.0f);
    bambu_canvas_free(&canvas);
}

static void render_all_fixtures(enum bambu_overlay_preset preset, int width, int height)
{
    struct bambu_canvas canvas;
    struct bambu_overlay_style style;
    static struct bambu_printer_snapshot snapshot;

    bambu_overlay_default_style(&style);
    style.preset = preset;
    style.width = width;
    style.height = height;
    style.ams_mode = BAMBU_AMS_ALL;
    CHECK(bambu_canvas_init(&canvas, width, height));

    for (int f = BAMBU_SIM_PRINTING; f < BAMBU_SIM_FIXTURE_COUNT; f++) {
        struct bambu_overlay_frame frame;

        bambu_simulation_build((enum bambu_simulation_fixture)f, 30, 1000000, &snapshot);
        memset(&frame, 0, sizeof(frame));
        frame.snapshot = &snapshot;
        frame.strings = bambu_overlay_default_strings();
        frame.visual_state = bambu_overlay_visual_state(&snapshot, 1000000, 30000);
        frame.displayed_progress = snapshot.progress_percent;
        frame.simulated = true;
        text_calls = 0;
        bambu_overlay_render(&canvas, &fake_text, &style, &frame);
        CHECK(text_calls > 0);

        /* The panel fills the source and the corners stay transparent. */
        CHECK(pixel(&canvas, width / 2, height - 2)[3] > 0);
        CHECK(pixel(&canvas, 0, 0)[3] == 0);
    }
    bambu_canvas_free(&canvas);
}

static void test_render_fixtures(void)
{
    render_all_fixtures(BAMBU_PRESET_COMPACT, 640, 104);
    render_all_fixtures(BAMBU_PRESET_DASHBOARD, 960, 260);
    render_all_fixtures(BAMBU_PRESET_EXPANDED, 960, 400);
    render_all_fixtures(BAMBU_PRESET_DASHBOARD, 320, 90);
    render_all_fixtures(BAMBU_PRESET_EXPANDED, 1920, 800);
}

static float smallest_text_at(enum bambu_overlay_preset preset, int width, int height)
{
    struct bambu_canvas canvas;
    struct bambu_overlay_style style;
    static struct bambu_printer_snapshot snapshot;

    bambu_overlay_default_style(&style);
    style.preset = preset;
    style.width = width;
    style.height = height;
    style.ams_mode = BAMBU_AMS_ALL;
    CHECK(bambu_canvas_init(&canvas, width, height));
    smallest_text = 1.0e9f;

    for (int f = BAMBU_SIM_PRINTING; f < BAMBU_SIM_FIXTURE_COUNT; f++) {
        struct bambu_overlay_frame frame;

        bambu_simulation_build((enum bambu_simulation_fixture)f, 30, 1000000, &snapshot);
        memset(&frame, 0, sizeof(frame));
        frame.snapshot = &snapshot;
        frame.strings = bambu_overlay_default_strings();
        frame.visual_state = bambu_overlay_visual_state(&snapshot, 1000000, 30000);
        frame.displayed_progress = snapshot.progress_percent;
        frame.simulated = true;
        bambu_overlay_render(&canvas, &fake_text, &style, &frame);
    }
    bambu_canvas_free(&canvas);
    return smallest_text;
}

/* The readability floor: full size at preset size, and at no source size allowed
 * in the properties does text fall past the point where a tier step takes over. */
static void test_text_stays_readable(void)
{
    static const enum bambu_overlay_preset presets[] = {
        BAMBU_PRESET_COMPACT, BAMBU_PRESET_DASHBOARD, BAMBU_PRESET_EXPANDED
    };
    const float floor_px = BAMBU_TEXT_MIN * BAMBU_READABLE_FIT - 0.01f;

    for (size_t p = 0; p < 3; p++) {
        int width;
        int height;

        bambu_overlay_preset_size(presets[p], &width, &height);
        CHECK(smallest_text_at(presets[p], width, height) >= BAMBU_TEXT_MIN - 0.01f);

        for (int w = BAMBU_MIN_WIDTH; w <= 1280; w += 80) {
            for (int h = BAMBU_MIN_HEIGHT; h <= 560; h += 44)
                CHECK(smallest_text_at(presets[p], w, h) >= floor_px);
        }
    }
}

static void test_tiers_step_down(void)
{
    struct bambu_overlay_style style;
    struct bambu_overlay_geometry g;

    bambu_overlay_default_style(&style);
    bambu_overlay_layout(&style, &g);
    CHECK(!g.compact && !g.minimal && g.has_preview && g.has_ams && g.has_badge);

    style.width = 600;
    style.height = 130;
    bambu_overlay_layout(&style, &g);
    CHECK(g.compact && !g.has_preview && !g.has_ams && !g.minimal);

    style.width = 480;
    bambu_overlay_layout(&style, &g);
    CHECK(g.compact && g.minimal);

    style.width = BAMBU_MIN_WIDTH;
    style.height = BAMBU_MIN_HEIGHT;
    bambu_overlay_layout(&style, &g);
    CHECK(g.compact && g.minimal && !g.has_badge);
    CHECK(g.job.h > 0.0f && g.track.h > 0.0f);
}

/* With no profile name, the state and stage are often the same word. */
static void test_subtitle_does_not_repeat(void)
{
    struct bambu_canvas canvas;
    struct bambu_overlay_style style;
    struct bambu_overlay_frame frame;
    static struct bambu_printer_snapshot snapshot;

    bambu_overlay_default_style(&style);
    CHECK(bambu_canvas_init(&canvas, style.width, style.height));
    bambu_simulation_build(BAMBU_SIM_PRINTING, 30, 1000000, &snapshot);
    memset(&frame, 0, sizeof(frame));
    frame.snapshot = &snapshot;
    frame.strings = bambu_overlay_default_strings();
    frame.displayed_progress = snapshot.progress_percent;
    snapshot.print_profile[0] = '\0';
    bambu_copy_text(snapshot.stage, sizeof(snapshot.stage), frame.strings->job_printing);

    wanted_text = frame.strings->job_printing;
    wanted_text_seen = false;
    bambu_overlay_render(&canvas, &fake_text, &style, &frame);
    CHECK(wanted_text_seen);

    wanted_text = NULL;
    bambu_canvas_free(&canvas);
}

/* The minimal tier has no badge, so a problem must still reach the viewer. */
static void test_minimal_tier_shows_problems(void)
{
    struct bambu_canvas canvas;
    struct bambu_overlay_style style;
    struct bambu_overlay_frame frame;
    static struct bambu_printer_snapshot snapshot;

    bambu_overlay_default_style(&style);
    style.width = BAMBU_MIN_WIDTH;
    style.height = BAMBU_MIN_HEIGHT;
    CHECK(bambu_canvas_init(&canvas, style.width, style.height));
    bambu_simulation_build(BAMBU_SIM_PRINTING, 30, 1000000, &snapshot);
    memset(&frame, 0, sizeof(frame));
    frame.snapshot = &snapshot;
    frame.strings = bambu_overlay_default_strings();
    frame.displayed_progress = snapshot.progress_percent;

    frame.visual_state = BAMBU_VISUAL_OFFLINE;
    wanted_text = frame.strings->badge_offline;
    wanted_text_seen = false;
    bambu_overlay_render(&canvas, &fake_text, &style, &frame);
    CHECK(wanted_text_seen);

    frame.visual_state = BAMBU_VISUAL_LIVE;
    wanted_text = snapshot.job_name;
    wanted_text_seen = false;
    bambu_overlay_render(&canvas, &fake_text, &style, &frame);
    CHECK(wanted_text_seen);

    frame.simulated = true;
    wanted_text = frame.strings->simulated;
    wanted_text_seen = false;
    bambu_overlay_render(&canvas, &fake_text, &style, &frame);
    CHECK(wanted_text_seen);

    wanted_text = NULL;
    bambu_canvas_free(&canvas);
}

static void test_render_empty_and_null_text(void)
{
    struct bambu_canvas canvas;
    struct bambu_overlay_style style;
    struct bambu_printer_snapshot snapshot;
    struct bambu_overlay_frame frame;

    bambu_overlay_default_style(&style);
    bambu_snapshot_init(&snapshot);
    memset(&frame, 0, sizeof(frame));
    frame.snapshot = &snapshot;
    frame.strings = bambu_overlay_default_strings();
    frame.visual_state = BAMBU_VISUAL_CONFIG_REQUIRED;

    CHECK(bambu_canvas_init(&canvas, style.width, style.height));
    /* An empty snapshot must render placeholders, with or without a text backend. */
    bambu_overlay_render(&canvas, &fake_text, &style, &frame);
    bambu_overlay_render(&canvas, NULL, &style, &frame);
    frame.strings = NULL;
    bambu_overlay_render(&canvas, &fake_text, &style, &frame);
    bambu_canvas_free(&canvas);
}

static void test_simulation_cycle(void)
{
    static struct bambu_printer_snapshot snapshot;
    bool saw_paused = false;
    bool saw_offline = false;

    for (uint64_t second = 0; second < 13 * BAMBU_SIM_CYCLE_SECONDS; second += BAMBU_SIM_CYCLE_SECONDS) {
        bambu_simulation_build(BAMBU_SIM_CYCLE, second, 500000, &snapshot);
        saw_paused = saw_paused || snapshot.job_state == BAMBU_JOB_PAUSED;
        saw_offline = saw_offline || snapshot.connection_state == BAMBU_CONNECTION_OFFLINE;
    }
    CHECK(saw_paused);
    CHECK(saw_offline);

    bambu_simulation_build(BAMBU_SIM_IDLE, 0, 500000, &snapshot);
    CHECK(snapshot.job_state == BAMBU_JOB_IDLE);
    CHECK(snapshot.has_nozzle_temperature);

    bambu_simulation_build(BAMBU_SIM_MULTI_AMS, 0, 500000, &snapshot);
    CHECK(snapshot.ams_unit_count == 2);
    CHECK(snapshot.active_unit_id == 1 && snapshot.active_tray_index == 2);

    bambu_simulation_build(BAMBU_SIM_STALE, 0, 500000, &snapshot);
    CHECK(bambu_overlay_visual_state(&snapshot, 500000, 30000) == BAMBU_VISUAL_STALE);
}

int main(void)
{
    test_geometry();
    test_no_layout_shift();
    test_visual_state();
    test_motion();
    test_canvas();
    test_image_draw();
    test_render_fixtures();
    test_text_stays_readable();
    test_tiers_step_down();
    test_minimal_tier_shows_problems();
    test_subtitle_does_not_repeat();
    test_render_empty_and_null_text();
    test_simulation_cycle();

    if (bambu_test_failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", bambu_test_failures);
        return 1;
    }
    puts("render tests passed");
    return 0;
}
