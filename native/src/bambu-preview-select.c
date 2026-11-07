#include "bambu-preview-select.h"

#include "bambu-json.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool ends_with_nocase(const char *text, size_t length, const char *suffix)
{
    size_t suffix_length = strlen(suffix);
    if (length < suffix_length)
        return false;
    for (size_t i = 0; i < suffix_length; i++) {
        if (tolower((unsigned char)text[length - suffix_length + i]) != tolower((unsigned char)suffix[i]))
            return false;
    }
    return true;
}

void bambu_strip_print_suffix(const char *value, char *out, size_t capacity)
{
    static const char *const suffixes[] = {".gcode.3mf", ".3mf", ".gcode"};
    const char *name = value != NULL ? value : "";
    const char *cursor;
    size_t length;
    bool stripped = false;

    if (out == NULL || capacity == 0)
        return;
    while (isspace((unsigned char)*name))
        name++;
    for (cursor = name; *cursor != '\0'; cursor++) {
        if ((*cursor == '/' || *cursor == '\\') && cursor[1] != '\0')
            name = cursor + 1;
    }
    length = strlen(name);
    while (length > 0 && isspace((unsigned char)name[length - 1]))
        length--;

    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]) && !stripped; i++) {
        if (ends_with_nocase(name, length, suffixes[i])) {
            length -= strlen(suffixes[i]);
            stripped = true;
        }
    }
    if (length >= capacity)
        length = capacity - 1;
    memcpy(out, name, length);
    out[length] = '\0';
}

void bambu_normalize_preview_name(const char *value, char *out, size_t capacity)
{
    size_t used = 0;

    if (out == NULL || capacity == 0)
        return;
    for (; value != NULL && *value != '\0' && used + 1 < capacity; value++) {
        unsigned char c = (unsigned char)*value;
        if (c < 0x80 && isalnum(c))
            out[used++] = (char)tolower(c);
    }
    out[used] = '\0';
}

static int month_index(const char *month)
{
    static const char *const months[] = {"jan", "feb", "mar", "apr", "may", "jun",
                                         "jul", "aug", "sep", "oct", "nov", "dec"};
    char lowered[4] = {0};

    for (int i = 0; i < 3 && month[i] != '\0'; i++)
        lowered[i] = (char)tolower((unsigned char)month[i]);
    for (int i = 0; i < 12; i++) {
        if (strcmp(months[i], lowered) == 0)
            return i + 1;
    }
    return 0;
}

/* Splits off the next whitespace-delimited field. */
static const char *next_field(const char *cursor, char *out, size_t capacity)
{
    size_t used = 0;

    while (*cursor == ' ' || *cursor == '\t')
        cursor++;
    if (*cursor == '\0')
        return NULL;
    while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t') {
        if (used + 1 < capacity)
            out[used++] = *cursor;
        cursor++;
    }
    out[used] = '\0';
    return cursor;
}

bool bambu_ftp_parse_list_line(const char *line, int current_year, struct bambu_ftp_entry *entry)
{
    char fields[8][32];
    const char *cursor = line;
    size_t name_length;
    int month;
    int day;
    int year = current_year;
    int hour = 0;
    int minute = 0;

    if (line == NULL || entry == NULL)
        return false;
    for (int i = 0; i < 8; i++) {
        cursor = next_field(cursor, fields[i], sizeof(fields[i]));
        if (cursor == NULL)
            return false;
    }
    while (*cursor == ' ' || *cursor == '\t')
        cursor++;
    name_length = strcspn(cursor, "\r\n");
    if (name_length == 0)
        return false;

    memset(entry, 0, sizeof(*entry));
    if (name_length >= sizeof(entry->name))
        name_length = sizeof(entry->name) - 1;
    memcpy(entry->name, cursor, name_length);
    entry->is_directory = fields[0][0] == 'd';
    entry->size = strtoull(fields[4], NULL, 10);

    month = month_index(fields[5]);
    day = atoi(fields[6]);
    if (strchr(fields[7], ':') != NULL) {
        if (sscanf(fields[7], "%d:%d", &hour, &minute) != 2)
            month = 0;
    } else {
        year = atoi(fields[7]);
    }
    if (month > 0 && day > 0 && year > 0)
        entry->sort_time = (((uint64_t)year * 13 + (uint64_t)month) * 32 + (uint64_t)day) * 1440 +
                           (uint64_t)hour * 60 + (uint64_t)minute;
    return true;
}

int bambu_score_archive_name(const char *archive_name, const char *const *candidates, size_t candidate_count)
{
    char stripped[BAMBU_FTP_NAME_CAPACITY];
    char normalized[BAMBU_FTP_NAME_CAPACITY];
    size_t normalized_length;
    int best = 0;

    if (archive_name == NULL || candidate_count == 0)
        return 0;
    bambu_strip_print_suffix(archive_name, stripped, sizeof(stripped));
    bambu_normalize_preview_name(stripped, normalized, sizeof(normalized));
    normalized_length = strlen(normalized);

    for (size_t i = 0; i < candidate_count; i++) {
        const char *candidate = candidates[i];
        size_t candidate_length = candidate != NULL ? strlen(candidate) : 0;
        int score = 0;

        if (candidate_length == 0 || normalized_length == 0)
            continue;
        if (strcmp(normalized, candidate) == 0)
            score = 100;
        else if (strncmp(normalized, candidate, candidate_length) == 0 ||
                 strncmp(candidate, normalized, normalized_length) == 0)
            score = 80;
        else if (strstr(normalized, candidate) != NULL)
            score = 60;
        if (score > best)
            best = score;
    }
    if (ends_with_nocase(archive_name, strlen(archive_name), ".gcode.3mf"))
        best += 5;
    return best;
}

static bool entry_newer(const struct bambu_ftp_entry *left, const struct bambu_ftp_entry *right)
{
    if (left->sort_time != right->sort_time)
        return left->sort_time > right->sort_time;
    return left->size > right->size;
}

int bambu_select_archive(
    const struct bambu_ftp_entry *entries,
    size_t entry_count,
    const char *const *candidates,
    size_t candidate_count,
    bool *matched_by_name
)
{
    int best_index = -1;
    int best_score = -1;
    int newest_index = -1;
    bool matched;

    for (size_t i = 0; i < entry_count; i++) {
        int score = bambu_score_archive_name(entries[i].name, candidates, candidate_count);
        if (newest_index < 0 || entry_newer(&entries[i], &entries[newest_index]))
            newest_index = (int)i;
        if (score > best_score || (score == best_score && entry_newer(&entries[i], &entries[best_index]))) {
            best_score = score;
            best_index = (int)i;
        }
    }
    /* A bare .gcode.3mf bonus of 5 is not a name match. */
    matched = candidate_count > 0 && best_score > 5;
    if (matched_by_name != NULL)
        *matched_by_name = matched;
    return matched ? best_index : newest_index;
}

static bool ignored_basename(const char *name)
{
    static const char *const ignored[] = {"", "plate_1", "plate_1_small", "plate_no_light_1", "top_1", "pick_1"};
    for (size_t i = 0; i < sizeof(ignored) / sizeof(ignored[0]); i++) {
        if (strcmp(ignored[i], name) == 0)
            return true;
    }
    return false;
}

size_t bambu_preview_candidates(
    const struct bambu_printer_snapshot *snapshot,
    char out[BAMBU_MAX_NAME_CANDIDATES][BAMBU_FTP_NAME_CAPACITY]
)
{
    const char *sources[3];
    size_t count = 0;

    if (snapshot == NULL)
        return 0;
    sources[0] = snapshot->job_name;
    sources[1] = snapshot->subtask_name;
    sources[2] = snapshot->gcode_file;

    for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); i++) {
        char stripped[BAMBU_FTP_NAME_CAPACITY];
        char lowered[BAMBU_FTP_NAME_CAPACITY];
        char normalized[BAMBU_FTP_NAME_CAPACITY];
        bool duplicate = false;
        size_t j;

        bambu_strip_print_suffix(sources[i], stripped, sizeof(stripped));
        for (j = 0; stripped[j] != '\0'; j++)
            lowered[j] = (char)tolower((unsigned char)stripped[j]);
        lowered[j] = '\0';
        if (ignored_basename(lowered))
            continue;
        bambu_normalize_preview_name(stripped, normalized, sizeof(normalized));
        if (normalized[0] == '\0')
            continue;
        for (size_t k = 0; k < count; k++)
            duplicate = duplicate || strcmp(out[k], normalized) == 0;
        if (!duplicate && count < BAMBU_MAX_NAME_CANDIDATES) {
            memcpy(out[count], normalized, strlen(normalized) + 1);
            count++;
        }
    }
    return count;
}

void bambu_preview_job_key(const struct bambu_printer_snapshot *snapshot, char *out, size_t capacity)
{
    if (out == NULL || capacity == 0)
        return;
    if (snapshot == NULL ||
        (snapshot->task_id[0] == '\0' && snapshot->subtask_name[0] == '\0' && snapshot->gcode_file[0] == '\0')) {
        snprintf(out, capacity, "unknown_job");
        return;
    }
    snprintf(out, capacity, "%s|%s|%s", snapshot->task_id, snapshot->subtask_name, snapshot->gcode_file);
}

bool bambu_is_active_local_print(const struct bambu_printer_snapshot *snapshot)
{
    bool local;

    if (snapshot == NULL)
        return false;
    local = strcmp(snapshot->print_type, "local") == 0 || strcmp(snapshot->print_type, "lan") == 0;
    return local && (snapshot->job_state == BAMBU_JOB_PREPARING || snapshot->job_state == BAMBU_JOB_PRINTING ||
                     snapshot->job_state == BAMBU_JOB_PAUSED);
}

static bool parse_ipv4(const char *host, unsigned parts[4])
{
    char tail;
    if (sscanf(host, "%u.%u.%u.%u%c", &parts[0], &parts[1], &parts[2], &parts[3], &tail) != 4)
        return false;
    return parts[0] <= 255 && parts[1] <= 255 && parts[2] <= 255 && parts[3] <= 255;
}

static bool ipv4_is_global(const unsigned parts[4])
{
    if (parts[0] == 0 || parts[0] == 10 || parts[0] == 127 || parts[0] >= 224)
        return false;
    if (parts[0] == 169 && parts[1] == 254)
        return false;
    if (parts[0] == 172 && parts[1] >= 16 && parts[1] <= 31)
        return false;
    if (parts[0] == 192 && parts[1] == 168)
        return false;
    if (parts[0] == 100 && parts[1] >= 64 && parts[1] <= 127)
        return false;
    return true;
}

bool bambu_url_host(const char *url, char *out, size_t capacity)
{
    const char *authority;
    size_t length;

    if (url == NULL || out == NULL || capacity == 0)
        return false;
    authority = strstr(url, "://");
    if (authority == NULL)
        return false;
    authority += 3;
    length = strcspn(authority, "/?#");
    if (length == 0 || length >= capacity)
        return false;
    for (size_t i = 0; i < length; i++)
        out[i] = (char)tolower((unsigned char)authority[i]);
    out[length] = '\0';
    /* Remove a port, but leave bracketed IPv6 literals for the caller to reject. */
    if (out[0] != '[') {
        char *colon = strchr(out, ':');
        if (colon != NULL)
            *colon = '\0';
    }
    length = strlen(out);
    while (length > 0 && out[length - 1] == '.')
        out[--length] = '\0';
    return length > 0;
}

bool bambu_validate_remote_url(const char *url)
{
    char host[256];
    unsigned parts[4];
    const char *authority;
    size_t authority_length;
    size_t host_length;

    if (url == NULL || strlen(url) >= BAMBU_URL_CAPACITY)
        return false;
    for (const char *cursor = url; *cursor != '\0'; cursor++) {
        if ((unsigned char)*cursor <= 0x20 || *cursor == '\\')
            return false;
    }
    if (strncmp(url, "https://", 8) != 0)
        return false;
    authority = url + 8;
    authority_length = strcspn(authority, "/?#");
    if (memchr(authority, '@', authority_length) != NULL)
        return false;
    if (!bambu_url_host(url, host, sizeof(host)))
        return false;
    /* IPv6 literals are never legitimate for this cloud; refuse them outright. */
    if (host[0] == '[')
        return false;
    host_length = strlen(host);
    if (strcmp(host, "localhost") == 0)
        return false;
    if (host_length > 10 && strcmp(host + host_length - 10, ".localhost") == 0)
        return false;
    if (parse_ipv4(host, parts) && !ipv4_is_global(parts))
        return false;
    /* Decimal, octal, or hex host forms can smuggle a private address. */
    if (strspn(host, "0123456789.xabcdef") == host_length && strchr(host, '.') == NULL)
        return false;
    return true;
}

static const struct bambu_json *array_or_null(const struct bambu_json *node)
{
    return node != NULL && node->type == BAMBU_JSON_ARRAY ? node : NULL;
}

static const struct bambu_json *find_hits(const struct bambu_json *object)
{
    const struct bambu_json *hits;
    const struct bambu_json *found;

    if ((found = array_or_null(bambu_json_get(object, "tasks"))) != NULL)
        return found;
    hits = bambu_json_get(object, "hits");
    if ((found = array_or_null(hits)) != NULL)
        return found;
    if ((found = array_or_null(bambu_json_get(hits, "hits"))) != NULL)
        return found;
    return array_or_null(bambu_json_get(object, "list"));
}

static const struct bambu_json *find_task_array(const struct bambu_json *root)
{
    const struct bambu_json *found = find_hits(root);
    return found != NULL ? found : find_hits(bambu_json_get(root, "data"));
}

static bool task_matches(const struct bambu_json *task, const char *identifier)
{
    static const char *const keys[] = {"deviceId", "device_id", "device_sn", "sn", "printerSn"};

    if (identifier == NULL || identifier[0] == '\0')
        return true;
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        const char *value = bambu_json_as_string(bambu_json_get(task, keys[i]));
        if (value != NULL && strcmp(value, identifier) == 0)
            return true;
    }
    return false;
}

/* ISO-8601 timestamps sort correctly as text; numeric ids are the fallback. */
static int task_compare(const struct bambu_json *left, const struct bambu_json *right)
{
    static const char *const keys[] = {"endTime", "startTime", "createTime", "updatedAt", "createdAt"};
    double left_id = 0.0;
    double right_id = 0.0;

    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        const char *left_time = bambu_json_as_string(bambu_json_get(left, keys[i]));
        const char *right_time = bambu_json_as_string(bambu_json_get(right, keys[i]));
        if (left_time != NULL && left_time[0] != '\0' && right_time != NULL && right_time[0] != '\0') {
            int order = strcmp(left_time, right_time);
            if (order != 0)
                return order;
        }
    }
    bambu_json_as_double(bambu_json_get(left, "id"), &left_id);
    bambu_json_as_double(bambu_json_get(right, "id"), &right_id);
    return (left_id > right_id) - (left_id < right_id);
}

static void collect_preview_urls(const struct bambu_json *source, struct bambu_cloud_task *task)
{
    static const char *const keys[] = {"cover", "thumbnail", "preview", "image"};

    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        const char *value = bambu_json_as_string(bambu_json_get(source, keys[i]));
        bool duplicate = false;

        if (value == NULL || value[0] == '\0' || strcmp(value, "N/A") == 0 || strcmp(value, "Unknown") == 0)
            continue;
        if (strlen(value) >= BAMBU_URL_CAPACITY || task->preview_url_count >= BAMBU_MAX_PREVIEW_URLS)
            continue;
        for (int j = 0; j < task->preview_url_count; j++)
            duplicate = duplicate || strcmp(task->preview_urls[j], value) == 0;
        if (!duplicate)
            memcpy(task->preview_urls[task->preview_url_count++], value, strlen(value) + 1);
    }
}

bool bambu_cloud_parse_tasks(
    const char *payload,
    size_t length,
    const char *identifier,
    struct bambu_cloud_task *task
)
{
    struct bambu_json *root;
    const struct bambu_json *tasks;
    const struct bambu_json *candidate;
    const struct bambu_json *best = NULL;
    bool best_matches = false;
    double number;
    int integer;

    if (task == NULL)
        return false;
    memset(task, 0, sizeof(*task));
    root = bambu_json_parse(payload, length);
    if (root == NULL)
        return false;

    tasks = find_task_array(root);
    if (tasks != NULL) {
        for (candidate = tasks->child; candidate != NULL; candidate = candidate->next) {
            bool matches;
            if (candidate->type != BAMBU_JSON_OBJECT)
                continue;
            matches = task_matches(candidate, identifier);
            if (best == NULL || (matches && !best_matches) ||
                (matches == best_matches && task_compare(candidate, best) > 0)) {
                best = candidate;
                best_matches = matches;
            }
        }
    }

    if (best != NULL) {
        const char *design_title = bambu_json_as_string(bambu_json_get(best, "designTitle"));
        const char *title = bambu_json_as_string(bambu_json_get(best, "title"));
        const struct bambu_json *message = bambu_json_get(best, "taskMessage");

        task->found = true;
        bambu_json_as_text(bambu_json_get(best, "id"), task->id, sizeof(task->id));
        bambu_copy_text(task->design_title, sizeof(task->design_title), design_title);
        bambu_copy_text(task->title, sizeof(task->title), title);
        if (bambu_json_as_double(bambu_json_get(best, "weight"), &number) && number >= 0.0) {
            task->has_weight = true;
            task->weight_grams = number;
        }
        if (bambu_json_as_int(bambu_json_get(best, "costTime"), &integer) && integer > 0) {
            task->has_cost_time = true;
            task->cost_time_seconds = integer;
        }
        collect_preview_urls(best, task);
        if (message != NULL && message->type == BAMBU_JSON_OBJECT)
            collect_preview_urls(message, task);
    }
    bambu_json_free(root);
    return task->found;
}

bool bambu_cloud_parse_device_id(
    const char *payload,
    size_t length,
    const char *serial,
    char *device_id,
    size_t capacity
)
{
    static const char *const serial_keys[] = {"sn", "serial", "device_sn", "printerSn", "dev_id"};
    static const char *const id_keys[] = {"deviceId", "dev_id", "device_id", "id"};
    struct bambu_json *root;
    const struct bambu_json *devices;
    const struct bambu_json *device;
    bool found = false;

    if (serial == NULL || serial[0] == '\0' || device_id == NULL || capacity == 0)
        return false;
    root = bambu_json_parse(payload, length);
    if (root == NULL)
        return false;

    devices = array_or_null(bambu_json_get(root, "devices"));
    if (devices == NULL)
        devices = array_or_null(bambu_json_get(bambu_json_get(root, "data"), "devices"));

    if (devices != NULL) {
        for (device = devices->child; device != NULL && !found; device = device->next) {
            bool matches = false;
            for (size_t i = 0; i < sizeof(serial_keys) / sizeof(serial_keys[0]) && !matches; i++) {
                const char *value = bambu_json_as_string(bambu_json_get(device, serial_keys[i]));
                matches = value != NULL && strcmp(value, serial) == 0;
            }
            for (size_t i = 0; matches && i < sizeof(id_keys) / sizeof(id_keys[0]) && !found; i++)
                found = bambu_json_as_text(bambu_json_get(device, id_keys[i]), device_id, capacity) && device_id[0] != '\0';
        }
    }
    bambu_json_free(root);
    return found;
}
