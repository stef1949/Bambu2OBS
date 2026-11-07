#include "bambu-log.h"

#include "bambu-util.h"

#include <stdio.h>

static bambu_log_sink active_sink = NULL;

void bambu_log_set_sink(bambu_log_sink sink)
{
    active_sink = sink;
}

void bambu_log(enum bambu_log_level level, const char *format, ...)
{
    char raw[1024];
    char clean[1024];
    va_list arguments;

    va_start(arguments, format);
    vsnprintf(raw, sizeof(raw), format, arguments);
    va_end(arguments);

    /* Defence in depth: bearer tokens, emails, and URL queries never reach a log
     * even if a caller formats one in by mistake. */
    bambu_redact_text(raw, NULL, 0, clean, sizeof(clean));
    if (active_sink != NULL)
        active_sink(level, clean);
    else if (level <= BAMBU_LOG_WARNING)
        fprintf(stderr, "[bambu-printer-overlay] %s\n", clean);
}
