#pragma once

/* Pure selection logic behind print previews: FTP listing parsing, archive name
 * matching, cloud task parsing, and remote URL validation. */

#include "printer-snapshot.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BAMBU_FTP_MAX_ENTRIES 512
#define BAMBU_FTP_NAME_CAPACITY 256
#define BAMBU_MAX_NAME_CANDIDATES 8
#define BAMBU_MAX_PREVIEW_URLS 8
#define BAMBU_URL_CAPACITY 2048

struct bambu_ftp_entry {
    char name[BAMBU_FTP_NAME_CAPACITY];
    bool is_directory;
    uint64_t size;
    /* Sort key built from the listing date; 0 when unparsable. Listings without
     * a year sort by month/day/time within the current year. */
    uint64_t sort_time;
};

/* Removes directories and .gcode.3mf / .3mf / .gcode suffixes. */
void bambu_strip_print_suffix(const char *value, char *out, size_t capacity);

/* Lowercases and removes everything except ASCII letters and digits. */
void bambu_normalize_preview_name(const char *value, char *out, size_t capacity);

/* Parses one Unix-style LIST line. current_year is used for "HH:MM" entries. */
bool bambu_ftp_parse_list_line(const char *line, int current_year, struct bambu_ftp_entry *entry);

/* Scores an archive name against normalised candidates: 100 exact, 80 prefix,
 * 60 substring, plus 5 for a .gcode.3mf name. */
int bambu_score_archive_name(const char *archive_name, const char *const *candidates, size_t candidate_count);

/* Returns the best entry index, or -1 when entries is empty. *matched_by_name
 * is false when no candidate matched and the newest archive was chosen. */
int bambu_select_archive(
    const struct bambu_ftp_entry *entries,
    size_t entry_count,
    const char *const *candidates,
    size_t candidate_count,
    bool *matched_by_name
);

/* Collects normalised, de-duplicated candidates from a snapshot. Returns count. */
size_t bambu_preview_candidates(
    const struct bambu_printer_snapshot *snapshot,
    char out[BAMBU_MAX_NAME_CANDIDATES][BAMBU_FTP_NAME_CAPACITY]
);

/* Stable job identity used as the preview cache key. */
void bambu_preview_job_key(const struct bambu_printer_snapshot *snapshot, char *out, size_t capacity);

/* True for a LAN/local print that is actively running, where SD fallback applies. */
bool bambu_is_active_local_print(const struct bambu_printer_snapshot *snapshot);

/* Accepts only https:// URLs with a public hostname, no embedded credentials,
 * and no localhost or private/link-local IP literal. */
bool bambu_validate_remote_url(const char *url);

/* Copies the host of a valid URL, lowercased, for exact comparison. */
bool bambu_url_host(const char *url, char *out, size_t capacity);

struct bambu_cloud_task {
    bool found;
    char id[BAMBU_TASK_ID_CAPACITY];
    char design_title[BAMBU_JOB_NAME_CAPACITY];
    char title[BAMBU_PROFILE_NAME_CAPACITY];
    bool has_weight;
    double weight_grams;
    bool has_cost_time;
    int cost_time_seconds;
    int preview_url_count;
    char preview_urls[BAMBU_MAX_PREVIEW_URLS][BAMBU_URL_CAPACITY];
};

/* Parses any known task-list response shape and selects the newest task that
 * matches identifier, or the newest task overall if none match. */
bool bambu_cloud_parse_tasks(
    const char *payload,
    size_t length,
    const char *identifier,
    struct bambu_cloud_task *task
);

/* Resolves a serial number to its cloud device id from a bind response. */
bool bambu_cloud_parse_device_id(
    const char *payload,
    size_t length,
    const char *serial,
    char *device_id,
    size_t capacity
);
