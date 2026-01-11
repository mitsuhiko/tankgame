/*
 * Tank Game - Platform Layer
 *
 * Platform-specific functionality abstracted for portability.
 */

#ifndef PZ_PLATFORM_H
#define PZ_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * High-Precision Timer
 * ============================================================================
 */

// Initialize the timer system (call once at startup)
void pz_time_init(void);

// Get current time in seconds (high precision)
double pz_time_now(void);

// Get current time in milliseconds
uint64_t pz_time_now_ms(void);

// Get current time in microseconds
uint64_t pz_time_now_us(void);

// Sleep for given number of milliseconds
void pz_time_sleep_ms(uint32_t ms);

/* ============================================================================
 * File Operations
 * ============================================================================
 */

// Read entire file into memory (caller must pz_free the result)
// Returns NULL on failure, sets out_size to file size
char *pz_file_read(const char *path, size_t *out_size);

// Read file as null-terminated string (caller must pz_free)
// Returns NULL on failure
char *pz_file_read_text(const char *path);

// Write data to file (creates file if not exists, overwrites if exists)
// Returns true on success
bool pz_file_write(const char *path, const void *data, size_t size);

// Write null-terminated string to file
bool pz_file_write_text(const char *path, const char *text);

// Append data to file
bool pz_file_append(const char *path, const void *data, size_t size);

// Check if file exists
bool pz_file_exists(const char *path);

// Get file modification time (seconds since epoch)
// Returns 0 on failure
int64_t pz_file_mtime(const char *path);

// Get file size in bytes
// Returns -1 on failure
int64_t pz_file_size(const char *path);

// Delete a file
bool pz_file_delete(const char *path);

/* ============================================================================
 * Directory Operations
 * ============================================================================
 */

// Check if directory exists
bool pz_dir_exists(const char *path);

// Create directory (and parent directories if needed)
bool pz_dir_create(const char *path);

// Get current working directory (caller must pz_free)
char *pz_dir_cwd(void);

/* ============================================================================
 * Path Operations
 * ============================================================================
 */

// Join path components with separator (caller must pz_free)
char *pz_path_join(const char *a, const char *b);

// Get filename from path (caller must pz_free)
char *pz_path_filename(const char *path);

// Get directory from path (caller must pz_free)
char *pz_path_dirname(const char *path);

// Get file extension (caller must pz_free)
// Returns empty string if no extension
char *pz_path_extension(const char *path);

#ifdef __cplusplus
}
#endif

#endif // PZ_PLATFORM_H
