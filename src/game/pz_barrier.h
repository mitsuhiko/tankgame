/*
 * Tank Game - Destructible Barrier System
 *
 * Barriers are tile-sized objects that block movement, projectiles, and light
 * until destroyed. They render using tile textures (like walls) but are placed
 * via map tags like powerups or spawn points.
 *
 * When destroyed, barriers play an explosion effect and become passable.
 */

#ifndef PZ_BARRIER_H
#define PZ_BARRIER_H

#include <stdbool.h>
#include <stdint.h>

#include "../core/pz_math.h"
#include "../engine/render/pz_renderer.h"
#include "../engine/render/pz_texture.h"
#include "pz_tile_registry.h"

// Maximum number of barriers per map
#define PZ_MAX_BARRIERS 32

// Maximum expired barriers tracked per frame (matches max barriers)
#define PZ_MAX_EXPIRED_BARRIERS PZ_MAX_BARRIERS

// Info about an expired barrier (for crediting back to player)
typedef struct pz_expired_barrier {
    int barrier_index; // Index in barrier array
    int owner_tank_id; // Owner tank ID (-1 if map-placed)
    pz_vec2 pos; // Position (for effects)
} pz_expired_barrier;

// Forward declarations
typedef struct pz_map pz_map;
typedef struct pz_particle_manager pz_particle_manager;
typedef struct pz_lighting pz_lighting;

// Barrier structure
typedef struct pz_barrier {
    bool active; // Is this slot in use?
    bool destroyed; // Has this barrier been destroyed?

    pz_vec2 pos; // World position (center of tile)
    float health; // Current health
    float max_health; // Starting health

    char tile_name[32]; // Tile name for texture lookup

    // Floor level (for multi-floor support)
    int8_t floor_level; // Height level this barrier is on

    // Ownership (for player-placed barriers)
    int owner_tank_id; // -1 if map-placed, tank ID if player-placed
    pz_vec4 tint_color; // Color tint from owner's tank (1,1,1,1 = no tint)

    // Lifetime (for player-placed barriers with timed expiration)
    float lifetime; // Remaining lifetime in seconds (0 = infinite)
    float max_lifetime; // Starting lifetime (for alpha calculation)

    // Destruction animation state
    float destroy_timer; // Counts down during destruction effect
} pz_barrier;

// Barrier manager
typedef struct pz_barrier_manager {
    pz_barrier barriers[PZ_MAX_BARRIERS];
    int active_count;

    // Tile registry for texture lookups
    const pz_tile_registry *tile_registry;

    // Rendering resources
    pz_shader_handle shader;
    pz_pipeline_handle pipeline;
    pz_buffer_handle mesh_buffer;
    int mesh_vertex_count;
    bool render_ready;

    // Cached tile size from map (for mesh generation)
    float tile_size;

    // Expired barriers from last update (for crediting back to players)
    pz_expired_barrier expired[PZ_MAX_EXPIRED_BARRIERS];
    int expired_count;
} pz_barrier_manager;

/* ============================================================================
 * Manager API
 * ============================================================================
 */

// Create/destroy manager
pz_barrier_manager *pz_barrier_manager_create(pz_renderer *renderer,
    const pz_tile_registry *tile_registry, float tile_size);
void pz_barrier_manager_destroy(pz_barrier_manager *mgr, pz_renderer *renderer);

// Add a barrier at a position
// Returns barrier index, or -1 if full
int pz_barrier_add(pz_barrier_manager *mgr, pz_vec2 pos, const char *tile_name,
    float health, int8_t floor_level);

// Add a barrier with owner (for player-placed barriers)
// owner_tank_id: -1 for map-placed, tank ID for player-placed
// tint_color: color overlay (1,1,1,1 = no tint)
// lifetime: time in seconds until barrier auto-destroys (0 = infinite)
int pz_barrier_add_owned(pz_barrier_manager *mgr, pz_vec2 pos,
    const char *tile_name, float health, int8_t floor_level, int owner_tank_id,
    pz_vec4 tint_color, float lifetime);

// Update all barriers (destruction timers, etc.)
// After update, check mgr->expired_count for barriers that expired this frame
void pz_barrier_update(pz_barrier_manager *mgr, float dt);

// Get expired barriers from last update (for crediting back to players)
// Returns array of expired barrier info, count stored in *count
const pz_expired_barrier *pz_barrier_get_expired(
    const pz_barrier_manager *mgr, int *count);

// Apply damage to a barrier at a position (same floor only)
// Returns true if a barrier was hit
// If destroyed is not NULL, sets *destroyed = true if the barrier was destroyed
bool pz_barrier_apply_damage(pz_barrier_manager *mgr, pz_vec2 pos, float damage,
    int8_t floor_level, bool *destroyed);

// Check if a world position is blocked by a barrier on the same floor
// Returns the barrier if blocked, NULL otherwise
pz_barrier *pz_barrier_check_collision(
    pz_barrier_manager *mgr, pz_vec2 pos, float radius, int8_t floor_level);

// Resolve tank-barrier collision by pushing the tank out (same floor only)
// Modifies pos in place, returns true if collision occurred
bool pz_barrier_resolve_collision(
    pz_barrier_manager *mgr, pz_vec2 *pos, float radius, int8_t floor_level);

// Check if a line segment hits a barrier on the same floor (for projectile
// raycast) Returns true if hit, fills out hit_pos and hit_normal barrier_out
// receives the hit barrier (can be NULL if not needed)
bool pz_barrier_raycast(pz_barrier_manager *mgr, pz_vec2 start, pz_vec2 end,
    int8_t floor_level, pz_vec2 *hit_pos, pz_vec2 *hit_normal,
    pz_barrier **barrier_out);

// Add barrier occluders to lighting system
void pz_barrier_add_occluders(pz_barrier_manager *mgr, pz_lighting *lighting);

// Render all active barriers
// Lighting parameters for lit rendering
typedef struct pz_barrier_render_params {
    pz_texture_handle light_texture;
    float light_scale_x, light_scale_z;
    float light_offset_x, light_offset_z;
    pz_vec3 ambient;
    bool has_sun;
    pz_vec3 sun_direction;
    pz_vec3 sun_color;
} pz_barrier_render_params;

void pz_barrier_render(pz_barrier_manager *mgr, pz_renderer *renderer,
    const pz_mat4 *view_projection, const pz_barrier_render_params *params);

// Get number of active (not destroyed) barriers
int pz_barrier_count(const pz_barrier_manager *mgr);

// Clear all barriers (for map reload)
void pz_barrier_clear(pz_barrier_manager *mgr);

// Clear all barriers owned by a specific tank (for respawn)
void pz_barrier_clear_owned_by(pz_barrier_manager *mgr, int tank_id);

// Get barrier by index (for iteration)
pz_barrier *pz_barrier_get(pz_barrier_manager *mgr, int index);

// Check if a position is valid for barrier placement
// Returns true if no wall, no existing barrier, no tank at position
bool pz_barrier_is_valid_placement(const pz_barrier_manager *mgr,
    const struct pz_map *map, pz_vec2 pos, float tank_radius, pz_vec2 tank_pos);

#endif // PZ_BARRIER_H
