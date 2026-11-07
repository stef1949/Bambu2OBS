#include <obs.h>

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define SOURCE_ID "bambu_printer_overlay"

static bool source_type_is_registered(void)
{
    const char *source_id = NULL;
    size_t index = 0;

    while (obs_enum_input_types(index++, &source_id)) {
        if (source_id != NULL && strcmp(source_id, SOURCE_ID) == 0)
            return true;
    }
    return false;
}

int main(int argc, char **argv)
{
    obs_module_t *module = NULL;
    obs_data_t *settings = NULL;
    obs_source_t *source = NULL;
    obs_properties_t *properties = NULL;
    int module_result;
    int result = 1;

    if (argc != 3) {
        fprintf(stderr, "usage: %s <plugin-dll> <plugin-data>\n", argv[0]);
        return 2;
    }
    if (!obs_startup("en-US", NULL, NULL)) {
        fputs("obs_startup failed\n", stderr);
        return 3;
    }

    module_result = obs_open_module(&module, argv[1], argv[2]);
    if (module_result != MODULE_SUCCESS) {
        fprintf(stderr, "obs_open_module failed: %d\n", module_result);
        goto cleanup;
    }
    if (!obs_init_module(module)) {
        fputs("obs_init_module failed\n", stderr);
        goto cleanup;
    }
    if (!source_type_is_registered()) {
        fprintf(stderr, "source type was not registered: %s\n", SOURCE_ID);
        goto cleanup;
    }

    settings = obs_data_create();
    source = obs_source_create(SOURCE_ID, "Bambu overlay load test", settings, NULL);
    if (source == NULL) {
        fputs("obs_source_create failed\n", stderr);
        goto cleanup;
    }
    if (obs_source_get_width(source) != 760 || obs_source_get_height(source) != 180) {
        fprintf(
            stderr,
            "unexpected default dimensions: %ux%u\n",
            obs_source_get_width(source),
            obs_source_get_height(source)
        );
        goto cleanup;
    }

    properties = obs_source_properties(source);
    if (properties == NULL) {
        fputs("obs_source_properties failed\n", stderr);
        goto cleanup;
    }

    result = 0;
    puts("bambu-overlay-plugin-load-tests: module, source, and properties loaded successfully");

cleanup:
    if (properties != NULL)
        obs_properties_destroy(properties);
    if (source != NULL)
        obs_source_release(source);
    if (settings != NULL)
        obs_data_release(settings);
    obs_shutdown();
    return result;
}
