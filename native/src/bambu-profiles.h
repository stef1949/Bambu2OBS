#pragma once

/* Connection profiles. The file holds only non-secret settings: access codes and
 * cloud tokens live in the credential store, keyed by profile id. */

#include "bambu-util.h"

#include <stdbool.h>
#include <stddef.h>

#define BAMBU_PROFILE_ID_CAPACITY 40
#define BAMBU_PROFILE_LABEL_CAPACITY 128
#define BAMBU_HOST_CAPACITY 128
#define BAMBU_SERIAL_CAPACITY 64
#define BAMBU_MAX_PROFILES 16

struct bambu_profile {
    char id[BAMBU_PROFILE_ID_CAPACITY];
    char name[BAMBU_PROFILE_LABEL_CAPACITY];
    char host[BAMBU_HOST_CAPACITY];
    char serial[BAMBU_SERIAL_CAPACITY];
    bool lan_only;
    bool cloud_china_region;
    bool preview_enabled;
    char mqtt_fingerprint[BAMBU_FINGERPRINT_CAPACITY];
    char ftps_fingerprint[BAMBU_FINGERPRINT_CAPACITY];
};

struct bambu_profile_list {
    int count;
    struct bambu_profile profiles[BAMBU_MAX_PROFILES];
};

void bambu_profile_defaults(struct bambu_profile *profile);

/* True when the profile has everything needed to attempt a connection. */
bool bambu_profile_is_complete(const struct bambu_profile *profile);

/* Serialisation is pure so it can be tested; a missing or corrupt file simply
 * yields an empty list. */
size_t bambu_profiles_to_json(const struct bambu_profile_list *list, char *out, size_t capacity);
bool bambu_profiles_from_json(const char *text, size_t length, struct bambu_profile_list *list);

/* Process-wide store backed by one file, safe to call from any thread. */
void bambu_profile_store_init(const char *path);
void bambu_profile_store_shutdown(void);
void bambu_profile_store_list(struct bambu_profile_list *list);
bool bambu_profile_store_get(const char *id, struct bambu_profile *profile);
bool bambu_profile_store_put(const struct bambu_profile *profile);
bool bambu_profile_store_remove(const char *id);

/* Crash-safe replacement of a file's contents, and a bounded read of one. */
bool bambu_write_file_atomic(const char *path, const void *data, size_t length);
bool bambu_read_file(const char *path, size_t maximum_bytes, unsigned char **data, size_t *length);
void bambu_delete_file(const char *path);

/* Writes a new random profile id into out. */
void bambu_profile_new_id(char out[BAMBU_PROFILE_ID_CAPACITY]);
