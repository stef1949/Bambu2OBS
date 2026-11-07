#include <obs-module.h>
#include <graphics/vec4.h>
#include <util/bmem.h>

#include "printer-snapshot.h"

#define SOURCE_ID "bambu_printer_overlay"
#define SETTING_PROFILE "profile_id"
#define SETTING_PREVIEW_NOTICE "preview_notice"
#define SETTING_PLUGIN_VERSION "plugin_version"
#define SETTING_PRESET "layout_preset"
#define SETTING_SIMULATION "simulation_mode"
#define SETTING_PROGRESS "simulation_progress"
#define SETTING_WIDTH "source_width"
#define SETTING_HEIGHT "source_height"
#define SETTING_BACKGROUND "background_color"
#define SETTING_ACCENT "accent_color"

struct bambu_overlay_source {
    obs_source_t *source;
    struct bambu_printer_snapshot snapshot;
    uint32_t width;
    uint32_t height;
    uint32_t background_color;
    uint32_t accent_color;
    bool simulation_mode;
};

static const char *bambu_overlay_get_name(void *unused)
{
    UNUSED_PARAMETER(unused);
    return obs_module_text("BambuOverlay.SourceName");
}

static void bambu_overlay_defaults(obs_data_t *settings)
{
    obs_data_set_default_string(settings, SETTING_PROFILE, "");
    obs_data_set_default_string(settings, SETTING_PRESET, "dashboard");
    obs_data_set_default_bool(settings, SETTING_SIMULATION, true);
    obs_data_set_default_double(settings, SETTING_PROGRESS, 42.0);
    obs_data_set_default_int(settings, SETTING_WIDTH, 760);
    obs_data_set_default_int(settings, SETTING_HEIGHT, 180);
    obs_data_set_default_int(settings, SETTING_BACKGROUND, 0xe61f1b18);
    obs_data_set_default_int(settings, SETTING_ACCENT, 0xff42ae00);
}

static void bambu_overlay_update(void *data, obs_data_t *settings)
{
    struct bambu_overlay_source *context = data;
    struct bambu_printer_snapshot_update update = {0};

    if (context == NULL || settings == NULL)
        return;

    context->width = (uint32_t)obs_data_get_int(settings, SETTING_WIDTH);
    context->height = (uint32_t)obs_data_get_int(settings, SETTING_HEIGHT);
    context->background_color = (uint32_t)obs_data_get_int(settings, SETTING_BACKGROUND);
    context->accent_color = (uint32_t)obs_data_get_int(settings, SETTING_ACCENT);
    context->simulation_mode = obs_data_get_bool(settings, SETTING_SIMULATION);

    if (context->width < 320)
        context->width = 320;
    if (context->height < 96)
        context->height = 96;

    if (context->simulation_mode) {
        update.has_connection_state = true;
        update.connection_state = BAMBU_CONNECTION_LIVE;
        update.has_progress = true;
        update.progress_percent = obs_data_get_double(settings, SETTING_PROGRESS);
        update.has_job_name = true;
        update.job_name = "Simulation fixture";
        update.has_stage = true;
        update.stage = "printing";
        bambu_snapshot_apply(&context->snapshot, &update);
    }
}

static void *bambu_overlay_create(obs_data_t *settings, obs_source_t *source)
{
    struct bambu_overlay_source *context = bzalloc(sizeof(*context));
    context->source = source;
    bambu_snapshot_init(&context->snapshot);
    bambu_overlay_update(context, settings);
    blog(LOG_INFO, "[bambu-printer-overlay] source created");
    return context;
}

static void bambu_overlay_destroy(void *data)
{
    struct bambu_overlay_source *context = data;
    if (context == NULL)
        return;
    blog(LOG_INFO, "[bambu-printer-overlay] source destroyed");
    bfree(context);
}

static uint32_t bambu_overlay_width(void *data)
{
    const struct bambu_overlay_source *context = data;
    return context != NULL ? context->width : 760;
}

static uint32_t bambu_overlay_height(void *data)
{
    const struct bambu_overlay_source *context = data;
    return context != NULL ? context->height : 180;
}

static void draw_solid_rectangle(float x, float y, float width, float height, uint32_t color)
{
    gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_SOLID);
    gs_eparam_t *color_parameter = gs_effect_get_param_by_name(effect, "color");
    struct vec4 color_value;

    vec4_from_rgba(&color_value, color);
    gs_effect_set_vec4(color_parameter, &color_value);

    gs_matrix_push();
    gs_matrix_translate3f(x, y, 0.0f);
    while (gs_effect_loop(effect, "Solid")) {
        gs_render_start(true);
        gs_vertex2f(0.0f, 0.0f);
        gs_vertex2f(0.0f, height);
        gs_vertex2f(width, 0.0f);
        gs_vertex2f(width, height);
        gs_render_stop(GS_TRISTRIP);
    }
    gs_matrix_pop();
}

static void bambu_overlay_render(void *data, gs_effect_t *unused_effect)
{
    const struct bambu_overlay_source *context = data;
    float padding;
    float track_y;
    float track_width;
    float progress_width;

    UNUSED_PARAMETER(unused_effect);
    if (context == NULL)
        return;

    padding = context->height * 0.12f;
    track_y = context->height * 0.68f;
    track_width = (float)context->width - (padding * 2.0f);
    progress_width = track_width * (float)(context->snapshot.progress_percent / 100.0);

    gs_blend_state_push();
    gs_reset_blend_state();
    draw_solid_rectangle(0.0f, 0.0f, (float)context->width, (float)context->height, context->background_color);
    draw_solid_rectangle(padding, track_y, track_width, context->height * 0.14f, 0x66ffffff);
    draw_solid_rectangle(padding, track_y, progress_width, context->height * 0.14f, context->accent_color);
    gs_blend_state_pop();
}

static obs_properties_t *bambu_overlay_properties(void *data)
{
    obs_properties_t *properties = obs_properties_create();
    obs_property_t *notice;
    obs_property_t *profile;
    obs_property_t *preset;
    obs_property_t *version;

    UNUSED_PARAMETER(data);
    notice = obs_properties_add_text(
        properties,
        SETTING_PREVIEW_NOTICE,
        obs_module_text("BambuOverlay.PreviewNotice"),
        OBS_TEXT_INFO
    );
    obs_property_text_set_info_type(notice, OBS_TEXT_INFO_WARNING);
    obs_property_text_set_info_word_wrap(notice, true);

    profile = obs_properties_add_text(
        properties,
        SETTING_PROFILE,
        obs_module_text("BambuOverlay.Profile"),
        OBS_TEXT_DEFAULT
    );
    obs_property_set_enabled(profile, false);
    preset = obs_properties_add_list(
        properties,
        SETTING_PRESET,
        obs_module_text("BambuOverlay.LayoutPreset"),
        OBS_COMBO_TYPE_LIST,
        OBS_COMBO_FORMAT_STRING
    );
    obs_property_list_add_string(preset, obs_module_text("BambuOverlay.Preset.Compact"), "compact");
    obs_property_list_add_string(preset, obs_module_text("BambuOverlay.Preset.Dashboard"), "dashboard");
    obs_property_list_add_string(preset, obs_module_text("BambuOverlay.Preset.Expanded"), "expanded");
    obs_properties_add_bool(properties, SETTING_SIMULATION, obs_module_text("BambuOverlay.SimulationMode"));
    obs_properties_add_float_slider(
        properties,
        SETTING_PROGRESS,
        obs_module_text("BambuOverlay.SimulationProgress"),
        0.0,
        100.0,
        1.0
    );
    obs_properties_add_int(properties, SETTING_WIDTH, obs_module_text("BambuOverlay.Width"), 320, 3840, 1);
    obs_properties_add_int(properties, SETTING_HEIGHT, obs_module_text("BambuOverlay.Height"), 96, 2160, 1);
    obs_properties_add_color_alpha(
        properties,
        SETTING_BACKGROUND,
        obs_module_text("BambuOverlay.BackgroundColor")
    );
    obs_properties_add_color_alpha(properties, SETTING_ACCENT, obs_module_text("BambuOverlay.AccentColor"));
    version = obs_properties_add_text(
        properties,
        SETTING_PLUGIN_VERSION,
        obs_module_text("BambuOverlay.PluginVersion"),
        OBS_TEXT_INFO
    );
    obs_property_set_long_description(version, BAMBU_OVERLAY_VERSION);
    return properties;
}

struct obs_source_info bambu_overlay_source_info = {
    .id = SOURCE_ID,
    .type = OBS_SOURCE_TYPE_INPUT,
    .output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB,
    .get_name = bambu_overlay_get_name,
    .create = bambu_overlay_create,
    .destroy = bambu_overlay_destroy,
    .update = bambu_overlay_update,
    .get_defaults = bambu_overlay_defaults,
    .get_properties = bambu_overlay_properties,
    .get_width = bambu_overlay_width,
    .get_height = bambu_overlay_height,
    .video_render = bambu_overlay_render,
};
