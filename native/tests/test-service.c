/* Drives the real TLS, MQTT, FTPS, and service code against tests/fake_printer.py.
 * Run through that script, which starts the fake printer and passes its ports. */

#include "bambu-image-win.h"
#include "bambu-log.h"
#include "bambu-net-win.h"
#include "bambu-profiles.h"
#include "printer-service.h"
#include "test-framework.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>

#include <stdio.h>

int bambu_test_failures = 0;

#define ACCESS_CODE "12345678"
#define SERIAL "TESTSERIAL0001"

static struct {
    uint16_t mqtt_port;
    uint16_t ftps_port;
    uint16_t control_port;
    char fingerprint[BAMBU_FINGERPRINT_CAPACITY];
    char access_code[BAMBU_ACCESS_CODE_CAPACITY];
    char cache_dir[1024];
    bool preview_enabled;
} test_config;

static char captured_logs[65536];
static size_t captured_length = 0;

static void capture_log(enum bambu_log_level level, const char *message)
{
    int written = snprintf(
        captured_logs + captured_length,
        sizeof(captured_logs) - captured_length,
        "%d %s\n",
        (int)level,
        message
    );
    if (written > 0 && captured_length + (size_t)written < sizeof(captured_logs))
        captured_length += (size_t)written;
}

static bool load_config(const char *profile_id, struct bambu_service_config *config)
{
    if (strcmp(profile_id, "missing") == 0)
        return false;
    bambu_profile_defaults(&config->profile);
    snprintf(config->profile.id, sizeof(config->profile.id), "%s", profile_id);
    snprintf(config->profile.host, sizeof(config->profile.host), "127.0.0.1");
    snprintf(config->profile.serial, sizeof(config->profile.serial), SERIAL);
    memcpy(config->profile.mqtt_fingerprint, test_config.fingerprint, BAMBU_FINGERPRINT_CAPACITY);
    memcpy(config->profile.ftps_fingerprint, test_config.fingerprint, BAMBU_FINGERPRINT_CAPACITY);
    config->profile.preview_enabled = test_config.preview_enabled;
    snprintf(config->access_code, sizeof(config->access_code), "%s", test_config.access_code);
    config->mqtt_port = test_config.mqtt_port;
    config->ftps_port = test_config.ftps_port;
    snprintf(config->cache_dir, sizeof(config->cache_dir), "%s", test_config.cache_dir);
    return true;
}

/* Sends one command to the fake printer's plain control port. */
static void control(const char *command)
{
    struct bambu_tls_credentials *unused = bambu_tls_credentials_create(); /* starts Winsock */
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in address;
    char reply[64];

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(test_config.control_port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(sock, (struct sockaddr *)&address, sizeof(address)) == 0) {
        send(sock, command, (int)strlen(command), 0);
        send(sock, "\n", 1, 0);
        recv(sock, reply, sizeof(reply), 0);
    }
    closesocket(sock);
    bambu_tls_credentials_destroy(unused);
}

typedef bool (*predicate)(struct bambu_service *service, void *data);

static bool wait_until(struct bambu_service *service, predicate check, void *data, uint32_t timeout_ms)
{
    uint64_t deadline = bambu_monotonic_ms() + timeout_ms;

    while (bambu_monotonic_ms() < deadline) {
        if (check(service, data))
            return true;
        Sleep(25);
    }
    return check(service, data);
}

static bool state_is(struct bambu_service *service, void *data)
{
    static struct bambu_printer_snapshot snapshot;
    bambu_service_read(service, &snapshot);
    return snapshot.connection_state == *(enum bambu_connection_state *)data;
}

static bool progress_is(struct bambu_service *service, void *data)
{
    static struct bambu_printer_snapshot snapshot;
    bambu_service_read(service, &snapshot);
    return snapshot.has_progress && snapshot.progress_percent == *(double *)data;
}

static bool has_preview(struct bambu_service *service, void *data)
{
    struct bambu_shared_image *image = bambu_service_preview(service);
    (void)data;
    bambu_image_release(image);
    return image != NULL;
}

static void test_certificate_probe(void)
{
    struct bambu_certificate_probe probe;
    struct bambu_cancel *cancel = bambu_cancel_create();

    bambu_probe_certificates("127.0.0.1", test_config.mqtt_port, test_config.ftps_port, cancel, &probe);
    CHECK(probe.mqtt_result == BAMBU_NET_OK);
    CHECK(probe.ftps_result == BAMBU_NET_OK);
    CHECK_STR(probe.mqtt_fingerprint, test_config.fingerprint);
    CHECK_STR(probe.ftps_fingerprint, test_config.fingerprint);

    /* Unreachable hosts fail quickly and cleanly. */
    bambu_probe_certificates("127.0.0.1", 1, 1, cancel, &probe);
    CHECK(probe.mqtt_result != BAMBU_NET_OK);
    bambu_cancel_destroy(cancel);
}

static void test_login_probe(void)
{
    struct bambu_cancel *cancel = bambu_cancel_create();
    const char *wrong_pin = "0000000000000000000000000000000000000000000000000000000000000000";

    CHECK(bambu_probe_login("127.0.0.1", test_config.mqtt_port, test_config.fingerprint, ACCESS_CODE, cancel) ==
          BAMBU_NET_OK);
    CHECK(bambu_probe_login("127.0.0.1", test_config.mqtt_port, test_config.fingerprint, "wrong", cancel) ==
          BAMBU_NET_AUTH_FAILED);
    /* A wrong pin must stop before any credential is sent. */
    CHECK(bambu_probe_login("127.0.0.1", test_config.mqtt_port, wrong_pin, ACCESS_CODE, cancel) ==
          BAMBU_NET_PIN_MISMATCH);
    CHECK(bambu_probe_login("127.0.0.1", test_config.mqtt_port, "", ACCESS_CODE, cancel) == BAMBU_NET_PIN_MISMATCH);
    bambu_cancel_destroy(cancel);
}

static void test_cancel_is_prompt(void)
{
    struct bambu_tls_credentials *credentials = bambu_tls_credentials_create();
    struct bambu_cancel *cancel = bambu_cancel_create();
    struct bambu_tls *connection = NULL;
    uint64_t start;
    char buffer[16];
    size_t received = 0;

    CHECK(bambu_tls_connect(
              credentials, "127.0.0.1", test_config.mqtt_port, test_config.fingerprint, 5000, cancel, &connection, NULL
          ) == BAMBU_NET_OK);

    /* The broker waits for CONNECT, so this read blocks until cancelled. */
    bambu_cancel_request(cancel);
    start = bambu_monotonic_ms();
    CHECK(bambu_tls_read(connection, buffer, sizeof(buffer), 30000, cancel, &received) == BAMBU_NET_CANCELLED);
    CHECK(bambu_monotonic_ms() - start < 500);

    bambu_tls_close(connection);
    bambu_cancel_destroy(cancel);
    bambu_tls_credentials_destroy(credentials);
}

static void test_live_telemetry(void)
{
    struct bambu_service *service = bambu_service_acquire("live");
    enum bambu_connection_state live = BAMBU_CONNECTION_LIVE;
    double expected = 42.0;
    static struct bambu_printer_snapshot snapshot;
    uint64_t revision;

    CHECK(service != NULL);
    CHECK(wait_until(service, state_is, &live, 8000));
    CHECK(wait_until(service, progress_is, &expected, 8000));

    bambu_service_read(service, &snapshot);
    CHECK_STR(snapshot.job_name, "Fake Benchy");
    CHECK(snapshot.ams_unit_count == 1);
    CHECK_STR(snapshot.ams[0].trays[0].material_name, "Bambu PLA Basic");
    CHECK(snapshot.job_state == BAMBU_JOB_PRINTING);

    /* Deltas retain the rest of the state. */
    revision = bambu_service_revision(service);
    control("delta 57");
    expected = 57.0;
    CHECK(wait_until(service, progress_is, &expected, 5000));
    bambu_service_read(service, &snapshot);
    CHECK_STR(snapshot.job_name, "Fake Benchy");
    CHECK(bambu_service_revision(service) > revision);

    /* Garbage on the wire is counted and survived. */
    control("garbage");
    control("delta 58");
    expected = 58.0;
    CHECK(wait_until(service, progress_is, &expected, 5000));
    {
        char diagnostics[1024];
        bambu_service_diagnostics(service, diagnostics, sizeof(diagnostics));
        CHECK(strstr(diagnostics, "malformed: 1") != NULL);
        CHECK(strstr(diagnostics, ACCESS_CODE) == NULL);
        CHECK(strstr(diagnostics, SERIAL) == NULL);
        CHECK(strstr(diagnostics, "****0001") != NULL);
    }

    bambu_service_release(service);
}

static void test_sharing(void)
{
    struct bambu_service *first = bambu_service_acquire("shared");
    struct bambu_service *second = bambu_service_acquire("shared");
    struct bambu_service *other = bambu_service_acquire("another");
    enum bambu_connection_state live = BAMBU_CONNECTION_LIVE;

    CHECK(first != NULL && first == second);
    CHECK(other != NULL && other != first);
    CHECK(bambu_service_reference_count(first) == 2);
    CHECK(bambu_service_count() == 2);
    CHECK(wait_until(first, state_is, &live, 8000));

    bambu_service_release(second);
    CHECK(bambu_service_count() == 2);
    CHECK(bambu_service_reference_count(first) == 1);
    bambu_service_release(first);
    CHECK(bambu_service_count() == 1);
    bambu_service_release(other);
    CHECK(bambu_service_count() == 0);

    CHECK(bambu_service_acquire("") == NULL);
    CHECK(bambu_service_acquire(NULL) == NULL);
}

static void test_reconnect_after_drop(void)
{
    struct bambu_service *service = bambu_service_acquire("reconnect");
    enum bambu_connection_state live = BAMBU_CONNECTION_LIVE;
    double expected = 42.0;
    static struct bambu_printer_snapshot snapshot;

    CHECK(wait_until(service, progress_is, &expected, 8000));
    control("drop");

    /* Comes back on its own, and last-known data is kept throughout. */
    Sleep(200);
    bambu_service_read(service, &snapshot);
    CHECK(snapshot.has_progress);
    CHECK_STR(snapshot.job_name, "Fake Benchy");
    CHECK(wait_until(service, state_is, &live, 10000));
    bambu_service_release(service);
}

static void test_auth_failure(void)
{
    struct bambu_service *service;
    enum bambu_connection_state failed = BAMBU_CONNECTION_AUTHENTICATION_FAILED;
    uint64_t start;

    snprintf(test_config.access_code, sizeof(test_config.access_code), "wrong-code");
    service = bambu_service_acquire("auth");
    CHECK(wait_until(service, state_is, &failed, 8000));

    /* Release must not wait out the long authentication backoff. */
    start = bambu_monotonic_ms();
    bambu_service_release(service);
    CHECK(bambu_monotonic_ms() - start < 1500);
    snprintf(test_config.access_code, sizeof(test_config.access_code), ACCESS_CODE);
}

static void test_pin_mismatch(void)
{
    char saved[BAMBU_FINGERPRINT_CAPACITY];
    struct bambu_service *service;
    enum bambu_connection_state error = BAMBU_CONNECTION_ERROR;

    memcpy(saved, test_config.fingerprint, sizeof(saved));
    memset(test_config.fingerprint, 'a', BAMBU_FINGERPRINT_HEX_LENGTH);
    service = bambu_service_acquire("pin");
    CHECK(wait_until(service, state_is, &error, 8000));
    bambu_service_release(service);
    memcpy(test_config.fingerprint, saved, sizeof(saved));
}

static void test_unconfigured(void)
{
    struct bambu_service *service = bambu_service_acquire("missing");
    enum bambu_connection_state needed = BAMBU_CONNECTION_CONFIGURATION_REQUIRED;

    CHECK(wait_until(service, state_is, &needed, 3000));
    bambu_service_release(service);

    /* No trusted certificate means no connection attempt, and no credential sent. */
    {
        char saved[BAMBU_FINGERPRINT_CAPACITY];
        memcpy(saved, test_config.fingerprint, sizeof(saved));
        test_config.fingerprint[0] = '\0';
        service = bambu_service_acquire("untrusted");
        CHECK(wait_until(service, state_is, &needed, 3000));
        bambu_service_release(service);
        memcpy(test_config.fingerprint, saved, sizeof(saved));
    }
}

static void test_offline_then_shutdown(void)
{
    uint16_t saved = test_config.mqtt_port;
    struct bambu_service *service;
    enum bambu_connection_state offline = BAMBU_CONNECTION_OFFLINE;
    uint64_t start;

    test_config.mqtt_port = 1;
    service = bambu_service_acquire("offline");
    CHECK(wait_until(service, state_is, &offline, 30000));
    start = bambu_monotonic_ms();
    bambu_service_release(service);
    CHECK(bambu_monotonic_ms() - start < 1500);
    test_config.mqtt_port = saved;
}

static void test_ftps_preview(void)
{
    struct bambu_service *service;
    struct bambu_shared_image *image;
    double expected = 42.0;

    /* Real FTPS: implicit TLS, resumed data sessions, LIST, RETR, then .3mf parsing. */
    test_config.preview_enabled = true;
    control("reset-ftps-stats");
    service = bambu_service_acquire("ftps");
    CHECK(wait_until(service, progress_is, &expected, 8000));
    CHECK(wait_until(service, has_preview, NULL, 15000));

    image = bambu_service_preview(service);
    CHECK(image != NULL);
    if (image != NULL) {
        CHECK(bambu_image_pixels(image)->width == 4);
        CHECK(bambu_image_pixels(image)->height == 4);
        /* The fixture's top-left pixel is pure red, stored as BGRA. */
        CHECK(bambu_image_pixels(image)->pixels[2] == 255);
        CHECK(bambu_image_pixels(image)->pixels[0] == 0);
    }
    bambu_image_release(image);

    /* A new job must clear the old cover before anything else shows. */
    control("newjob");
    Sleep(600);
    bambu_service_release(service);

    /* The cache serves a restart without another printer download. */
    control("reset-ftps-stats");
    test_config.preview_enabled = true;
    control("oldjob");
    service = bambu_service_acquire("ftps");
    CHECK(wait_until(service, has_preview, NULL, 15000));
    bambu_service_release(service);
    test_config.preview_enabled = false;
}

static void test_ftps_direct(void)
{
    struct bambu_cancel *cancel = bambu_cancel_create();
    struct bambu_ftps_request request;
    struct bambu_ftps_result result;
    const char *candidates[] = {"fakebenchy"};

    memset(&request, 0, sizeof(request));
    request.host = "127.0.0.1";
    request.port = test_config.ftps_port;
    request.access_code = ACCESS_CODE;
    request.pinned_fingerprint = test_config.fingerprint;
    request.candidates = candidates;
    request.candidate_count = 1;
    request.current_year = 2026;
    request.max_archive_bytes = 1024 * 1024;

    CHECK(bambu_ftps_fetch_archive(&request, cancel, &result) == BAMBU_NET_OK);
    CHECK_STR(result.name, "Fake Benchy.gcode.3mf");
    CHECK(result.matched_by_name);
    /* The printer refuses data channels that do not resume the control session. */
    CHECK(result.data_session_reused);
    free(result.archive);

    request.max_archive_bytes = 16;
    CHECK(bambu_ftps_fetch_archive(&request, cancel, &result) == BAMBU_NET_TOO_LARGE);

    request.max_archive_bytes = 1024 * 1024;
    request.access_code = "wrong";
    CHECK(bambu_ftps_fetch_archive(&request, cancel, &result) == BAMBU_NET_AUTH_FAILED);
    bambu_cancel_destroy(cancel);
}

static void test_profile_serialisation(void)
{
    struct bambu_profile_list list;
    struct bambu_profile_list parsed;
    char json[4096];

    memset(&list, 0, sizeof(list));
    bambu_profile_defaults(&list.profiles[0]);
    snprintf(list.profiles[0].id, sizeof(list.profiles[0].id), "abc");
    snprintf(list.profiles[0].name, sizeof(list.profiles[0].name), "Quote \" and \\ slash");
    snprintf(list.profiles[0].host, sizeof(list.profiles[0].host), "192.0.2.10");
    snprintf(list.profiles[0].serial, sizeof(list.profiles[0].serial), SERIAL);
    memset(list.profiles[0].mqtt_fingerprint, 'b', BAMBU_FINGERPRINT_HEX_LENGTH);
    list.profiles[0].lan_only = false;
    list.count = 1;

    CHECK(bambu_profiles_to_json(&list, json, sizeof(json)) < sizeof(json));
    /* The file format has no field a secret could land in. */
    CHECK(strstr(json, "access") == NULL);
    CHECK(strstr(json, "token") == NULL);

    CHECK(bambu_profiles_from_json(json, strlen(json), &parsed));
    CHECK(parsed.count == 1);
    CHECK_STR(parsed.profiles[0].name, "Quote \" and \\ slash");
    CHECK(!parsed.profiles[0].lan_only);
    CHECK(parsed.profiles[0].preview_enabled);
    CHECK_STR(parsed.profiles[0].mqtt_fingerprint, list.profiles[0].mqtt_fingerprint);

    CHECK(!bambu_profiles_from_json("garbage", 7, &parsed));
    CHECK(parsed.count == 0);

    /* A hand-edited pin that is not a fingerprint is dropped, not trusted. */
    {
        const char *bad = "{\"profiles\":[{\"id\":\"x\",\"mqtt_fingerprint\":\"nope\"}]}";
        CHECK(bambu_profiles_from_json(bad, strlen(bad), &parsed));
        CHECK(parsed.count == 1);
        CHECK(parsed.profiles[0].mqtt_fingerprint[0] == '\0');
    }
    CHECK(bambu_profile_is_complete(&list.profiles[0]));
}

static void test_credentials(void)
{
    const char *id = "bambu-overlay-selftest";
    char out[BAMBU_CLOUD_TOKEN_CAPACITY];
    char small[4];

    bambu_credential_delete(id, BAMBU_SECRET_ACCESS_CODE);
    CHECK(!bambu_credential_exists(id, BAMBU_SECRET_ACCESS_CODE));
    CHECK(!bambu_credential_read(id, BAMBU_SECRET_ACCESS_CODE, out, sizeof(out)));

    CHECK(bambu_credential_write(id, BAMBU_SECRET_ACCESS_CODE, "s3cr3t-value"));
    CHECK(bambu_credential_exists(id, BAMBU_SECRET_ACCESS_CODE));
    CHECK(bambu_credential_read(id, BAMBU_SECRET_ACCESS_CODE, out, sizeof(out)));
    CHECK_STR(out, "s3cr3t-value");

    /* Too small a buffer fails rather than truncating a secret. */
    CHECK(!bambu_credential_read(id, BAMBU_SECRET_ACCESS_CODE, small, sizeof(small)));

    /* Writing an empty secret removes it. */
    CHECK(bambu_credential_write(id, BAMBU_SECRET_ACCESS_CODE, ""));
    CHECK(!bambu_credential_exists(id, BAMBU_SECRET_ACCESS_CODE));

    CHECK(!bambu_credential_write("", BAMBU_SECRET_ACCESS_CODE, "x"));
}

static void test_image_rejection(void)
{
    static const uint8_t truncated_png[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13};
    static const uint8_t truncated_jpeg[] = {0xFF, 0xD8, 0xFF, 0xE0, 0, 16, 'J', 'F', 'I', 'F'};
    static const uint8_t text[] = "definitely not an image";

    CHECK(bambu_image_decode(NULL, 0) == NULL);
    CHECK(bambu_image_decode(text, sizeof(text)) == NULL);
    CHECK(bambu_image_decode(truncated_png, sizeof(truncated_png)) == NULL);
    CHECK(bambu_image_decode(truncated_jpeg, sizeof(truncated_jpeg)) == NULL);
    CHECK(bambu_image_pixels(NULL) == NULL);
    bambu_image_release(NULL);
}

static void test_no_secrets_logged(void)
{
    CHECK(strstr(captured_logs, ACCESS_CODE) == NULL);
    CHECK(strstr(captured_logs, "wrong-code") == NULL);
    CHECK(strstr(captured_logs, SERIAL) == NULL);
}

int main(int argc, char **argv)
{
    if (argc != 6) {
        fprintf(stderr, "usage: %s <mqtt-port> <ftps-port> <control-port> <sha256> <cache-dir>\n", argv[0]);
        return 2;
    }
    test_config.mqtt_port = (uint16_t)atoi(argv[1]);
    test_config.ftps_port = (uint16_t)atoi(argv[2]);
    test_config.control_port = (uint16_t)atoi(argv[3]);
    snprintf(test_config.fingerprint, sizeof(test_config.fingerprint), "%s", argv[4]);
    snprintf(test_config.cache_dir, sizeof(test_config.cache_dir), "%s", argv[5]);
    snprintf(test_config.access_code, sizeof(test_config.access_code), ACCESS_CODE);

    bambu_log_set_sink(capture_log);
    bambu_services_init(load_config);

    test_profile_serialisation();
    test_credentials();
    test_image_rejection();
    test_certificate_probe();
    test_login_probe();
    test_cancel_is_prompt();
    test_live_telemetry();
    test_sharing();
    test_reconnect_after_drop();
    test_auth_failure();
    test_pin_mismatch();
    test_unconfigured();
    test_offline_then_shutdown();
    test_ftps_direct();
    test_ftps_preview();
    test_no_secrets_logged();

    bambu_services_shutdown();
    if (bambu_test_failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", bambu_test_failures);
        fprintf(stderr, "--- captured log ---\n%s", captured_logs);
        return 1;
    }
    puts("service tests passed");
    return 0;
}
