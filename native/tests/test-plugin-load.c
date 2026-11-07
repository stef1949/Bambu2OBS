/* Loads the real plugin into real libobs. With a usable graphics device it also
 * renders through the D3D11 pipeline and reads the pixels back. No OBS user
 * configuration is touched: all module state goes to the temporary directory.
 *
 * Usage: bambu-overlay-plugin-load-tests <plugin-dll> <plugin-data> <libobs-data>
 *            <temp-config-dir> [png-output-directory] [soak-seconds]
 */

#include <obs.h>
#include <graphics/graphics.h>
#include <graphics/vec4.h>

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>
#include <psapi.h>

#include "png-writer.h"

#define SOURCE_ID "bambu_printer_overlay"

static int failures = 0;
static volatile LONG leaked_references = 0;

static void count_leak_warnings(int level, const char *format, va_list arguments, void *data)
{
    char message[1024];

    (void)level;
    (void)data;
    vsnprintf(message, sizeof(message), format, arguments);
    if (strstr(message, "still referenced at shutdown") != NULL)
        InterlockedIncrement(&leaked_references);
    if (level <= LOG_WARNING)
        fprintf(stderr, "%s\n", message);
}

#define EXPECT(condition)                                                     \
    do {                                                                      \
        if (!(condition)) {                                                   \
            fprintf(stderr, "%d: EXPECT failed: %s\n", __LINE__, #condition); \
            failures++;                                                       \
        }                                                                     \
    } while (0)

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

static obs_source_t *make_source(const char *name, obs_data_t *settings)
{
    return obs_source_create(SOURCE_ID, name, settings, NULL);
}

static void test_defaults_and_properties(void)
{
    obs_data_t *settings = obs_data_create();
    obs_source_t *source = make_source("defaults", settings);
    obs_properties_t *properties;
    obs_data_t *defaults;

    EXPECT(source != NULL);
    EXPECT(obs_source_get_width(source) == 960);
    EXPECT(obs_source_get_height(source) == 260);

    properties = obs_source_properties(source);
    EXPECT(properties != NULL);
    EXPECT(obs_properties_get(properties, "profile_id") != NULL);
    EXPECT(obs_properties_get(properties, "layout_preset") != NULL);
    EXPECT(obs_properties_get(properties, "theme") != NULL);
    obs_properties_destroy(properties);

    /* OBS may ask for the properties of the type itself, with a NULL data pointer. */
    properties = obs_get_source_properties(SOURCE_ID);
    EXPECT(properties != NULL);
    obs_properties_destroy(properties);

    defaults = obs_get_source_defaults(SOURCE_ID);
    EXPECT(obs_data_get_bool(defaults, "simulation_mode") == false);
    obs_data_release(defaults);

    obs_source_release(source);
    obs_data_release(settings);
}

/* A scene saved by the 0.2 preview must keep loading, with its size intact. */
static void test_settings_migration(void)
{
    obs_data_t *settings = obs_data_create();
    obs_source_t *source;

    obs_data_set_string(settings, "profile_id", "");
    obs_data_set_string(settings, "layout_preset", "dashboard");
    obs_data_set_bool(settings, "simulation_mode", true);
    obs_data_set_double(settings, "simulation_progress", 42.0);
    obs_data_set_int(settings, "source_width", 760);
    obs_data_set_int(settings, "source_height", 180);
    obs_data_set_int(settings, "background_color", 0xe61f1b18);
    obs_data_set_int(settings, "accent_color", 0xff42ae00);

    source = make_source("legacy", settings);
    EXPECT(source != NULL);
    EXPECT(obs_source_get_width(source) == 760);
    EXPECT(obs_source_get_height(source) == 180);
    obs_source_release(source);
    obs_data_release(settings);
}

static void test_bounds(void)
{
    obs_data_t *settings = obs_data_create();
    obs_source_t *source;

    obs_data_set_int(settings, "source_width", 1);
    obs_data_set_int(settings, "source_height", 100000);
    obs_data_set_int(settings, "theme", 99);
    obs_data_set_int(settings, "ams_mode", -4);
    source = make_source("bounds", settings);
    EXPECT(obs_source_get_width(source) == 320);
    EXPECT(obs_source_get_height(source) == 2160);
    obs_source_release(source);
    obs_data_release(settings);
}

/* A profile that does not exist must fail soft, and must not hang teardown. */
static void test_lifecycle_churn(void)
{
    obs_data_t *settings = obs_data_create();
    ULONGLONG start = GetTickCount64();
    ULONGLONG elapsed;

    obs_data_set_string(settings, "profile_id", "no-such-profile");
    for (int i = 0; i < 40; i++) {
        obs_source_t *first = make_source("churn-a", settings);
        obs_source_t *second = make_source("churn-b", settings);
        obs_source_t *copy = obs_source_duplicate(first, "churn-copy", true);

        obs_source_update(first, settings);
        obs_source_release(copy);
        obs_source_release(second);
        obs_source_release(first);
    }
    elapsed = GetTickCount64() - start;
    printf("lifecycle: 40 create/duplicate/destroy rounds in %llu ms\n", (unsigned long long)elapsed);
    EXPECT(elapsed < 20000);
    obs_data_release(settings);
}

static bool capture(obs_source_t *source, uint32_t width, uint32_t height, uint8_t *bgra)
{
    gs_texrender_t *render;
    gs_stagesurf_t *stage;
    uint8_t *mapped = NULL;
    uint32_t line = 0;
    bool ok = false;

    obs_enter_graphics();
    render = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
    stage = gs_stagesurface_create(width, height, GS_BGRA);
    if (gs_texrender_begin(render, width, height)) {
        struct vec4 clear;
        vec4_zero(&clear);
        gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
        gs_ortho(0.0f, (float)width, 0.0f, (float)height, -100.0f, 100.0f);
        obs_source_video_render(source);
        gs_texrender_end(render);

        gs_stage_texture(stage, gs_texrender_get_texture(render));
        if (gs_stagesurface_map(stage, &mapped, &line)) {
            for (uint32_t y = 0; y < height; y++)
                memcpy(bgra + (size_t)y * width * 4, mapped + (size_t)y * line, (size_t)width * 4);
            gs_stagesurface_unmap(stage);
            ok = true;
        }
    }
    gs_stagesurface_destroy(stage);
    gs_texrender_destroy(render);
    obs_leave_graphics();
    return ok;
}

static void save_preview(const char *directory, const char *name, const uint8_t *bgra, uint32_t width, uint32_t height)
{
    char path[1024];
    size_t count = (size_t)width * height;
    uint8_t *rgba = malloc(count * 4);

    /* Flattened over mid grey, so the output is viewable anywhere. */
    for (size_t i = 0; i < count; i++) {
        const uint8_t *s = bgra + i * 4;
        unsigned inverse = 255u - s[3];
        rgba[i * 4 + 0] = (uint8_t)(s[2] + 128u * inverse / 255u);
        rgba[i * 4 + 1] = (uint8_t)(s[1] + 128u * inverse / 255u);
        rgba[i * 4 + 2] = (uint8_t)(s[0] + 128u * inverse / 255u);
        rgba[i * 4 + 3] = 255;
    }
    snprintf(path, sizeof(path), "%s/%s.png", directory, name);
    png_write_rgba(path, rgba, (int)width, (int)height);
    free(rgba);
    printf("wrote %s\n", path);
}

static bool frame_has_content(const uint8_t *bgra, uint32_t width, uint32_t height)
{
    const uint8_t *center = bgra + ((size_t)(height / 2) * width + width / 2) * 4;
    return center[3] > 0;
}

static void process_stats(double *cpu_seconds, double *working_set_mb, unsigned *threads)
{
    FILETIME created;
    FILETIME exited;
    FILETIME kernel;
    FILETIME user;
    PROCESS_MEMORY_COUNTERS memory;
    ULARGE_INTEGER k;
    ULARGE_INTEGER u;

    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    k.LowPart = kernel.dwLowDateTime;
    k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime;
    u.HighPart = user.dwHighDateTime;
    *cpu_seconds = (double)(k.QuadPart + u.QuadPart) / 1.0e7;
    GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory));
    *working_set_mb = (double)memory.WorkingSetSize / (1024.0 * 1024.0);
    *threads = 0;
    (void)threads;
}

static void measure(const char *label, double seconds)
{
    double cpu_before;
    double cpu_after;
    double memory_before;
    double memory_after;
    unsigned threads;

    process_stats(&cpu_before, &memory_before, &threads);
    Sleep((DWORD)(seconds * 1000.0));
    process_stats(&cpu_after, &memory_after, &threads);
    printf(
        "%s: %.2f%% of one core over %.0fs, working set %.1f -> %.1f MB\n",
        label,
        (cpu_after - cpu_before) / seconds * 100.0,
        seconds,
        memory_before,
        memory_after
    );
}

static void test_gpu_pipeline(const char *png_directory, double soak_seconds)
{
    static const struct {
        const char *name;
        const char *preset;
        int fixture;
        uint32_t width;
        uint32_t height;
    } cases[] = {
        {"gpu-compact", "compact", 1, 640, 104},
        {"gpu-dashboard", "dashboard", 1, 960, 260},
        {"gpu-expanded", "expanded", 7, 960, 400},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        obs_data_t *settings = obs_data_create();
        obs_source_t *source;
        uint32_t width = cases[i].width;
        uint32_t height = cases[i].height;
        uint8_t *pixels = calloc((size_t)width * height, 4);

        obs_data_set_bool(settings, "simulation_mode", true);
        obs_data_set_int(settings, "simulation_fixture", cases[i].fixture);
        obs_data_set_string(settings, "layout_preset", cases[i].preset);
        obs_data_set_int(settings, "source_width", width);
        obs_data_set_int(settings, "source_height", height);
        obs_data_set_int(settings, "ams_mode", 2);
        source = make_source(cases[i].name, settings);
        obs_set_output_source(0, source);

        /* The first frame arrives from the worker, so allow it a moment. */
        for (int attempt = 0; attempt < 100; attempt++) {
            Sleep(30);
            EXPECT(capture(source, width, height, pixels));
            if (frame_has_content(pixels, width, height))
                break;
        }

        EXPECT(frame_has_content(pixels, width, height));
        EXPECT(pixels[3] == 0); /* rounded corner stays transparent */
        EXPECT(pixels[((size_t)height * width - 1) * 4 + 3] == 0);
        if (png_directory != NULL)
            save_preview(png_directory, cases[i].name, pixels, width, height);

        if (i == 1 && soak_seconds > 0.0) {
            /* Baseline first: OBS's own 1080p pipeline runs with or without us. */
            printf("--- measurements, Release build, 1920x1080 at 30 fps ---\n");
            obs_set_output_source(0, NULL);
            Sleep(300);
            measure("baseline, no source on output", soak_seconds);
            obs_set_output_source(0, source);
            obs_data_set_int(settings, "simulation_fixture", 3); /* paused: nothing changes */
            obs_source_update(source, settings);
            Sleep(300);
            measure("settled overlay", soak_seconds);
            obs_data_set_int(settings, "simulation_fixture", 1); /* printing: redraws each second */
            obs_source_update(source, settings);
            Sleep(300);
            measure("changing overlay", soak_seconds);
        }

        /* Live resize through the same pipeline. */
        obs_data_set_int(settings, "source_width", width / 2 + 40);
        obs_data_set_int(settings, "source_height", height / 2 + 40);
        obs_source_update(source, settings);
        /* OBS applies updates to video sources on the next tick. */
        Sleep(250);
        EXPECT(obs_source_get_width(source) == width / 2 + 40);
        EXPECT(capture(source, width / 2 + 40, height / 2 + 40, pixels));

        obs_set_output_source(0, NULL);
        obs_source_release(source);
        obs_data_release(settings);
        free(pixels);
    }
}

/* libobs loads modules with a restricted DLL search that ignores PATH. In OBS the
 * dependencies sit beside the executable; here they sit beside obs.dll. */
static void add_obs_dll_directory(void)
{
    wchar_t path[MAX_PATH];
    wchar_t *slash;

    if (GetModuleFileNameW(GetModuleHandleW(L"obs.dll"), path, MAX_PATH) == 0)
        return;
    slash = wcsrchr(path, L'\\');
    if (slash != NULL) {
        *slash = L'\0';
        AddDllDirectory(path);
    }

    /* Qt sits beside obs.dll in an installed OBS, but not in the SDK. */
    if (GetEnvironmentVariableW(L"BAMBU_TEST_QT_BIN", path, MAX_PATH) > 0)
        AddDllDirectory(path);
}

static bool start_video(void)
{
    struct obs_video_info video;

    memset(&video, 0, sizeof(video));
    video.graphics_module = "libobs-d3d11";
    video.fps_num = 30;
    video.fps_den = 1;
    video.base_width = 1920;
    video.base_height = 1080;
    video.output_width = 1920;
    video.output_height = 1080;
    video.output_format = VIDEO_FORMAT_NV12;
    video.colorspace = VIDEO_CS_709;
    video.range = VIDEO_RANGE_PARTIAL;
    video.adapter = 0;
    video.gpu_conversion = true;
    video.scale_type = OBS_SCALE_BICUBIC;
    return obs_reset_video(&video) == OBS_VIDEO_SUCCESS;
}

int main(int argc, char **argv)
{
    obs_module_t *module = NULL;
    const char *png_directory;
    double soak_seconds;
    int module_result;
    bool has_video;
    ULONGLONG shutdown_start;

    if (argc < 5) {
        fprintf(stderr, "usage: %s <plugin> <plugin-data> <libobs-data> <config-dir> [png-dir] [soak-seconds]\n", argv[0]);
        return 2;
    }
    png_directory = argc > 5 && argv[5][0] != '\0' ? argv[5] : NULL;
    soak_seconds = argc > 6 ? atof(argv[6]) : 0.0;

    base_set_log_handler(count_leak_warnings, NULL);
    add_obs_dll_directory();
    if (!obs_startup("en-US", argv[4], NULL)) {
        fputs("obs_startup failed\n", stderr);
        return 3;
    }
    printf("libobs %s\n", obs_get_version_string());
    obs_add_data_path(argv[3]);
    has_video = start_video();
    printf("graphics: %s\n", has_video ? "D3D11 available" : "unavailable, GPU checks skipped");

    module_result = obs_open_module(&module, argv[1], argv[2]);
    if (module_result != MODULE_SUCCESS) {
        fprintf(stderr, "obs_open_module failed: %d\n", module_result);
        return 4;
    }
    if (!obs_init_module(module)) {
        fputs("obs_init_module failed\n", stderr);
        return 5;
    }
    EXPECT(source_type_is_registered());
    EXPECT(strcmp(obs_get_module_name(module), "Bambu Printer Overlay") == 0);

    test_defaults_and_properties();
    test_settings_migration();
    test_bounds();
    test_lifecycle_churn();
    if (has_video)
        test_gpu_pipeline(png_directory, soak_seconds);

    /* OBS destroys sources on a queue, which can lag briefly when no graphics
     * subsystem is running. Past this point anything still referenced is a leak. */
    obs_wait_for_destroy_queue();
    Sleep(700);

    shutdown_start = GetTickCount64();
    obs_shutdown();
    printf("obs_shutdown took %llu ms\n", (unsigned long long)(GetTickCount64() - shutdown_start));

    EXPECT(leaked_references == 0);
    if (failures != 0) {
        fprintf(stderr, "%d expectation(s) failed\n", failures);
        return 1;
    }
    puts("bambu-overlay-plugin-load-tests passed");
    return 0;
}
