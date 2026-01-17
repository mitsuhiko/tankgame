/*
 * Tank Game - A* Pathfinding System
 *
 * Grid-based A* pathfinding for AI navigation.
 * Works with pz_map's tile grid to find paths around obstacles.
 */

#ifndef PZ_PATHFINDING_H
#define PZ_PATHFINDING_H

#include <stdbool.h>
#include <stdint.h>

#include "../core/pz_math.h"

// Forward declarations
typedef struct pz_map pz_map;

// Maximum path length (waypoints)
#define PZ_PATH_MAX_LENGTH 128

// Maximum A* iterations (prevents infinite loops on impossible paths)
#define PZ_PATHFIND_MAX_ITERATIONS 2000

// Path result structure
typedef struct pz_path {
    pz_vec2 points[PZ_PATH_MAX_LENGTH]; // Waypoints in world coordinates
    int count; // Number of waypoints
    int current; // Current waypoint index (for following)
    bool valid; // True if path was found
} pz_path;

// ============================================================================
// Pathfinding API
// ============================================================================

// Find a path from start to goal on the map.
// entity_radius: collision radius of the entity (for obstacle inflation)
// floor_level: current floor level of the entity (for multi-floor maps)
// Returns a path with waypoints in world coordinates.
// If no path found, returns a path with valid=false.
pz_path pz_pathfind(const pz_map *map, pz_vec2 start, pz_vec2 goal,
    float entity_radius, int8_t floor_level);

// Check if the current path is still valid (no obstacles blocking it).
// Useful for detecting when to repath.
bool pz_path_is_valid(const pz_path *path, const pz_map *map,
    float entity_radius, int8_t floor_level);

// Check if path has been completed (reached the end).
bool pz_path_is_complete(const pz_path *path);

// Get the current target waypoint to move toward.
// Returns the final goal if path is complete.
pz_vec2 pz_path_get_target(const pz_path *path);

// Get the final destination of the path.
pz_vec2 pz_path_get_goal(const pz_path *path);

// Advance to the next waypoint if we're close enough to the current one.
// arrival_threshold: how close to waypoint before advancing
// Returns true if advanced to next waypoint.
bool pz_path_advance(
    pz_path *path, pz_vec2 current_pos, float arrival_threshold);

// Reset path to invalid/empty state.
void pz_path_clear(pz_path *path);

// ============================================================================
// Path Smoothing (optional post-processing)
// ============================================================================

// Smooth a path by removing unnecessary waypoints using line-of-sight checks.
// This creates more natural movement by allowing diagonal shortcuts.
void pz_path_smooth(
    pz_path *path, const pz_map *map, float entity_radius, int8_t floor_level);

// ============================================================================
// Debug
// ============================================================================

// Get the number of nodes expanded in the last pathfind call.
// Useful for performance monitoring.
int pz_pathfind_get_last_iterations(void);

#endif // PZ_PATHFINDING_H
