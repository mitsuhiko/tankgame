/*
 * Tank Game - Logging Implementation
 */

#include "pz_log.h"
#include "pz_mem.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

// ANSI color codes
#define ANSI_RESET "\x1b[0m"
#define ANSI_GRAY "\x1b[90m"
#define ANSI_WHITE "\x1b[97m"
#define ANSI_GREEN "\x1b[32m"
#define ANSI_YELLOW "\x1b[33m"
#define ANSI_RED "\x1b[31m"
#define ANSI_CYAN "\x1b[36m"

// Global state
static pz_log_level g_min_level = PZ_LOG_TRACE;
static bool g_category_enabled[PZ_LOG_CAT_COUNT];
static bool g_color_enabled = true;
static FILE *g_log_file = NULL;
static bool g_initialized = false;

// Level names and colors
static const char *g_level_names[]
    = { "TRACE", "DEBUG", "INFO", "WARN", "ERROR" };

static const char *g_level_colors[] = {
    ANSI_GRAY, // TRACE
    ANSI_CYAN, // DEBUG
    ANSI_GREEN, // INFO
    ANSI_YELLOW, // WARN
    ANSI_RED // ERROR
};

// Category names
static const char *g_category_names[]
    = { "CORE", "RENDER", "AUDIO", "INPUT", "GAME", "NET", "EDITOR" };

void
pz_log_init(void)
{
    g_min_level = PZ_LOG_TRACE;
    g_color_enabled = true;
    g_log_file = NULL;

    // Enable all categories by default
    for (int i = 0; i < PZ_LOG_CAT_COUNT; i++) {
        g_category_enabled[i] = true;
    }

    g_initialized = true;
}

void
pz_log_shutdown(void)
{
    if (g_log_file) {
        fclose(g_log_file);
        g_log_file = NULL;
    }
    g_initialized = false;
}

void
pz_log_set_level(pz_log_level level)
{
    g_min_level = level;
}

void
pz_log_set_category_enabled(pz_log_category cat, bool enabled)
{
    if (cat < PZ_LOG_CAT_COUNT) {
        g_category_enabled[cat] = enabled;
    }
}

void
pz_log_set_color_enabled(bool enabled)
{
    g_color_enabled = enabled;
}

bool
pz_log_set_file(const char *path)
{
    if (g_log_file) {
        fclose(g_log_file);
        g_log_file = NULL;
    }

    if (path) {
        g_log_file = fopen(path, "a");
        if (!g_log_file) {
            fprintf(stderr, "[LOG] Failed to open log file: %s\n", path);
            return false;
        }
    }
    return true;
}

void
pz_log(pz_log_level level, pz_log_category cat, const char *fmt, ...)
{
    // Filter by level
    if (level < g_min_level)
        return;

    // Filter by category
    if (cat < PZ_LOG_CAT_COUNT && !g_category_enabled[cat])
        return;

    // Get timestamp
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char time_buf[16];
    strftime(time_buf, sizeof(time_buf), "%H:%M:%S", tm_info);

    // Format the message
    char msg_buf[1024];
    const char *message = msg_buf;
    char *heap_message = NULL;

    va_list args;
    va_list args_copy;
    va_start(args, fmt);
    va_copy(args_copy, args);
    int needed = vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);
    va_end(args);

    if (needed < 0) {
        va_end(args_copy);
        return;
    }

    if ((size_t)needed >= sizeof(msg_buf)) {
        heap_message = pz_alloc((size_t)needed + 1);
        if (!heap_message) {
            va_end(args_copy);
            return;
        }
        vsnprintf(heap_message, (size_t)needed + 1, fmt, args_copy);
        message = heap_message;
    }
    va_end(args_copy);

    // Output to console with colors
    FILE *out = (level >= PZ_LOG_WARN) ? stderr : stdout;

    if (g_color_enabled) {
        fprintf(out, "%s[%s] %s%-5s%s %s[%s]%s %s\n", ANSI_GRAY, time_buf,
            g_level_colors[level], g_level_names[level], ANSI_RESET, ANSI_WHITE,
            g_category_names[cat], ANSI_RESET, message);
    } else {
        fprintf(out, "[%s] %-5s [%s] %s\n", time_buf, g_level_names[level],
            g_category_names[cat], message);
    }

    // Output to file (no colors)
    if (g_log_file) {
        fprintf(g_log_file, "[%s] %-5s [%s] %s\n", time_buf,
            g_level_names[level], g_category_names[cat], message);
        fflush(g_log_file);
    }

    pz_free(heap_message);
}
