/*
 * Tank Game - Projectile System
 *
 * Handles bullets that can bounce off walls.
 */

#ifndef PZ_PROJECTILE_H
#define PZ_PROJECTILE_H

#include <stdbool.h>
#include <stdint.h>

#include "../core/pz_math.h"
#include "../engine/render/pz_renderer.h"
#include "pz_map.h"
#include "pz_mesh.h"

// Forward declaration
typedef struct pz_tank_manager pz_tank_manager;

// Maximum number of active projectiles
#define PZ_MAX_PROJECTILES 64

// Maximum collision events per frame
#define PZ_MAX_PROJECTILE_HITS 32

// Collision event types for particle spawning
typedef enum pz_projectile_hit_type {
    PZ_HIT_NONE = 0,
    PZ_HIT_TANK, // Hit a tank (killed it)
    PZ_HIT_TANK_NON_FATAL, // Hit a tank but didn't kill it
    PZ_HIT_PROJECTILE, // Hit another projectile
    PZ_HIT_WALL, // Destroyed on wall (no bounces left)
    PZ_HIT_WALL_RICOCHET, // Bounced off a wall
} pz_projectile_hit_type;

// Collision event for particle spawning
typedef struct pz_projectile_hit {
    pz_projectile_hit_type type;
    pz_vec2 pos; // Position of hit
} pz_projectile_hit;

// Projectile structure
typedef struct pz_projectile {
    bool active; // Is this slot in use?

    pz_vec2 pos; // Position in world space (X, Z)
    pz_vec2 velocity; // Velocity vector
    float speed; // Movement speed (units/sec)

    int bounces_remaining; // How many more bounces before destruction
    float lifetime; // Time remaining before auto-destruct
    float age; // Time since spawned (for self-damage grace period)
    float bounce_cooldown; // Time until next bounce allowed (prevents
                           // double-bounce)

    int owner_id; // Who fired this (for friendly fire checks)
    int damage; // Damage on hit
    int8_t floor_level; // Floor level (for multi-floor collision)

    float scale; // Visual scale
    pz_vec4 color; // Projectile color

    float fog_timer; // Spawn timer for fog trail
} pz_projectile;

// Projectile manager
typedef struct pz_projectile_manager {
    pz_projectile projectiles[PZ_MAX_PROJECTILES];
    int active_count;

    // Collision events from last update (for particle spawning)
    pz_projectile_hit hits[PZ_MAX_PROJECTILE_HITS];
    int hit_count;

    // Rendering resources
    pz_mesh *mesh;
    pz_shader_handle shader;
    pz_pipeline_handle pipeline;
    bool render_ready;
} pz_projectile_manager;

// Configuration for projectile spawning
typedef struct pz_projectile_config {
    float speed; // Default: 15.0
    int max_bounces; // Default: 1
    float lifetime; // Default: 5.0 seconds
    int damage; // Default: 1
    float scale; // Visual scale (1.0 = normal)
    pz_vec4 color; // Projectile color
    int8_t floor_level; // Floor level for collision (default: 0)
} pz_projectile_config;

// Default configuration
extern const pz_projectile_config PZ_PROJECTILE_DEFAULT;

/* ============================================================================
 * API
 * ============================================================================
 */

// Create/destroy manager
pz_projectile_manager *pz_projectile_manager_create(pz_renderer *renderer);
void pz_projectile_manager_destroy(
    pz_projectile_manager *mgr, pz_renderer *renderer);

// Spawn a new projectile
// Returns the projectile index, or -1 if no slots available
int pz_projectile_spawn(pz_projectile_manager *mgr, pz_vec2 pos,
    pz_vec2 direction, const pz_projectile_config *config, int owner_id);

// Update all projectiles (movement, collision, bouncing)
// tank_mgr can be NULL if no tank collision is desired
void pz_projectile_update(pz_projectile_manager *mgr, const pz_map *map,
    pz_tank_manager *tank_mgr, float dt);

// Lighting parameters for projectile rendering
typedef struct pz_projectile_render_params {
    pz_texture_handle light_texture;
    float light_scale_x, light_scale_z;
    float light_offset_x, light_offset_z;
} pz_projectile_render_params;

// Render all active projectiles
void pz_projectile_render(pz_projectile_manager *mgr, pz_renderer *renderer,
    const pz_mat4 *view_projection, const pz_projectile_render_params *params);

// Get number of active projectiles
int pz_projectile_count(const pz_projectile_manager *mgr);

// Get number of active projectiles owned by a specific tank
int pz_projectile_count_by_owner(
    const pz_projectile_manager *mgr, int owner_id);

// Get collision events from the last update (for particle spawning)
// Returns number of hits, fills hits array
int pz_projectile_get_hits(
    const pz_projectile_manager *mgr, pz_projectile_hit *hits, int max_hits);

#endif // PZ_PROJECTILE_H
