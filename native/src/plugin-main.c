#include <obs-module.h>
#include <util/bmem.h>
#include <util/platform.h>

#include "bambu-log.h"
#include "bambu-settings.h"
#include "printer-service.h"

#include <stdio.h>
#include <string.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("bambu-printer-overlay", "en-US")

MODULE_EXPORT const char *obs_module_name(void)
{
    return "Bambu Printer Overlay";
}

MODULE_EXPORT const char *obs_module_description(void)
{
    return "Native Bambu printer telemetry overlay source";
}

extern struct obs_source_info bambu_overlay_source_info;

static char cache_directory[1024] = "";

static void log_to_obs(enum bambu_log_level level, const char *message)
{
    static const int levels[] = {LOG_ERROR, LOG_WARNING, LOG_INFO, LOG_DEBUG};
    blog(levels[level], "[bambu-printer-overlay] %s", message);
}

static bool load_service_config(const char *profile_id, struct bambu_service_config *config)
{
    if (!bambu_profile_store_get(profile_id, &config->profile))
        return false;
    bambu_credential_read(profile_id, BAMBU_SECRET_ACCESS_CODE, config->access_code, sizeof(config->access_code));
    bambu_credential_read(profile_id, BAMBU_SECRET_CLOUD_TOKEN, config->cloud_token, sizeof(config->cloud_token));
    snprintf(config->cache_dir, sizeof(config->cache_dir), "%s", cache_directory);
    return true;
}

/* Mutable state lives in the per-user OBS config tree, never beside the plugin. */
static void prepare_storage(void)
{
    char *profiles_path = obs_module_config_path("profiles.json");
    char *cache_path = obs_module_config_path("cache");

    if (cache_path != NULL && os_mkdirs(cache_path) != MKDIR_ERROR)
        snprintf(cache_directory, sizeof(cache_directory), "%s", cache_path);
    bambu_profile_store_init(profiles_path != NULL ? profiles_path : "");
    bfree(profiles_path);
    bfree(cache_path);
}

bool obs_module_load(void)
{
    bambu_log_set_sink(log_to_obs);
    prepare_storage();
    bambu_services_init(load_service_config);
    obs_register_source(&bambu_overlay_source_info);
    bambu_settings_register();
    blog(LOG_INFO, "[bambu-printer-overlay] module loaded (version %s)", BAMBU_OVERLAY_VERSION);
    return true;
}

void obs_module_unload(void)
{
    bambu_settings_shutdown();
    bambu_services_shutdown();
    bambu_profile_store_shutdown();
    blog(LOG_INFO, "[bambu-printer-overlay] module unloaded");
    bambu_log_set_sink(NULL);
}
