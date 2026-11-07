#pragma once

/* One background connection per printer profile, shared by every overlay source
 * using it. A service owns its worker threads: when the last reference is
 * released they are stopped and joined before release returns, so no worker can
 * outlive its consumers or touch their memory.
 *
 * Nothing here blocks on the network from a caller's thread. */

#include "bambu-credentials.h"
#include "bambu-image-win.h"
#include "bambu-net-win.h"
#include "bambu-profiles.h"
#include "printer-snapshot.h"

#define BAMBU_DEFAULT_MQTT_PORT 8883
#define BAMBU_DEFAULT_FTPS_PORT 990

struct bambu_service;

struct bambu_service_config {
    struct bambu_profile profile;
    char access_code[BAMBU_ACCESS_CODE_CAPACITY];
    char cloud_token[BAMBU_CLOUD_TOKEN_CAPACITY];
    uint16_t mqtt_port;
    uint16_t ftps_port;
    char cache_dir[1024];
};

/* Fills config for a profile, or returns false if it does not exist. Called on
 * worker threads at the start of every connection attempt, so edits apply on the
 * next attempt. Tests inject their own loader to avoid the credential store. */
typedef bool (*bambu_service_config_loader)(const char *profile_id, struct bambu_service_config *config);

void bambu_services_init(bambu_service_config_loader loader);
void bambu_services_shutdown(void);

/* Monotonic clock shared with snapshot timestamps. */
uint64_t bambu_monotonic_ms(void);

struct bambu_service *bambu_service_acquire(const char *profile_id);
void bambu_service_release(struct bambu_service *service);
int bambu_service_count(void);
int bambu_service_reference_count(const struct bambu_service *service);

/* Cheap change check, safe to call from any thread at frame rate. */
uint64_t bambu_service_revision(const struct bambu_service *service);
void bambu_service_read(struct bambu_service *service, struct bambu_printer_snapshot *snapshot);

/* Returns a retained image, or NULL. The caller must release it. */
struct bambu_shared_image *bambu_service_preview(struct bambu_service *service);

void bambu_service_reconnect(struct bambu_service *service);
void bambu_service_clear_preview(struct bambu_service *service);

/* Applies edited profile settings to a running service, if there is one. */
void bambu_services_profile_changed(const char *profile_id);

/* Redacted human-readable status. Safe to share. Returns bytes written. */
size_t bambu_service_diagnostics(struct bambu_service *service, char *out, size_t capacity);

/* The same report for every running service. */
size_t bambu_services_diagnostics(char *out, size_t capacity);

struct bambu_certificate_probe {
    enum bambu_net_result mqtt_result;
    enum bambu_net_result ftps_result;
    char mqtt_fingerprint[BAMBU_FINGERPRINT_CAPACITY];
    char ftps_fingerprint[BAMBU_FINGERPRINT_CAPACITY];
};

/* Step one of a connection test: reads each service's certificate and sends no
 * credentials. Blocking, so run it off the UI thread. */
void bambu_probe_certificates(
    const char *host,
    uint16_t mqtt_port,
    uint16_t ftps_port,
    struct bambu_cancel *cancel,
    struct bambu_certificate_probe *probe
);

/* Step two, only after the user has trusted the certificate: tries the access
 * code over the pinned connection. Blocking. */
enum bambu_net_result bambu_probe_login(
    const char *host,
    uint16_t mqtt_port,
    const char *pinned_fingerprint,
    const char *access_code,
    struct bambu_cancel *cancel
);
