/* OBS source adapter.
 *
 * Threading: a worker thread per source owns the printer service reference and
 * composes frames on the CPU. video_tick only signals it, and video_render only
 * uploads a finished frame when one is waiting, so neither ever touches the
 * network, the disk, or GDI. Destroy joins the worker before freeing anything. */

#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/bmem.h>

#include "bambu-log.h"
#include "bambu-settings.h"
#include "bambu-simulation.h"
#include "overlay-render.h"
#include "printer-service.h"
#include "text-gdi-win.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>

#include <stdio.h>
#include <string.h>
#include <time.h>

#define SOURCE_ID "bambu_printer_overlay"

/* Setting keys are part of saved scene collections and must not change. */
#define SETTING_PROFILE "profile_id"
#define SETTING_PRESET "layout_preset"
#define SETTING_APPLIED_PRESET "applied_preset"
#define SETTING_SIMULATION "simulation_mode"
#define SETTING_FIXTURE "simulation_fixture"
#define SETTING_WIDTH "source_width"
#define SETTING_HEIGHT "source_height"
#define SETTING_DENSITY "density"
#define SETTING_AMS_MODE "ams_mode"
#define SETTING_THEME "theme"
#define SETTING_BACKGROUND "background_color"
#define SETTING_ACCENT "accent_color"
#define SETTING_TEXT "text_color"
#define SETTING_OPACITY "panel_opacity"
#define SETTING_RADIUS "corner_radius"
#define SETTING_PADDING "padding"
#define SETTING_FONT_SCALE "font_scale"
#define SETTING_REDUCE_MOTION "reduce_motion"
#define SETTING_TEMP_UNIT "temperature_unit"
#define SETTING_TEMP_DECIMALS "temperature_decimals"
#define SETTING_TIME_FORMAT "time_format"
#define SETTING_CLOCK_ETA "show_clock_eta"
#define SETTING_STALE_SECONDS "stale_seconds"
#define SETTING_SHOW_PREVIEW "show_preview"
#define SETTING_SHOW_AMS "show_ams"
#define SETTING_SHOW_STAGE "show_stage"
#define SETTING_SHOW_LAYERS "show_layers"
#define SETTING_SHOW_TEMPS "show_temperatures"
#define SETTING_SHOW_FAN "show_fan"
#define SETTING_SHOW_SPEED "show_speed"
#define SETTING_SHOW_TOTAL_TIME "show_total_time"
#define SETTING_SHOW_TOTAL_WEIGHT "show_total_weight"
#define SETTING_SHOW_BADGE "show_badge"

#define DEFAULT_ACCENT_ABGR 0xFF42AE00u
#define DEFAULT_PANEL_ABGR 0xFF1A1612u
#define DEFAULT_TEXT_ABGR 0xFFF8F6F3u
#define DEFAULT_STALE_SECONDS 30
#define WORKER_HEARTBEAT_SECONDS 1.0f

struct render_key {
    uint64_t revision;
    uint64_t preview_revision;
    uint64_t style_generation;
    uint64_t simulation_second;
    int visual_state;
    int stale_seconds;
    int clock_hour;
    int clock_minute;
    int progress_tenths;
};

struct overlay_source {
    obs_source_t *source;

    HANDLE thread;
    HANDLE wake;
    volatile LONG stop;
    volatile LONG animating;
    volatile LONG reconnect_requested;
    volatile LONG visual_state;
    volatile LONG width;
    volatile LONG height;

    /* Guards the settings copy and the front buffer. */
    CRITICAL_SECTION lock;
    struct bambu_overlay_style style;
    uint64_t style_generation;
    char profile_id[BAMBU_PROFILE_ID_CAPACITY];
    bool simulation;
    enum bambu_simulation_fixture fixture;
    bool reduce_motion;
    uint64_t stale_after_ms;
    struct bambu_overlay_strings strings;

    uint8_t *front;
    size_t front_capacity;
    uint32_t front_width;
    uint32_t front_height;
    bool front_dirty;

    /* Graphics thread only. */
    gs_texture_t *texture;
    uint32_t texture_width;
    uint32_t texture_height;
    float tick_seconds;
};

static const char *bambu_overlay_get_name(void *unused)
{
    UNUSED_PARAMETER(unused);
    return obs_module_text("BambuOverlay.SourceName");
}

static uint32_t abgr_to_rgb(uint32_t abgr)
{
    return ((abgr & 0xFFu) << 16) | (abgr & 0xFF00u) | ((abgr >> 16) & 0xFFu);
}

static enum bambu_overlay_preset preset_from_name(const char *name)
{
    if (name != NULL && strcmp(name, "compact") == 0)
        return BAMBU_PRESET_COMPACT;
    if (name != NULL && strcmp(name, "expanded") == 0)
        return BAMBU_PRESET_EXPANDED;
    return BAMBU_PRESET_DASHBOARD;
}

static void bambu_overlay_defaults(obs_data_t *settings)
{
    int width;
    int height;

    bambu_overlay_preset_size(BAMBU_PRESET_DASHBOARD, &width, &height);
    obs_data_set_default_string(settings, SETTING_PROFILE, "");
    obs_data_set_default_string(settings, SETTING_PRESET, "dashboard");
    obs_data_set_default_bool(settings, SETTING_SIMULATION, false);
    obs_data_set_default_int(settings, SETTING_FIXTURE, BAMBU_SIM_CYCLE);
    obs_data_set_default_int(settings, SETTING_WIDTH, width);
    obs_data_set_default_int(settings, SETTING_HEIGHT, height);
    obs_data_set_default_double(settings, SETTING_DENSITY, 1.0);
    obs_data_set_default_int(settings, SETTING_AMS_MODE, BAMBU_AMS_ACTIVE);
    obs_data_set_default_int(settings, SETTING_THEME, BAMBU_THEME_DARK);
    obs_data_set_default_int(settings, SETTING_BACKGROUND, DEFAULT_PANEL_ABGR);
    obs_data_set_default_int(settings, SETTING_ACCENT, DEFAULT_ACCENT_ABGR);
    obs_data_set_default_int(settings, SETTING_TEXT, DEFAULT_TEXT_ABGR);
    obs_data_set_default_double(settings, SETTING_OPACITY, 94.0);
    obs_data_set_default_int(settings, SETTING_RADIUS, 14);
    obs_data_set_default_int(settings, SETTING_PADDING, 16);
    obs_data_set_default_double(settings, SETTING_FONT_SCALE, 1.0);
    obs_data_set_default_bool(settings, SETTING_REDUCE_MOTION, false);
    obs_data_set_default_int(settings, SETTING_TEMP_UNIT, BAMBU_UNIT_CELSIUS);
    obs_data_set_default_int(settings, SETTING_TEMP_DECIMALS, 0);
    obs_data_set_default_int(settings, SETTING_TIME_FORMAT, 24);
    obs_data_set_default_bool(settings, SETTING_CLOCK_ETA, true);
    obs_data_set_default_int(settings, SETTING_STALE_SECONDS, DEFAULT_STALE_SECONDS);
    obs_data_set_default_bool(settings, SETTING_SHOW_PREVIEW, true);
    obs_data_set_default_bool(settings, SETTING_SHOW_AMS, true);
    obs_data_set_default_bool(settings, SETTING_SHOW_STAGE, true);
    obs_data_set_default_bool(settings, SETTING_SHOW_LAYERS, true);
    obs_data_set_default_bool(settings, SETTING_SHOW_TEMPS, true);
    obs_data_set_default_bool(settings, SETTING_SHOW_FAN, true);
    obs_data_set_default_bool(settings, SETTING_SHOW_SPEED, true);
    obs_data_set_default_bool(settings, SETTING_SHOW_TOTAL_TIME, true);
    obs_data_set_default_bool(settings, SETTING_SHOW_TOTAL_WEIGHT, true);
    obs_data_set_default_bool(settings, SETTING_SHOW_BADGE, true);
}

static int clamp_int(long long value, int minimum, int maximum)
{
    if (value < minimum)
        return minimum;
    if (value > maximum)
        return maximum;
    return (int)value;
}

static void read_style(obs_data_t *settings, struct bambu_overlay_style *style)
{
    bambu_overlay_default_style(style);
    style->preset = preset_from_name(obs_data_get_string(settings, SETTING_PRESET));
    style->theme = (enum bambu_overlay_theme)clamp_int(obs_data_get_int(settings, SETTING_THEME), 0, BAMBU_THEME_CUSTOM);
    style->ams_mode = (enum bambu_ams_mode)clamp_int(obs_data_get_int(settings, SETTING_AMS_MODE), 0, BAMBU_AMS_ALL);
    style->width = clamp_int(obs_data_get_int(settings, SETTING_WIDTH), BAMBU_MIN_WIDTH, BAMBU_MAX_WIDTH);
    style->height = clamp_int(obs_data_get_int(settings, SETTING_HEIGHT), BAMBU_MIN_HEIGHT, BAMBU_MAX_HEIGHT);
    style->density = (float)obs_data_get_double(settings, SETTING_DENSITY);
    style->font_scale = (float)obs_data_get_double(settings, SETTING_FONT_SCALE);
    style->corner_radius = (float)obs_data_get_int(settings, SETTING_RADIUS);
    style->padding = (float)obs_data_get_int(settings, SETTING_PADDING);
    style->panel_opacity = (float)(obs_data_get_double(settings, SETTING_OPACITY) / 100.0);
    style->accent_rgb = abgr_to_rgb((uint32_t)obs_data_get_int(settings, SETTING_ACCENT));
    style->panel_rgb = abgr_to_rgb((uint32_t)obs_data_get_int(settings, SETTING_BACKGROUND));
    style->text_rgb = abgr_to_rgb((uint32_t)obs_data_get_int(settings, SETTING_TEXT));
    style->show_preview = obs_data_get_bool(settings, SETTING_SHOW_PREVIEW);
    style->show_ams = obs_data_get_bool(settings, SETTING_SHOW_AMS);
    style->show_stage = obs_data_get_bool(settings, SETTING_SHOW_STAGE);
    style->show_layers = obs_data_get_bool(settings, SETTING_SHOW_LAYERS);
    style->show_temperatures = obs_data_get_bool(settings, SETTING_SHOW_TEMPS);
    style->show_fan = obs_data_get_bool(settings, SETTING_SHOW_FAN);
    style->show_speed = obs_data_get_bool(settings, SETTING_SHOW_SPEED);
    style->show_total_time = obs_data_get_bool(settings, SETTING_SHOW_TOTAL_TIME);
    style->show_total_weight = obs_data_get_bool(settings, SETTING_SHOW_TOTAL_WEIGHT);
    style->show_badge = obs_data_get_bool(settings, SETTING_SHOW_BADGE);
    style->temperature_unit =
        obs_data_get_int(settings, SETTING_TEMP_UNIT) == BAMBU_UNIT_FAHRENHEIT ? BAMBU_UNIT_FAHRENHEIT
                                                                               : BAMBU_UNIT_CELSIUS;
    style->temperature_decimals = clamp_int(obs_data_get_int(settings, SETTING_TEMP_DECIMALS), 0, 2);
    style->use_24_hour = obs_data_get_int(settings, SETTING_TIME_FORMAT) != 12;
    style->show_clock_eta = obs_data_get_bool(settings, SETTING_CLOCK_ETA);
}

static void bambu_overlay_update(void *data, obs_data_t *settings)
{
    struct overlay_source *context = data;
    struct bambu_overlay_style style;

    if (context == NULL || settings == NULL)
        return;
    read_style(settings, &style);

    EnterCriticalSection(&context->lock);
    context->style = style;
    context->style_generation++;
    bambu_copy_text(context->profile_id, sizeof(context->profile_id), obs_data_get_string(settings, SETTING_PROFILE));
    context->simulation = obs_data_get_bool(settings, SETTING_SIMULATION);
    context->fixture = (enum bambu_simulation_fixture)clamp_int(
        obs_data_get_int(settings, SETTING_FIXTURE), 0, BAMBU_SIM_FIXTURE_COUNT - 1
    );
    context->reduce_motion = obs_data_get_bool(settings, SETTING_REDUCE_MOTION);
    context->stale_after_ms = (uint64_t)clamp_int(obs_data_get_int(settings, SETTING_STALE_SECONDS), 5, 600) * 1000u;
    LeaveCriticalSection(&context->lock);

    InterlockedExchange(&context->width, style.width);
    InterlockedExchange(&context->height, style.height);
    SetEvent(context->wake);
}

static void load_strings(struct bambu_overlay_strings *s)
{
    s->badge_live = obs_module_text("BambuOverlay.Badge.Live");
    s->badge_stale = obs_module_text("BambuOverlay.Badge.Stale");
    s->badge_connecting = obs_module_text("BambuOverlay.Badge.Connecting");
    s->badge_reconnecting = obs_module_text("BambuOverlay.Badge.Reconnecting");
    s->badge_offline = obs_module_text("BambuOverlay.Badge.Offline");
    s->badge_auth_failed = obs_module_text("BambuOverlay.Badge.AuthFailed");
    s->badge_config_required = obs_module_text("BambuOverlay.Badge.ConfigRequired");
    s->badge_error = obs_module_text("BambuOverlay.Badge.Error");
    s->simulated = obs_module_text("BambuOverlay.Simulated");
    s->no_job = obs_module_text("BambuOverlay.NoJob");
    s->job_idle = obs_module_text("BambuOverlay.Job.Idle");
    s->job_preparing = obs_module_text("BambuOverlay.Job.Preparing");
    s->job_printing = obs_module_text("BambuOverlay.Job.Printing");
    s->job_paused = obs_module_text("BambuOverlay.Job.Paused");
    s->job_finished = obs_module_text("BambuOverlay.Job.Finished");
    s->job_failed = obs_module_text("BambuOverlay.Job.Failed");
    s->label_nozzle = obs_module_text("BambuOverlay.Label.Nozzle");
    s->label_bed = obs_module_text("BambuOverlay.Label.Bed");
    s->label_fan = obs_module_text("BambuOverlay.Label.Fan");
    s->label_speed = obs_module_text("BambuOverlay.Label.Speed");
    s->label_layer = obs_module_text("BambuOverlay.Label.Layer");
    s->label_eta = obs_module_text("BambuOverlay.Label.Eta");
    s->label_remaining = obs_module_text("BambuOverlay.Label.Remaining");
    s->label_total_time = obs_module_text("BambuOverlay.Label.TotalTime");
    s->label_total_weight = obs_module_text("BambuOverlay.Label.TotalWeight");
    s->tray_empty = obs_module_text("BambuOverlay.Tray.Empty");
    s->no_ams = obs_module_text("BambuOverlay.Tray.None");
    s->external_spool = obs_module_text("BambuOverlay.Tray.External");
    s->missing_value = "\xE2\x80\x94";
}

struct worker_state {
    struct bambu_service *service;
    char service_profile[BAMBU_PROFILE_ID_CAPACITY];
    struct bambu_gdi_text *gdi;
    struct bambu_text_backend text;
    struct bambu_canvas canvas;
    bool canvas_ready;
    uint8_t *back;
    size_t back_capacity;
    struct bambu_shared_image *preview;
    uint64_t preview_revision;
    double displayed_progress;
    bool progress_known;
    uint64_t last_frame_ms;
    uint64_t simulation_start_ms;
    struct render_key last_key;
    bool has_frame;
    struct bambu_printer_snapshot snapshot;
};

/* Only this thread touches the service, so a last-reference join can never stall
 * the UI thread that changed the setting. */
static void sync_service(struct worker_state *worker, const char *profile_id, bool simulation)
{
    const char *wanted = simulation ? "" : profile_id;

    if (strcmp(worker->service_profile, wanted) == 0)
        return;
    bambu_image_release(worker->preview);
    worker->preview = NULL;
    worker->preview_revision = 0;
    bambu_service_release(worker->service);
    worker->service = bambu_service_acquire(wanted);
    bambu_copy_text(worker->service_profile, sizeof(worker->service_profile), wanted);
}

static void refresh_preview(struct worker_state *worker)
{
    if (worker->service == NULL || worker->snapshot.preview_revision == worker->preview_revision)
        return;
    worker->preview_revision = worker->snapshot.preview_revision;
    bambu_image_release(worker->preview);
    worker->preview = bambu_service_preview(worker->service);
}

static bool ensure_buffers(struct worker_state *worker, int width, int height)
{
    size_t needed = (size_t)width * (size_t)height * 4;

    if (!worker->canvas_ready || worker->canvas.width != width || worker->canvas.height != height) {
        if (worker->canvas_ready)
            bambu_canvas_free(&worker->canvas);
        worker->canvas_ready = bambu_canvas_init(&worker->canvas, width, height);
        if (!worker->canvas_ready)
            return false;
    }
    if (worker->back == NULL || worker->back_capacity < needed) {
        bfree(worker->back);
        worker->back = bmalloc(needed);
        worker->back_capacity = needed;
    }
    return true;
}

static void render_frame(struct overlay_source *context, struct worker_state *worker)
{
    struct bambu_overlay_style style;
    struct bambu_overlay_strings strings;
    struct bambu_overlay_frame frame;
    struct render_key key;
    char profile_id[BAMBU_PROFILE_ID_CAPACITY];
    uint64_t generation;
    uint64_t stale_after_ms;
    uint64_t now = bambu_monotonic_ms();
    uint64_t simulation_second = 0;
    enum bambu_simulation_fixture fixture;
    bool simulation;
    bool reduce_motion;
    double elapsed;
    double target;
    time_t wall;
    struct tm local;

    EnterCriticalSection(&context->lock);
    style = context->style;
    strings = context->strings;
    generation = context->style_generation;
    memcpy(profile_id, context->profile_id, sizeof(profile_id));
    simulation = context->simulation;
    fixture = context->fixture;
    reduce_motion = context->reduce_motion;
    stale_after_ms = context->stale_after_ms;
    LeaveCriticalSection(&context->lock);

    sync_service(worker, profile_id, simulation);
    if (InterlockedExchange(&context->reconnect_requested, 0) != 0)
        bambu_service_reconnect(worker->service);

    if (simulation) {
        uint64_t elapsed_seconds = (now - worker->simulation_start_ms) / 1000u + 1;
        bambu_simulation_build(fixture, elapsed_seconds, now, &worker->snapshot);
        /* Only these fixtures change over time; the rest must settle like live data. */
        if (fixture == BAMBU_SIM_CYCLE || fixture == BAMBU_SIM_PRINTING)
            simulation_second = elapsed_seconds;
    } else if (worker->service != NULL) {
        bambu_service_read(worker->service, &worker->snapshot);
        refresh_preview(worker);
    } else {
        bambu_snapshot_init(&worker->snapshot);
    }

    memset(&frame, 0, sizeof(frame));
    frame.snapshot = &worker->snapshot;
    frame.strings = &strings;
    frame.preview = simulation ? NULL : bambu_image_pixels(worker->preview);
    frame.simulated = simulation;
    frame.visual_state = bambu_overlay_visual_state(&worker->snapshot, now, stale_after_ms);
    InterlockedExchange(&context->visual_state, (LONG)frame.visual_state);
    if (frame.visual_state == BAMBU_VISUAL_STALE && now > worker->snapshot.updated_monotonic_ms)
        frame.stale_seconds = (int)((now - worker->snapshot.updated_monotonic_ms) / 1000u);

    /* Progress eases toward the real value; a decrease means a new job, and
     * animating backwards would show motion the printer never reported. */
    target = worker->snapshot.has_progress ? worker->snapshot.progress_percent : 0.0;
    elapsed = worker->last_frame_ms != 0 ? (double)(now - worker->last_frame_ms) / 1000.0 : 0.0;
    worker->last_frame_ms = now;
    if (!worker->progress_known || target < worker->displayed_progress) {
        worker->displayed_progress = target;
        worker->progress_known = worker->snapshot.has_progress;
    } else {
        worker->displayed_progress =
            bambu_overlay_animate_progress(worker->displayed_progress, target, elapsed, reduce_motion);
    }
    frame.displayed_progress = worker->displayed_progress;
    InterlockedExchange(&context->animating, worker->displayed_progress != target ? 1 : 0);

    wall = time(NULL);
    if (localtime_s(&local, &wall) == 0) {
        frame.clock_hour = local.tm_hour;
        frame.clock_minute = local.tm_min;
    }

    memset(&key, 0, sizeof(key));
    key.revision = worker->snapshot.revision;
    key.preview_revision = worker->snapshot.preview_revision;
    key.style_generation = generation;
    key.simulation_second = simulation_second;
    key.visual_state = (int)frame.visual_state;
    key.stale_seconds = frame.stale_seconds;
    key.clock_hour = frame.clock_hour;
    key.clock_minute = frame.clock_minute;
    key.progress_tenths = (int)(worker->displayed_progress * 10.0);

    /* The common case: nothing visible changed, so no work happens at all. */
    if (worker->has_frame && memcmp(&key, &worker->last_key, sizeof(key)) == 0)
        return;
    if (!ensure_buffers(worker, style.width, style.height))
        return;

    bambu_overlay_render(&worker->canvas, &worker->text, &style, &frame);
    bambu_canvas_encode(&worker->canvas, worker->back);

    /* Exchange buffers; ensure_buffers() regrows the old front when it is reused. */
    EnterCriticalSection(&context->lock);
    {
        uint8_t *old_front = context->front;
        size_t old_capacity = context->front_capacity;

        context->front = worker->back;
        context->front_capacity = worker->back_capacity;
        context->front_width = (uint32_t)style.width;
        context->front_height = (uint32_t)style.height;
        context->front_dirty = true;
        worker->back = old_front;
        worker->back_capacity = old_capacity;
    }
    LeaveCriticalSection(&context->lock);

    worker->last_key = key;
    worker->has_frame = true;
}

static unsigned __stdcall worker_main(void *argument)
{
    struct overlay_source *context = argument;
    struct worker_state *worker = bzalloc(sizeof(*worker));

    worker->gdi = bambu_gdi_text_create();
    if (worker->gdi != NULL)
        bambu_gdi_text_backend(worker->gdi, &worker->text);
    worker->simulation_start_ms = bambu_monotonic_ms();

    while (context->stop == 0) {
        WaitForSingleObject(context->wake, INFINITE);
        if (context->stop != 0)
            break;
        /* A hidden source keeps its connection but composes nothing. */
        if (worker->has_frame && !obs_source_showing(context->source) && context->reconnect_requested == 0) {
            InterlockedExchange(&context->animating, 0);
            continue;
        }
        render_frame(context, worker);
    }

    /* Released here, so destroy's join means no worker holds a printer reference. */
    bambu_image_release(worker->preview);
    bambu_service_release(worker->service);
    if (worker->canvas_ready)
        bambu_canvas_free(&worker->canvas);
    bambu_gdi_text_destroy(worker->gdi);
    bfree(worker->back);
    bfree(worker);
    return 0;
}

static void *bambu_overlay_create(obs_data_t *settings, obs_source_t *source)
{
    struct overlay_source *context = bzalloc(sizeof(*context));

    context->source = source;
    InitializeCriticalSection(&context->lock);
    context->wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    context->stale_after_ms = DEFAULT_STALE_SECONDS * 1000u;
    load_strings(&context->strings);
    bambu_overlay_update(context, settings);

    context->thread = (HANDLE)_beginthreadex(NULL, 0, worker_main, context, 0, NULL);
    if (context->thread == NULL)
        blog(LOG_ERROR, "[bambu-printer-overlay] could not start the render worker");
    SetEvent(context->wake);
    return context;
}

static void bambu_overlay_destroy(void *data)
{
    struct overlay_source *context = data;

    if (context == NULL)
        return;
    InterlockedExchange(&context->stop, 1);
    SetEvent(context->wake);
    if (context->thread != NULL) {
        WaitForSingleObject(context->thread, INFINITE);
        CloseHandle(context->thread);
    }

    obs_enter_graphics();
    if (context->texture != NULL)
        gs_texture_destroy(context->texture);
    obs_leave_graphics();

    CloseHandle(context->wake);
    DeleteCriticalSection(&context->lock);
    bfree(context->front);
    bfree(context);
}

static uint32_t bambu_overlay_width(void *data)
{
    const struct overlay_source *context = data;
    return context != NULL ? (uint32_t)context->width : 0;
}

static uint32_t bambu_overlay_height(void *data)
{
    const struct overlay_source *context = data;
    return context != NULL ? (uint32_t)context->height : 0;
}

static void bambu_overlay_tick(void *data, float seconds)
{
    struct overlay_source *context = data;

    /* Runs on the graphics thread, so it does nothing beyond deciding whether to
     * wake the worker. When settled that is about once a second. */
    context->tick_seconds += seconds;
    if (context->animating != 0 || context->tick_seconds >= WORKER_HEARTBEAT_SECONDS) {
        context->tick_seconds = 0.0f;
        SetEvent(context->wake);
    }
}

static void upload_frame(struct overlay_source *context)
{
    EnterCriticalSection(&context->lock);
    if (context->front_dirty && context->front != NULL) {
        if (context->texture == NULL || context->texture_width != context->front_width ||
            context->texture_height != context->front_height) {
            if (context->texture != NULL)
                gs_texture_destroy(context->texture);
            context->texture = gs_texture_create(
                context->front_width, context->front_height, GS_BGRA, 1, NULL, GS_DYNAMIC
            );
            context->texture_width = context->front_width;
            context->texture_height = context->front_height;
        }
        if (context->texture != NULL)
            gs_texture_set_image(context->texture, context->front, context->front_width * 4, false);
        context->front_dirty = false;
    }
    LeaveCriticalSection(&context->lock);
}

static void overlay_source_render(void *data, gs_effect_t *unused_effect)
{
    struct overlay_source *context = data;
    gs_effect_t *effect;
    gs_eparam_t *image;
    bool previous_srgb;

    UNUSED_PARAMETER(unused_effect);
    if (context == NULL)
        return;
    upload_frame(context);
    if (context->texture == NULL)
        return;

    effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
    image = gs_effect_get_param_by_name(effect, "image");
    previous_srgb = gs_framebuffer_srgb_enabled();
    gs_enable_framebuffer_srgb(true);
    gs_blend_state_push();
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
    gs_effect_set_texture_srgb(image, context->texture);
    while (gs_effect_loop(effect, "Draw"))
        gs_draw_sprite(context->texture, 0, context->texture_width, context->texture_height);
    gs_blend_state_pop();
    gs_enable_framebuffer_srgb(previous_srgb);
}

static void bambu_overlay_show(void *data)
{
    struct overlay_source *context = data;
    SetEvent(context->wake);
}

/* The dialog does not block, so this cannot wait to refresh the printer list;
 * bambu_overlay_refresh_properties() does that when the dialog closes. */
static bool on_open_settings(obs_properties_t *properties, obs_property_t *property, void *data)
{
    UNUSED_PARAMETER(properties);
    UNUSED_PARAMETER(property);
    UNUSED_PARAMETER(data);
    bambu_settings_open();
    return false;
}

static bool refresh_source_properties(void *data, obs_source_t *source)
{
    UNUSED_PARAMETER(data);
    if (strcmp(obs_source_get_id(source), SOURCE_ID) == 0)
        obs_source_update_properties(source);
    return true;
}

void bambu_overlay_refresh_properties(void)
{
    obs_enum_sources(refresh_source_properties, NULL);
}

static bool on_reconnect(obs_properties_t *properties, obs_property_t *property, void *data)
{
    struct overlay_source *context = data;

    UNUSED_PARAMETER(properties);
    UNUSED_PARAMETER(property);
    if (context != NULL) {
        InterlockedExchange(&context->reconnect_requested, 1);
        SetEvent(context->wake);
    }
    return false;
}

static bool on_export_diagnostics(obs_properties_t *properties, obs_property_t *property, void *data)
{
    UNUSED_PARAMETER(properties);
    UNUSED_PARAMETER(property);
    UNUSED_PARAMETER(data);
    bambu_settings_export_diagnostics();
    return false;
}

/* Choosing a preset applies its natural size once; a width or height the user
 * then types is left alone, including across reloads of this dialog. */
static bool on_preset_modified(obs_properties_t *properties, obs_property_t *property, obs_data_t *settings)
{
    const char *preset = obs_data_get_string(settings, SETTING_PRESET);
    const char *applied = obs_data_get_string(settings, SETTING_APPLIED_PRESET);

    UNUSED_PARAMETER(properties);
    UNUSED_PARAMETER(property);
    if (applied[0] != '\0' && strcmp(applied, preset) != 0) {
        int width;
        int height;
        bambu_overlay_preset_size(preset_from_name(preset), &width, &height);
        obs_data_set_int(settings, SETTING_WIDTH, width);
        obs_data_set_int(settings, SETTING_HEIGHT, height);
    }
    obs_data_set_string(settings, SETTING_APPLIED_PRESET, preset);
    return true;
}

static obs_properties_t *add_group(obs_properties_t *root, const char *name, const char *text_key)
{
    obs_properties_t *group = obs_properties_create();
    obs_properties_add_group(root, name, obs_module_text(text_key), OBS_GROUP_NORMAL, group);
    return group;
}

static void add_connection_group(obs_properties_t *root, const struct overlay_source *context)
{
    static const char *const status_keys[] = {
        "BambuOverlay.Status.Live",           "BambuOverlay.Status.Stale",      "BambuOverlay.Status.Connecting",
        "BambuOverlay.Status.Reconnecting",   "BambuOverlay.Status.Offline",    "BambuOverlay.Status.AuthFailed",
        "BambuOverlay.Status.ConfigRequired", "BambuOverlay.Status.Error",
    };
    obs_properties_t *group = add_group(root, "group_connection", "BambuOverlay.Group.Connection");
    struct bambu_profile_list *profiles = bzalloc(sizeof(*profiles));
    obs_property_t *list = obs_properties_add_list(
        group, SETTING_PROFILE, obs_module_text("BambuOverlay.Profile"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING
    );
    LONG state = context != NULL ? context->visual_state : BAMBU_VISUAL_CONFIG_REQUIRED;

    obs_property_list_add_string(list, obs_module_text("BambuOverlay.Profile.None"), "");
    bambu_profile_store_list(profiles);
    for (int i = 0; i < profiles->count; i++)
        obs_property_list_add_string(list, profiles->profiles[i].name, profiles->profiles[i].id);
    bfree(profiles);

    if (state < 0 || state >= (LONG)(sizeof(status_keys) / sizeof(status_keys[0])))
        state = BAMBU_VISUAL_ERROR;
    obs_properties_add_text(group, "connection_status", obs_module_text(status_keys[state]), OBS_TEXT_INFO);

    obs_properties_add_button(group, "open_settings", obs_module_text("BambuOverlay.OpenSettings"), on_open_settings);
    obs_properties_add_button(group, "reconnect", obs_module_text("BambuOverlay.Reconnect"), on_reconnect);
}

static void add_layout_group(obs_properties_t *root)
{
    obs_properties_t *group = add_group(root, "group_layout", "BambuOverlay.Group.Layout");
    obs_property_t *preset = obs_properties_add_list(
        group, SETTING_PRESET, obs_module_text("BambuOverlay.LayoutPreset"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING
    );
    obs_property_t *ams;

    obs_property_list_add_string(preset, obs_module_text("BambuOverlay.Preset.Compact"), "compact");
    obs_property_list_add_string(preset, obs_module_text("BambuOverlay.Preset.Dashboard"), "dashboard");
    obs_property_list_add_string(preset, obs_module_text("BambuOverlay.Preset.Expanded"), "expanded");
    obs_property_set_modified_callback(preset, on_preset_modified);

    obs_properties_add_int(group, SETTING_WIDTH, obs_module_text("BambuOverlay.Width"), BAMBU_MIN_WIDTH, BAMBU_MAX_WIDTH, 1);
    obs_properties_add_int(group, SETTING_HEIGHT, obs_module_text("BambuOverlay.Height"), BAMBU_MIN_HEIGHT, BAMBU_MAX_HEIGHT, 1);
    obs_properties_add_float_slider(group, SETTING_DENSITY, obs_module_text("BambuOverlay.Density"), 0.6, 1.6, 0.05);

    ams = obs_properties_add_list(
        group, SETTING_AMS_MODE, obs_module_text("BambuOverlay.AmsMode"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT
    );
    obs_property_list_add_int(ams, obs_module_text("BambuOverlay.AmsMode.First"), BAMBU_AMS_FIRST);
    obs_property_list_add_int(ams, obs_module_text("BambuOverlay.AmsMode.Active"), BAMBU_AMS_ACTIVE);
    obs_property_list_add_int(ams, obs_module_text("BambuOverlay.AmsMode.All"), BAMBU_AMS_ALL);

    obs_properties_add_bool(group, SETTING_SHOW_PREVIEW, obs_module_text("BambuOverlay.Show.Preview"));
    obs_properties_add_bool(group, SETTING_SHOW_AMS, obs_module_text("BambuOverlay.Show.Ams"));
    obs_properties_add_bool(group, SETTING_SHOW_STAGE, obs_module_text("BambuOverlay.Show.Stage"));
    obs_properties_add_bool(group, SETTING_SHOW_LAYERS, obs_module_text("BambuOverlay.Show.Layers"));
    obs_properties_add_bool(group, SETTING_SHOW_TEMPS, obs_module_text("BambuOverlay.Show.Temperatures"));
    obs_properties_add_bool(group, SETTING_SHOW_FAN, obs_module_text("BambuOverlay.Show.Fan"));
    obs_properties_add_bool(group, SETTING_SHOW_SPEED, obs_module_text("BambuOverlay.Show.Speed"));
    obs_properties_add_bool(group, SETTING_SHOW_TOTAL_TIME, obs_module_text("BambuOverlay.Show.TotalTime"));
    obs_properties_add_bool(group, SETTING_SHOW_TOTAL_WEIGHT, obs_module_text("BambuOverlay.Show.TotalWeight"));
    obs_properties_add_bool(group, SETTING_SHOW_BADGE, obs_module_text("BambuOverlay.Show.Badge"));
}

static void add_appearance_group(obs_properties_t *root)
{
    obs_properties_t *group = add_group(root, "group_appearance", "BambuOverlay.Group.Appearance");
    obs_property_t *theme = obs_properties_add_list(
        group, SETTING_THEME, obs_module_text("BambuOverlay.Theme"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT
    );

    obs_property_list_add_int(theme, obs_module_text("BambuOverlay.Theme.Dark"), BAMBU_THEME_DARK);
    obs_property_list_add_int(theme, obs_module_text("BambuOverlay.Theme.Light"), BAMBU_THEME_LIGHT);
    obs_property_list_add_int(theme, obs_module_text("BambuOverlay.Theme.HighContrast"), BAMBU_THEME_HIGH_CONTRAST);
    obs_property_list_add_int(theme, obs_module_text("BambuOverlay.Theme.Custom"), BAMBU_THEME_CUSTOM);

    obs_properties_add_color(group, SETTING_ACCENT, obs_module_text("BambuOverlay.AccentColor"));
    obs_properties_add_color(group, SETTING_BACKGROUND, obs_module_text("BambuOverlay.BackgroundColor"));
    obs_properties_add_color(group, SETTING_TEXT, obs_module_text("BambuOverlay.TextColor"));
    obs_properties_add_float_slider(group, SETTING_OPACITY, obs_module_text("BambuOverlay.Opacity"), 30.0, 100.0, 1.0);
    obs_properties_add_int_slider(group, SETTING_RADIUS, obs_module_text("BambuOverlay.Radius"), 0, 40, 1);
    obs_properties_add_int_slider(group, SETTING_PADDING, obs_module_text("BambuOverlay.Padding"), 6, 32, 1);
    obs_properties_add_float_slider(group, SETTING_FONT_SCALE, obs_module_text("BambuOverlay.FontScale"), 0.7, 1.4, 0.05);
    obs_properties_add_bool(group, SETTING_REDUCE_MOTION, obs_module_text("BambuOverlay.ReduceMotion"));
}

static void add_formatting_group(obs_properties_t *root)
{
    obs_properties_t *group = add_group(root, "group_formatting", "BambuOverlay.Group.Formatting");
    obs_property_t *unit = obs_properties_add_list(
        group, SETTING_TEMP_UNIT, obs_module_text("BambuOverlay.TemperatureUnit"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT
    );
    obs_property_t *clock;

    obs_property_list_add_int(unit, obs_module_text("BambuOverlay.Celsius"), BAMBU_UNIT_CELSIUS);
    obs_property_list_add_int(unit, obs_module_text("BambuOverlay.Fahrenheit"), BAMBU_UNIT_FAHRENHEIT);
    obs_properties_add_int(group, SETTING_TEMP_DECIMALS, obs_module_text("BambuOverlay.TemperatureDecimals"), 0, 2, 1);

    clock = obs_properties_add_list(
        group, SETTING_TIME_FORMAT, obs_module_text("BambuOverlay.TimeFormat"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT
    );
    obs_property_list_add_int(clock, obs_module_text("BambuOverlay.TimeFormat.24"), 24);
    obs_property_list_add_int(clock, obs_module_text("BambuOverlay.TimeFormat.12"), 12);
    obs_properties_add_bool(group, SETTING_CLOCK_ETA, obs_module_text("BambuOverlay.ClockEta"));
    obs_properties_add_int(group, SETTING_STALE_SECONDS, obs_module_text("BambuOverlay.StaleSeconds"), 5, 600, 1);
}

static void add_diagnostics_group(obs_properties_t *root)
{
    static const struct {
        const char *key;
        enum bambu_simulation_fixture fixture;
    } fixtures[] = {
        {"BambuOverlay.Fixture.Cycle", BAMBU_SIM_CYCLE},
        {"BambuOverlay.Fixture.Printing", BAMBU_SIM_PRINTING},
        {"BambuOverlay.Fixture.Preparing", BAMBU_SIM_PREPARING},
        {"BambuOverlay.Fixture.Paused", BAMBU_SIM_PAUSED},
        {"BambuOverlay.Fixture.Finished", BAMBU_SIM_FINISHED},
        {"BambuOverlay.Fixture.Failed", BAMBU_SIM_FAILED},
        {"BambuOverlay.Fixture.Idle", BAMBU_SIM_IDLE},
        {"BambuOverlay.Fixture.MultiAms", BAMBU_SIM_MULTI_AMS},
        {"BambuOverlay.Fixture.LongText", BAMBU_SIM_LONG_TEXT},
        {"BambuOverlay.Fixture.Stale", BAMBU_SIM_STALE},
        {"BambuOverlay.Fixture.Reconnecting", BAMBU_SIM_RECONNECTING},
        {"BambuOverlay.Fixture.Offline", BAMBU_SIM_OFFLINE},
        {"BambuOverlay.Fixture.AuthFailed", BAMBU_SIM_AUTH_FAILED},
    };
    obs_properties_t *group = add_group(root, "group_diagnostics", "BambuOverlay.Group.Diagnostics");
    obs_property_t *fixture;
    char version[128];

    obs_properties_add_bool(group, SETTING_SIMULATION, obs_module_text("BambuOverlay.SimulationMode"));
    fixture = obs_properties_add_list(
        group, SETTING_FIXTURE, obs_module_text("BambuOverlay.SimulationFixture"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT
    );
    for (size_t i = 0; i < sizeof(fixtures) / sizeof(fixtures[0]); i++)
        obs_property_list_add_int(fixture, obs_module_text(fixtures[i].key), fixtures[i].fixture);

    obs_properties_add_button(
        group, "export_diagnostics", obs_module_text("BambuOverlay.ExportDiagnostics"), on_export_diagnostics
    );
    snprintf(version, sizeof(version), "%s %s", obs_module_text("BambuOverlay.PluginVersion"), BAMBU_OVERLAY_VERSION);
    obs_properties_add_text(group, "plugin_version", version, OBS_TEXT_INFO);
}

static obs_properties_t *bambu_overlay_properties(void *data)
{
    obs_properties_t *properties = obs_properties_create();

    /* data is NULL when OBS asks for properties of the type, not an instance. */
    add_connection_group(properties, data);
    add_layout_group(properties);
    add_appearance_group(properties);
    add_formatting_group(properties);
    add_diagnostics_group(properties);
    return properties;
}

struct obs_source_info bambu_overlay_source_info = {
    .id = SOURCE_ID,
    .type = OBS_SOURCE_TYPE_INPUT,
    .output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB,
    .icon_type = OBS_ICON_TYPE_TEXT,
    .get_name = bambu_overlay_get_name,
    .create = bambu_overlay_create,
    .destroy = bambu_overlay_destroy,
    .update = bambu_overlay_update,
    .get_defaults = bambu_overlay_defaults,
    .get_properties = bambu_overlay_properties,
    .get_width = bambu_overlay_width,
    .get_height = bambu_overlay_height,
    .video_tick = bambu_overlay_tick,
    .video_render = overlay_source_render,
    .show = bambu_overlay_show,
};
