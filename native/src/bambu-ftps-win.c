#include "bambu-log.h"
#include "bambu-net-win.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONTROL_TIMEOUT_MS 10000
#define DATA_TIMEOUT_MS 15000
#define MAX_LISTING_BYTES (2u * 1024u * 1024u)
#define LINE_CAPACITY 1024
#define CONTROL_BUFFER 4096

struct ftp_session {
    struct bambu_tls_credentials *credentials;
    struct bambu_tls *control;
    struct bambu_cancel *cancel;
    const struct bambu_ftps_request *request;
    uint8_t pending[CONTROL_BUFFER];
    size_t pending_length;
};

static enum bambu_net_result read_line(struct ftp_session *session, char *line, size_t capacity)
{
    size_t used = 0;

    for (;;) {
        size_t received = 0;
        enum bambu_net_result result;

        for (size_t i = 0; i < session->pending_length; i++) {
            char c = (char)session->pending[i];
            if (c == '\n') {
                memmove(session->pending, session->pending + i + 1, session->pending_length - i - 1);
                session->pending_length -= i + 1;
                if (used > 0 && line[used - 1] == '\r')
                    used--;
                line[used] = '\0';
                return BAMBU_NET_OK;
            }
            /* Over-long lines are truncated rather than allowed to grow. */
            if (used + 1 < capacity)
                line[used++] = c;
        }
        session->pending_length = 0;

        result = bambu_tls_read(
            session->control,
            session->pending,
            sizeof(session->pending),
            CONTROL_TIMEOUT_MS,
            session->cancel,
            &received
        );
        if (result != BAMBU_NET_OK)
            return result;
        session->pending_length = received;
    }
}

/* Reads a reply, following multi-line "123-" continuations to the final line. */
static enum bambu_net_result read_reply(struct ftp_session *session, int *code, char *text, size_t text_capacity)
{
    char line[LINE_CAPACITY];
    enum bambu_net_result result;
    int first_code = 0;

    for (int lines = 0; lines < 200; lines++) {
        result = read_line(session, line, sizeof(line));
        if (result != BAMBU_NET_OK)
            return result;
        if (strlen(line) < 4 || !isdigit((unsigned char)line[0]) || !isdigit((unsigned char)line[1]) ||
            !isdigit((unsigned char)line[2])) {
            if (first_code == 0)
                return BAMBU_NET_PROTOCOL_ERROR;
            continue;
        }
        if (first_code == 0)
            first_code = atoi(line);
        if (atoi(line) == first_code && line[3] == ' ') {
            *code = first_code;
            if (text != NULL)
                snprintf(text, text_capacity, "%s", line);
            return BAMBU_NET_OK;
        }
    }
    return BAMBU_NET_PROTOCOL_ERROR;
}

static enum bambu_net_result send_command(struct ftp_session *session, const char *command, const char *argument)
{
    char line[LINE_CAPACITY];
    int length;

    /* A CR or LF in a printer-supplied name would smuggle a second command. */
    if (argument != NULL && strpbrk(argument, "\r\n") != NULL)
        return BAMBU_NET_PROTOCOL_ERROR;
    length = snprintf(
        line,
        sizeof(line),
        argument != NULL ? "%s %s\r\n" : "%s\r\n",
        command,
        argument != NULL ? argument : ""
    );
    if (length <= 0 || (size_t)length >= sizeof(line))
        return BAMBU_NET_PROTOCOL_ERROR;
    return bambu_tls_write(session->control, line, (size_t)length, CONTROL_TIMEOUT_MS, session->cancel);
}

static enum bambu_net_result command_expect(
    struct ftp_session *session,
    const char *command,
    const char *argument,
    int expected_class,
    char *text,
    size_t text_capacity
)
{
    int code = 0;
    enum bambu_net_result result = send_command(session, command, argument);

    if (result == BAMBU_NET_OK)
        result = read_reply(session, &code, text, text_capacity);
    if (result != BAMBU_NET_OK)
        return result;
    return code / 100 == expected_class ? BAMBU_NET_OK : BAMBU_NET_PROTOCOL_ERROR;
}

static bool parse_pasv_port(const char *reply, uint16_t *port)
{
    unsigned parts[6];
    const char *open = strchr(reply, '(');

    if (open == NULL)
        return false;
    if (sscanf(open, "(%u,%u,%u,%u,%u,%u)", &parts[0], &parts[1], &parts[2], &parts[3], &parts[4], &parts[5]) != 6)
        return false;
    if (parts[4] > 255 || parts[5] > 255)
        return false;
    *port = (uint16_t)(parts[4] * 256 + parts[5]);
    return *port != 0;
}

/* Opens PASV, issues the command, and drains the data channel into a bounded
 * buffer. The reply's host is ignored in favour of the control peer, so a hostile
 * server cannot redirect the data connection elsewhere. */
static enum bambu_net_result transfer(
    struct ftp_session *session,
    const char *command,
    const char *argument,
    size_t maximum_bytes,
    uint8_t **out,
    size_t *out_length,
    bool *session_reused
)
{
    char reply[LINE_CAPACITY];
    struct bambu_tls *data = NULL;
    uint16_t port = 0;
    uint8_t *buffer = NULL;
    size_t used = 0;
    size_t capacity = 64 * 1024;
    int code = 0;
    enum bambu_net_result result;

    *out = NULL;
    *out_length = 0;

    result = command_expect(session, "PASV", NULL, 2, reply, sizeof(reply));
    if (result != BAMBU_NET_OK)
        return result;
    if (!parse_pasv_port(reply, &port))
        return BAMBU_NET_PROTOCOL_ERROR;

    result = send_command(session, command, argument);
    if (result != BAMBU_NET_OK)
        return result;

    /* Same credentials and target name, so Schannel resumes the control session. */
    result = bambu_tls_connect(
        session->credentials,
        session->request->host,
        port,
        session->request->pinned_fingerprint,
        DATA_TIMEOUT_MS,
        session->cancel,
        &data,
        NULL
    );
    if (result != BAMBU_NET_OK)
        return result;
    if (session_reused != NULL)
        *session_reused = bambu_tls_session_reused(data);

    result = read_reply(session, &code, NULL, 0);
    if (result == BAMBU_NET_OK && code / 100 != 1)
        result = code == 550 ? BAMBU_NET_NOT_FOUND : BAMBU_NET_PROTOCOL_ERROR;

    if (result == BAMBU_NET_OK) {
        buffer = malloc(capacity);
        if (buffer == NULL)
            result = BAMBU_NET_TOO_LARGE;
    }

    while (result == BAMBU_NET_OK) {
        size_t received = 0;
        enum bambu_net_result step;

        if (used == capacity) {
            size_t grown = capacity * 2 > maximum_bytes + 1 ? maximum_bytes + 1 : capacity * 2;
            uint8_t *resized = grown > capacity ? realloc(buffer, grown) : NULL;
            if (resized == NULL) {
                result = BAMBU_NET_TOO_LARGE;
                break;
            }
            buffer = resized;
            capacity = grown;
        }
        step = bambu_tls_read(data, buffer + used, capacity - used, DATA_TIMEOUT_MS, session->cancel, &received);
        if (step == BAMBU_NET_CLOSED)
            break;
        if (step != BAMBU_NET_OK) {
            result = step;
            break;
        }
        used += received;
        if (used > maximum_bytes)
            result = BAMBU_NET_TOO_LARGE;
    }
    bambu_tls_close(data);

    if (result == BAMBU_NET_OK) {
        result = read_reply(session, &code, NULL, 0);
        if (result == BAMBU_NET_OK && code / 100 != 2)
            result = BAMBU_NET_PROTOCOL_ERROR;
    }
    if (result != BAMBU_NET_OK) {
        free(buffer);
        return result;
    }
    *out = buffer;
    *out_length = used;
    return BAMBU_NET_OK;
}

static bool is_archive_name(const char *name)
{
    size_t length = strlen(name);
    return length > 4 && _stricmp(name + length - 4, ".3mf") == 0;
}

/* Appends the .3mf files in one directory listing to entries. */
static size_t collect_listing(
    const uint8_t *listing,
    size_t length,
    int current_year,
    const char *directory,
    struct bambu_ftp_entry *entries,
    size_t count
)
{
    size_t offset = 0;

    while (offset < length && count < BAMBU_FTP_MAX_ENTRIES) {
        char line[LINE_CAPACITY];
        size_t line_length = 0;
        struct bambu_ftp_entry entry;

        while (offset < length && listing[offset] != '\n') {
            if (line_length + 1 < sizeof(line))
                line[line_length++] = (char)listing[offset];
            offset++;
        }
        offset++;
        line[line_length] = '\0';

        if (!bambu_ftp_parse_list_line(line, current_year, &entry) || entry.is_directory)
            continue;
        if (!is_archive_name(entry.name))
            continue;
        if (directory[0] != '\0') {
            char path[BAMBU_FTP_NAME_CAPACITY];
            int written = snprintf(path, sizeof(path), "%s/%s", directory, entry.name);
            if (written < 0 || (size_t)written >= sizeof(path))
                continue;
            memcpy(entry.name, path, (size_t)written + 1);
        }
        entries[count++] = entry;
    }
    return count;
}

enum bambu_net_result bambu_ftps_fetch_archive(
    const struct bambu_ftps_request *request,
    struct bambu_cancel *cancel,
    struct bambu_ftps_result *result_out
)
{
    /* Printers keep SD prints in the root and cloud or LAN jobs under cache. */
    static const char *const directories[] = {"", "cache"};
    struct ftp_session session;
    struct bambu_ftp_entry *entries = NULL;
    size_t entry_count = 0;
    int code = 0;
    int selected;
    enum bambu_net_result result;

    if (request == NULL || result_out == NULL || request->host == NULL || request->access_code == NULL ||
        request->pinned_fingerprint == NULL)
        return BAMBU_NET_PROTOCOL_ERROR;
    memset(result_out, 0, sizeof(*result_out));
    memset(&session, 0, sizeof(session));
    session.cancel = cancel;
    session.request = request;

    session.credentials = bambu_tls_credentials_create();
    if (session.credentials == NULL)
        return BAMBU_NET_TLS_FAILED;

    result = bambu_tls_connect(
        session.credentials,
        request->host,
        request->port,
        request->pinned_fingerprint,
        CONTROL_TIMEOUT_MS,
        cancel,
        &session.control,
        NULL
    );
    if (result != BAMBU_NET_OK)
        goto done;

    result = read_reply(&session, &code, NULL, 0);
    if (result == BAMBU_NET_OK && code / 100 != 2)
        result = BAMBU_NET_PROTOCOL_ERROR;

    if (result == BAMBU_NET_OK) {
        result = send_command(&session, "USER", "bblp");
        if (result == BAMBU_NET_OK)
            result = read_reply(&session, &code, NULL, 0);
        if (result == BAMBU_NET_OK && code / 100 != 3 && code / 100 != 2)
            result = BAMBU_NET_AUTH_FAILED;
    }
    if (result == BAMBU_NET_OK && code / 100 == 3) {
        result = send_command(&session, "PASS", request->access_code);
        if (result == BAMBU_NET_OK)
            result = read_reply(&session, &code, NULL, 0);
        if (result == BAMBU_NET_OK && code / 100 != 2)
            result = BAMBU_NET_AUTH_FAILED;
    }
    if (result == BAMBU_NET_OK)
        result = command_expect(&session, "PBSZ", "0", 2, NULL, 0);
    if (result == BAMBU_NET_OK)
        result = command_expect(&session, "PROT", "P", 2, NULL, 0);
    if (result == BAMBU_NET_OK)
        result = command_expect(&session, "TYPE", "I", 2, NULL, 0);
    if (result != BAMBU_NET_OK)
        goto done;

    entries = calloc(BAMBU_FTP_MAX_ENTRIES, sizeof(*entries));
    if (entries == NULL) {
        result = BAMBU_NET_TOO_LARGE;
        goto done;
    }

    for (size_t i = 0; i < sizeof(directories) / sizeof(directories[0]); i++) {
        uint8_t *listing = NULL;
        size_t listing_length = 0;
        const char *directory = directories[i];
        enum bambu_net_result listed = transfer(
            &session,
            "LIST",
            directory[0] != '\0' ? directory : NULL,
            MAX_LISTING_BYTES,
            &listing,
            &listing_length,
            &result_out->data_session_reused
        );

        if (listed == BAMBU_NET_OK) {
            entry_count = collect_listing(
                listing, listing_length, request->current_year, directory, entries, entry_count
            );
            free(listing);
        } else if (listed == BAMBU_NET_CANCELLED) {
            result = listed;
            goto done;
        } else if (i == 0) {
            /* The root must be listable; a missing cache directory is normal. */
            result = listed;
            goto done;
        }
    }

    selected = bambu_select_archive(
        entries,
        entry_count,
        request->candidates,
        request->candidate_count,
        &result_out->matched_by_name
    );
    if (selected < 0) {
        result = BAMBU_NET_NOT_FOUND;
        goto done;
    }

    if (entries[selected].size > request->max_archive_bytes) {
        result = BAMBU_NET_TOO_LARGE;
        goto done;
    }
    snprintf(result_out->name, sizeof(result_out->name), "%s", entries[selected].name);
    result = transfer(
        &session,
        "RETR",
        entries[selected].name,
        request->max_archive_bytes,
        &result_out->archive,
        &result_out->archive_length,
        &result_out->data_session_reused
    );

done:
    if (session.control != NULL) {
        if (result != BAMBU_NET_CANCELLED)
            send_command(&session, "QUIT", NULL);
        bambu_tls_close(session.control);
    }
    free(entries);
    bambu_tls_credentials_destroy(session.credentials);
    return result;
}
