#include <obs-module.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("bambu-printer-overlay", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
    return "Native Bambu printer telemetry overlay source";
}

extern struct obs_source_info bambu_overlay_source_info;

bool obs_module_load(void)
{
    obs_register_source(&bambu_overlay_source_info);
    blog(LOG_INFO, "[bambu-printer-overlay] module loaded");
    return true;
}

void obs_module_unload(void)
{
    blog(LOG_INFO, "[bambu-printer-overlay] module unloaded");
}
