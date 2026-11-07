#pragma once

/* Composes a printer snapshot into a canvas. Layout, visual state, and motion
 * are pure functions so they can be tested without OBS or a GPU. */

#include "bambu-format.h"
#include "overlay-canvas.h"
#include "printer-snapshot.h"

enum bambu_overlay_preset {
    BAMBU_PRESET_COMPACT = 0,
    BAMBU_PRESET_DASHBOARD,
    BAMBU_PRESET_EXPANDED,
};

enum bambu_overlay_theme {
    BAMBU_THEME_DARK = 0,
    BAMBU_THEME_LIGHT,
    BAMBU_THEME_HIGH_CONTRAST,
    BAMBU_THEME_CUSTOM,
};

enum bambu_ams_mode {
    BAMBU_AMS_FIRST = 0,
    BAMBU_AMS_ACTIVE,
    BAMBU_AMS_ALL,
};

enum bambu_visual_state {
    BAMBU_VISUAL_LIVE = 0,
    BAMBU_VISUAL_STALE,
    BAMBU_VISUAL_CONNECTING,
    BAMBU_VISUAL_RECONNECTING,
    BAMBU_VISUAL_OFFLINE,
    BAMBU_VISUAL_AUTH_FAILED,
    BAMBU_VISUAL_CONFIG_REQUIRED,
    BAMBU_VISUAL_ERROR,
};

#define BAMBU_MIN_WIDTH 320
#define BAMBU_MIN_HEIGHT 72
#define BAMBU_MAX_WIDTH 3840
#define BAMBU_MAX_HEIGHT 2160

/* Overlays are read through 720p scaling and stream compression, where text
 * under about 12 px turns to smudges. Nothing is drawn smaller than this at
 * preset size on a 1080p canvas: 18 px there is 12 px on a 720p stream. */
#define BAMBU_TEXT_MIN 18.0f
/* A source below this fraction of its preset size steps down a tier rather than
 * shrinking further, so text stays at or above BAMBU_TEXT_MIN * this fraction. */
#define BAMBU_READABLE_FIT 0.85f

struct bambu_overlay_style {
    enum bambu_overlay_preset preset;
    enum bambu_overlay_theme theme;
    enum bambu_ams_mode ams_mode;
    int width;
    int height;
    float density;
    float font_scale;
    float corner_radius;
    float padding;
    float panel_opacity;
    uint32_t accent_rgb;
    uint32_t panel_rgb;
    uint32_t text_rgb;
    bool show_preview;
    bool show_ams;
    bool show_stage;
    bool show_layers;
    bool show_temperatures;
    bool show_fan;
    bool show_speed;
    bool show_total_time;
    bool show_total_weight;
    bool show_badge;
    enum bambu_temperature_unit temperature_unit;
    int temperature_decimals;
    bool use_24_hour;
    bool show_clock_eta;
};

/* Localised text. bambu_overlay_default_strings() gives English. */
struct bambu_overlay_strings {
    const char *badge_live;
    const char *badge_stale;
    const char *badge_connecting;
    const char *badge_reconnecting;
    const char *badge_offline;
    const char *badge_auth_failed;
    const char *badge_config_required;
    const char *badge_error;
    const char *simulated;
    const char *no_job;
    const char *job_idle;
    const char *job_preparing;
    const char *job_printing;
    const char *job_paused;
    const char *job_finished;
    const char *job_failed;
    const char *label_nozzle;
    const char *label_bed;
    const char *label_fan;
    const char *label_speed;
    const char *label_layer;
    const char *label_eta;
    const char *label_remaining;
    const char *label_total_time;
    const char *label_total_weight;
    const char *tray_empty;
    const char *no_ams;
    const char *external_spool;
    const char *missing_value;
};

struct bambu_overlay_frame {
    const struct bambu_printer_snapshot *snapshot;
    const struct bambu_overlay_strings *strings;
    const struct bambu_image *preview; /* NULL shows the placeholder */
    enum bambu_visual_state visual_state;
    double displayed_progress; /* animated; equals the real value when settled */
    int stale_seconds;
    bool simulated;
    int clock_hour;
    int clock_minute;
};

/* The text backend keeps rendering portable: GDI in production, a deterministic
 * block rasteriser in tests. */
enum bambu_text_align {
    BAMBU_ALIGN_LEFT = 0,
    BAMBU_ALIGN_CENTER,
    BAMBU_ALIGN_RIGHT,
};

struct bambu_text_backend {
    void *context;
    /* Rasterises one line into a width * height coverage mask, ellipsising text
     * that does not fit. The mask is zeroed by the caller. */
    void (*rasterize)(
        void *context,
        const char *utf8,
        float size_px,
        bool bold,
        enum bambu_text_align align,
        uint8_t *mask,
        int width,
        int height
    );
    float (*measure)(void *context, const char *utf8, float size_px, bool bold);
};

struct bambu_overlay_geometry {
    float unit;
    struct bambu_rect panel;
    struct bambu_rect preview;
    struct bambu_rect job;
    struct bambu_rect subtitle;
    struct bambu_rect percent;
    struct bambu_rect eta;
    struct bambu_rect track;
    struct bambu_rect details;
    struct bambu_rect metrics;
    struct bambu_rect totals;
    struct bambu_rect ams;
    struct bambu_rect badge;
    bool has_preview;
    bool has_metrics;
    bool has_ams;
    bool has_totals;
    bool has_badge;
    bool compact; /* the compact layout, chosen or stepped down to */
    bool minimal; /* compact with only percent, one text row, and the track */
};

void bambu_overlay_default_style(struct bambu_overlay_style *style);
const struct bambu_overlay_strings *bambu_overlay_default_strings(void);
void bambu_overlay_preset_size(enum bambu_overlay_preset preset, int *width, int *height);

/* Geometry depends only on style, so optional data never shifts the layout. */
void bambu_overlay_layout(const struct bambu_overlay_style *style, struct bambu_overlay_geometry *geometry);

enum bambu_visual_state bambu_overlay_visual_state(
    const struct bambu_printer_snapshot *snapshot,
    uint64_t now_monotonic_ms,
    uint64_t stale_after_ms
);

/* Advances displayed toward target, at most 'elapsed' seconds of a short ease.
 * Moves monotonically and never overshoots; reduced motion jumps immediately. */
double bambu_overlay_animate_progress(double displayed, double target, double elapsed_seconds, bool reduce_motion);

void bambu_overlay_render(
    struct bambu_canvas *canvas,
    const struct bambu_text_backend *text,
    const struct bambu_overlay_style *style,
    const struct bambu_overlay_frame *frame
);
