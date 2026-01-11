/*
 * Tank Game - Logging System
 *
 * Supports:
 * - Log levels (TRACE, DEBUG, INFO, WARN, ERROR)
 * - Categories for filtering
 * - ANSI color output
 * - Optional file logging
 * - TRACE/DEBUG compiled out in release builds
 */

#ifndef PZ_LOG_H
#define PZ_LOG_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Log levels
typedef enum {
    PZ_LOG_TRACE = 0,
    PZ_LOG_DEBUG,
    PZ_LOG_INFO,
    PZ_LOG_WARN,
    PZ_LOG_ERROR
} pz_log_level;

// Log categories
typedef enum {
    PZ_LOG_CAT_CORE = 0,
    PZ_LOG_CAT_RENDER,
    PZ_LOG_CAT_AUDIO,
    PZ_LOG_CAT_INPUT,
    PZ_LOG_CAT_GAME,
    PZ_LOG_CAT_NET,
    PZ_LOG_CAT_EDITOR,
    PZ_LOG_CAT_COUNT
} pz_log_category;

// Initialize logging system
void pz_log_init(void);

// Shutdown logging
void pz_log_shutdown(void);

// Set minimum log level (messages below this are ignored)
void pz_log_set_level(pz_log_level level);

// Enable/disable a category
void pz_log_set_category_enabled(pz_log_category cat, bool enabled);

// Enable/disable color output
void pz_log_set_color_enabled(bool enabled);

// Set log file (NULL to disable file logging)
bool pz_log_set_file(const char *path);

// Core logging function
void pz_log(pz_log_level level, pz_log_category cat, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

// Convenience macros

#if defined(PZ_DEBUG) || defined(PZ_DEV)
#    define PZ_LOG_TRACE(cat, ...) pz_log(PZ_LOG_TRACE, (cat), __VA_ARGS__)
#    define PZ_LOG_DEBUG(cat, ...) pz_log(PZ_LOG_DEBUG, (cat), __VA_ARGS__)
#else
#    define PZ_LOG_TRACE(cat, ...) ((void)0)
#    define PZ_LOG_DEBUG(cat, ...) ((void)0)
#endif

#define PZ_LOG_INFO(cat, ...) pz_log(PZ_LOG_INFO, (cat), __VA_ARGS__)
#define PZ_LOG_WARN(cat, ...) pz_log(PZ_LOG_WARN, (cat), __VA_ARGS__)
#define PZ_LOG_ERROR(cat, ...) pz_log(PZ_LOG_ERROR, (cat), __VA_ARGS__)

// Shorthand for common categories
#define LOG_TRACE(...) PZ_LOG_TRACE(PZ_LOG_CAT_CORE, __VA_ARGS__)
#define LOG_DEBUG(...) PZ_LOG_DEBUG(PZ_LOG_CAT_CORE, __VA_ARGS__)
#define LOG_INFO(...) PZ_LOG_INFO(PZ_LOG_CAT_CORE, __VA_ARGS__)
#define LOG_WARN(...) PZ_LOG_WARN(PZ_LOG_CAT_CORE, __VA_ARGS__)
#define LOG_ERROR(...) PZ_LOG_ERROR(PZ_LOG_CAT_CORE, __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif // PZ_LOG_H
