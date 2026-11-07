#include "bambu-profiles.h"

#include "bambu-json.h"
#include "bambu-log.h"
#include "printer-snapshot.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROFILE_FILE_VERSION 1
#define MAX_PROFILE_FILE_BYTES (256u * 1024u)

static CRITICAL_SECTION store_lock;
static bool store_ready = false;
static char store_path[1024];
static struct bambu_profile_list store;

void bambu_profile_defaults(struct bambu_profile *profile)
{
    memset(profile, 0, sizeof(*profile));
    profile->lan_only = true;
    profile->preview_enabled = true;
}

bool bambu_profile_is_complete(const struct bambu_profile *profile)
{
    return profile != NULL && profile->host[0] != '\0' && profile->serial[0] != '\0';
}

static size_t append(char *out, size_t capacity, size_t used, const char *text)
{
    size_t length = strlen(text);
    if (out != NULL && used + length < capacity)
        memcpy(out + used, text, length + 1);
    return used + length;
}

static size_t append_field(char *out, size_t capacity, size_t used, const char *key, const char *value, bool last)
{
    char escaped[1024];

    bambu_json_escape(value, escaped, sizeof(escaped));
    used = append(out, capacity, used, "\"");
    used = append(out, capacity, used, key);
    used = append(out, capacity, used, "\":");
    used = append(out, capacity, used, escaped);
    return append(out, capacity, used, last ? "" : ",");
}

static size_t append_flag(char *out, size_t capacity, size_t used, const char *key, bool value)
{
    used = append(out, capacity, used, "\"");
    used = append(out, capacity, used, key);
    used = append(out, capacity, used, "\":");
    used = append(out, capacity, used, value ? "true" : "false");
    return append(out, capacity, used, ",");
}

size_t bambu_profiles_to_json(const struct bambu_profile_list *list, char *out, size_t capacity)
{
    char header[64];
    size_t used = 0;

    snprintf(header, sizeof(header), "{\"version\":%d,\"profiles\":[", PROFILE_FILE_VERSION);
    used = append(out, capacity, used, header);
    for (int i = 0; i < list->count; i++) {
        const struct bambu_profile *p = &list->profiles[i];

        used = append(out, capacity, used, i > 0 ? ",{" : "{");
        used = append_field(out, capacity, used, "id", p->id, false);
        used = append_field(out, capacity, used, "name", p->name, false);
        used = append_field(out, capacity, used, "host", p->host, false);
        used = append_field(out, capacity, used, "serial", p->serial, false);
        used = append_flag(out, capacity, used, "lan_only", p->lan_only);
        used = append_flag(out, capacity, used, "cloud_china_region", p->cloud_china_region);
        used = append_flag(out, capacity, used, "preview_enabled", p->preview_enabled);
        used = append_field(out, capacity, used, "mqtt_fingerprint", p->mqtt_fingerprint, false);
        used = append_field(out, capacity, used, "ftps_fingerprint", p->ftps_fingerprint, true);
        used = append(out, capacity, used, "}");
    }
    return append(out, capacity, used, "]}");
}

static void read_text(const struct bambu_json *object, const char *key, char *out, size_t capacity)
{
    const char *value = bambu_json_as_string(bambu_json_get(object, key));
    if (value != NULL)
        bambu_copy_text(out, capacity, value);
}

static void read_flag(const struct bambu_json *object, const char *key, bool *out)
{
    const struct bambu_json *node = bambu_json_get(object, key);
    if (node != NULL && node->type == BAMBU_JSON_BOOL)
        *out = node->boolean;
}

static void read_fingerprint(const struct bambu_json *object, const char *key, char *out)
{
    char normalized[BAMBU_FINGERPRINT_CAPACITY];

    /* A hand-edited, invalid pin is dropped so the profile asks to be trusted again. */
    if (bambu_normalize_fingerprint(bambu_json_as_string(bambu_json_get(object, key)), normalized))
        memcpy(out, normalized, sizeof(normalized));
}

bool bambu_profiles_from_json(const char *text, size_t length, struct bambu_profile_list *list)
{
    struct bambu_json *root = bambu_json_parse(text, length);
    const struct bambu_json *profiles;
    const struct bambu_json *node;

    memset(list, 0, sizeof(*list));
    if (root == NULL)
        return false;
    profiles = bambu_json_get(root, "profiles");
    if (profiles == NULL || profiles->type != BAMBU_JSON_ARRAY) {
        bambu_json_free(root);
        return false;
    }

    for (node = profiles->child; node != NULL && list->count < BAMBU_MAX_PROFILES; node = node->next) {
        struct bambu_profile *p = &list->profiles[list->count];

        bambu_profile_defaults(p);
        read_text(node, "id", p->id, sizeof(p->id));
        read_text(node, "name", p->name, sizeof(p->name));
        read_text(node, "host", p->host, sizeof(p->host));
        read_text(node, "serial", p->serial, sizeof(p->serial));
        read_flag(node, "lan_only", &p->lan_only);
        read_flag(node, "cloud_china_region", &p->cloud_china_region);
        read_flag(node, "preview_enabled", &p->preview_enabled);
        read_fingerprint(node, "mqtt_fingerprint", p->mqtt_fingerprint);
        read_fingerprint(node, "ftps_fingerprint", p->ftps_fingerprint);
        if (p->id[0] != '\0')
            list->count++;
    }
    bambu_json_free(root);
    return true;
}

bool bambu_write_file_atomic(const char *path, const void *data, size_t length)
{
    wchar_t wide_path[1100];
    wchar_t wide_temp[1110];
    HANDLE file;
    DWORD written = 0;
    BOOL ok;

    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wide_path, 1100) == 0)
        return false;
    swprintf(wide_temp, 1110, L"%ls.tmp", wide_path);

    file = CreateFileW(wide_temp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    ok = WriteFile(file, data, (DWORD)length, &written, NULL) && written == length && FlushFileBuffers(file);
    CloseHandle(file);
    if (!ok || !MoveFileExW(wide_temp, wide_path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(wide_temp);
        return false;
    }
    return true;
}

bool bambu_read_file(const char *path, size_t maximum_bytes, unsigned char **data, size_t *length)
{
    wchar_t wide_path[1100];
    HANDLE file;
    LARGE_INTEGER size;
    unsigned char *buffer = NULL;
    DWORD read = 0;
    bool ok = false;

    *data = NULL;
    *length = 0;
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wide_path, 1100) == 0)
        return false;
    file = CreateFileW(wide_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && (unsigned long long)size.QuadPart <= maximum_bytes) {
        buffer = malloc((size_t)size.QuadPart + 1);
        if (buffer != NULL && ReadFile(file, buffer, (DWORD)size.QuadPart, &read, NULL) && read == size.QuadPart) {
            buffer[read] = 0;
            *data = buffer;
            *length = read;
            ok = true;
        } else {
            free(buffer);
        }
    }
    CloseHandle(file);
    return ok;
}

void bambu_delete_file(const char *path)
{
    wchar_t wide_path[1100];

    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wide_path, 1100) != 0)
        DeleteFileW(wide_path);
}

static void load_file(void)
{
    unsigned char *text = NULL;
    size_t length = 0;

    memset(&store, 0, sizeof(store));
    if (!bambu_read_file(store_path, MAX_PROFILE_FILE_BYTES, &text, &length))
        return;
    if (!bambu_profiles_from_json((const char *)text, length, &store))
        bambu_log(BAMBU_LOG_WARNING, "profile file is corrupt; starting with no profiles");
    free(text);
}

static bool save_file(void)
{
    size_t capacity = bambu_profiles_to_json(&store, NULL, 0) + 1;
    char *text = malloc(capacity);
    bool ok = false;

    if (text != NULL) {
        bambu_profiles_to_json(&store, text, capacity);
        ok = bambu_write_file_atomic(store_path, text, capacity - 1);
        free(text);
    }
    if (!ok)
        bambu_log(BAMBU_LOG_WARNING, "could not write the profile file");
    return ok;
}

void bambu_profile_store_init(const char *path)
{
    if (!store_ready) {
        InitializeCriticalSection(&store_lock);
        store_ready = true;
    }
    EnterCriticalSection(&store_lock);
    snprintf(store_path, sizeof(store_path), "%s", path);
    load_file();
    LeaveCriticalSection(&store_lock);
}

void bambu_profile_store_shutdown(void)
{
    if (store_ready) {
        DeleteCriticalSection(&store_lock);
        store_ready = false;
    }
}

void bambu_profile_store_list(struct bambu_profile_list *list)
{
    memset(list, 0, sizeof(*list));
    if (!store_ready)
        return;
    EnterCriticalSection(&store_lock);
    *list = store;
    LeaveCriticalSection(&store_lock);
}

bool bambu_profile_store_get(const char *id, struct bambu_profile *profile)
{
    bool found = false;

    if (!store_ready || id == NULL)
        return false;
    EnterCriticalSection(&store_lock);
    for (int i = 0; i < store.count && !found; i++) {
        if (strcmp(store.profiles[i].id, id) == 0) {
            *profile = store.profiles[i];
            found = true;
        }
    }
    LeaveCriticalSection(&store_lock);
    return found;
}

bool bambu_profile_store_put(const struct bambu_profile *profile)
{
    bool ok;
    int slot = -1;

    if (!store_ready || profile == NULL || profile->id[0] == '\0')
        return false;
    EnterCriticalSection(&store_lock);
    for (int i = 0; i < store.count; i++) {
        if (strcmp(store.profiles[i].id, profile->id) == 0)
            slot = i;
    }
    if (slot < 0 && store.count < BAMBU_MAX_PROFILES)
        slot = store.count++;
    ok = slot >= 0;
    if (ok) {
        store.profiles[slot] = *profile;
        ok = save_file();
    }
    LeaveCriticalSection(&store_lock);
    return ok;
}

bool bambu_profile_store_remove(const char *id)
{
    bool removed = false;

    if (!store_ready || id == NULL)
        return false;
    EnterCriticalSection(&store_lock);
    for (int i = 0; i < store.count; i++) {
        if (strcmp(store.profiles[i].id, id) == 0) {
            memmove(&store.profiles[i], &store.profiles[i + 1], sizeof(store.profiles[0]) * (size_t)(store.count - i - 1));
            store.count--;
            removed = save_file();
            break;
        }
    }
    LeaveCriticalSection(&store_lock);
    return removed;
}

void bambu_profile_new_id(char out[BAMBU_PROFILE_ID_CAPACITY])
{
    unsigned char bytes[16];

    if (!BCRYPT_SUCCESS(BCryptGenRandom(NULL, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
        memset(bytes, (int)GetTickCount(), sizeof(bytes));
    for (size_t i = 0; i < sizeof(bytes); i++)
        snprintf(out + i * 2, 3, "%02x", bytes[i]);
}
