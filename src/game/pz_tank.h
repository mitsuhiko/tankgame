/*
 * Tank Game - Tank Entity System
 *
 * Handles tank entities with health, movement, and rendering.
 */

#ifndef PZ_TANK_H
#define PZ_TANK_H

#include <stdbool.h>
#include <stdint.h>

#include "../core/pz_math.h"
#include "../engine/pz_camera.h"
#include "../engine/render/pz_renderer.h"
#include "pz_map.h"
#include "pz_mesh.h"

// Maximum number of tanks
#define PZ_MAX_TANKS 8

// Maximum death events per frame
#define PZ_MAX_DEATH_EVENTS 8

// Maximum respawn events per frame
#define PZ_MAX_RESPAWN_EVENTS 8

// Death event - records when a tank dies
typedef struct pz_tank_death_event {
    int tank_id;
    pz_vec2 pos;
    bool is_player;
    int8_t floor_level; // Floor level where tank died
} pz_tank_death_event;

// Respawn event - records when a tank respawns
typedef struct pz_tank_respawn_event {
    int tank_id;
    bool is_player;
} pz_tank_respawn_event;

// Tank state flags
typedef enum {
    PZ_TANK_FLAG_ACTIVE = (1 << 0),
    PZ_TANK_FLAG_DEAD = (1 << 1),
    PZ_TANK_FLAG_INVULNERABLE = (1 << 2),
    PZ_TANK_FLAG_PLAYER = (1 << 3), // Is this a player-controlled tank?
    PZ_TANK_FLAG_INVINCIBLE = (1 << 4), // Debug: permanent invincibility
} pz_tank_flags;

// Forward declaration for weapon type
typedef enum pz_powerup_type pz_powerup_type;

// Maximum weapons in loadout
#define PZ_MAX_LOADOUT_WEAPONS 8

// Maximum barriers a tank can place at once
#define PZ_MAX_PLACED_BARRIERS 8

// Barrier placer state (when holding barrier_placer weapon)
typedef struct pz_tank_barrier_placer {
    char barrier_tile[32]; // Tile name for barriers
    float barrier_health; // Health for placed barriers
    int max_barriers; // Max barriers allowed at once
    float barrier_lifetime; // Barrier lifetime in seconds (0 = infinite)
    int placed_barrier_ids[PZ_MAX_PLACED_BARRIERS]; // IDs of placed barriers
    int placed_count; // How many barriers currently placed
} pz_tank_barrier_placer;

// Tank structure
typedef struct pz_tank {
    uint32_t flags;
    int id; // Unique tank ID (for projectile ownership)

    // Position and movement
    pz_vec2 pos; // Position in world space (X, Z)
    pz_vec2 vel; // Velocity
    float body_angle; // Body rotation (radians)
    float turret_angle; // Turret rotation in world space (radians)
    int8_t floor_level; // Current floor level (height of tile tank is on)

    // Jump pad state
    int jump_state; // 0 = none, 1 = countdown, 2 = in-air
    float jump_timer; // Countdown or elapsed jump time
    float jump_duration; // Total in-air time
    pz_vec2 jump_start_pos;
    pz_vec2 jump_end_pos;
    float jump_start_angle;
    float jump_end_angle;
    float jump_height; // Visual vertical offset for rendering
    float jump_apex_height; // Calculated arc apex to clear obstacles
    int jump_pad_tile_x; // Tile that triggered the jump (for countdown cancel)
    int jump_pad_tile_y;
    int jump_cooldown_tile_x; // Landing tile on cooldown
    int jump_cooldown_tile_y;
    bool jump_cooldown_active; // Whether cooldown is active
    bool just_jumped; // Set to true when a jump begins (for sound trigger)

    // Combat
    int health;
    int max_health;
    float fire_cooldown;

    // Weapon loadout (collected weapons)
    int loadout[PZ_MAX_LOADOUT_WEAPONS]; // Array of pz_powerup_type values
    int loadout_count; // Number of weapons in loadout
    int loadout_index; // Currently selected weapon index

    // Respawn
    float respawn_timer; // Countdown when dead
    float invuln_timer; // Invulnerability time remaining
    pz_vec2 spawn_pos; // Where to respawn
    int8_t spawn_floor_level; // Floor level at spawn point

    // Toxic cloud
    float toxic_damage_timer; // Time until next damage tick
    float toxic_grace_timer; // Invulnerability after respawn
    bool in_toxic_cloud; // Cached for rendering/UI

    // Mines
    int mine_count; // Number of mines the tank is carrying

    // Visual feedback
    float damage_flash; // Timer for damage flash effect (0 = no flash)
    float recoil; // Visual recoil amount (decays over time)
    pz_vec4 body_color;
    pz_vec4 turret_color; // Updated based on selected weapon
    float fog_timer; // Spawn timer for fog trail
    float idle_time; // How long the tank has been idle
    float spawn_indicator_timer; // Timer for spawn indicator (1.5 seconds)
    int player_number; // Player number (1-4) for spawn indicator display

    // Barrier placer state (when holding barrier_placer weapon)
    pz_tank_barrier_placer barrier_placer;
} pz_tank;

// Input for a tank (per-tick input state)
typedef struct pz_tank_input {
    pz_vec2 move_dir; // Normalized movement direction (0,0 = no input)
    float target_turret; // Target turret angle (world space radians)
    bool fire; // Fire button pressed
} pz_tank_input;

// Tank manager
typedef struct pz_tank_manager {
    pz_tank tanks[PZ_MAX_TANKS];
    int tank_count;
    int next_id;

    // Death events for this frame (cleared each tick)
    pz_tank_death_event death_events[PZ_MAX_DEATH_EVENTS];
    int death_event_count;

    // Respawn events for this frame (cleared each tick)
    pz_tank_respawn_event respawn_events[PZ_MAX_RESPAWN_EVENTS];
    int respawn_event_count;

    // Shared rendering resources
    pz_mesh *body_mesh;
    pz_mesh *turret_mesh;
    pz_mesh *shadow_mesh;
    pz_shader_handle shader;
    pz_pipeline_handle pipeline;
    pz_pipeline_handle shadow_pipeline;
    bool render_ready;

    // Movement parameters (shared by all tanks)
    float accel;
    float friction;
    float max_speed;
    float body_turn_speed;
    float turret_turn_speed;
    float collision_radius;
} pz_tank_manager;

// Configuration for tank manager
typedef struct pz_tank_manager_config {
    float accel; // Default: 40.0
    float friction; // Default: 25.0
    float max_speed; // Default: 5.0
    float body_turn_speed; // Default: 5.0 rad/s
    float turret_turn_speed; // Default: 8.0 rad/s
    float collision_radius; // Default: 0.7
} pz_tank_manager_config;

// Default configuration
extern const pz_tank_manager_config PZ_TANK_DEFAULT_CONFIG;

/* ============================================================================
 * Manager API
 * ============================================================================
 */

// Create/destroy manager
pz_tank_manager *pz_tank_manager_create(
    pz_renderer *renderer, const pz_tank_manager_config *config);
void pz_tank_manager_destroy(pz_tank_manager *mgr, pz_renderer *renderer);

// Spawn a new tank (returns tank pointer, or NULL if no slots)
pz_tank *pz_tank_spawn(
    pz_tank_manager *mgr, pz_vec2 pos, pz_vec4 color, bool is_player);

// Get tank by ID (returns NULL if not found or inactive)
pz_tank *pz_tank_get_by_id(pz_tank_manager *mgr, int id);

// Get player tank (first tank with PLAYER flag, or NULL)
pz_tank *pz_tank_get_player(pz_tank_manager *mgr);

// Iterate active tanks
typedef void (*pz_tank_iter_fn)(pz_tank *tank, void *user_data);
void pz_tank_foreach(pz_tank_manager *mgr, pz_tank_iter_fn fn, void *user_data);

/* ============================================================================
 * Tank Update
 * ============================================================================
 */

// Update a single tank with input
void pz_tank_update(pz_tank_manager *mgr, pz_tank *tank,
    const pz_tank_input *input, const pz_map *map,
    const pz_toxic_cloud *toxic_cloud, float dt);

// Utility queries for jump pad state
bool pz_tank_is_in_air(const pz_tank *tank);
bool pz_tank_can_fire(const pz_tank *tank);

// Update all tanks (for AI/respawn)
void pz_tank_update_all(pz_tank_manager *mgr, const pz_map *map,
    const pz_toxic_cloud *toxic_cloud, float dt);

/* ============================================================================
 * Combat
 * ============================================================================
 */

// Apply damage to a tank (returns true if tank was killed)
// Use pz_tank_apply_damage for proper death event recording
bool pz_tank_damage(pz_tank *tank, int amount);

// Apply damage to a tank and record death event if killed
// This is the preferred way to damage tanks
bool pz_tank_apply_damage(pz_tank_manager *mgr, pz_tank *tank, int amount);

// Check if a circle at pos with radius hits any tank (except exclude_id)
// Returns the tank that was hit, or NULL
pz_tank *pz_tank_check_collision(
    pz_tank_manager *mgr, pz_vec2 pos, float radius, int exclude_id);

// Check collision only with tanks on a specific floor level
// Returns the tank that was hit, or NULL
pz_tank *pz_tank_check_collision_on_floor(pz_tank_manager *mgr, pz_vec2 pos,
    float radius, int exclude_id, int8_t floor_level);

// Respawn a dead tank at its spawn point
void pz_tank_respawn(pz_tank *tank);

/* ============================================================================
 * Weapon Loadout
 * ============================================================================
 */

// Add a weapon to the tank's loadout (returns true if added, false if already
// has it)
bool pz_tank_add_weapon(pz_tank *tank, int weapon_type);

// Cycle weapon selection (scroll_delta: +1 = next, -1 = previous)
void pz_tank_cycle_weapon(pz_tank *tank, int scroll_delta);

// Get currently selected weapon type
int pz_tank_get_current_weapon(const pz_tank *tank);

// Reset loadout to default weapon only
void pz_tank_reset_loadout(pz_tank *tank);

// Set barrier placer data when collecting a barrier_placer powerup
// lifetime: time in seconds until placed barriers auto-destroy (0 = infinite)
void pz_tank_set_barrier_placer(pz_tank *tank, const char *tile, float health,
    int max_count, float lifetime);

// Add a placed barrier to the tank's tracking
// Returns true if added successfully
bool pz_tank_add_placed_barrier(pz_tank *tank, int barrier_id);

// Notify tank that one of its barriers was destroyed
void pz_tank_on_barrier_destroyed(pz_tank *tank, int barrier_id);

// Check if tank can place another barrier
bool pz_tank_can_place_barrier(const pz_tank *tank);

// Get barrier placer state (NULL if not holding barrier_placer)
const pz_tank_barrier_placer *pz_tank_get_barrier_placer(const pz_tank *tank);

/* ============================================================================
 * Rendering
 * ============================================================================
 */

// Lighting parameters for tank rendering
typedef struct pz_tank_render_params {
    pz_texture_handle light_texture;
    float light_scale_x, light_scale_z;
    float light_offset_x, light_offset_z;
    bool has_toxic;
    pz_vec3 toxic_color;
} pz_tank_render_params;

// Render all active tanks
void pz_tank_render(pz_tank_manager *mgr, pz_renderer *renderer,
    const pz_mat4 *view_projection, const pz_tank_render_params *params);

/* ============================================================================
 * Utility
 * ============================================================================
 */

// Get barrel tip position for a tank (for projectile spawning)
pz_vec2 pz_tank_get_barrel_tip(const pz_tank *tank);

// Get fire direction (normalized)
pz_vec2 pz_tank_get_fire_direction(const pz_tank *tank);

// Compute spawn position + firing direction (handles barrel-wall deflection)
bool pz_tank_get_fire_solution(const pz_tank *tank, const pz_map *map,
    pz_vec2 *out_pos, pz_vec2 *out_dir, int *out_bounce_cost);

// Returns true if the barrel path from the tank center to the tip is clear
bool pz_tank_barrel_is_clear(const pz_tank *tank, const pz_map *map);

// Update tank's floor level based on current position and map
// Call after spawn, respawn, and jump pad landing
void pz_tank_update_floor_level(pz_tank *tank, const pz_map *map);

// Get count of active (non-dead) tanks
int pz_tank_count_active(const pz_tank_manager *mgr);

// Get count of active enemy tanks (non-player, non-dead)
int pz_tank_count_enemies_alive(const pz_tank_manager *mgr);

// Get death events from this frame
// Returns number of events, fills events array
int pz_tank_get_death_events(
    const pz_tank_manager *mgr, pz_tank_death_event *events, int max_events);

// Clear death events (call at start of each frame)
void pz_tank_clear_death_events(pz_tank_manager *mgr);

// Get respawn events from this frame
// Returns number of events, fills events array
int pz_tank_get_respawn_events(
    const pz_tank_manager *mgr, pz_tank_respawn_event *events, int max_events);

// Clear respawn events (call at start of each frame)
void pz_tank_clear_respawn_events(pz_tank_manager *mgr);

#endif // PZ_TANK_H
