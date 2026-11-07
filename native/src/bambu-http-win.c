#include "bambu-log.h"
#include "bambu-net-win.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define URL_WIDE_CAPACITY 2300
#define RESOLVE_TIMEOUT_MS 5000
#define CONNECT_TIMEOUT_MS 5000
#define SEND_TIMEOUT_MS 5000
#define RECEIVE_TIMEOUT_MS 10000

void bambu_cancel_track_http(struct bambu_cancel *cancel, void *request);
void *bambu_cancel_untrack_http(struct bambu_cancel *cancel);

static bool add_bearer(HINTERNET request, const char *bearer)
{
    wchar_t header[4096];
    int written = _snwprintf(header, sizeof(header) / sizeof(header[0]) - 1, L"Authorization: Bearer %hs", bearer);
    bool ok;

    if (written <= 0)
        return false;
    header[written] = L'\0';
    ok = WinHttpAddRequestHeaders(request, header, (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD) != FALSE;
    SecureZeroMemory(header, sizeof(header));
    return ok;
}

enum bambu_net_result bambu_https_get(
    const char *url,
    const char *bearer,
    size_t max_bytes,
    struct bambu_cancel *cancel,
    uint8_t **body,
    size_t *length,
    int *status
)
{
    wchar_t wide_url[URL_WIDE_CAPACITY];
    URL_COMPONENTS parts;
    wchar_t host[256];
    wchar_t path[URL_WIDE_CAPACITY];
    HINTERNET session = NULL;
    HINTERNET connection = NULL;
    HINTERNET request = NULL;
    DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    DWORD status_code = 0;
    DWORD status_size = sizeof(status_code);
    uint8_t *buffer = NULL;
    size_t used = 0;
    size_t capacity = 0;
    bool tracked = false;
    enum bambu_net_result result = BAMBU_NET_CONNECT_FAILED;

    if (url == NULL || body == NULL || length == NULL || status == NULL)
        return BAMBU_NET_PROTOCOL_ERROR;
    *body = NULL;
    *length = 0;
    *status = 0;
    if (!bambu_validate_remote_url(url))
        return BAMBU_NET_PROTOCOL_ERROR;
    if (MultiByteToWideChar(CP_UTF8, 0, url, -1, wide_url, URL_WIDE_CAPACITY) == 0)
        return BAMBU_NET_PROTOCOL_ERROR;

    memset(&parts, 0, sizeof(parts));
    parts.dwStructSize = sizeof(parts);
    parts.lpszHostName = host;
    parts.dwHostNameLength = sizeof(host) / sizeof(host[0]);
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = sizeof(path) / sizeof(path[0]);
    if (!WinHttpCrackUrl(wide_url, 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS)
        return BAMBU_NET_PROTOCOL_ERROR;

    session = WinHttpOpen(
        L"bambu-printer-overlay",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0
    );
    if (session == NULL)
        goto done;
    WinHttpSetTimeouts(session, RESOLVE_TIMEOUT_MS, CONNECT_TIMEOUT_MS, SEND_TIMEOUT_MS, RECEIVE_TIMEOUT_MS);

    connection = WinHttpConnect(session, host, parts.nPort, 0);
    if (connection == NULL)
        goto done;
    request = WinHttpOpenRequest(
        connection,
        L"GET",
        path,
        NULL,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE
    );
    if (request == NULL)
        goto done;

    /* A redirect could point at an address that URL validation would have refused. */
    WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirect_policy, sizeof(redirect_policy));
    if (bearer != NULL && bearer[0] != '\0' && !add_bearer(request, bearer))
        goto done;

    bambu_cancel_track_http(cancel, request);
    tracked = true;
    if (bambu_cancel_requested(cancel)) {
        result = BAMBU_NET_CANCELLED;
        goto done;
    }

    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, NULL)) {
        result = bambu_cancel_requested(cancel) ? BAMBU_NET_CANCELLED : BAMBU_NET_CONNECT_FAILED;
        goto done;
    }
    if (!WinHttpQueryHeaders(
            request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &status_code,
            &status_size,
            WINHTTP_NO_HEADER_INDEX
        )) {
        result = BAMBU_NET_PROTOCOL_ERROR;
        goto done;
    }
    *status = (int)status_code;
    if (status_code == 401 || status_code == 403) {
        result = BAMBU_NET_AUTH_FAILED;
        goto done;
    }
    if (status_code < 200 || status_code >= 300) {
        result = BAMBU_NET_NOT_FOUND;
        goto done;
    }

    for (;;) {
        DWORD available = 0;
        DWORD read = 0;

        if (bambu_cancel_requested(cancel)) {
            result = BAMBU_NET_CANCELLED;
            goto done;
        }
        if (!WinHttpQueryDataAvailable(request, &available)) {
            result = bambu_cancel_requested(cancel) ? BAMBU_NET_CANCELLED : BAMBU_NET_CLOSED;
            goto done;
        }
        if (available == 0)
            break;
        if (used + available > max_bytes) {
            result = BAMBU_NET_TOO_LARGE;
            goto done;
        }
        if (used + available > capacity) {
            size_t grown = capacity == 0 ? 64 * 1024 : capacity * 2;
            uint8_t *resized;
            while (grown < used + available)
                grown *= 2;
            resized = realloc(buffer, grown);
            if (resized == NULL) {
                result = BAMBU_NET_TOO_LARGE;
                goto done;
            }
            buffer = resized;
            capacity = grown;
        }
        if (!WinHttpReadData(request, buffer + used, available, &read)) {
            result = bambu_cancel_requested(cancel) ? BAMBU_NET_CANCELLED : BAMBU_NET_CLOSED;
            goto done;
        }
        used += read;
    }

    *body = buffer;
    *length = used;
    buffer = NULL;
    result = BAMBU_NET_OK;

done:
    /* Once tracked, the request belongs to whoever untracks it: a NULL here means
     * a concurrent cancel has already closed it. */
    if (request != NULL) {
        bool closed_by_cancel = tracked && cancel != NULL && bambu_cancel_untrack_http(cancel) == NULL;
        if (!closed_by_cancel)
            WinHttpCloseHandle(request);
    }
    if (connection != NULL)
        WinHttpCloseHandle(connection);
    if (session != NULL)
        WinHttpCloseHandle(session);
    free(buffer);
    return result;
}
