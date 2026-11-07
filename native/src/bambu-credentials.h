#pragma once

/* Operating-system credential store. Secrets live here, and only here: never in
 * scene collections, profile files, logs, or diagnostics. The interface is
 * portable; the Windows implementation uses Credential Manager. */

#include <stdbool.h>
#include <stddef.h>

#define BAMBU_SECRET_ACCESS_CODE "access_code"
#define BAMBU_SECRET_CLOUD_TOKEN "cloud_token"
#define BAMBU_ACCESS_CODE_CAPACITY 64
#define BAMBU_CLOUD_TOKEN_CAPACITY 2560

/* An empty or NULL secret deletes the entry. */
bool bambu_credential_write(const char *profile_id, const char *kind, const char *secret);
bool bambu_credential_read(const char *profile_id, const char *kind, char *out, size_t capacity);
bool bambu_credential_exists(const char *profile_id, const char *kind);
void bambu_credential_delete(const char *profile_id, const char *kind);
