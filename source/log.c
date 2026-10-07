#include "log.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>

static FILE *s_log = NULL;

bool log_init(void)
{
    if (mkdir("sdmc:/3ds", 0777) != 0 && errno != EEXIST)
        return false;
    if (mkdir(APP_DIR, 0777) != 0 && errno != EEXIST)
        return false;

    s_log = fopen(LOG_PATH, "a");
    if (!s_log)
        return false;

    fprintf(s_log, "\n==================== session start ====================\n");
    fflush(s_log);
    return true;
}

void log_close(void)
{
    if (s_log) {
        LOGI("session end");
        fclose(s_log);
        s_log = NULL;
    }
}

void log_write(const char *level, const char *fmt, ...)
{
    if (!s_log)
        return;

    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    fprintf(s_log, "%04d-%02d-%02d %02d:%02d:%02d [%s] ",
            t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
            t->tm_hour, t->tm_min, t->tm_sec, level);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(s_log, fmt, ap);
    va_end(ap);

    fputc('\n', s_log);
    fflush(s_log);
}

Result log_result(const char *what, Result rc)
{
    if (R_FAILED(rc)) {
        LOGE("%s failed: rc=0x%08lX (level %ld, summary %ld, module %ld, desc %ld)",
             what, (unsigned long)rc,
             (long)R_LEVEL(rc), (long)R_SUMMARY(rc), (long)R_MODULE(rc), (long)R_DESCRIPTION(rc));
    } else {
        LOGI("%s ok", what);
    }
    return rc;
}
