#pragma once

/* Logging seam. The core never calls blog() directly, so it can run without OBS;
 * the plugin installs a sink that forwards to blog(). Callers must never pass
 * secrets, and the sink additionally redacts anything registered here. */

#include <stdarg.h>
#include <stddef.h>

enum bambu_log_level {
    BAMBU_LOG_ERROR = 0,
    BAMBU_LOG_WARNING,
    BAMBU_LOG_INFO,
    BAMBU_LOG_DEBUG,
};

typedef void (*bambu_log_sink)(enum bambu_log_level level, const char *message);

void bambu_log_set_sink(bambu_log_sink sink);

#if defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
void bambu_log(enum bambu_log_level level, const char *format, ...);
