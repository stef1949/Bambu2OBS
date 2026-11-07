#include "overlay-render.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define COLOR_PAUSED 0xE8A317u
#define COLOR_FAILED 0xE5484Du
#define COLOR_FINISHED 0x3FA7F5u
#define COLOR_MUTED 0x8A949Eu

struct palette {
    struct bambu_color panel;
    struct bambu_color edge;
    struct bambu_color text;
    struct bambu_color dim;
    struct bambu_color track;
    struct bambu_color chip;
    struct bambu_color accent;
};

static struct bambu_color rgb(uint32_t value, uint8_t alpha)
{
    struct bambu_color color;
    color.r = (uint8_t)(value >> 16);
    color.g = (uint8_t)(value >> 8);
    color.b = (uint8_t)value;
    color.a = alpha;
    return color;
}

static uint8_t unit_to_byte(float value)
{
    if (value <= 0.0f)
        return 0;
    if (value >= 1.0f)
        return 255;
    return (uint8_t)(value * 255.0f + 0.5f);
}

static void build_palette(const struct bambu_overlay_style *style, struct palette *palette)
{
    uint8_t panel_alpha = unit_to_byte(style->panel_opacity);

    switch (style->theme) {
    case BAMBU_THEME_LIGHT:
        palette->panel = rgb(0xF5F7F9, panel_alpha);
        palette->edge = rgb(0x000000, 30);
        palette->text = rgb(0x14181C, 255);
        palette->dim = rgb(0x3A444E, 255);
        palette->track = rgb(0xCCD3DA, 255);
        palette->chip = rgb(0x000000, 14);
        break;
    case BAMBU_THEME_HIGH_CONTRAST:
        /* Legibility beats translucency here, whatever opacity was chosen. */
        palette->panel = rgb(0x000000, panel_alpha < 235 ? 235 : panel_alpha);
        palette->edge = rgb(0xFFFFFF, 255);
        palette->text = rgb(0xFFFFFF, 255);
        palette->dim = rgb(0xFFFFFF, 255);
        palette->track = rgb(0x5A5A5A, 255);
        palette->chip = rgb(0xFFFFFF, 34);
        break;
    case BAMBU_THEME_CUSTOM:
        palette->panel = rgb(style->panel_rgb, panel_alpha);
        palette->edge = rgb(style->text_rgb, 40);
        palette->text = rgb(style->text_rgb, 255);
        palette->dim = rgb(style->text_rgb, 225);
        palette->track = rgb(style->text_rgb, 60);
        palette->chip = rgb(style->text_rgb, 22);
        break;
    case BAMBU_THEME_DARK:
    default:
        palette->panel = rgb(0x12161A, panel_alpha);
        palette->edge = rgb(0xFFFFFF, 32);
        palette->text = rgb(0xF3F6F8, 255);
        palette->dim = rgb(0xCBD3DA, 255);
        palette->track = rgb(0xFFFFFF, 40);
        palette->chip = rgb(0xFFFFFF, 16);
        break;
    }
    palette->accent = rgb(style->accent_rgb, 255);
}

void bambu_overlay_default_style(struct bambu_overlay_style *style)
{
    memset(style, 0, sizeof(*style));
    style->preset = BAMBU_PRESET_DASHBOARD;
    style->theme = BAMBU_THEME_DARK;
    style->ams_mode = BAMBU_AMS_ACTIVE;
    bambu_overlay_preset_size(style->preset, &style->width, &style->height);
    style->density = 1.0f;
    style->font_scale = 1.0f;
    style->corner_radius = 14.0f;
    style->padding = 16.0f;
    style->panel_opacity = 0.94f;
    style->accent_rgb = 0x00AE42;
    style->panel_rgb = 0x12161A;
    style->text_rgb = 0xF3F6F8;
    style->show_preview = true;
    style->show_ams = true;
    style->show_stage = true;
    style->show_layers = true;
    style->show_temperatures = true;
    style->show_fan = true;
    style->show_speed = true;
    style->show_total_time = true;
    style->show_total_weight = true;
    style->show_badge = true;
    style->temperature_unit = BAMBU_UNIT_CELSIUS;
    style->temperature_decimals = 0;
    style->use_24_hour = true;
    style->show_clock_eta = true;
}

const struct bambu_overlay_strings *bambu_overlay_default_strings(void)
{
    static const struct bambu_overlay_strings strings = {
        .badge_live = "LIVE",
        .badge_stale = "STALE",
        .badge_connecting = "CONNECTING",
        .badge_reconnecting = "RECONNECTING",
        .badge_offline = "OFFLINE",
        .badge_auth_failed = "AUTH FAILED",
        .badge_config_required = "SETUP NEEDED",
        .badge_error = "ERROR",
        .simulated = "SIMULATED",
        .no_job = "No active print",
        .job_idle = "Idle",
        .job_preparing = "Preparing",
        .job_printing = "Printing",
        .job_paused = "Paused",
        .job_finished = "Finished",
        .job_failed = "Failed",
        .label_nozzle = "Nozzle",
        .label_bed = "Bed",
        .label_fan = "Fan",
        .label_speed = "Speed",
        .label_layer = "Layer",
        .label_eta = "ETA",
        .label_remaining = "Remaining",
        .label_total_time = "Total time",
        .label_total_weight = "Total weight",
        .tray_empty = "Empty",
        .no_ams = "No AMS data",
        .external_spool = "External spool",
        .missing_value = "\xE2\x80\x94",
    };
    return &strings;
}

void bambu_overlay_preset_size(enum bambu_overlay_preset preset, int *width, int *height)
{
    switch (preset) {
    case BAMBU_PRESET_COMPACT:
        *width = 640;
        *height = 104;
        break;
    case BAMBU_PRESET_EXPANDED:
        *width = 960;
        *height = 400;
        break;
    case BAMBU_PRESET_DASHBOARD:
    default:
        *width = 960;
        *height = 260;
        break;
    }
}

static struct bambu_rect make_rect(float x, float y, float w, float h)
{
    struct bambu_rect rect;
    rect.x = x;
    rect.y = y;
    rect.w = w > 0.0f ? w : 0.0f;
    rect.h = h > 0.0f ? h : 0.0f;
    return rect;
}

static float preset_fit(const struct bambu_overlay_style *style, enum bambu_overlay_preset preset)
{
    int base_width;
    int base_height;

    bambu_overlay_preset_size(preset, &base_width, &base_height);
    return fminf((float)style->width / (float)base_width, (float)style->height / (float)base_height);
}

static void layout_compact(
    const struct bambu_overlay_style *style,
    struct bambu_overlay_geometry *g,
    struct bambu_rect inner
)
{
    float u = g->unit;
    float percent_width = 92.0f * u;
    float column_x = inner.x + percent_width + 12.0f * u;
    float column_w = inner.x + inner.w - column_x;
    float badge_width = style->show_badge ? 172.0f * u : 0.0f;
    float row = inner.h / 3.0f;

    g->percent = make_rect(inner.x, inner.y, percent_width, inner.h);
    g->subtitle = make_rect(0, 0, 0, 0);
    g->eta = make_rect(0, 0, 0, 0);

    if (g->minimal) {
        /* Too small for three rows of readable text: two large ones instead. */
        g->job = make_rect(column_x, inner.y, column_w, inner.h * 0.64f);
        g->track = make_rect(column_x, inner.y + inner.h * 0.72f, column_w, 14.0f * u);
        g->details = make_rect(0, 0, 0, 0);
        return;
    }

    g->has_badge = style->show_badge;
    g->job = make_rect(column_x, inner.y, column_w - badge_width - 8.0f * u, row * 1.15f);
    g->badge = make_rect(inner.x + inner.w - badge_width, inner.y, badge_width, 27.0f * u);
    g->track = make_rect(column_x, inner.y + row * 1.35f, column_w, 10.0f * u);
    g->details = make_rect(column_x, inner.y + row * 1.9f, column_w, row * 1.1f);
}

static void layout_panel(
    const struct bambu_overlay_style *style,
    struct bambu_overlay_geometry *g,
    struct bambu_rect inner
)
{
    bool expanded = style->preset == BAMBU_PRESET_EXPANDED;
    float u = g->unit;
    float gap = 14.0f * u;
    float rail_height = expanded ? 128.0f * u : 56.0f * u;
    float totals_height = expanded ? 56.0f * u : 0.0f;
    bool wants_metrics = style->show_temperatures || style->show_fan || style->show_speed;
    struct bambu_rect content = inner;
    struct bambu_rect main_area;
    float center_w;
    float y;

    if (style->show_preview) {
        float side = fminf(inner.h, inner.w * 0.30f);
        g->has_preview = true;
        g->preview = make_rect(inner.x, inner.y, side, side);
        content.x += side + gap;
        content.w -= side + gap;
    }

    main_area = content;
    if (style->show_ams) {
        g->has_ams = true;
        g->ams = make_rect(content.x, content.y + content.h - rail_height, content.w, rail_height);
        main_area.h -= rail_height + 10.0f * u;
    }
    if (expanded && (style->show_total_time || style->show_total_weight)) {
        g->has_totals = true;
        g->totals = make_rect(main_area.x, main_area.y + main_area.h - totals_height, main_area.w, totals_height);
        main_area.h -= totals_height + 8.0f * u;
    }

    center_w = main_area.w;
    if (wants_metrics) {
        float metrics_w = fminf(224.0f * u, main_area.w * 0.42f);
        float badge_room = style->show_badge ? 34.0f * u : 0.0f;
        g->has_metrics = true;
        g->metrics = make_rect(
            main_area.x + main_area.w - metrics_w,
            main_area.y + badge_room,
            metrics_w,
            main_area.h - badge_room
        );
        center_w -= metrics_w + gap;
    }

    if (style->show_badge) {
        float badge_w = 172.0f * u;
        g->has_badge = true;
        g->badge = make_rect(main_area.x + main_area.w - badge_w, main_area.y, badge_w, 28.0f * u);
        if (!wants_metrics)
            center_w -= badge_w + 8.0f * u;
    }

    /* The centre column is laid out from the top, then pinned to the bottom, so
     * the track always sits on the same line whatever text is above it. */
    y = main_area.y;
    g->job = make_rect(main_area.x, y, center_w, 34.0f * u);
    y += 34.0f * u;
    g->subtitle = make_rect(main_area.x, y, center_w, 26.0f * u);

    y = main_area.y + main_area.h;
    g->details = make_rect(main_area.x, y - 26.0f * u, center_w, 26.0f * u);
    y -= 26.0f * u + 4.0f * u;
    g->track = make_rect(main_area.x, y - 12.0f * u, center_w, 12.0f * u);
    y -= 12.0f * u + 2.0f * u;
    {
        float top = g->subtitle.y + g->subtitle.h;
        float height = fminf(46.0f * u, y - top);
        g->percent = make_rect(main_area.x, y - height, center_w * 0.42f, height);
        g->eta = make_rect(main_area.x + center_w * 0.42f, y - height, center_w * 0.58f, height);
    }
}

void bambu_overlay_layout(const struct bambu_overlay_style *style, struct bambu_overlay_geometry *g)
{
    struct bambu_rect inner;
    float padding;

    enum bambu_overlay_preset preset = style->preset;

    memset(g, 0, sizeof(*g));
    /* Below the readable size, drop detail rather than shrink text past the floor. */
    if (preset != BAMBU_PRESET_COMPACT && preset_fit(style, preset) < BAMBU_READABLE_FIT)
        preset = BAMBU_PRESET_COMPACT;
    g->compact = preset == BAMBU_PRESET_COMPACT;
    g->minimal = g->compact && preset_fit(style, BAMBU_PRESET_COMPACT) < BAMBU_READABLE_FIT;
    g->unit = preset_fit(style, preset) * (style->density > 0.0f ? style->density : 1.0f);
    g->panel = make_rect(0.0f, 0.0f, (float)style->width, (float)style->height);
    padding = style->padding * g->unit;
    inner = make_rect(padding, padding, (float)style->width - padding * 2.0f, (float)style->height - padding * 2.0f);

    if (g->compact)
        layout_compact(style, g, inner);
    else
        layout_panel(style, g, inner);
}

enum bambu_visual_state bambu_overlay_visual_state(
    const struct bambu_printer_snapshot *snapshot,
    uint64_t now_monotonic_ms,
    uint64_t stale_after_ms
)
{
    if (snapshot == NULL)
        return BAMBU_VISUAL_CONFIG_REQUIRED;

    switch (snapshot->connection_state) {
    case BAMBU_CONNECTION_CONFIGURATION_REQUIRED:
        return BAMBU_VISUAL_CONFIG_REQUIRED;
    case BAMBU_CONNECTION_CONNECTING:
        return BAMBU_VISUAL_CONNECTING;
    case BAMBU_CONNECTION_RECONNECTING:
        return BAMBU_VISUAL_RECONNECTING;
    case BAMBU_CONNECTION_OFFLINE:
        return BAMBU_VISUAL_OFFLINE;
    case BAMBU_CONNECTION_AUTHENTICATION_FAILED:
        return BAMBU_VISUAL_AUTH_FAILED;
    case BAMBU_CONNECTION_ERROR:
        return BAMBU_VISUAL_ERROR;
    case BAMBU_CONNECTION_LIVE:
    default:
        return bambu_snapshot_is_stale(snapshot, now_monotonic_ms, stale_after_ms) ? BAMBU_VISUAL_STALE
                                                                                   : BAMBU_VISUAL_LIVE;
    }
}

double bambu_overlay_animate_progress(double displayed, double target, double elapsed_seconds, bool reduce_motion)
{
    double difference = target - displayed;
    double step;

    if (reduce_motion || fabs(difference) < 0.05 || elapsed_seconds >= 1.0)
        return target;
    if (elapsed_seconds <= 0.0)
        return displayed;
    /* Exponential approach with a ~250 ms time constant cannot overshoot. */
    step = difference * (1.0 - exp(-elapsed_seconds / 0.25));
    return displayed + step;
}

struct draw_context {
    struct bambu_canvas *canvas;
    const struct bambu_text_backend *text;
    const struct bambu_overlay_style *style;
    const struct bambu_overlay_frame *frame;
    const struct bambu_overlay_geometry *geometry;
    struct palette palette;
    float unit;
    float font;
};

static void draw_text(
    struct draw_context *c,
    const char *text,
    struct bambu_rect rect,
    float size,
    bool bold,
    enum bambu_text_align align,
    struct bambu_color color
)
{
    int x = (int)floorf(rect.x);
    int y = (int)floorf(rect.y);
    int width = (int)ceilf(rect.w);
    int height = (int)ceilf(rect.h);
    size_t bytes;

    if (text == NULL || text[0] == '\0' || width <= 0 || height <= 0 || c->text == NULL)
        return;
    if (x + width > c->canvas->width)
        width = c->canvas->width - x;
    if (y + height > c->canvas->height)
        height = c->canvas->height - y;
    if (width <= 0 || height <= 0)
        return;

    bytes = (size_t)width * (size_t)height;
    memset(c->canvas->coverage, 0, bytes);
    c->text->rasterize(c->text->context, text, size * c->font, bold, align, c->canvas->coverage, width, height);
    bambu_canvas_blit_mask(c->canvas, c->canvas->coverage, width, height, x, y, color);
}

static const char *job_state_text(const struct bambu_overlay_strings *strings, enum bambu_job_state state)
{
    switch (state) {
    case BAMBU_JOB_IDLE:
        return strings->job_idle;
    case BAMBU_JOB_PREPARING:
        return strings->job_preparing;
    case BAMBU_JOB_PRINTING:
        return strings->job_printing;
    case BAMBU_JOB_PAUSED:
        return strings->job_paused;
    case BAMBU_JOB_FINISHED:
        return strings->job_finished;
    case BAMBU_JOB_FAILED:
        return strings->job_failed;
    case BAMBU_JOB_UNKNOWN:
    default:
        return strings->missing_value;
    }
}

static bool data_is_current(const struct bambu_overlay_frame *frame)
{
    return frame->visual_state == BAMBU_VISUAL_LIVE;
}

/* The progress colour carries job state; stale data loses its accent entirely so
 * a frozen bar can never be mistaken for a live one. */
static struct bambu_color progress_color(const struct draw_context *c)
{
    if (!data_is_current(c->frame))
        return rgb(COLOR_MUTED, 255);
    switch (c->frame->snapshot->job_state) {
    case BAMBU_JOB_PAUSED:
        return rgb(COLOR_PAUSED, 255);
    case BAMBU_JOB_FAILED:
        return rgb(COLOR_FAILED, 255);
    case BAMBU_JOB_FINISHED:
        return rgb(COLOR_FINISHED, 255);
    default:
        return c->palette.accent;
    }
}

static void draw_track(struct draw_context *c, struct bambu_rect track)
{
    const struct bambu_printer_snapshot *s = c->frame->snapshot;
    struct bambu_rect fill = track;
    double progress = c->frame->displayed_progress;

    bambu_canvas_fill_rounded(c->canvas, track, track.h / 2.0f, c->palette.track);
    if (!s->has_progress)
        return;
    if (progress < 0.0)
        progress = 0.0;
    if (progress > 100.0)
        progress = 100.0;
    fill.w = track.w * (float)(progress / 100.0);
    if (fill.w > 0.0f) {
        /* Keep the rounded cap visible at very low progress. */
        if (fill.w < track.h)
            fill.w = track.h;
        bambu_canvas_fill_rounded(c->canvas, fill, track.h / 2.0f, progress_color(c));
    }
}

static void badge_state(
    const struct draw_context *c,
    const char **label,
    struct bambu_color *color,
    bool *hollow
)
{
    const struct bambu_overlay_strings *strings = c->frame->strings;

    switch (c->frame->visual_state) {
    case BAMBU_VISUAL_LIVE:
        *label = strings->badge_live;
        *color = c->palette.accent;
        break;
    case BAMBU_VISUAL_STALE:
        *label = strings->badge_stale;
        *color = rgb(COLOR_PAUSED, 255);
        *hollow = true;
        break;
    case BAMBU_VISUAL_CONNECTING:
        *label = strings->badge_connecting;
        *color = rgb(COLOR_MUTED, 255);
        *hollow = true;
        break;
    case BAMBU_VISUAL_RECONNECTING:
        *label = strings->badge_reconnecting;
        *color = rgb(COLOR_PAUSED, 255);
        *hollow = true;
        break;
    case BAMBU_VISUAL_OFFLINE:
        *label = strings->badge_offline;
        *color = rgb(COLOR_MUTED, 255);
        *hollow = true;
        break;
    case BAMBU_VISUAL_AUTH_FAILED:
        *label = strings->badge_auth_failed;
        *color = rgb(COLOR_FAILED, 255);
        break;
    case BAMBU_VISUAL_CONFIG_REQUIRED:
        *label = strings->badge_config_required;
        *color = rgb(COLOR_PAUSED, 255);
        break;
    case BAMBU_VISUAL_ERROR:
    default:
        *label = strings->badge_error;
        *color = rgb(COLOR_FAILED, 255);
        break;
    }
}

static void draw_badge(struct draw_context *c, struct bambu_rect rect)
{
    const char *label;
    char text[64];
    struct bambu_color color;
    struct bambu_rect pill = rect;
    float u = c->unit;
    float dot = 11.0f * u;
    float left;
    bool hollow = false;

    badge_state(c, &label, &color, &hollow);

    if (c->frame->visual_state == BAMBU_VISUAL_STALE && c->frame->stale_seconds > 0)
        snprintf(text, sizeof(text), "%s %ds", label, c->frame->stale_seconds);
    else
        snprintf(text, sizeof(text), "%s", label);

    /* The pill hugs its text and stays pinned to the right edge. */
    if (c->text != NULL && c->text->measure != NULL) {
        float measured = c->text->measure(c->text->context, text, BAMBU_TEXT_MIN * c->font, true);
        float width = measured + dot + 22.0f * u;
        if (width < rect.w) {
            pill.x = rect.x + rect.w - width;
            pill.w = width;
        }
    }
    bambu_canvas_fill_rounded(c->canvas, pill, pill.h / 2.0f, c->palette.chip);
    left = pill.x + 9.0f * u;
    {
        /* State is shown by shape as well as colour: filled when live, ring when not. */
        struct bambu_rect mark = make_rect(left, pill.y + (pill.h - dot) / 2.0f, dot, dot);
        if (hollow)
            bambu_canvas_stroke_rounded(c->canvas, mark, dot / 2.0f, 2.0f * u, color);
        else
            bambu_canvas_fill_rounded(c->canvas, mark, dot / 2.0f, color);
    }
    draw_text(
        c,
        text,
        make_rect(left + dot + 5.0f * u, pill.y, pill.w - dot - 16.0f * u, pill.h),
        BAMBU_TEXT_MIN,
        true,
        BAMBU_ALIGN_LEFT,
        c->palette.text
    );
}

/* A neutral layered-print glyph, drawn procedurally so no artwork is shipped. */
static void draw_placeholder(struct draw_context *c, struct bambu_rect rect, float radius)
{
    struct bambu_color layer = c->palette.dim;
    float side = fminf(rect.w, rect.h);
    float bar_h = side * 0.07f;
    float center = rect.x + rect.w / 2.0f;
    float y = rect.y + rect.h * 0.70f;

    bambu_canvas_fill_rounded(c->canvas, rect, radius, c->palette.chip);
    layer.a = 150;
    for (int i = 0; i < 4; i++) {
        float width = side * (0.54f - 0.09f * (float)i);
        bambu_canvas_fill_rounded(
            c->canvas,
            make_rect(center - width / 2.0f, y - bar_h, width, bar_h),
            bar_h / 2.0f,
            layer
        );
        y -= bar_h * 1.45f;
    }
    bambu_canvas_fill_triangle(
        c->canvas,
        center - side * 0.09f,
        y - side * 0.13f,
        center + side * 0.09f,
        y - side * 0.13f,
        center,
        y - bar_h * 0.1f,
        layer
    );
}

static void draw_preview(struct draw_context *c, struct bambu_rect rect)
{
    float radius = 10.0f * c->unit;

    if (c->frame->preview != NULL) {
        bambu_canvas_fill_rounded(c->canvas, rect, radius, c->palette.chip);
        bambu_canvas_draw_image(c->canvas, c->frame->preview, rect, radius);
    } else {
        draw_placeholder(c, rect, radius);
    }
}

static void draw_metric(
    struct draw_context *c,
    struct bambu_rect cell,
    const char *label,
    const char *value
)
{
    float label_h = fminf(24.0f * c->unit, cell.h * 0.42f);

    draw_text(c, label, make_rect(cell.x, cell.y, cell.w, label_h), BAMBU_TEXT_MIN, false, BAMBU_ALIGN_LEFT, c->palette.dim);
    draw_text(
        c,
        value,
        make_rect(cell.x, cell.y + label_h, cell.w, cell.h - label_h),
        22.0f,
        true,
        BAMBU_ALIGN_LEFT,
        c->palette.text
    );
}

static void draw_metrics(struct draw_context *c, struct bambu_rect rect)
{
    const struct bambu_overlay_style *style = c->style;
    const struct bambu_printer_snapshot *s = c->frame->snapshot;
    const struct bambu_overlay_strings *strings = c->frame->strings;
    const char *missing = strings->missing_value;
    char nozzle[32];
    char bed[32];
    char fan[32];
    float cell_w = rect.w / 2.0f;
    float cell_h = rect.h / 2.0f;

    snprintf(nozzle, sizeof(nozzle), "%s", missing);
    snprintf(bed, sizeof(bed), "%s", missing);
    snprintf(fan, sizeof(fan), "%s", missing);
    if (s->has_nozzle_temperature)
        bambu_format_temperature(
            s->nozzle_temperature_c, style->temperature_unit, style->temperature_decimals, nozzle, sizeof(nozzle)
        );
    if (s->has_bed_temperature)
        bambu_format_temperature(
            s->bed_temperature_c, style->temperature_unit, style->temperature_decimals, bed, sizeof(bed)
        );
    if (s->has_fan)
        bambu_format_percent(s->fan_percent, fan, sizeof(fan));

    if (style->show_temperatures) {
        draw_metric(c, make_rect(rect.x, rect.y, cell_w, cell_h), strings->label_nozzle, nozzle);
        draw_metric(c, make_rect(rect.x + cell_w, rect.y, cell_w, cell_h), strings->label_bed, bed);
    }
    if (style->show_fan)
        draw_metric(c, make_rect(rect.x, rect.y + cell_h, cell_w, cell_h), strings->label_fan, fan);
    if (style->show_speed)
        draw_metric(
            c,
            make_rect(rect.x + cell_w, rect.y + cell_h, cell_w, cell_h),
            strings->label_speed,
            s->has_speed ? s->speed_name : missing
        );
}

/* Chips lose words, never size: text in a chip stays at the floor or goes. */
enum tray_form {
    BAMBU_TRAY_FULL = 0,
    BAMBU_TRAY_SHORT,
    BAMBU_TRAY_SWATCH,
};

struct tray_view {
    const struct bambu_ams_tray *tray;
    int unit_id;
    int tray_index;
    bool active;
};

static int collect_trays(const struct draw_context *c, struct tray_view *views, int capacity)
{
    const struct bambu_printer_snapshot *s = c->frame->snapshot;
    int first = 0;
    int last = s->ams_unit_count;
    int count = 0;

    if (s->ams_unit_count == 0)
        return 0;
    if (c->style->ams_mode == BAMBU_AMS_FIRST) {
        last = 1;
    } else if (c->style->ams_mode == BAMBU_AMS_ACTIVE) {
        last = 1;
        if (s->has_active_tray) {
            for (int i = 0; i < s->ams_unit_count; i++) {
                if (s->ams[i].unit_id == s->active_unit_id) {
                    first = i;
                    last = i + 1;
                }
            }
        }
    }

    for (int unit = first; unit < last; unit++) {
        for (int tray = 0; tray < BAMBU_TRAYS_PER_UNIT && count < capacity; tray++) {
            views[count].tray = &s->ams[unit].trays[tray];
            views[count].unit_id = s->ams[unit].unit_id;
            views[count].tray_index = tray;
            views[count].active =
                s->has_active_tray && s->active_unit_id == s->ams[unit].unit_id && s->active_tray_index == tray;
            count++;
        }
    }
    return count;
}

/* The maker prefix costs width and tells a viewer nothing. */
static const char *material_label(const char *name)
{
    return strncmp(name, "Bambu ", 6) == 0 ? name + 6 : name;
}

/* "Bambu PLA Basic" -> "PLA", for chips with room for one short word. */
static void material_family(const char *name, char *out, size_t capacity)
{
    const char *label = material_label(name);
    size_t length;

    if (strncmp(label, "Generic ", 8) == 0)
        label += 8;
    length = strcspn(label, " ");
    if (length >= capacity)
        length = capacity - 1;
    memcpy(out, label, length);
    out[length] = '\0';
}

static void draw_swatch(struct draw_context *c, struct bambu_rect mark, const struct tray_view *view, bool loaded)
{
    float u = c->unit;
    struct bambu_color outline = c->palette.dim;

    if (loaded && view->tray->has_color) {
        /* The outline keeps black and white spools visible on any panel colour. */
        outline.a = 170;
        bambu_canvas_fill_rounded(c->canvas, mark, 6.0f * u, rgb(view->tray->color_rgb, 255));
        bambu_canvas_stroke_rounded(c->canvas, mark, 6.0f * u, 1.5f * u, outline);
    } else {
        bambu_canvas_stroke_rounded(c->canvas, mark, 6.0f * u, 2.0f * u, outline);
    }
}

static void draw_tray(struct draw_context *c, struct bambu_rect chip, const struct tray_view *view, enum tray_form form)
{
    const struct bambu_overlay_strings *strings = c->frame->strings;
    float u = c->unit;
    bool loaded = view->tray->present && view->tray->loaded;
    struct bambu_color name_color = loaded ? c->palette.text : c->palette.dim;

    bambu_canvas_fill_rounded(c->canvas, chip, 8.0f * u, c->palette.chip);

    if (form == BAMBU_TRAY_FULL) {
        float swatch = fminf(chip.h - 14.0f * u, 30.0f * u);
        float left = chip.x + 8.0f * u;
        float text_x = left + swatch + 8.0f * u;
        float text_w = chip.x + chip.w - text_x - 4.0f * u;
        float row = chip.h / 2.0f;
        char label[32];

        draw_swatch(c, make_rect(left, chip.y + (chip.h - swatch) / 2.0f, swatch, swatch), view, loaded);
        snprintf(label, sizeof(label), "A%d \xC2\xB7 T%d", view->unit_id + 1, view->tray_index + 1);
        draw_text(
            c,
            loaded ? material_label(view->tray->material_name) : strings->tray_empty,
            make_rect(text_x, chip.y + 2.0f * u, text_w, row),
            BAMBU_TEXT_MIN,
            false,
            BAMBU_ALIGN_LEFT,
            name_color
        );
        draw_text(
            c,
            label,
            make_rect(text_x, chip.y + row - 1.0f * u, text_w, row),
            BAMBU_TEXT_MIN,
            false,
            BAMBU_ALIGN_LEFT,
            c->palette.dim
        );
    } else if (form == BAMBU_TRAY_SHORT) {
        float bar_h = chip.h * 0.36f;
        char family[16];

        draw_swatch(c, make_rect(chip.x + 6.0f * u, chip.y + 6.0f * u, chip.w - 12.0f * u, bar_h), view, loaded);
        material_family(loaded ? view->tray->material_name : strings->tray_empty, family, sizeof(family));
        draw_text(
            c,
            family,
            make_rect(chip.x + 1.0f * u, chip.y + 6.0f * u + bar_h, chip.w - 2.0f * u, chip.h - bar_h - 7.0f * u),
            BAMBU_TEXT_MIN,
            false,
            BAMBU_ALIGN_CENTER,
            name_color
        );
    } else {
        draw_swatch(c, make_rect(chip.x + 3.0f * u, chip.y + 3.0f * u, chip.w - 6.0f * u, chip.h - 6.0f * u), view, loaded);
    }

    if (view->active) {
        /* Active is shown by outline and a pointer as well as by colour. */
        float tip = 8.0f * u;
        bambu_canvas_stroke_rounded(c->canvas, chip, 8.0f * u, 3.0f * u, c->palette.accent);
        if (form != BAMBU_TRAY_SWATCH)
            bambu_canvas_fill_triangle(
                c->canvas,
                chip.x + chip.w / 2.0f - tip,
                chip.y,
                chip.x + chip.w / 2.0f + tip,
                chip.y,
                chip.x + chip.w / 2.0f,
                chip.y + tip,
                c->palette.accent
            );
    }
}

static void draw_ams(struct draw_context *c, struct bambu_rect rect)
{
    struct tray_view views[BAMBU_MAX_AMS_UNITS * BAMBU_TRAYS_PER_UNIT];
    const struct bambu_printer_snapshot *s = c->frame->snapshot;
    const struct bambu_overlay_strings *strings = c->frame->strings;
    int count = collect_trays(c, views, (int)(sizeof(views) / sizeof(views[0])));
    float u = c->unit;
    float gap = 8.0f * u;
    int columns;
    int rows;
    float chip_w;
    float chip_h;
    enum tray_form form;

    if (count == 0) {
        bambu_canvas_fill_rounded(c->canvas, rect, 8.0f * u, c->palette.chip);
        draw_text(
            c,
            s->external_spool_active ? strings->external_spool : strings->no_ams,
            make_rect(rect.x + 12.0f * u, rect.y, rect.w - 24.0f * u, rect.h),
            BAMBU_TEXT_MIN,
            false,
            BAMBU_ALIGN_LEFT,
            c->palette.dim
        );
        return;
    }

    /* Four trays per row, unit-aligned, until the rail runs out of height; then
     * eight per row. No tray is dropped, chips just carry fewer words. */
    columns = count < BAMBU_TRAYS_PER_UNIT ? count : BAMBU_TRAYS_PER_UNIT;
    rows = (count + columns - 1) / columns;
    if (rows * 52.0f * u > rect.h) {
        columns = BAMBU_TRAYS_PER_UNIT * 2;
        rows = (count + columns - 1) / columns;
    }
    chip_w = (rect.w - gap * (float)(columns - 1)) / (float)columns;
    chip_h = (rect.h - gap * (float)(rows - 1)) / (float)rows;
    if (chip_h > 56.0f * u)
        chip_h = 56.0f * u;
    if (chip_h < 46.0f * u)
        form = BAMBU_TRAY_SWATCH;
    else if (chip_w < 130.0f * u)
        form = BAMBU_TRAY_SHORT;
    else
        form = BAMBU_TRAY_FULL;

    for (int i = 0; i < count; i++) {
        struct bambu_rect chip = make_rect(
            rect.x + (chip_w + gap) * (float)(i % columns),
            rect.y + (chip_h + gap) * (float)(i / columns),
            chip_w,
            chip_h
        );
        draw_tray(c, chip, &views[i], form);
    }
}

static void draw_totals(struct draw_context *c, struct bambu_rect rect)
{
    const struct bambu_printer_snapshot *s = c->frame->snapshot;
    const struct bambu_overlay_strings *strings = c->frame->strings;
    char time_text[32];
    char weight_text[32];
    float half = rect.w / 2.0f;

    snprintf(time_text, sizeof(time_text), "%s", strings->missing_value);
    snprintf(weight_text, sizeof(weight_text), "%s", strings->missing_value);
    if (s->has_total_time)
        bambu_format_duration_seconds(s->total_time_seconds, time_text, sizeof(time_text));
    if (s->has_total_weight)
        bambu_format_weight(s->total_weight_grams, weight_text, sizeof(weight_text));

    if (c->style->show_total_time)
        draw_metric(c, make_rect(rect.x, rect.y, half, rect.h), strings->label_total_time, time_text);
    if (c->style->show_total_weight)
        draw_metric(c, make_rect(rect.x + half, rect.y, half, rect.h), strings->label_total_weight, weight_text);
}

static void eta_text(const struct draw_context *c, char *out, size_t capacity)
{
    const struct bambu_printer_snapshot *s = c->frame->snapshot;
    const struct bambu_overlay_strings *strings = c->frame->strings;
    char remaining[32];
    char clock[32];

    if (!s->has_remaining) {
        snprintf(out, capacity, "%s", strings->missing_value);
        return;
    }
    bambu_format_remaining(s->remaining_minutes, remaining, sizeof(remaining));
    /* A clock ETA from frozen data would be a guess, so only live data shows one. */
    if (c->style->show_clock_eta && data_is_current(c->frame)) {
        bambu_format_eta(
            c->frame->clock_hour,
            c->frame->clock_minute,
            s->remaining_minutes,
            c->style->use_24_hour,
            clock,
            sizeof(clock)
        );
        snprintf(out, capacity, "%s \xC2\xB7 %s %s", remaining, strings->label_eta, clock);
    } else {
        snprintf(out, capacity, "%s", remaining);
    }
}

static void subtitle_text(const struct draw_context *c, char *out, size_t capacity)
{
    const struct bambu_printer_snapshot *s = c->frame->snapshot;
    const char *state = job_state_text(c->frame->strings, s->job_state);
    const char *profile = s->print_profile;
    const char *stage = c->style->show_stage ? s->stage : "";
    bool distinct_profile = profile[0] != '\0' && strcmp(profile, s->job_name) != 0;

    if (distinct_profile && stage[0] != '\0')
        snprintf(out, capacity, "%s \xC2\xB7 %s", stage, profile);
    else if (distinct_profile)
        snprintf(out, capacity, "%s", profile);
    else if (stage[0] != '\0' && strcmp(stage, state) != 0)
        snprintf(out, capacity, "%s \xC2\xB7 %s", state, stage);
    else
        snprintf(out, capacity, "%s", state);
}

static void draw_simulated_tag(struct draw_context *c)
{
    const struct bambu_overlay_geometry *g = c->geometry;
    float u = c->unit;
    struct bambu_rect tag;

    /* The minimal tier says so in its one text row instead. */
    if (!c->frame->simulated || g->minimal)
        return;
    if (g->has_preview)
        tag = make_rect(g->preview.x + 6.0f * u, g->preview.y + 6.0f * u, 122.0f * u, 26.0f * u);
    else
        tag = make_rect(g->panel.w - 134.0f * u, g->panel.h - 32.0f * u, 126.0f * u, 26.0f * u);
    bambu_canvas_fill_rounded(c->canvas, tag, 6.0f * u, rgb(0x000000, 170));
    draw_text(c, c->frame->strings->simulated, tag, BAMBU_TEXT_MIN, true, BAMBU_ALIGN_CENTER, rgb(0xFFFFFF, 255));
}

static void draw_compact(struct draw_context *c)
{
    const struct bambu_overlay_geometry *g = c->geometry;
    const struct bambu_printer_snapshot *s = c->frame->snapshot;
    const struct bambu_overlay_strings *strings = c->frame->strings;
    char percent[16];
    char eta[96];
    const char *job = s->job_name[0] != '\0' ? s->job_name : strings->no_job;

    if (s->has_progress)
        bambu_format_percent(c->frame->displayed_progress, percent, sizeof(percent));
    else
        snprintf(percent, sizeof(percent), "%s", strings->missing_value);
    eta_text(c, eta, sizeof(eta));

    draw_text(c, percent, g->percent, 36.0f, true, BAMBU_ALIGN_LEFT, c->palette.text);
    draw_track(c, g->track);

    if (g->minimal) {
        /* No badge here, so anything but normal running takes the one text row. */
        const char *label = job;
        struct bambu_color color;
        bool hollow = false;

        if (c->frame->simulated)
            label = strings->simulated;
        else if (c->frame->visual_state != BAMBU_VISUAL_LIVE)
            badge_state(c, &label, &color, &hollow);
        draw_text(c, label, g->job, 32.0f, true, BAMBU_ALIGN_LEFT, c->palette.text);
        return;
    }

    draw_text(c, job, g->job, 20.0f, true, BAMBU_ALIGN_LEFT, c->palette.text);
    draw_text(c, eta, g->details, BAMBU_TEXT_MIN, false, BAMBU_ALIGN_LEFT, c->palette.dim);
}

static void draw_panel(struct draw_context *c)
{
    const struct bambu_overlay_geometry *g = c->geometry;
    const struct bambu_printer_snapshot *s = c->frame->snapshot;
    const struct bambu_overlay_strings *strings = c->frame->strings;
    const char *job = s->job_name[0] != '\0' ? s->job_name : strings->no_job;
    char percent[16];
    char eta[96];
    char subtitle[BAMBU_PROFILE_NAME_CAPACITY + BAMBU_STAGE_CAPACITY + 8];
    char layers[48];
    char layer_value[32];

    if (g->has_preview)
        draw_preview(c, g->preview);

    if (s->has_progress)
        bambu_format_percent(c->frame->displayed_progress, percent, sizeof(percent));
    else
        snprintf(percent, sizeof(percent), "%s", strings->missing_value);
    eta_text(c, eta, sizeof(eta));
    subtitle_text(c, subtitle, sizeof(subtitle));

    draw_text(c, job, g->job, 26.0f, true, BAMBU_ALIGN_LEFT, c->palette.text);
    draw_text(c, subtitle, g->subtitle, BAMBU_TEXT_MIN, false, BAMBU_ALIGN_LEFT, c->palette.dim);
    draw_text(c, percent, g->percent, 40.0f, true, BAMBU_ALIGN_LEFT, c->palette.text);
    draw_text(c, eta, g->eta, BAMBU_TEXT_MIN, false, BAMBU_ALIGN_RIGHT, c->palette.dim);
    draw_track(c, g->track);

    if (c->style->show_layers) {
        if (s->has_layer)
            bambu_format_layers(s->current_layer, s->total_layers, layer_value, sizeof(layer_value));
        else
            snprintf(layer_value, sizeof(layer_value), "%s", strings->missing_value);
        snprintf(layers, sizeof(layers), "%s %s", strings->label_layer, layer_value);
        draw_text(c, layers, g->details, BAMBU_TEXT_MIN, false, BAMBU_ALIGN_LEFT, c->palette.dim);
    }

    if (g->has_metrics)
        draw_metrics(c, g->metrics);
    if (g->has_totals)
        draw_totals(c, g->totals);
    if (g->has_ams)
        draw_ams(c, g->ams);
}

void bambu_overlay_render(
    struct bambu_canvas *canvas,
    const struct bambu_text_backend *text,
    const struct bambu_overlay_style *style,
    const struct bambu_overlay_frame *frame
)
{
    struct bambu_overlay_geometry geometry;
    struct draw_context context;
    float radius;

    if (canvas == NULL || style == NULL || frame == NULL || frame->snapshot == NULL || frame->strings == NULL)
        return;

    bambu_overlay_layout(style, &geometry);
    memset(&context, 0, sizeof(context));
    context.canvas = canvas;
    context.text = text;
    context.style = style;
    context.frame = frame;
    context.geometry = &geometry;
    context.unit = geometry.unit;
    context.font = geometry.unit * (style->font_scale > 0.0f ? style->font_scale : 1.0f);
    build_palette(style, &context.palette);

    bambu_canvas_clear(canvas);
    radius = style->corner_radius * geometry.unit;
    bambu_canvas_fill_rounded(canvas, geometry.panel, radius, context.palette.panel);
    bambu_canvas_stroke_rounded(canvas, geometry.panel, radius, 1.0f, context.palette.edge);

    if (geometry.compact)
        draw_compact(&context);
    else
        draw_panel(&context);

    if (geometry.has_badge)
        draw_badge(&context, geometry.badge);
    draw_simulated_tag(&context);
}
