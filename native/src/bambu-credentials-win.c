#include "bambu-credentials.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincred.h>

#include <stdio.h>
#include <string.h>

static bool target_name(const char *profile_id, const char *kind, wchar_t *out, size_t capacity)
{
    char target[256];
    int written = snprintf(target, sizeof(target), "bambu-printer-overlay/%s/%s", profile_id, kind);

    if (profile_id == NULL || profile_id[0] == '\0' || written <= 0 || (size_t)written >= sizeof(target))
        return false;
    return MultiByteToWideChar(CP_UTF8, 0, target, -1, out, (int)capacity) > 0;
}

bool bambu_credential_write(const char *profile_id, const char *kind, const char *secret)
{
    wchar_t target[300];
    CREDENTIALW credential;

    if (secret == NULL || secret[0] == '\0') {
        bambu_credential_delete(profile_id, kind);
        return true;
    }
    if (!target_name(profile_id, kind, target, sizeof(target) / sizeof(target[0])))
        return false;

    memset(&credential, 0, sizeof(credential));
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = target;
    credential.CredentialBlobSize = (DWORD)strlen(secret);
    credential.CredentialBlob = (LPBYTE)secret;
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    credential.UserName = L"bambu-printer-overlay";
    return CredWriteW(&credential, 0) != FALSE;
}

bool bambu_credential_read(const char *profile_id, const char *kind, char *out, size_t capacity)
{
    wchar_t target[300];
    PCREDENTIALW credential = NULL;
    bool ok = false;

    if (out == NULL || capacity == 0)
        return false;
    out[0] = '\0';
    if (!target_name(profile_id, kind, target, sizeof(target) / sizeof(target[0])))
        return false;
    if (!CredReadW(target, CRED_TYPE_GENERIC, 0, &credential))
        return false;
    if (credential->CredentialBlobSize < capacity) {
        memcpy(out, credential->CredentialBlob, credential->CredentialBlobSize);
        out[credential->CredentialBlobSize] = '\0';
        ok = true;
    }
    SecureZeroMemory(credential->CredentialBlob, credential->CredentialBlobSize);
    CredFree(credential);
    return ok;
}

bool bambu_credential_exists(const char *profile_id, const char *kind)
{
    wchar_t target[300];
    PCREDENTIALW credential = NULL;

    if (!target_name(profile_id, kind, target, sizeof(target) / sizeof(target[0])))
        return false;
    if (!CredReadW(target, CRED_TYPE_GENERIC, 0, &credential))
        return false;
    SecureZeroMemory(credential->CredentialBlob, credential->CredentialBlobSize);
    CredFree(credential);
    return true;
}

void bambu_credential_delete(const char *profile_id, const char *kind)
{
    wchar_t target[300];

    if (target_name(profile_id, kind, target, sizeof(target) / sizeof(target[0])))
        CredDeleteW(target, CRED_TYPE_GENERIC, 0);
}
