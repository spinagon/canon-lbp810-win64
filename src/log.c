/*
 * log.c - Simple logging implementation
 */
#include "log.h"
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <string.h>

static log_level_t  g_level  = LOG_LEVEL_INFO;
static log_target_t g_target = LOG_TARGET_CONSOLE;
static FILE        *g_file   = NULL;

static const char *level_str[] = { "DEBUG", "INFO ", "WARN ", "ERROR" };

void log_init(log_target_t target, const char *file_path)
{
    g_target = target;
    if (target == LOG_TARGET_FILE && file_path) {
        g_file = fopen(file_path, "a");
        if (!g_file) {
            fprintf(stderr, "log: cannot open %s, falling back to console\n",
                    file_path);
            g_target = LOG_TARGET_CONSOLE;
        }
    }
}

void log_shutdown(void)
{
    if (g_file) {
        fclose(g_file);
        g_file = NULL;
    }
}

void log_set_level(log_level_t level)
{
    g_level = level;
}

void log_write(log_level_t level, const char *file, int line,
               const char *fmt, ...)
{
    if (level < g_level) return;

    FILE *out = (g_target == LOG_TARGET_FILE && g_file) ? g_file : stderr;

    /* Timestamp */
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm);

    /* Extract basename from file path */
    const char *base = strrchr(file, '/');
    if (!base) base = strrchr(file, '\\');
    base = base ? base + 1 : file;

    fprintf(out, "[%s] %s %s:%d: ", ts, level_str[level], base, line);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);

    fputc('\n', out);
    fflush(out);
}
