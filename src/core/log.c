/* The core's log: messages at or above the level PDF_TOOLS_LOG names (error, warn, info, debug; default warn) go to
 * a sink, stderr unless the window sets its own. */
#include "pt_core.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static pt_log_sink sink;
static int threshold = -1;

static const char *NAMES[] = {"debug", "info", "warn", "error"};

pt_log_level pt_log_threshold(void)
{
    if (threshold < 0) {
        const char *env = getenv("PDF_TOOLS_LOG");
        threshold = PT_LOG_WARN;
        for (int i = 0; env && i < 4; i++)
            if (!strcmp(env, NAMES[i])) threshold = i;
    }
    return (pt_log_level)threshold;
}

void pt_log_set_sink(pt_log_sink s) { sink = s; }

void pt_log(pt_log_level level, const char *fmt, ...)
{
    if (level < pt_log_threshold()) return;
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (sink) sink(level, msg);
    else fprintf(stderr, "pdf-tools: %s: %s\n", NAMES[level], msg);
}

/* MuPDF reports every thrown error, including the ones the caller handles, so those count as debug */
static void mupdf_warning(void *user, const char *message) { (void)user, pt_log(PT_LOG_INFO, "mupdf: %s", message); }
static void mupdf_error(void *user, const char *message) { (void)user, pt_log(PT_LOG_DEBUG, "mupdf error: %s", message); }

void pt_log_mupdf(fz_context *ctx)
{
    fz_set_warning_callback(ctx, mupdf_warning, NULL);
    fz_set_error_callback(ctx, mupdf_error, NULL);
}
