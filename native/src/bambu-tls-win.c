#include "bambu-net-win.h"

#include "bambu-log.h"

#define WIN32_LEAN_AND_MEAN
#define SECURITY_WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <schannel.h>
#include <security.h>
#include <sspi.h>
#include <winhttp.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define POLL_SLICE_MS 100
#define RECEIVE_CAPACITY (64 * 1024)

struct bambu_cancel {
    volatile LONG requested;
    CRITICAL_SECTION lock;
    void *http_request;
};

struct bambu_tls_credentials {
    CredHandle handle;
    bool winsock_started;
};

struct bambu_tls {
    SOCKET socket;
    struct bambu_tls_credentials *credentials; /* outlives the connection */
    CtxtHandle context;
    bool has_context;
    bool peer_closed;
    bool session_reused;
    SecPkgContext_StreamSizes sizes;
    uint8_t *incoming;
    size_t incoming_length;
    uint8_t *plain;      /* decrypted data not yet handed to the caller */
    size_t plain_offset;
    size_t plain_length;
};

struct bambu_cancel *bambu_cancel_create(void)
{
    struct bambu_cancel *cancel = calloc(1, sizeof(*cancel));

    if (cancel != NULL)
        InitializeCriticalSection(&cancel->lock);
    return cancel;
}

void bambu_cancel_destroy(struct bambu_cancel *cancel)
{
    if (cancel == NULL)
        return;
    DeleteCriticalSection(&cancel->lock);
    free(cancel);
}

void bambu_cancel_request(struct bambu_cancel *cancel)
{
    if (cancel == NULL)
        return;
    InterlockedExchange(&cancel->requested, 1);
    /* Closing an in-flight WinHTTP handle is what aborts a blocking call. */
    EnterCriticalSection(&cancel->lock);
    if (cancel->http_request != NULL) {
        WinHttpCloseHandle(cancel->http_request);
        cancel->http_request = NULL;
    }
    LeaveCriticalSection(&cancel->lock);
}

void bambu_cancel_reset(struct bambu_cancel *cancel)
{
    if (cancel != NULL)
        InterlockedExchange(&cancel->requested, 0);
}

bool bambu_cancel_requested(const struct bambu_cancel *cancel)
{
    return cancel != NULL && cancel->requested != 0;
}

/* Used by bambu-http-win.c, which shares the token's in-flight request slot. */
void bambu_cancel_track_http(struct bambu_cancel *cancel, void *request);
void *bambu_cancel_untrack_http(struct bambu_cancel *cancel);

void bambu_cancel_track_http(struct bambu_cancel *cancel, void *request)
{
    if (cancel == NULL)
        return;
    EnterCriticalSection(&cancel->lock);
    cancel->http_request = request;
    LeaveCriticalSection(&cancel->lock);
}

void *bambu_cancel_untrack_http(struct bambu_cancel *cancel)
{
    void *request;

    if (cancel == NULL)
        return NULL;
    EnterCriticalSection(&cancel->lock);
    request = cancel->http_request;
    cancel->http_request = NULL;
    LeaveCriticalSection(&cancel->lock);
    return request;
}

const char *bambu_net_result_name(enum bambu_net_result result)
{
    switch (result) {
    case BAMBU_NET_OK:
        return "ok";
    case BAMBU_NET_TIMEOUT:
        return "timeout";
    case BAMBU_NET_CANCELLED:
        return "cancelled";
    case BAMBU_NET_CLOSED:
        return "closed";
    case BAMBU_NET_RESOLVE_FAILED:
        return "resolve failed";
    case BAMBU_NET_CONNECT_FAILED:
        return "connect failed";
    case BAMBU_NET_TLS_FAILED:
        return "TLS failed";
    case BAMBU_NET_PIN_MISMATCH:
        return "certificate does not match the pinned fingerprint";
    case BAMBU_NET_PROTOCOL_ERROR:
        return "protocol error";
    case BAMBU_NET_AUTH_FAILED:
        return "authentication failed";
    case BAMBU_NET_TOO_LARGE:
        return "response too large";
    case BAMBU_NET_NOT_FOUND:
        return "not found";
    default:
        return "unknown";
    }
}

struct bambu_tls_credentials *bambu_tls_credentials_create(void)
{
    struct bambu_tls_credentials *credentials = calloc(1, sizeof(*credentials));
    SCHANNEL_CRED schannel;
    WSADATA winsock;
    SECURITY_STATUS status;

    if (credentials == NULL)
        return NULL;
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) {
        free(credentials);
        return NULL;
    }
    credentials->winsock_started = true;

    memset(&schannel, 0, sizeof(schannel));
    schannel.dwVersion = SCHANNEL_CRED_VERSION;
    /* TLS 1.2 is what printer firmware speaks, and its session resumption is what
     * the FTPS data channel depends on. */
    schannel.grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT;
    /* Manual validation is safe only because every connection then pins the peer
     * certificate; see bambu_tls_connect(). */
    schannel.dwFlags = SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS | SCH_CRED_NO_SERVERNAME_CHECK;

    status = AcquireCredentialsHandleW(
        NULL,
        (SEC_WCHAR *)UNISP_NAME_W,
        SECPKG_CRED_OUTBOUND,
        NULL,
        &schannel,
        NULL,
        NULL,
        &credentials->handle,
        NULL
    );
    if (status != SEC_E_OK) {
        bambu_log(BAMBU_LOG_ERROR, "AcquireCredentialsHandle failed: 0x%08lx", (unsigned long)status);
        WSACleanup();
        free(credentials);
        return NULL;
    }
    return credentials;
}

void bambu_tls_credentials_destroy(struct bambu_tls_credentials *credentials)
{
    if (credentials == NULL)
        return;
    FreeCredentialsHandle(&credentials->handle);
    if (credentials->winsock_started)
        WSACleanup();
    free(credentials);
}

/* Waits for socket readiness in short slices so a cancel is seen quickly. */
static enum bambu_net_result wait_socket(SOCKET socket, bool for_write, uint32_t timeout_ms, struct bambu_cancel *cancel)
{
    ULONGLONG deadline = GetTickCount64() + timeout_ms;

    for (;;) {
        fd_set ready;
        fd_set failed;
        struct timeval slice;
        ULONGLONG now;
        ULONGLONG remaining;
        int count;

        if (bambu_cancel_requested(cancel))
            return BAMBU_NET_CANCELLED;
        now = GetTickCount64();
        if (now >= deadline)
            return BAMBU_NET_TIMEOUT;
        remaining = deadline - now;
        if (remaining > POLL_SLICE_MS)
            remaining = POLL_SLICE_MS;

        FD_ZERO(&ready);
        FD_ZERO(&failed);
        FD_SET(socket, &ready);
        FD_SET(socket, &failed);
        slice.tv_sec = 0;
        slice.tv_usec = (long)remaining * 1000;
        count = select(0, for_write ? NULL : &ready, for_write ? &ready : NULL, &failed, &slice);
        if (count == SOCKET_ERROR || FD_ISSET(socket, &failed))
            return BAMBU_NET_CLOSED;
        if (count > 0)
            return BAMBU_NET_OK;
    }
}

static enum bambu_net_result send_all(
    SOCKET socket,
    const void *data,
    size_t length,
    uint32_t timeout_ms,
    struct bambu_cancel *cancel
)
{
    const char *cursor = data;

    while (length > 0) {
        enum bambu_net_result waited = wait_socket(socket, true, timeout_ms, cancel);
        int sent;

        if (waited != BAMBU_NET_OK)
            return waited;
        sent = send(socket, cursor, length > 0x10000 ? 0x10000 : (int)length, 0);
        if (sent == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAEWOULDBLOCK)
                continue;
            return BAMBU_NET_CLOSED;
        }
        cursor += sent;
        length -= (size_t)sent;
    }
    return BAMBU_NET_OK;
}

static enum bambu_net_result connect_tcp(
    const char *host,
    uint16_t port,
    uint32_t timeout_ms,
    struct bambu_cancel *cancel,
    SOCKET *out
)
{
    struct addrinfo hints;
    struct addrinfo *addresses = NULL;
    struct addrinfo *address;
    char service[16];
    enum bambu_net_result result = BAMBU_NET_CONNECT_FAILED;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    snprintf(service, sizeof(service), "%u", (unsigned)port);
    if (getaddrinfo(host, service, &hints, &addresses) != 0)
        return BAMBU_NET_RESOLVE_FAILED;

    for (address = addresses; address != NULL; address = address->ai_next) {
        u_long non_blocking = 1;
        int error = 0;
        int error_size = sizeof(error);
        SOCKET candidate = socket(address->ai_family, address->ai_socktype, address->ai_protocol);

        if (candidate == INVALID_SOCKET)
            continue;
        ioctlsocket(candidate, FIONBIO, &non_blocking);
        if (connect(candidate, address->ai_addr, (int)address->ai_addrlen) == SOCKET_ERROR &&
            WSAGetLastError() != WSAEWOULDBLOCK) {
            closesocket(candidate);
            continue;
        }
        result = wait_socket(candidate, true, timeout_ms, cancel);
        if (result == BAMBU_NET_CLOSED)
            result = BAMBU_NET_CONNECT_FAILED;
        if (result == BAMBU_NET_OK) {
            getsockopt(candidate, SOL_SOCKET, SO_ERROR, (char *)&error, &error_size);
            if (error == 0) {
                *out = candidate;
                freeaddrinfo(addresses);
                return BAMBU_NET_OK;
            }
            result = BAMBU_NET_CONNECT_FAILED;
        }
        closesocket(candidate);
        if (result == BAMBU_NET_CANCELLED)
            break;
    }
    freeaddrinfo(addresses);
    return result;
}

static bool wide_host(const char *host, wchar_t *out, int capacity)
{
    return MultiByteToWideChar(CP_UTF8, 0, host, -1, out, capacity) > 0;
}

/* Reads more ciphertext into the connection's receive buffer. */
static enum bambu_net_result receive_more(struct bambu_tls *tls, uint32_t timeout_ms, struct bambu_cancel *cancel)
{
    enum bambu_net_result waited;
    int received;

    if (tls->incoming_length >= RECEIVE_CAPACITY)
        return BAMBU_NET_PROTOCOL_ERROR;
    waited = wait_socket(tls->socket, false, timeout_ms, cancel);
    if (waited != BAMBU_NET_OK)
        return waited;
    received = recv(
        tls->socket,
        (char *)tls->incoming + tls->incoming_length,
        (int)(RECEIVE_CAPACITY - tls->incoming_length),
        0
    );
    if (received == 0)
        return BAMBU_NET_CLOSED;
    if (received == SOCKET_ERROR)
        return WSAGetLastError() == WSAEWOULDBLOCK ? BAMBU_NET_OK : BAMBU_NET_CLOSED;
    tls->incoming_length += (size_t)received;
    return BAMBU_NET_OK;
}

static enum bambu_net_result handshake(
    struct bambu_tls_credentials *credentials,
    struct bambu_tls *tls,
    const wchar_t *target,
    uint32_t timeout_ms,
    struct bambu_cancel *cancel
)
{
    const DWORD request_flags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
                                ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM;
    ULONGLONG deadline = GetTickCount64() + timeout_ms;
    bool first = true;

    for (;;) {
        SecBuffer in_buffers[2];
        SecBuffer out_buffer;
        SecBufferDesc in_description;
        SecBufferDesc out_description;
        SECURITY_STATUS status;
        DWORD attributes = 0;
        ULONGLONG now = GetTickCount64();

        if (now >= deadline)
            return BAMBU_NET_TIMEOUT;

        in_buffers[0].BufferType = SECBUFFER_TOKEN;
        in_buffers[0].pvBuffer = tls->incoming;
        in_buffers[0].cbBuffer = (unsigned long)tls->incoming_length;
        in_buffers[1].BufferType = SECBUFFER_EMPTY;
        in_buffers[1].pvBuffer = NULL;
        in_buffers[1].cbBuffer = 0;
        in_description.ulVersion = SECBUFFER_VERSION;
        in_description.cBuffers = 2;
        in_description.pBuffers = in_buffers;

        out_buffer.BufferType = SECBUFFER_TOKEN;
        out_buffer.pvBuffer = NULL;
        out_buffer.cbBuffer = 0;
        out_description.ulVersion = SECBUFFER_VERSION;
        out_description.cBuffers = 1;
        out_description.pBuffers = &out_buffer;

        status = InitializeSecurityContextW(
            &credentials->handle,
            first ? NULL : &tls->context,
            first ? (SEC_WCHAR *)target : NULL,
            request_flags,
            0,
            0,
            first ? NULL : &in_description,
            0,
            first ? &tls->context : NULL,
            &out_description,
            &attributes,
            NULL
        );
        if (first && (status == SEC_I_CONTINUE_NEEDED || status == SEC_E_OK))
            tls->has_context = true;
        first = false;

        if (out_buffer.pvBuffer != NULL) {
            enum bambu_net_result sent = BAMBU_NET_OK;
            if (out_buffer.cbBuffer > 0)
                sent = send_all(tls->socket, out_buffer.pvBuffer, out_buffer.cbBuffer, timeout_ms, cancel);
            FreeContextBuffer(out_buffer.pvBuffer);
            if (sent != BAMBU_NET_OK)
                return sent;
        }

        if (status == SEC_E_INCOMPLETE_MESSAGE) {
            enum bambu_net_result more = receive_more(tls, (uint32_t)(deadline - now), cancel);
            if (more != BAMBU_NET_OK)
                return more == BAMBU_NET_CLOSED ? BAMBU_NET_TLS_FAILED : more;
            continue;
        }

        if (status == SEC_E_OK || status == SEC_I_CONTINUE_NEEDED) {
            /* Keep bytes the handshake did not consume; they belong to the next step. */
            if (in_buffers[1].BufferType == SECBUFFER_EXTRA && in_buffers[1].cbBuffer > 0) {
                memmove(
                    tls->incoming,
                    tls->incoming + tls->incoming_length - in_buffers[1].cbBuffer,
                    in_buffers[1].cbBuffer
                );
                tls->incoming_length = in_buffers[1].cbBuffer;
            } else {
                tls->incoming_length = 0;
            }
            if (status == SEC_E_OK)
                return BAMBU_NET_OK;
            if (tls->incoming_length == 0) {
                enum bambu_net_result more = receive_more(tls, (uint32_t)(deadline - now), cancel);
                if (more != BAMBU_NET_OK)
                    return more == BAMBU_NET_CLOSED ? BAMBU_NET_TLS_FAILED : more;
            }
            continue;
        }

        bambu_log(BAMBU_LOG_WARNING, "TLS handshake failed: 0x%08lx", (unsigned long)status);
        return BAMBU_NET_TLS_FAILED;
    }
}

static bool hash_sha256(const uint8_t *data, ULONG length, uint8_t digest[32])
{
    return BCRYPT_SUCCESS(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, NULL, 0, (PUCHAR)data, length, digest, 32));
}

static bool peer_fingerprint(struct bambu_tls *tls, char out[BAMBU_FINGERPRINT_CAPACITY])
{
    PCCERT_CONTEXT certificate = NULL;
    uint8_t digest[32];
    bool ok;

    if (QueryContextAttributesW(&tls->context, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &certificate) != SEC_E_OK ||
        certificate == NULL)
        return false;
    ok = hash_sha256(certificate->pbCertEncoded, certificate->cbCertEncoded, digest);
    CertFreeCertificateContext(certificate);
    if (!ok)
        return false;
    for (int i = 0; i < 32; i++)
        snprintf(out + i * 2, 3, "%02x", digest[i]);
    return true;
}

static void free_connection(struct bambu_tls *tls)
{
    if (tls->has_context)
        DeleteSecurityContext(&tls->context);
    if (tls->socket != INVALID_SOCKET)
        closesocket(tls->socket);
    free(tls->incoming);
    free(tls->plain);
    free(tls);
}

/* Constant-time compare: the pin is not secret, but this costs nothing. */
static bool fingerprints_equal(const char *left, const char *right)
{
    unsigned difference = 0;

    for (int i = 0; i < BAMBU_FINGERPRINT_HEX_LENGTH; i++)
        difference |= (unsigned)(left[i] ^ right[i]);
    return difference == 0;
}

enum bambu_net_result bambu_tls_connect(
    struct bambu_tls_credentials *credentials,
    const char *host,
    uint16_t port,
    const char *pinned_fingerprint,
    uint32_t timeout_ms,
    struct bambu_cancel *cancel,
    struct bambu_tls **connection,
    char seen_fingerprint[BAMBU_FINGERPRINT_CAPACITY]
)
{
    struct bambu_tls *tls;
    wchar_t target[256];
    char fingerprint[BAMBU_FINGERPRINT_CAPACITY];
    SecPkgContext_SessionInfo session;
    enum bambu_net_result result;

    if (credentials == NULL || host == NULL || connection == NULL || !wide_host(host, target, 256))
        return BAMBU_NET_RESOLVE_FAILED;
    *connection = NULL;

    tls = calloc(1, sizeof(*tls));
    if (tls == NULL)
        return BAMBU_NET_CONNECT_FAILED;
    tls->socket = INVALID_SOCKET;
    tls->credentials = credentials;
    tls->incoming = malloc(RECEIVE_CAPACITY);
    if (tls->incoming == NULL) {
        free_connection(tls);
        return BAMBU_NET_CONNECT_FAILED;
    }

    result = connect_tcp(host, port, timeout_ms, cancel, &tls->socket);
    if (result == BAMBU_NET_OK)
        result = handshake(credentials, tls, target, timeout_ms, cancel);
    if (result != BAMBU_NET_OK) {
        free_connection(tls);
        return result;
    }

    if (!peer_fingerprint(tls, fingerprint)) {
        free_connection(tls);
        return BAMBU_NET_TLS_FAILED;
    }
    if (seen_fingerprint != NULL)
        memcpy(seen_fingerprint, fingerprint, BAMBU_FINGERPRINT_CAPACITY);
    if (pinned_fingerprint != NULL && !fingerprints_equal(fingerprint, pinned_fingerprint)) {
        free_connection(tls);
        return BAMBU_NET_PIN_MISMATCH;
    }

    if (QueryContextAttributesW(&tls->context, SECPKG_ATTR_STREAM_SIZES, &tls->sizes) != SEC_E_OK) {
        free_connection(tls);
        return BAMBU_NET_TLS_FAILED;
    }
    tls->plain = malloc(RECEIVE_CAPACITY);
    if (tls->plain == NULL) {
        free_connection(tls);
        return BAMBU_NET_CONNECT_FAILED;
    }
    memset(&session, 0, sizeof(session));
    if (QueryContextAttributesW(&tls->context, SECPKG_ATTR_SESSION_INFO, &session) == SEC_E_OK)
        tls->session_reused = (session.dwFlags & SSL_SESSION_RECONNECT) != 0;

    *connection = tls;
    return BAMBU_NET_OK;
}

bool bambu_tls_session_reused(const struct bambu_tls *connection)
{
    return connection != NULL && connection->session_reused;
}

/* Decrypts one TLS record from the receive buffer if a whole record is present. */
static enum bambu_net_result decrypt_available(struct bambu_tls *tls, bool *need_more)
{
    SecBuffer buffers[4];
    SecBufferDesc description;
    SECURITY_STATUS status;
    const SecBuffer *data = NULL;
    const SecBuffer *extra = NULL;

    *need_more = false;
    if (tls->incoming_length == 0) {
        *need_more = true;
        return BAMBU_NET_OK;
    }

    buffers[0].BufferType = SECBUFFER_DATA;
    buffers[0].pvBuffer = tls->incoming;
    buffers[0].cbBuffer = (unsigned long)tls->incoming_length;
    for (int i = 1; i < 4; i++) {
        buffers[i].BufferType = SECBUFFER_EMPTY;
        buffers[i].pvBuffer = NULL;
        buffers[i].cbBuffer = 0;
    }
    description.ulVersion = SECBUFFER_VERSION;
    description.cBuffers = 4;
    description.pBuffers = buffers;

    status = DecryptMessage(&tls->context, &description, 0, NULL);
    if (status == SEC_E_INCOMPLETE_MESSAGE) {
        *need_more = true;
        return BAMBU_NET_OK;
    }
    if (status == SEC_I_CONTEXT_EXPIRED) {
        tls->peer_closed = true;
        tls->incoming_length = 0;
        return BAMBU_NET_OK;
    }
    if (status != SEC_E_OK && status != SEC_I_RENEGOTIATE) {
        bambu_log(BAMBU_LOG_WARNING, "TLS decrypt failed: 0x%08lx", (unsigned long)status);
        return BAMBU_NET_TLS_FAILED;
    }

    for (int i = 1; i < 4; i++) {
        if (buffers[i].BufferType == SECBUFFER_DATA)
            data = &buffers[i];
        else if (buffers[i].BufferType == SECBUFFER_EXTRA)
            extra = &buffers[i];
    }
    if (data != NULL && data->cbBuffer > 0) {
        memcpy(tls->plain, data->pvBuffer, data->cbBuffer);
        tls->plain_offset = 0;
        tls->plain_length = data->cbBuffer;
    }
    if (extra != NULL && extra->cbBuffer > 0) {
        memmove(tls->incoming, extra->pvBuffer, extra->cbBuffer);
        tls->incoming_length = extra->cbBuffer;
    } else {
        tls->incoming_length = 0;
    }
    if (status == SEC_I_RENEGOTIATE) {
        /* Printers do not renegotiate, and silently continuing would be unsafe. */
        return BAMBU_NET_TLS_FAILED;
    }
    return BAMBU_NET_OK;
}

enum bambu_net_result bambu_tls_read(
    struct bambu_tls *connection,
    void *buffer,
    size_t capacity,
    uint32_t timeout_ms,
    struct bambu_cancel *cancel,
    size_t *received
)
{
    ULONGLONG deadline = GetTickCount64() + timeout_ms;

    *received = 0;
    for (;;) {
        ULONGLONG now;
        bool need_more = false;
        enum bambu_net_result result;

        if (connection->plain_length > 0) {
            size_t count = connection->plain_length < capacity ? connection->plain_length : capacity;
            memcpy(buffer, connection->plain + connection->plain_offset, count);
            connection->plain_offset += count;
            connection->plain_length -= count;
            *received = count;
            return BAMBU_NET_OK;
        }
        if (connection->peer_closed)
            return BAMBU_NET_CLOSED;

        result = decrypt_available(connection, &need_more);
        if (result != BAMBU_NET_OK)
            return result;
        if (!need_more)
            continue;

        now = GetTickCount64();
        if (now >= deadline)
            return BAMBU_NET_TIMEOUT;
        result = receive_more(connection, (uint32_t)(deadline - now), cancel);
        if (result != BAMBU_NET_OK)
            return result;
    }
}

enum bambu_net_result bambu_tls_write(
    struct bambu_tls *connection,
    const void *data,
    size_t length,
    uint32_t timeout_ms,
    struct bambu_cancel *cancel
)
{
    const uint8_t *cursor = data;
    size_t header = connection->sizes.cbHeader;
    size_t trailer = connection->sizes.cbTrailer;
    size_t maximum = connection->sizes.cbMaximumMessage;
    uint8_t *record = malloc(header + maximum + trailer);
    enum bambu_net_result result = BAMBU_NET_OK;

    if (record == NULL)
        return BAMBU_NET_CLOSED;
    while (length > 0 && result == BAMBU_NET_OK) {
        size_t chunk = length < maximum ? length : maximum;
        SecBuffer buffers[4];
        SecBufferDesc description;

        memcpy(record + header, cursor, chunk);
        buffers[0].BufferType = SECBUFFER_STREAM_HEADER;
        buffers[0].pvBuffer = record;
        buffers[0].cbBuffer = (unsigned long)header;
        buffers[1].BufferType = SECBUFFER_DATA;
        buffers[1].pvBuffer = record + header;
        buffers[1].cbBuffer = (unsigned long)chunk;
        buffers[2].BufferType = SECBUFFER_STREAM_TRAILER;
        buffers[2].pvBuffer = record + header + chunk;
        buffers[2].cbBuffer = (unsigned long)trailer;
        buffers[3].BufferType = SECBUFFER_EMPTY;
        buffers[3].pvBuffer = NULL;
        buffers[3].cbBuffer = 0;
        description.ulVersion = SECBUFFER_VERSION;
        description.cBuffers = 4;
        description.pBuffers = buffers;

        if (EncryptMessage(&connection->context, 0, &description, 0) != SEC_E_OK) {
            result = BAMBU_NET_TLS_FAILED;
            break;
        }
        result = send_all(
            connection->socket,
            record,
            buffers[0].cbBuffer + buffers[1].cbBuffer + buffers[2].cbBuffer,
            timeout_ms,
            cancel
        );
        cursor += chunk;
        length -= chunk;
    }
    /* The record held plaintext such as the access code. */
    SecureZeroMemory(record, header + maximum + trailer);
    free(record);
    return result;
}

void bambu_tls_close(struct bambu_tls *connection)
{
    if (connection == NULL)
        return;
    if (connection->has_context && connection->socket != INVALID_SOCKET) {
        /* Best-effort close_notify. A TLS server may discard a session whose socket
         * closes without one, which would break the next FTPS data channel. */
        DWORD token = SCHANNEL_SHUTDOWN;
        SecBuffer in_buffer;
        SecBufferDesc in_description;
        SecBuffer out_buffer;
        SecBufferDesc out_description;
        DWORD attributes = 0;

        in_buffer.BufferType = SECBUFFER_TOKEN;
        in_buffer.pvBuffer = &token;
        in_buffer.cbBuffer = sizeof(token);
        in_description.ulVersion = SECBUFFER_VERSION;
        in_description.cBuffers = 1;
        in_description.pBuffers = &in_buffer;
        if (ApplyControlToken(&connection->context, &in_description) == SEC_E_OK) {
            out_buffer.BufferType = SECBUFFER_TOKEN;
            out_buffer.pvBuffer = NULL;
            out_buffer.cbBuffer = 0;
            out_description.ulVersion = SECBUFFER_VERSION;
            out_description.cBuffers = 1;
            out_description.pBuffers = &out_buffer;
            InitializeSecurityContextW(
                &connection->credentials->handle,
                &connection->context,
                NULL,
                ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY | ISC_REQ_ALLOCATE_MEMORY |
                    ISC_REQ_STREAM,
                0,
                0,
                NULL,
                0,
                NULL,
                &out_description,
                &attributes,
                NULL
            );
            if (out_buffer.pvBuffer != NULL) {
                if (out_buffer.cbBuffer > 0)
                    send(connection->socket, out_buffer.pvBuffer, (int)out_buffer.cbBuffer, 0);
                FreeContextBuffer(out_buffer.pvBuffer);
            }
        }
    }
    free_connection(connection);
}
