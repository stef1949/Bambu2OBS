#pragma once

/* Windows networking: TCP with Schannel TLS, implicit FTPS, and WinHTTP.
 *
 * Every call is bounded by a timeout and polls a cancel token, so a worker can
 * always be joined promptly, even with a printer that has gone silent.
 *
 * Printer TLS uses manual validation against a pinned SHA-256 certificate
 * fingerprint, because printers present device certificates that no public CA
 * chains to. Cloud HTTPS goes through WinHTTP and keeps full default validation;
 * the two trust models are never mixed. */

#include "bambu-preview-select.h"
#include "bambu-util.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct bambu_cancel;
struct bambu_tls_credentials;
struct bambu_tls;

struct bambu_cancel *bambu_cancel_create(void);
void bambu_cancel_destroy(struct bambu_cancel *cancel);
void bambu_cancel_request(struct bambu_cancel *cancel);
void bambu_cancel_reset(struct bambu_cancel *cancel);
bool bambu_cancel_requested(const struct bambu_cancel *cancel);

enum bambu_net_result {
    BAMBU_NET_OK = 0,
    BAMBU_NET_TIMEOUT,
    BAMBU_NET_CANCELLED,
    BAMBU_NET_CLOSED,
    BAMBU_NET_RESOLVE_FAILED,
    BAMBU_NET_CONNECT_FAILED,
    BAMBU_NET_TLS_FAILED,
    BAMBU_NET_PIN_MISMATCH,
    BAMBU_NET_PROTOCOL_ERROR,
    BAMBU_NET_AUTH_FAILED,
    BAMBU_NET_TOO_LARGE,
    BAMBU_NET_NOT_FOUND,
};

const char *bambu_net_result_name(enum bambu_net_result result);

/* A credentials object owns the Schannel session cache. Connections sharing one
 * resume the same TLS session, which FTPS data channels on the printer require. */
struct bambu_tls_credentials *bambu_tls_credentials_create(void);
void bambu_tls_credentials_destroy(struct bambu_tls_credentials *credentials);

/* pinned_fingerprint may be NULL to observe a certificate without trusting it;
 * callers must then send no credentials on that connection. seen_fingerprint may
 * be NULL. A mismatch fails with BAMBU_NET_PIN_MISMATCH and closes the socket. */
enum bambu_net_result bambu_tls_connect(
    struct bambu_tls_credentials *credentials,
    const char *host,
    uint16_t port,
    const char *pinned_fingerprint,
    uint32_t timeout_ms,
    struct bambu_cancel *cancel,
    struct bambu_tls **connection,
    char seen_fingerprint[BAMBU_FINGERPRINT_CAPACITY]
);

/* Reads whatever is available, up to capacity. BAMBU_NET_TIMEOUT means no data
 * arrived in time and the connection is still usable. */
enum bambu_net_result bambu_tls_read(
    struct bambu_tls *connection,
    void *buffer,
    size_t capacity,
    uint32_t timeout_ms,
    struct bambu_cancel *cancel,
    size_t *received
);
enum bambu_net_result bambu_tls_write(
    struct bambu_tls *connection,
    const void *data,
    size_t length,
    uint32_t timeout_ms,
    struct bambu_cancel *cancel
);
void bambu_tls_close(struct bambu_tls *connection);

/* True when the handshake resumed an earlier session of the same credentials. */
bool bambu_tls_session_reused(const struct bambu_tls *connection);

struct bambu_ftps_request {
    const char *host;
    uint16_t port;
    const char *access_code;
    const char *pinned_fingerprint;
    const char *const *candidates;
    size_t candidate_count;
    int current_year;
    size_t max_archive_bytes;
};

struct bambu_ftps_result {
    uint8_t *archive; /* malloc'd, owned by the caller */
    size_t archive_length;
    char name[BAMBU_FTP_NAME_CAPACITY];
    bool matched_by_name;
    bool data_session_reused;
};

/* Logs in, lists the root and cache directories, and downloads the best archive. */
enum bambu_net_result bambu_ftps_fetch_archive(
    const struct bambu_ftps_request *request,
    struct bambu_cancel *cancel,
    struct bambu_ftps_result *result
);

/* HTTPS GET with full certificate validation and no redirects. The URL must
 * already have passed bambu_validate_remote_url(). bearer may be NULL. */
enum bambu_net_result bambu_https_get(
    const char *url,
    const char *bearer,
    size_t max_bytes,
    struct bambu_cancel *cancel,
    uint8_t **body,
    size_t *length,
    int *status
);
