/*
 * Tank Game - A* Pathfinding Implementation
 *
 * Uses a binary heap for the open set and a grid-based closed set.
 * Supports 8-directional movement with proper diagonal costs.
 */

#include "pz_pathfinding.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../core/pz_log.h"
#include "pz_map.h"

// ============================================================================
// Internal Types
// ============================================================================

// Node in the A* search
typedef struct {
    int x, y; // Tile coordinates
    float g; // Cost from start
    float f; // g + heuristic (total estimated cost)
    int parent_x; // Parent tile for path reconstruction
    int parent_y;
} pz_astar_node;

// Binary heap for open set (min-heap by f-cost)
#define PZ_HEAP_MAX_SIZE 4096

typedef struct {
    pz_astar_node nodes[PZ_HEAP_MAX_SIZE];
    int count;
} pz_heap;

// Grid for tracking visited nodes and their costs
typedef struct {
    float *g_costs; // Best g-cost to reach each cell (FLT_MAX if unvisited)
    int *parent_x; // Parent x for each cell (-1 if no parent)
    int *parent_y; // Parent y for each cell
    bool *closed; // Whether cell is in closed set
    int width;
    int height;
} pz_astar_grid;

// 8-directional movement
static const int DIR_X[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
static const int DIR_Y[8] = { 1, 1, 0, -1, -1, -1, 0, 1 };
static const float DIR_COST[8]
    = { 1.0f, 1.414f, 1.0f, 1.414f, 1.0f, 1.414f, 1.0f, 1.414f };

// Track iterations for debugging
static int s_last_iterations = 0;

// ============================================================================
// Binary Heap Operations
// ============================================================================

static void
heap_init(pz_heap *heap)
{
    heap->count = 0;
}

static bool
heap_is_empty(const pz_heap *heap)
{
    return heap->count == 0;
}

static void
heap_push(pz_heap *heap, pz_astar_node node)
{
    if (heap->count >= PZ_HEAP_MAX_SIZE) {
        return; // Heap full
    }

    // Add at end
    int i = heap->count++;
    heap->nodes[i] = node;

    // Bubble up
    while (i > 0) {
        int parent = (i - 1) / 2;
        if (heap->nodes[i].f < heap->nodes[parent].f) {
            // Swap
            pz_astar_node temp = heap->nodes[i];
            heap->nodes[i] = heap->nodes[parent];
            heap->nodes[parent] = temp;
            i = parent;
        } else {
            break;
        }
    }
}

static pz_astar_node
heap_pop(pz_heap *heap)
{
    pz_astar_node result = heap->nodes[0];

    // Move last to root
    heap->nodes[0] = heap->nodes[--heap->count];

    // Bubble down
    int i = 0;
    while (true) {
        int left = 2 * i + 1;
        int right = 2 * i + 2;
        int smallest = i;

        if (left < heap->count
            && heap->nodes[left].f < heap->nodes[smallest].f) {
            smallest = left;
        }
        if (right < heap->count
            && heap->nodes[right].f < heap->nodes[smallest].f) {
            smallest = right;
        }

        if (smallest != i) {
            pz_astar_node temp = heap->nodes[i];
            heap->nodes[i] = heap->nodes[smallest];
            heap->nodes[smallest] = temp;
            i = smallest;
        } else {
            break;
        }
    }

    return result;
}

// ============================================================================
// Grid Operations
// ============================================================================

static pz_astar_grid *
grid_create(int width, int height)
{
    pz_astar_grid *grid = malloc(sizeof(pz_astar_grid));
    if (!grid) {
        return NULL;
    }

    int size = width * height;
    grid->g_costs = malloc(size * sizeof(float));
    grid->parent_x = malloc(size * sizeof(int));
    grid->parent_y = malloc(size * sizeof(int));
    grid->closed = malloc(size * sizeof(bool));
    grid->width = width;
    grid->height = height;

    if (!grid->g_costs || !grid->parent_x || !grid->parent_y || !grid->closed) {
        free(grid->g_costs);
        free(grid->parent_x);
        free(grid->parent_y);
        free(grid->closed);
        free(grid);
        return NULL;
    }

    // Initialize
    for (int i = 0; i < size; i++) {
        grid->g_costs[i] = 1e30f; // Very large number
        grid->parent_x[i] = -1;
        grid->parent_y[i] = -1;
        grid->closed[i] = false;
    }

    return grid;
}

static void
grid_destroy(pz_astar_grid *grid)
{
    if (grid) {
        free(grid->g_costs);
        free(grid->parent_x);
        free(grid->parent_y);
        free(grid->closed);
        free(grid);
    }
}

static inline int
grid_index(const pz_astar_grid *grid, int x, int y)
{
    return y * grid->width + x;
}

// Check if a position is solid for a given floor level
// A position is solid if the tile's height doesn't match the floor level
static bool
is_solid_for_floor(const pz_map *map, pz_vec2 pos, int8_t floor_level)
{
    int tx, ty;
    pz_map_world_to_tile(map, pos, &tx, &ty);
    if (!pz_map_in_bounds(map, tx, ty)) {
        return true; // Out of bounds is solid
    }
    return pz_map_get_height(map, tx, ty) != floor_level;
}

static bool
is_position_clear(
    const pz_map *map, pz_vec2 pos, float radius, int8_t floor_level)
{
    if (!map) {
        return true;
    }

    if (is_solid_for_floor(map, pos, floor_level)) {
        return false;
    }

    if (radius <= 0.0f) {
        return true;
    }

    if (is_solid_for_floor(
            map, (pz_vec2) { pos.x + radius, pos.y }, floor_level)) {
        return false;
    }
    if (is_solid_for_floor(
            map, (pz_vec2) { pos.x - radius, pos.y }, floor_level)) {
        return false;
    }
    if (is_solid_for_floor(
            map, (pz_vec2) { pos.x, pos.y + radius }, floor_level)) {
        return false;
    }
    if (is_solid_for_floor(
            map, (pz_vec2) { pos.x, pos.y - radius }, floor_level)) {
        return false;
    }

    float diag = radius * 0.707f;
    if (is_solid_for_floor(
            map, (pz_vec2) { pos.x + diag, pos.y + diag }, floor_level)) {
        return false;
    }
    if (is_solid_for_floor(
            map, (pz_vec2) { pos.x + diag, pos.y - diag }, floor_level)) {
        return false;
    }
    if (is_solid_for_floor(
            map, (pz_vec2) { pos.x - diag, pos.y + diag }, floor_level)) {
        return false;
    }
    if (is_solid_for_floor(
            map, (pz_vec2) { pos.x - diag, pos.y - diag }, floor_level)) {
        return false;
    }

    return true;
}

static bool
segment_has_clearance(const pz_map *map, pz_vec2 start, pz_vec2 end,
    float radius, int8_t floor_level)
{
    pz_vec2 delta = pz_vec2_sub(end, start);
    float dist = pz_vec2_len(delta);
    if (dist <= 0.001f) {
        return is_position_clear(map, start, radius, floor_level);
    }

    float step = pz_maxf(radius * 0.5f, 0.2f);
    int steps = (int)ceilf(dist / step);
    pz_vec2 increment = pz_vec2_scale(delta, 1.0f / (float)steps);
    pz_vec2 pos = start;

    for (int i = 0; i <= steps; i++) {
        if (!is_position_clear(map, pos, radius, floor_level)) {
            return false;
        }
        pos = pz_vec2_add(pos, increment);
    }

    return true;
}

// ============================================================================
// Walkability Check
// ============================================================================

// Check if a tile is walkable for a given floor level, considering entity
// radius. We need to check not just the tile itself but nearby tiles that the
// entity's collision circle would touch.
static bool
is_tile_walkable(
    const pz_map *map, int tx, int ty, float entity_radius, int8_t floor_level)
{
    if (!pz_map_in_bounds(map, tx, ty)) {
        return false;
    }

    // A tile is walkable if its height matches the floor level
    if (pz_map_get_height(map, tx, ty) != floor_level) {
        return false;
    }

    // Check center of tile
    pz_vec2 center = pz_map_tile_to_world(map, tx, ty);

    // If entity radius is significant, check corners too
    if (entity_radius > 0.1f) {
        // Check that the entity can fit in this tile
        // We check points at entity_radius distance from center
        float r = entity_radius * 0.8f; // Slightly smaller to avoid being too
                                        // conservative

        pz_vec2 offsets[4] = {
            { r, 0 },
            { -r, 0 },
            { 0, r },
            { 0, -r },
        };

        for (int i = 0; i < 4; i++) {
            pz_vec2 check = pz_vec2_add(center, offsets[i]);
            if (is_solid_for_floor(map, check, floor_level)) {
                return false;
            }
        }
    }

    return true;
}

// Check if we can move diagonally between two tiles
// This prevents cutting corners through walls
static bool
can_move_diagonal(const pz_map *map, int from_x, int from_y, int to_x, int to_y,
    float entity_radius, int8_t floor_level)
{
    // Check that both adjacent cardinal tiles are walkable
    // This prevents corner-cutting
    if (!is_tile_walkable(map, to_x, from_y, entity_radius, floor_level)) {
        return false;
    }
    if (!is_tile_walkable(map, from_x, to_y, entity_radius, floor_level)) {
        return false;
    }
    return true;
}

// ============================================================================
// Heuristic
// ============================================================================

// Euclidean distance heuristic (admissible for 8-directional movement)
static float
heuristic(int x1, int y1, int x2, int y2)
{
    float dx = (float)(x2 - x1);
    float dy = (float)(y2 - y1);
    return sqrtf(dx * dx + dy * dy);
}

// ============================================================================
// Path Reconstruction
// ============================================================================

static void
reconstruct_path(pz_path *path, const pz_astar_grid *grid, const pz_map *map,
    int goal_x, int goal_y)
{
    // Trace back from goal to start
    int trace_x[PZ_PATH_MAX_LENGTH];
    int trace_y[PZ_PATH_MAX_LENGTH];
    int trace_count = 0;

    int x = goal_x;
    int y = goal_y;

    while (x >= 0 && y >= 0 && trace_count < PZ_PATH_MAX_LENGTH) {
        trace_x[trace_count] = x;
        trace_y[trace_count] = y;
        trace_count++;

        int idx = grid_index(grid, x, y);
        int px = grid->parent_x[idx];
        int py = grid->parent_y[idx];

        if (px == x && py == y) {
            break; // Start node
        }

        x = px;
        y = py;
    }

    // Reverse into path (so it goes start -> goal)
    path->count = 0;
    path->current = 0;
    path->valid = true;

    for (int i = trace_count - 1; i >= 0 && path->count < PZ_PATH_MAX_LENGTH;
         i--) {
        path->points[path->count++]
            = pz_map_tile_to_world(map, trace_x[i], trace_y[i]);
    }
}

// ============================================================================
// A* Implementation
// ============================================================================

pz_path
pz_pathfind(const pz_map *map, pz_vec2 start, pz_vec2 goal, float entity_radius,
    int8_t floor_level)
{
    pz_path result;
    pz_path_clear(&result);
    s_last_iterations = 0;

    if (!map) {
        return result;
    }

    // Convert to tile coordinates
    int start_tx, start_ty, goal_tx, goal_ty;
    pz_map_world_to_tile(map, start, &start_tx, &start_ty);
    pz_map_world_to_tile(map, goal, &goal_tx, &goal_ty);

    // Bounds check
    if (!pz_map_in_bounds(map, start_tx, start_ty)
        || !pz_map_in_bounds(map, goal_tx, goal_ty)) {
        pz_log(PZ_LOG_DEBUG, PZ_LOG_CAT_GAME,
            "Pathfind: start or goal out of bounds");
        return result;
    }

    // Quick check: is goal reachable on the same floor level?
    if (!is_tile_walkable(map, goal_tx, goal_ty, entity_radius, floor_level)) {
        // Try to find nearest walkable tile to goal on the same floor
        bool found = false;
        for (int r = 1; r <= 3 && !found; r++) {
            for (int dy = -r; dy <= r && !found; dy++) {
                for (int dx = -r; dx <= r && !found; dx++) {
                    if (abs(dx) == r || abs(dy) == r) {
                        int nx = goal_tx + dx;
                        int ny = goal_ty + dy;
                        if (is_tile_walkable(
                                map, nx, ny, entity_radius, floor_level)) {
                            goal_tx = nx;
                            goal_ty = ny;
                            found = true;
                        }
                    }
                }
            }
        }
        if (!found) {
            pz_log(PZ_LOG_DEBUG, PZ_LOG_CAT_GAME,
                "Pathfind: goal not walkable and no nearby walkable tile");
            return result;
        }
    }

    // Trivial case: already at goal
    if (start_tx == goal_tx && start_ty == goal_ty) {
        result.valid = true;
        result.count = 1;
        result.current = 0;
        result.points[0] = goal;
        return result;
    }

    // Create search structures
    pz_astar_grid *grid = grid_create(map->width, map->height);
    if (!grid) {
        pz_log(
            PZ_LOG_WARN, PZ_LOG_CAT_GAME, "Pathfind: failed to allocate grid");
        return result;
    }

    pz_heap open;
    heap_init(&open);

    // Add start node
    pz_astar_node start_node = {
        .x = start_tx,
        .y = start_ty,
        .g = 0.0f,
        .f = heuristic(start_tx, start_ty, goal_tx, goal_ty),
        .parent_x = start_tx,
        .parent_y = start_ty,
    };
    heap_push(&open, start_node);

    int start_idx = grid_index(grid, start_tx, start_ty);
    grid->g_costs[start_idx] = 0.0f;
    grid->parent_x[start_idx] = start_tx;
    grid->parent_y[start_idx] = start_ty;

    // A* main loop
    int iterations = 0;
    bool found = false;

    while (!heap_is_empty(&open) && iterations < PZ_PATHFIND_MAX_ITERATIONS) {
        iterations++;

        pz_astar_node current = heap_pop(&open);
        int current_idx = grid_index(grid, current.x, current.y);

        // Skip if already processed (we might have duplicate entries)
        if (grid->closed[current_idx]) {
            continue;
        }
        grid->closed[current_idx] = true;

        // Check if we reached the goal
        if (current.x == goal_tx && current.y == goal_ty) {
            found = true;
            break;
        }

        // Explore neighbors (8 directions)
        for (int d = 0; d < 8; d++) {
            int nx = current.x + DIR_X[d];
            int ny = current.y + DIR_Y[d];

            // Bounds check
            if (!pz_map_in_bounds(map, nx, ny)) {
                continue;
            }

            int neighbor_idx = grid_index(grid, nx, ny);

            // Skip if already in closed set
            if (grid->closed[neighbor_idx]) {
                continue;
            }

            // Check walkability on the same floor level
            if (!is_tile_walkable(map, nx, ny, entity_radius, floor_level)) {
                continue;
            }

            // For diagonal moves, check corner-cutting
            bool is_diagonal = (d == 1 || d == 3 || d == 5 || d == 7);
            if (is_diagonal) {
                if (!can_move_diagonal(map, current.x, current.y, nx, ny,
                        entity_radius, floor_level)) {
                    continue;
                }
            }

            // Calculate tentative g cost
            float tentative_g = grid->g_costs[current_idx] + DIR_COST[d];

            // If this path is better than any previous one
            if (tentative_g < grid->g_costs[neighbor_idx]) {
                grid->g_costs[neighbor_idx] = tentative_g;
                grid->parent_x[neighbor_idx] = current.x;
                grid->parent_y[neighbor_idx] = current.y;

                float f = tentative_g + heuristic(nx, ny, goal_tx, goal_ty);

                pz_astar_node neighbor = {
                    .x = nx,
                    .y = ny,
                    .g = tentative_g,
                    .f = f,
                    .parent_x = current.x,
                    .parent_y = current.y,
                };
                heap_push(&open, neighbor);
            }
        }
    }

    s_last_iterations = iterations;

    if (found) {
        reconstruct_path(&result, grid, map, goal_tx, goal_ty);

        // Replace the last waypoint with the exact goal position
        if (result.count > 0) {
            result.points[result.count - 1] = goal;
        }

        pz_log(PZ_LOG_DEBUG, PZ_LOG_CAT_GAME,
            "Pathfind: found path with %d waypoints in %d iterations",
            result.count, iterations);
    } else {
        pz_log(PZ_LOG_DEBUG, PZ_LOG_CAT_GAME,
            "Pathfind: no path found after %d iterations", iterations);
    }

    grid_destroy(grid);
    return result;
}

// ============================================================================
// Path Validation
// ============================================================================

bool
pz_path_is_valid(const pz_path *path, const pz_map *map, float entity_radius,
    int8_t floor_level)
{
    if (!path || !path->valid || !map) {
        return false;
    }

    // Check that each waypoint is still walkable on the floor level
    for (int i = path->current; i < path->count; i++) {
        int tx, ty;
        pz_map_world_to_tile(map, path->points[i], &tx, &ty);
        if (!is_tile_walkable(map, tx, ty, entity_radius, floor_level)) {
            return false;
        }
    }

    // Check line-of-sight between consecutive waypoints
    for (int i = path->current; i < path->count - 1; i++) {
        pz_raycast_result ray
            = pz_map_raycast_ex(map, path->points[i], path->points[i + 1]);
        if (ray.hit) {
            // Path is blocked
            return false;
        }
    }

    return true;
}

// ============================================================================
// Path Following
// ============================================================================

bool
pz_path_is_complete(const pz_path *path)
{
    if (!path || !path->valid) {
        return true;
    }
    return path->current >= path->count;
}

pz_vec2
pz_path_get_target(const pz_path *path)
{
    if (!path || !path->valid || path->count == 0) {
        return (pz_vec2) { 0, 0 };
    }

    if (path->current >= path->count) {
        return path->points[path->count - 1]; // Return final goal
    }

    return path->points[path->current];
}

pz_vec2
pz_path_get_goal(const pz_path *path)
{
    if (!path || !path->valid || path->count == 0) {
        return (pz_vec2) { 0, 0 };
    }
    return path->points[path->count - 1];
}

bool
pz_path_advance(pz_path *path, pz_vec2 current_pos, float arrival_threshold)
{
    if (!path || !path->valid || path->current >= path->count) {
        return false;
    }

    pz_vec2 target = path->points[path->current];
    float dist = pz_vec2_dist(current_pos, target);

    if (dist <= arrival_threshold) {
        path->current++;
        return true;
    }

    return false;
}

void
pz_path_clear(pz_path *path)
{
    if (path) {
        path->count = 0;
        path->current = 0;
        path->valid = false;
    }
}

// ============================================================================
// Path Smoothing
// ============================================================================

void
pz_path_smooth(
    pz_path *path, const pz_map *map, float entity_radius, int8_t floor_level)
{
    if (!path || !path->valid || path->count <= 2 || !map) {
        return;
    }

    // Simple path smoothing: try to skip waypoints using line-of-sight
    pz_vec2 smoothed[PZ_PATH_MAX_LENGTH];
    int smoothed_count = 0;

    // Always keep the first point
    smoothed[smoothed_count++] = path->points[0];

    int current = 0;
    while (current < path->count - 1) {
        // Try to find the furthest point we can reach directly
        int furthest = current + 1;

        for (int i = path->count - 1; i > current + 1; i--) {
            if (segment_has_clearance(map, path->points[current],
                    path->points[i], entity_radius, floor_level)) {
                furthest = i;
                break;
            }
        }

        current = furthest;
        if (smoothed_count < PZ_PATH_MAX_LENGTH) {
            smoothed[smoothed_count++] = path->points[current];
        }
    }

    // Copy smoothed path back
    for (int i = 0; i < smoothed_count; i++) {
        path->points[i] = smoothed[i];
    }
    path->count = smoothed_count;

    // Adjust current waypoint index if needed
    if (path->current >= path->count) {
        path->current = path->count - 1;
    }
}

// ============================================================================
// Debug
// ============================================================================

int
pz_pathfind_get_last_iterations(void)
{
    return s_last_iterations;
}
