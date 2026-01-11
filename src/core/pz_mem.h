/*
 * Tank Game - Memory Management
 *
 * All allocations should go through these functions for:
 * - Leak detection in debug builds
 * - Allocation tracking/statistics
 * - Future: memory pools, arenas
 */

#ifndef PZ_MEM_H
#define PZ_MEM_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Memory categories for tracking
typedef enum {
    PZ_MEM_GENERAL = 0,
    PZ_MEM_RENDER,
    PZ_MEM_AUDIO,
    PZ_MEM_GAME,
    PZ_MEM_NETWORK,
    PZ_MEM_TEMP,
    PZ_MEM_CATEGORY_COUNT
} pz_mem_category;

// Initialize memory system (call once at startup)
void pz_mem_init(void);

// Shutdown and report leaks (call once at exit)
void pz_mem_shutdown(void);

// Allocate memory (returns NULL on failure)
void *pz_alloc(size_t size);
void *pz_alloc_tagged(size_t size, pz_mem_category category);

// Allocate zeroed memory
void *pz_calloc(size_t count, size_t size);
void *pz_calloc_tagged(size_t count, size_t size, pz_mem_category category);

// Reallocate memory
void *pz_realloc(void *ptr, size_t new_size);

// Free memory
void pz_free(void *ptr);

// Get total allocated bytes
size_t pz_mem_get_allocated(void);

// Get allocation count
size_t pz_mem_get_alloc_count(void);

// Get bytes allocated by category
size_t pz_mem_get_category_allocated(pz_mem_category category);

// Dump leak report to stderr (debug builds)
void pz_mem_dump_leaks(void);

// Check if there are any leaks
bool pz_mem_has_leaks(void);

#ifdef __cplusplus
}
#endif

#endif // PZ_MEM_H
