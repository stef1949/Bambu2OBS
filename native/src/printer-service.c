#include "printer-service.h"

#include "bambu-json.h"
#include "bambu-log.h"
#include "bambu-mqtt-codec.h"
#include "bambu-preview-select.h"
#include "bambu-report.h"
#include "bambu-zip.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <process.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONNECT_TIMEOUT_MS 10000
#define WRITE_TIMEOUT_MS 10000
#define READ_SLICE_MS 250
#define CONNACK_TIMEOUT_MS 10000
#define KEEPALIVE_SECONDS 30
#define PING_INTERVAL_MS 20000
#define LINK_DEAD_AFTER_MS 45000
#define BACKOFF_BASE_MS 1000
#define BACKOFF_MAX_MS 120000
#define AUTH_BACKOFF_MS 60000
#define OFFLINE_AFTER_ATTEMPTS 3
#define PREVIEW_RETRY_MS 45000
#define PREVIEW_MAX_ATTEMPTS 6
#define MAX_CLOUD_RESPONSE_BYTES (4u * 1024u * 1024u)
#define JOB_KEY_CAPACITY 640
#define ERROR_CAPACITY 160

struct bambu_service {
    char profile_id[BAMBU_PROFILE_ID_CAPACITY];
    int references; /* guarded by registry_lock */
    struct bambu_service *next;

    CRITICAL_SECTION lock;
    struct bambu_printer_snapshot snapshot;
    struct bambu_shared_image *preview;
    volatile LONG64 revision;

    /* Preview bookkeeping, guarded by lock. */
    char preview_key[JOB_KEY_CAPACITY];
    bool preview_done;
    int preview_attempts;

    HANDLE connection_thread;
    HANDLE preview_thread;
    HANDLE wake_event;
    HANDLE preview_event;
    struct bambu_cancel *connection_cancel;
    struct bambu_cancel *preview_cancel;
    volatile LONG stop;
    volatile LONG restart;
    bool stopped; /* workers already joined; guarded by registry_lock */

    uint64_t random_state;

    /* Counters, guarded by lock. */
    uint64_t messages;
    uint64_t malformed;
    uint64_t connects;
    char last_error[ERROR_CAPACITY];
    char preview_status[ERROR_CAPACITY];
};

static CRITICAL_SECTION registry_lock;
static bool registry_ready = false;
static struct bambu_service *registry_head = NULL;
static bambu_service_config_loader config_loader = NULL;

uint64_t bambu_monotonic_ms(void)
{
    return GetTickCount64();
}

static double next_random(struct bambu_service *service)
{
    uint64_t x = service->random_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    service->random_state = x;
    return (double)(x >> 11) / 9007199254740992.0;
}

static void publish_revision(struct bambu_service *service)
{
    service->snapshot.revision++;
    InterlockedExchange64(&service->revision, (LONG64)service->snapshot.revision);
}

static void set_state(struct bambu_service *service, enum bambu_connection_state state)
{
    bool changed;

    EnterCriticalSection(&service->lock);
    changed = service->snapshot.connection_state != state;
    if (changed) {
        service->snapshot.connection_state = state;
        publish_revision(service);
    }
    LeaveCriticalSection(&service->lock);
    if (changed)
        bambu_log(BAMBU_LOG_INFO, "printer connection: %s", bambu_connection_state_name(state));
}

/* Repeated failures of the same kind are logged once, not once per retry. */
static void set_error(struct bambu_service *service, const char *message)
{
    bool repeated;

    EnterCriticalSection(&service->lock);
    repeated = strcmp(service->last_error, message) == 0;
    if (!repeated)
        snprintf(service->last_error, sizeof(service->last_error), "%s", message);
    LeaveCriticalSection(&service->lock);
    if (!repeated && message[0] != '\0')
        bambu_log(BAMBU_LOG_WARNING, "printer connection failed: %s", message);
}

static bool stop_requested(const struct bambu_service *service)
{
    return service->stop != 0;
}

/* Sleeps until the delay passes or the service is woken. */
static void interruptible_wait(struct bambu_service *service, uint32_t delay_ms)
{
    WaitForSingleObject(service->wake_event, delay_ms);
}

static bool job_wants_preview(const struct bambu_printer_snapshot *snapshot)
{
    return snapshot->job_state == BAMBU_JOB_PREPARING || snapshot->job_state == BAMBU_JOB_PRINTING ||
           snapshot->job_state == BAMBU_JOB_PAUSED || snapshot->job_state == BAMBU_JOB_FINISHED;
}

/* Must be called with lock held. */
static void update_preview_request(struct bambu_service *service, bool job_changed)
{
    char key[JOB_KEY_CAPACITY];

    if (job_changed && service->preview != NULL) {
        /* The old cover must not sit next to the new job's title. */
        bambu_image_release(service->preview);
        service->preview = NULL;
        service->snapshot.preview_revision++;
        publish_revision(service);
    }
    if (!job_wants_preview(&service->snapshot))
        return;
    bambu_preview_job_key(&service->snapshot, key, sizeof(key));
    if (strcmp(key, "unknown_job") == 0 || strcmp(key, service->preview_key) == 0)
        return;
    snprintf(service->preview_key, sizeof(service->preview_key), "%s", key);
    service->preview_done = false;
    service->preview_attempts = 0;
    SetEvent(service->preview_event);
}

static void handle_report(struct bambu_service *service, const char *payload, size_t length)
{
    struct bambu_report_events events;
    uint64_t revision;
    enum bambu_report_result result;

    EnterCriticalSection(&service->lock);
    revision = service->snapshot.revision;
    result = bambu_report_apply(&service->snapshot, payload, length, bambu_monotonic_ms(), &events);
    service->messages++;
    if (result == BAMBU_REPORT_INVALID)
        service->malformed++;
    if (result == BAMBU_REPORT_APPLIED) {
        update_preview_request(service, events.job_changed);
        if (service->snapshot.revision != revision)
            InterlockedExchange64(&service->revision, (LONG64)service->snapshot.revision);
    }
    LeaveCriticalSection(&service->lock);
}

/* A live link with no news is not stale data, so keepalive replies count as
 * freshness. P1-series printers can stay silent for long periods when idle. */
static void touch_freshness(struct bambu_service *service)
{
    EnterCriticalSection(&service->lock);
    service->snapshot.updated_monotonic_ms = bambu_monotonic_ms();
    LeaveCriticalSection(&service->lock);
}

static enum bambu_net_result send_packet(
    struct bambu_service *service,
    struct bambu_tls *connection,
    const uint8_t *packet,
    size_t length
)
{
    if (length == 0)
        return BAMBU_NET_PROTOCOL_ERROR;
    return bambu_tls_write(connection, packet, length, WRITE_TIMEOUT_MS, service->connection_cancel);
}

static bool serial_is_safe(const char *serial)
{
    /* The serial becomes an MQTT topic, where + and # are wildcards. */
    for (; *serial != '\0'; serial++) {
        if (!isalnum((unsigned char)*serial))
            return false;
    }
    return true;
}

struct mqtt_session {
    bool connack_seen;
    bool subscribed;
    bool auth_rejected;
    char report_topic[BAMBU_SERIAL_CAPACITY + 32];
    char request_topic[BAMBU_SERIAL_CAPACITY + 32];
};

static enum bambu_net_result handle_packet(
    struct bambu_service *service,
    struct bambu_tls *connection,
    struct mqtt_session *session,
    const struct bambu_mqtt_packet *packet
)
{
    static const char pushall[] = "{\"pushing\":{\"sequence_id\":\"0\",\"command\":\"pushall\"}}";
    uint8_t scratch[256];

    switch (packet->type) {
    case BAMBU_MQTT_CONNACK:
        if (packet->connack_code == 4 || packet->connack_code == 5) {
            session->auth_rejected = true;
            return BAMBU_NET_AUTH_FAILED;
        }
        if (packet->connack_code != 0)
            return BAMBU_NET_PROTOCOL_ERROR;
        session->connack_seen = true;
        return send_packet(
            service,
            connection,
            scratch,
            bambu_mqtt_encode_subscribe(scratch, sizeof(scratch), 1, session->report_topic)
        );
    case BAMBU_MQTT_SUBACK:
        if (packet->suback_code & 0x80)
            return BAMBU_NET_AUTH_FAILED;
        session->subscribed = true;
        EnterCriticalSection(&service->lock);
        service->connects++;
        service->last_error[0] = '\0';
        service->snapshot.updated_monotonic_ms = bambu_monotonic_ms();
        LeaveCriticalSection(&service->lock);
        set_state(service, BAMBU_CONNECTION_LIVE);
        /* One full-state request per connection: P1-series firmware only sends
         * deltas, and asking repeatedly is known to disturb it. */
        return send_packet(
            service,
            connection,
            scratch,
            bambu_mqtt_encode_publish(scratch, sizeof(scratch), session->request_topic, pushall, sizeof(pushall) - 1)
        );
    case BAMBU_MQTT_PUBLISH:
        if (packet->topic_length == strlen(session->report_topic) &&
            memcmp(packet->topic, session->report_topic, packet->topic_length) == 0)
            handle_report(service, packet->payload, packet->payload_length);
        return BAMBU_NET_OK;
    case BAMBU_MQTT_PINGRESP:
        touch_freshness(service);
        return BAMBU_NET_OK;
    default:
        return BAMBU_NET_OK;
    }
}

/* Runs one MQTT session until it ends. Always returns a non-OK result. */
static enum bambu_net_result run_session(struct bambu_service *service, const struct bambu_service_config *config)
{
    struct bambu_tls_credentials *credentials = bambu_tls_credentials_create();
    struct bambu_tls *connection = NULL;
    struct mqtt_session session;
    uint8_t *buffer = NULL;
    size_t buffered = 0;
    size_t capacity = BAMBU_MQTT_MAX_PACKET + 16;
    uint8_t connect_packet[256];
    uint64_t session_start;
    uint64_t last_received;
    uint64_t last_ping;
    char client_id[48];
    enum bambu_net_result result;

    if (credentials == NULL)
        return BAMBU_NET_TLS_FAILED;
    memset(&session, 0, sizeof(session));
    snprintf(session.report_topic, sizeof(session.report_topic), "device/%s/report", config->profile.serial);
    snprintf(session.request_topic, sizeof(session.request_topic), "device/%s/request", config->profile.serial);
    snprintf(client_id, sizeof(client_id), "bambu-overlay-%08x", (unsigned)(next_random(service) * 4294967295.0));

    result = bambu_tls_connect(
        credentials,
        config->profile.host,
        config->mqtt_port,
        config->profile.mqtt_fingerprint,
        CONNECT_TIMEOUT_MS,
        service->connection_cancel,
        &connection,
        NULL
    );
    if (result != BAMBU_NET_OK)
        goto done;

    buffer = malloc(capacity);
    if (buffer == NULL) {
        result = BAMBU_NET_TOO_LARGE;
        goto done;
    }

    result = send_packet(
        service,
        connection,
        connect_packet,
        bambu_mqtt_encode_connect(
            connect_packet, sizeof(connect_packet), client_id, "bblp", config->access_code, KEEPALIVE_SECONDS
        )
    );
    SecureZeroMemory(connect_packet, sizeof(connect_packet));
    if (result != BAMBU_NET_OK)
        goto done;

    session_start = bambu_monotonic_ms();
    last_received = session_start;
    last_ping = session_start;

    while (!stop_requested(service) && service->restart == 0) {
        size_t received = 0;
        uint64_t now;

        result = bambu_tls_read(
            connection,
            buffer + buffered,
            capacity - buffered,
            READ_SLICE_MS,
            service->connection_cancel,
            &received
        );
        now = bambu_monotonic_ms();

        if (result == BAMBU_NET_OK) {
            last_received = now;
            buffered += received;
            for (;;) {
                struct bambu_mqtt_packet packet;
                size_t consumed = 0;
                enum bambu_mqtt_decode_result decoded = bambu_mqtt_decode(buffer, buffered, &packet, &consumed);

                if (decoded == BAMBU_MQTT_NEED_MORE)
                    break;
                if (decoded == BAMBU_MQTT_MALFORMED) {
                    result = BAMBU_NET_PROTOCOL_ERROR;
                    goto done;
                }
                result = handle_packet(service, connection, &session, &packet);
                if (result != BAMBU_NET_OK)
                    goto done;
                memmove(buffer, buffer + consumed, buffered - consumed);
                buffered -= consumed;
            }
            if (buffered == capacity) {
                result = BAMBU_NET_TOO_LARGE;
                goto done;
            }
        } else if (result != BAMBU_NET_TIMEOUT) {
            goto done;
        }

        if (!session.connack_seen && now - session_start > CONNACK_TIMEOUT_MS) {
            result = BAMBU_NET_TIMEOUT;
            goto done;
        }
        if (now - last_received > LINK_DEAD_AFTER_MS) {
            result = BAMBU_NET_TIMEOUT;
            goto done;
        }
        if (now - last_ping >= PING_INTERVAL_MS) {
            uint8_t ping[2];
            last_ping = now;
            result = send_packet(service, connection, ping, bambu_mqtt_encode_pingreq(ping, sizeof(ping)));
            if (result != BAMBU_NET_OK)
                goto done;
        }
    }
    result = BAMBU_NET_CANCELLED;

    {
        uint8_t goodbye[2];
        send_packet(service, connection, goodbye, bambu_mqtt_encode_disconnect(goodbye, sizeof(goodbye)));
    }

done:
    bambu_tls_close(connection);
    free(buffer);
    bambu_tls_credentials_destroy(credentials);
    return result;
}

static bool load_config(struct bambu_service *service, struct bambu_service_config *config)
{
    memset(config, 0, sizeof(*config));
    config->mqtt_port = BAMBU_DEFAULT_MQTT_PORT;
    config->ftps_port = BAMBU_DEFAULT_FTPS_PORT;
    return config_loader != NULL && config_loader(service->profile_id, config);
}

static void wipe_config(struct bambu_service_config *config)
{
    SecureZeroMemory(config->access_code, sizeof(config->access_code));
    SecureZeroMemory(config->cloud_token, sizeof(config->cloud_token));
}

static const char *config_problem(const struct bambu_service_config *config)
{
    if (!bambu_profile_is_complete(&config->profile))
        return "profile is missing a host or serial number";
    if (!serial_is_safe(config->profile.serial))
        return "serial number contains invalid characters";
    if (config->access_code[0] == '\0')
        return "access code is not set";
    if (config->profile.mqtt_fingerprint[0] == '\0')
        return "printer certificate is not trusted yet; run Test Connection";
    return NULL;
}

static unsigned __stdcall connection_main(void *argument)
{
    struct bambu_service *service = argument;
    struct bambu_service_config *config = malloc(sizeof(*config));
    uint32_t attempt = 0;
    bool ever_connected = false;

    if (config == NULL)
        return 1;

    while (!stop_requested(service)) {
        const char *problem;
        uint64_t connects_before;
        enum bambu_net_result result;
        uint32_t delay;

        InterlockedExchange(&service->restart, 0);
        bambu_cancel_reset(service->connection_cancel);
        if (stop_requested(service))
            break;

        if (!load_config(service, config)) {
            set_state(service, BAMBU_CONNECTION_CONFIGURATION_REQUIRED);
            set_error(service, "profile does not exist");
            interruptible_wait(service, INFINITE);
            continue;
        }
        problem = config_problem(config);
        if (problem != NULL) {
            wipe_config(config);
            set_state(service, BAMBU_CONNECTION_CONFIGURATION_REQUIRED);
            set_error(service, problem);
            interruptible_wait(service, INFINITE);
            continue;
        }

        set_state(service, ever_connected ? BAMBU_CONNECTION_RECONNECTING : BAMBU_CONNECTION_CONNECTING);

        EnterCriticalSection(&service->lock);
        connects_before = service->connects;
        LeaveCriticalSection(&service->lock);

        result = run_session(service, config);
        wipe_config(config);

        EnterCriticalSection(&service->lock);
        if (service->connects != connects_before) {
            ever_connected = true;
            attempt = 0; /* the link worked, so backoff starts over */
        }
        LeaveCriticalSection(&service->lock);

        if (stop_requested(service))
            break;
        if (service->restart != 0)
            continue;

        if (result == BAMBU_NET_AUTH_FAILED) {
            set_state(service, BAMBU_CONNECTION_AUTHENTICATION_FAILED);
            set_error(service, "the printer rejected the access code");
            /* Hammering a wrong code gains nothing and may trip a lockout. */
            interruptible_wait(service, AUTH_BACKOFF_MS);
            continue;
        }
        if (result == BAMBU_NET_PIN_MISMATCH) {
            set_state(service, BAMBU_CONNECTION_ERROR);
            set_error(service, "printer certificate changed; re-run Test Connection to trust it");
            interruptible_wait(service, INFINITE);
            continue;
        }

        set_error(service, bambu_net_result_name(result));
        attempt++;
        set_state(
            service,
            attempt >= OFFLINE_AFTER_ATTEMPTS ? BAMBU_CONNECTION_OFFLINE : BAMBU_CONNECTION_RECONNECTING
        );
        delay = bambu_backoff_delay_ms(attempt, BACKOFF_BASE_MS, BACKOFF_MAX_MS, next_random(service));
        interruptible_wait(service, delay);
    }

    wipe_config(config);
    free(config);
    return 0;
}

static void set_preview_status(struct bambu_service *service, const char *status)
{
    EnterCriticalSection(&service->lock);
    snprintf(service->preview_status, sizeof(service->preview_status), "%s", status);
    LeaveCriticalSection(&service->lock);
}

/* Installs a decoded image, but only if the job did not change in the meantime. */
static bool install_preview(struct bambu_service *service, const char *key, struct bambu_shared_image *image)
{
    bool installed = false;

    EnterCriticalSection(&service->lock);
    if (strcmp(service->preview_key, key) == 0) {
        bambu_image_release(service->preview);
        service->preview = image;
        service->preview_done = true;
        service->snapshot.preview_revision++;
        publish_revision(service);
        installed = true;
    }
    LeaveCriticalSection(&service->lock);
    if (!installed)
        bambu_image_release(image);
    return installed;
}

static void cache_paths(
    const struct bambu_service *service,
    const struct bambu_service_config *config,
    char *image_path,
    char *key_path,
    size_t capacity
)
{
    snprintf(image_path, capacity, "%s/%s-preview.bin", config->cache_dir, service->profile_id);
    snprintf(key_path, capacity, "%s/%s-preview.key", config->cache_dir, service->profile_id);
}

/* The cache holds one image per profile, so it can never grow, and a new job
 * overwrites it. The key goes last: a crash between writes leaves a mismatch. */
static void cache_store(
    const struct bambu_service *service,
    const struct bambu_service_config *config,
    const char *key,
    const uint8_t *data,
    size_t length
)
{
    char image_path[1200];
    char key_path[1200];

    if (config->cache_dir[0] == '\0')
        return;
    cache_paths(service, config, image_path, key_path, sizeof(image_path));
    if (bambu_write_file_atomic(image_path, data, length))
        bambu_write_file_atomic(key_path, key, strlen(key));
}

static struct bambu_shared_image *cache_load(
    const struct bambu_service *service,
    const struct bambu_service_config *config,
    const char *key
)
{
    char image_path[1200];
    char key_path[1200];
    unsigned char *stored_key = NULL;
    unsigned char *data = NULL;
    size_t length = 0;
    struct bambu_shared_image *image = NULL;

    if (config->cache_dir[0] == '\0')
        return NULL;
    cache_paths(service, config, image_path, key_path, sizeof(image_path));
    if (bambu_read_file(key_path, JOB_KEY_CAPACITY, &stored_key, &length)) {
        if (length == strlen(key) && memcmp(stored_key, key, length) == 0 &&
            bambu_read_file(image_path, BAMBU_PREVIEW_MAX_IMAGE_BYTES, &data, &length)) {
            image = bambu_image_decode(data, length);
            free(data);
        }
        free(stored_key);
    }
    return image;
}

static bool safe_query_value(const char *value)
{
    for (; *value != '\0'; value++) {
        if (!isalnum((unsigned char)*value) && *value != '-' && *value != '_')
            return false;
    }
    return true;
}

static void apply_cloud_metadata(
    struct bambu_service *service,
    const char *key,
    const struct bambu_cloud_task *task
)
{
    EnterCriticalSection(&service->lock);
    if (strcmp(service->preview_key, key) == 0) {
        struct bambu_printer_snapshot *s = &service->snapshot;
        bool local = strcmp(s->print_type, "local") == 0 || strcmp(s->print_type, "lan") == 0;

        /* The newest cloud task is not this job during a LAN print unless the
         * ids agree, and a wrong title is worse than a plain one. */
        if (!local || (s->task_id[0] != '\0' && strcmp(s->task_id, task->id) == 0)) {
            if (task->design_title[0] != '\0') {
                bambu_copy_text(s->job_name, sizeof(s->job_name), task->design_title);
                s->cloud_job_title = true;
            }
            if (task->title[0] != '\0')
                bambu_copy_text(s->print_profile, sizeof(s->print_profile), task->title);
            if (task->has_weight) {
                s->has_total_weight = true;
                s->total_weight_grams = task->weight_grams;
            }
            if (task->has_cost_time) {
                s->has_total_time = true;
                s->total_time_seconds = task->cost_time_seconds;
            }
            publish_revision(service);
        }
    }
    LeaveCriticalSection(&service->lock);
}

static enum bambu_net_result cloud_get_json(
    struct bambu_service *service,
    const char *url,
    const char *token,
    uint8_t **body,
    size_t *length
)
{
    int status = 0;
    return bambu_https_get(url, token, MAX_CLOUD_RESPONSE_BYTES, service->preview_cancel, body, length, &status);
}

static struct bambu_shared_image *fetch_cloud_preview(
    struct bambu_service *service,
    const struct bambu_service_config *config,
    const char *key,
    uint8_t **raw,
    size_t *raw_length
)
{
    const char *base = config->profile.cloud_china_region ? "https://api.bambulab.cn" : "https://api.bambulab.com";
    struct bambu_cloud_task *task = malloc(sizeof(*task));
    struct bambu_shared_image *image = NULL;
    char device_id[128];
    char url[512];
    char api_host[128];
    uint8_t *body = NULL;
    size_t length = 0;

    if (task == NULL)
        return NULL;
    snprintf(device_id, sizeof(device_id), "%s", config->profile.serial);
    snprintf(url, sizeof(url), "%s/v1/iot-service/api/user/bind", base);
    if (cloud_get_json(service, url, config->cloud_token, &body, &length) == BAMBU_NET_OK) {
        bambu_cloud_parse_device_id((const char *)body, length, config->profile.serial, device_id, sizeof(device_id));
        free(body);
        body = NULL;
    }
    if (!safe_query_value(device_id))
        snprintf(device_id, sizeof(device_id), "%s", config->profile.serial);

    snprintf(url, sizeof(url), "%s/v1/user-service/my/tasks?deviceId=%s&limit=20", base, device_id);
    if (cloud_get_json(service, url, config->cloud_token, &body, &length) != BAMBU_NET_OK) {
        snprintf(url, sizeof(url), "%s/v1/iot-service/api/user/task?device_id=%s&limit=20", base, device_id);
        if (cloud_get_json(service, url, config->cloud_token, &body, &length) != BAMBU_NET_OK) {
            free(task);
            return NULL;
        }
    }

    if (bambu_cloud_parse_tasks((const char *)body, length, device_id, task)) {
        apply_cloud_metadata(service, key, task);
        bambu_url_host(base, api_host, sizeof(api_host));

        for (int i = 0; i < task->preview_url_count && image == NULL; i++) {
            char host[256];
            uint8_t *data = NULL;
            size_t data_length = 0;
            int status = 0;
            const char *url_text = task->preview_urls[i];
            /* The token is only ever sent to the API host, never to a CDN. */
            bool send_token = bambu_url_host(url_text, host, sizeof(host)) && strcmp(host, api_host) == 0;

            if (!bambu_validate_remote_url(url_text))
                continue;
            if (bambu_https_get(
                    url_text,
                    send_token ? config->cloud_token : NULL,
                    BAMBU_PREVIEW_MAX_IMAGE_BYTES,
                    service->preview_cancel,
                    &data,
                    &data_length,
                    &status
                ) == BAMBU_NET_OK) {
                image = bambu_image_decode(data, data_length);
                if (image != NULL) {
                    *raw = data;
                    *raw_length = data_length;
                    data = NULL;
                }
            }
            free(data);
        }
    }
    free(body);
    free(task);
    return image;
}

static struct bambu_shared_image *fetch_ftps_preview(
    struct bambu_service *service,
    const struct bambu_service_config *config,
    const struct bambu_printer_snapshot *snapshot,
    uint8_t **raw,
    size_t *raw_length
)
{
    char names[BAMBU_MAX_NAME_CANDIDATES][BAMBU_FTP_NAME_CAPACITY];
    const char *candidates[BAMBU_MAX_NAME_CANDIDATES];
    struct bambu_ftps_request request;
    struct bambu_ftps_result fetched;
    struct bambu_shared_image *image = NULL;
    SYSTEMTIME now;
    uint8_t *data = NULL;
    size_t length = 0;
    char member[128];
    size_t count;
    enum bambu_net_result result;
    enum bambu_zip_result extracted;

    if (config->profile.ftps_fingerprint[0] == '\0') {
        set_preview_status(service, "skipped: FTPS certificate is not trusted yet");
        return NULL;
    }

    count = bambu_preview_candidates(snapshot, names);
    for (size_t i = 0; i < count; i++)
        candidates[i] = names[i];

    GetLocalTime(&now);
    memset(&request, 0, sizeof(request));
    request.host = config->profile.host;
    request.port = config->ftps_port;
    request.access_code = config->access_code;
    request.pinned_fingerprint = config->profile.ftps_fingerprint;
    request.candidates = candidates;
    request.candidate_count = count;
    request.current_year = now.wYear;
    request.max_archive_bytes = BAMBU_ZIP_MAX_ARCHIVE_BYTES;

    result = bambu_ftps_fetch_archive(&request, service->preview_cancel, &fetched);
    if (result != BAMBU_NET_OK) {
        char status[ERROR_CAPACITY];
        snprintf(status, sizeof(status), "FTPS failed: %s", bambu_net_result_name(result));
        set_preview_status(service, status);
        return NULL;
    }

    extracted = bambu_3mf_extract_preview(
        fetched.archive, fetched.archive_length, &data, &length, member, sizeof(member)
    );
    free(fetched.archive);
    if (extracted != BAMBU_ZIP_OK) {
        char status[ERROR_CAPACITY];
        snprintf(status, sizeof(status), "archive had no usable preview (code %d)", (int)extracted);
        set_preview_status(service, status);
        return NULL;
    }

    image = bambu_image_decode(data, length);
    if (image != NULL) {
        *raw = data;
        *raw_length = length;
    } else {
        set_preview_status(service, "preview image could not be decoded");
        free(data);
    }
    return image;
}

/* Exactly one preview attempt runs at a time per service, because all of them
 * run on this one thread. */
static void attempt_preview(struct bambu_service *service, struct bambu_service_config *config)
{
    struct bambu_printer_snapshot *snapshot = malloc(sizeof(*snapshot));
    struct bambu_shared_image *image = NULL;
    char key[JOB_KEY_CAPACITY];
    uint8_t *raw = NULL;
    size_t raw_length = 0;
    bool from_cache = false;

    if (snapshot == NULL)
        return;

    EnterCriticalSection(&service->lock);
    snprintf(key, sizeof(key), "%s", service->preview_key);
    *snapshot = service->snapshot;
    service->preview_attempts++;
    LeaveCriticalSection(&service->lock);

    if (load_config(service, config)) {
        if (config->profile.preview_enabled) {
            image = cache_load(service, config, key);
            from_cache = image != NULL;
            if (image == NULL && !config->profile.lan_only && config->cloud_token[0] != '\0')
                image = fetch_cloud_preview(service, config, key, &raw, &raw_length);
            if (image == NULL && !bambu_cancel_requested(service->preview_cancel))
                image = fetch_ftps_preview(service, config, snapshot, &raw, &raw_length);
        } else {
            EnterCriticalSection(&service->lock);
            service->preview_done = true;
            LeaveCriticalSection(&service->lock);
            set_preview_status(service, "disabled in profile");
        }
    }

    if (image != NULL) {
        if (!from_cache && raw != NULL)
            cache_store(service, config, key, raw, raw_length);
        if (install_preview(service, key, image))
            set_preview_status(service, from_cache ? "loaded from cache" : "loaded");
    }
    free(raw);
    free(snapshot);
    wipe_config(config);
}

static unsigned __stdcall preview_main(void *argument)
{
    struct bambu_service *service = argument;
    struct bambu_service_config *config = malloc(sizeof(*config));
    DWORD wait_ms = INFINITE;

    if (config == NULL)
        return 1;

    for (;;) {
        bool wanted;

        WaitForSingleObject(service->preview_event, wait_ms);
        if (stop_requested(service))
            break;
        bambu_cancel_reset(service->preview_cancel);
        if (stop_requested(service))
            break;

        EnterCriticalSection(&service->lock);
        wanted = service->preview_key[0] != '\0' && !service->preview_done &&
                 service->preview_attempts < PREVIEW_MAX_ATTEMPTS;
        LeaveCriticalSection(&service->lock);

        if (wanted)
            attempt_preview(service, config);

        EnterCriticalSection(&service->lock);
        wanted = service->preview_key[0] != '\0' && !service->preview_done &&
                 service->preview_attempts < PREVIEW_MAX_ATTEMPTS;
        LeaveCriticalSection(&service->lock);
        wait_ms = wanted ? PREVIEW_RETRY_MS : INFINITE;
    }
    free(config);
    return 0;
}

void bambu_services_init(bambu_service_config_loader loader)
{
    if (!registry_ready) {
        InitializeCriticalSection(&registry_lock);
        registry_ready = true;
    }
    config_loader = loader;
}

static void stop_service(struct bambu_service *service)
{
    InterlockedExchange(&service->stop, 1);
    bambu_cancel_request(service->connection_cancel);
    bambu_cancel_request(service->preview_cancel);
    SetEvent(service->wake_event);
    SetEvent(service->preview_event);
    if (service->connection_thread != NULL) {
        WaitForSingleObject(service->connection_thread, INFINITE);
        CloseHandle(service->connection_thread);
        service->connection_thread = NULL;
    }
    if (service->preview_thread != NULL) {
        WaitForSingleObject(service->preview_thread, INFINITE);
        CloseHandle(service->preview_thread);
        service->preview_thread = NULL;
    }
}

static void free_service(struct bambu_service *service)
{
    bambu_image_release(service->preview);
    if (service->wake_event != NULL)
        CloseHandle(service->wake_event);
    if (service->preview_event != NULL)
        CloseHandle(service->preview_event);
    bambu_cancel_destroy(service->connection_cancel);
    bambu_cancel_destroy(service->preview_cancel);
    DeleteCriticalSection(&service->lock);
    free(service);
}

/* Stops every worker. A service that is still referenced stays allocated, because
 * a source destroyed after this point will read it and then release it; the last
 * release frees it. The registry lock is deliberately never deleted for the same
 * reason, and OBS does not unload module code. */
void bambu_services_shutdown(void)
{
    struct bambu_service *service;

    if (!registry_ready)
        return;
    EnterCriticalSection(&registry_lock);
    for (service = registry_head; service != NULL; service = service->next) {
        bambu_log(BAMBU_LOG_WARNING, "stopping a printer service that was still referenced at shutdown");
        stop_service(service);
        service->stopped = true;
    }
    LeaveCriticalSection(&registry_lock);
}

static struct bambu_service *create_service(const char *profile_id)
{
    struct bambu_service *service = calloc(1, sizeof(*service));
    uint64_t seed = 0;

    if (service == NULL)
        return NULL;
    InitializeCriticalSection(&service->lock);
    snprintf(service->profile_id, sizeof(service->profile_id), "%s", profile_id);
    bambu_snapshot_init(&service->snapshot);
    service->snapshot.connection_state = BAMBU_CONNECTION_CONNECTING;
    service->references = 1;

    if (!BCRYPT_SUCCESS(BCryptGenRandom(NULL, (PUCHAR)&seed, sizeof(seed), BCRYPT_USE_SYSTEM_PREFERRED_RNG)) ||
        seed == 0)
        seed = GetTickCount64() | 1;
    service->random_state = seed;

    service->wake_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    service->preview_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    service->connection_cancel = bambu_cancel_create();
    service->preview_cancel = bambu_cancel_create();
    if (service->wake_event == NULL || service->preview_event == NULL || service->connection_cancel == NULL ||
        service->preview_cancel == NULL) {
        free_service(service);
        return NULL;
    }

    service->connection_thread = (HANDLE)_beginthreadex(NULL, 0, connection_main, service, 0, NULL);
    service->preview_thread = (HANDLE)_beginthreadex(NULL, 0, preview_main, service, 0, NULL);
    if (service->connection_thread == NULL || service->preview_thread == NULL) {
        stop_service(service);
        free_service(service);
        return NULL;
    }
    return service;
}

struct bambu_service *bambu_service_acquire(const char *profile_id)
{
    struct bambu_service *service;

    if (!registry_ready || profile_id == NULL || profile_id[0] == '\0')
        return NULL;

    EnterCriticalSection(&registry_lock);
    for (service = registry_head; service != NULL; service = service->next) {
        if (strcmp(service->profile_id, profile_id) == 0) {
            service->references++;
            LeaveCriticalSection(&registry_lock);
            return service;
        }
    }
    service = create_service(profile_id);
    if (service != NULL) {
        service->next = registry_head;
        registry_head = service;
    }
    LeaveCriticalSection(&registry_lock);
    return service;
}

void bambu_service_release(struct bambu_service *service)
{
    bool last = false;

    if (service == NULL || !registry_ready)
        return;

    EnterCriticalSection(&registry_lock);
    if (--service->references == 0) {
        struct bambu_service **link = &registry_head;
        while (*link != NULL && *link != service)
            link = &(*link)->next;
        if (*link == service)
            *link = service->next;
        last = true;
    }
    LeaveCriticalSection(&registry_lock);

    /* Joined outside the registry lock so other profiles are never held up. */
    if (last) {
        stop_service(service);
        free_service(service);
    }
}

int bambu_service_count(void)
{
    int count = 0;

    if (!registry_ready)
        return 0;
    EnterCriticalSection(&registry_lock);
    for (struct bambu_service *service = registry_head; service != NULL; service = service->next)
        count++;
    LeaveCriticalSection(&registry_lock);
    return count;
}

int bambu_service_reference_count(const struct bambu_service *service)
{
    int count;

    EnterCriticalSection(&registry_lock);
    count = service->references;
    LeaveCriticalSection(&registry_lock);
    return count;
}

uint64_t bambu_service_revision(const struct bambu_service *service)
{
    return service != NULL ? (uint64_t)InterlockedCompareExchange64((volatile LONG64 *)&service->revision, 0, 0) : 0;
}

void bambu_service_read(struct bambu_service *service, struct bambu_printer_snapshot *snapshot)
{
    EnterCriticalSection(&service->lock);
    *snapshot = service->snapshot;
    LeaveCriticalSection(&service->lock);
}

struct bambu_shared_image *bambu_service_preview(struct bambu_service *service)
{
    struct bambu_shared_image *image;

    EnterCriticalSection(&service->lock);
    image = bambu_image_retain(service->preview);
    LeaveCriticalSection(&service->lock);
    return image;
}

void bambu_service_reconnect(struct bambu_service *service)
{
    if (service == NULL)
        return;
    InterlockedExchange(&service->restart, 1);
    bambu_cancel_request(service->connection_cancel);
    SetEvent(service->wake_event);
}

void bambu_service_clear_preview(struct bambu_service *service)
{
    struct bambu_service_config *config;

    if (service == NULL)
        return;

    EnterCriticalSection(&service->lock);
    bambu_image_release(service->preview);
    service->preview = NULL;
    service->snapshot.preview_revision++;
    publish_revision(service);
    service->preview_key[0] = '\0';
    service->preview_done = false;
    service->preview_attempts = 0;
    update_preview_request(service, false);
    LeaveCriticalSection(&service->lock);

    config = malloc(sizeof(*config));
    if (config != NULL) {
        if (load_config(service, config) && config->cache_dir[0] != '\0') {
            char image_path[1200];
            char key_path[1200];
            cache_paths(service, config, image_path, key_path, sizeof(image_path));
            bambu_delete_file(key_path);
            bambu_delete_file(image_path);
        }
        wipe_config(config);
        free(config);
    }
}

void bambu_services_profile_changed(const char *profile_id)
{
    if (!registry_ready || profile_id == NULL)
        return;
    EnterCriticalSection(&registry_lock);
    for (struct bambu_service *service = registry_head; service != NULL; service = service->next) {
        if (strcmp(service->profile_id, profile_id) == 0)
            bambu_service_reconnect(service);
    }
    LeaveCriticalSection(&registry_lock);
}

size_t bambu_service_diagnostics(struct bambu_service *service, char *out, size_t capacity)
{
    struct bambu_service_config *config;
    char serial[32] = "<unknown>";
    int written;

    if (service == NULL || out == NULL || capacity == 0)
        return 0;

    config = malloc(sizeof(*config));
    if (config != NULL) {
        if (load_config(service, config))
            bambu_redact_serial(config->profile.serial, serial, sizeof(serial));
        wipe_config(config);
        free(config);
    }

    EnterCriticalSection(&service->lock);
    written = snprintf(
        out,
        capacity,
        "serial: %s\nconnection: %s\njob state: %s\nmessages: %llu\nmalformed: %llu\nconnects: %llu\n"
        "progress known: %s\nams units: %d\nlast error: %s\npreview: %s\n",
        serial,
        bambu_connection_state_name(service->snapshot.connection_state),
        bambu_job_state_name(service->snapshot.job_state),
        (unsigned long long)service->messages,
        (unsigned long long)service->malformed,
        (unsigned long long)service->connects,
        service->snapshot.has_progress ? "yes" : "no",
        service->snapshot.ams_unit_count,
        service->last_error[0] != '\0' ? service->last_error : "<none>",
        service->preview_status[0] != '\0' ? service->preview_status : "<none>"
    );
    LeaveCriticalSection(&service->lock);
    return written > 0 ? (size_t)written : 0;
}

size_t bambu_services_diagnostics(char *out, size_t capacity)
{
    size_t used = 0;

    if (!registry_ready || out == NULL || capacity == 0)
        return 0;
    out[0] = '\0';
    EnterCriticalSection(&registry_lock);
    for (struct bambu_service *service = registry_head; service != NULL; service = service->next) {
        int header = snprintf(out + used, capacity - used, "\n[printer: %d reference(s)]\n", service->references);
        if (header < 0 || (size_t)header >= capacity - used)
            break;
        used += (size_t)header;
        used += bambu_service_diagnostics(service, out + used, capacity - used);
        if (used >= capacity) {
            used = capacity - 1;
            break;
        }
    }
    LeaveCriticalSection(&registry_lock);
    return used;
}

void bambu_probe_certificates(
    const char *host,
    uint16_t mqtt_port,
    uint16_t ftps_port,
    struct bambu_cancel *cancel,
    struct bambu_certificate_probe *probe
)
{
    struct bambu_tls_credentials *credentials = bambu_tls_credentials_create();
    struct bambu_tls *connection = NULL;

    memset(probe, 0, sizeof(*probe));
    probe->mqtt_result = BAMBU_NET_TLS_FAILED;
    probe->ftps_result = BAMBU_NET_TLS_FAILED;
    if (credentials == NULL)
        return;

    probe->mqtt_result = bambu_tls_connect(
        credentials, host, mqtt_port, NULL, CONNECT_TIMEOUT_MS, cancel, &connection, probe->mqtt_fingerprint
    );
    bambu_tls_close(connection);
    connection = NULL;
    probe->ftps_result = bambu_tls_connect(
        credentials, host, ftps_port, NULL, CONNECT_TIMEOUT_MS, cancel, &connection, probe->ftps_fingerprint
    );
    bambu_tls_close(connection);
    bambu_tls_credentials_destroy(credentials);
}

enum bambu_net_result bambu_probe_login(
    const char *host,
    uint16_t mqtt_port,
    const char *pinned_fingerprint,
    const char *access_code,
    struct bambu_cancel *cancel
)
{
    struct bambu_tls_credentials *credentials = bambu_tls_credentials_create();
    struct bambu_tls *connection = NULL;
    uint8_t packet[256];
    uint8_t reply[64];
    size_t buffered = 0;
    enum bambu_net_result result;
    uint64_t deadline = bambu_monotonic_ms() + CONNACK_TIMEOUT_MS;

    if (credentials == NULL)
        return BAMBU_NET_TLS_FAILED;
    if (pinned_fingerprint == NULL || pinned_fingerprint[0] == '\0') {
        bambu_tls_credentials_destroy(credentials);
        return BAMBU_NET_PIN_MISMATCH;
    }

    result = bambu_tls_connect(
        credentials, host, mqtt_port, pinned_fingerprint, CONNECT_TIMEOUT_MS, cancel, &connection, NULL
    );
    if (result == BAMBU_NET_OK) {
        size_t length = bambu_mqtt_encode_connect(
            packet, sizeof(packet), "bambu-overlay-test", "bblp", access_code, KEEPALIVE_SECONDS
        );
        result = length != 0 ? bambu_tls_write(connection, packet, length, WRITE_TIMEOUT_MS, cancel)
                             : BAMBU_NET_PROTOCOL_ERROR;
        SecureZeroMemory(packet, sizeof(packet));
    }

    while (result == BAMBU_NET_OK) {
        struct bambu_mqtt_packet decoded;
        size_t received = 0;
        size_t consumed = 0;
        enum bambu_mqtt_decode_result state;

        if (bambu_monotonic_ms() >= deadline) {
            result = BAMBU_NET_TIMEOUT;
            break;
        }
        result = bambu_tls_read(connection, reply + buffered, sizeof(reply) - buffered, READ_SLICE_MS, cancel, &received);
        if (result == BAMBU_NET_TIMEOUT) {
            result = BAMBU_NET_OK;
            continue;
        }
        if (result != BAMBU_NET_OK)
            break;
        buffered += received;
        state = bambu_mqtt_decode(reply, buffered, &decoded, &consumed);
        if (state == BAMBU_MQTT_NEED_MORE) {
            if (buffered == sizeof(reply))
                result = BAMBU_NET_PROTOCOL_ERROR;
            continue;
        }
        if (state == BAMBU_MQTT_MALFORMED || decoded.type != BAMBU_MQTT_CONNACK) {
            result = BAMBU_NET_PROTOCOL_ERROR;
            break;
        }
        if (decoded.connack_code == 4 || decoded.connack_code == 5)
            result = BAMBU_NET_AUTH_FAILED;
        else if (decoded.connack_code != 0)
            result = BAMBU_NET_PROTOCOL_ERROR;
        break;
    }

    bambu_tls_close(connection);
    bambu_tls_credentials_destroy(credentials);
    return result;
}
