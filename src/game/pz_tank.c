/*
 * Tank Game - Tank Entity System Implementation
 */

#include "pz_tank.h"
#include "pz_collision.h"
#include "pz_powerup.h"
#include "pz_toxic_cloud.h"

#include <math.h>
#include <string.h>

#include "../core/pz_log.h"
#include "../core/pz_mem.h"

/* ============================================================================
 * Constants
 * ============================================================================
 */

// Barrel length from turret center to tip (must match turret mesh)
static const float BARREL_LENGTH = 1.65f;
// Allow tiny overlap before treating the barrel as blocked
static const float BARREL_CLEAR_EPSILON = 0.02f;
// Small offset to push a deflected projectile off the wall
static const float BARREL_DEFLECT_EPSILON = 0.01f;

// Turret height offset above ground
static const float TURRET_Y_OFFSET = 0.65f;

// Shadow dimensions and offset
static const float SHADOW_WIDTH = 1.7f;
static const float SHADOW_LENGTH = 2.5f;
static const float SHADOW_Y_OFFSET = 0.02f;
static const float SHADOW_ALPHA = 0.35f;
static const float SHADOW_SOFTNESS = 0.09f;

// Time before respawn after death
static const float RESPAWN_DELAY = 3.0f;

// Duration of damage flash effect
static const float DAMAGE_FLASH_DURATION = 0.15f;

// Duration of invulnerability after respawn
static const float INVULN_DURATION = 1.5f;

// Default health
static const int DEFAULT_HEALTH = 10;

static pz_mesh *
create_shadow_mesh(void)
{
    float half_w = SHADOW_WIDTH * 0.5f;
    float half_l = SHADOW_LENGTH * 0.5f;
    pz_mesh_vertex verts[6] = {
        { -half_w, 0.0f, -half_l, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f },
        { half_w, 0.0f, -half_l, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f },
        { half_w, 0.0f, half_l, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f },
        { -half_w, 0.0f, -half_l, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f },
        { half_w, 0.0f, half_l, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f },
        { -half_w, 0.0f, half_l, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f },
    };

    return pz_mesh_create_from_data(verts, 6);
}

static bool
tank_circle_hits_map(
    const pz_map *map, pz_vec2 center, float radius, int8_t floor_level)
{
    if (!map)
        return false;

    float half_w = map->world_width / 2.0f;
    float half_h = map->world_height / 2.0f;
    float ts = map->tile_size;

    int min_tx = (int)floorf((center.x - radius + half_w) / ts);
    int max_tx = (int)floorf((center.x + radius + half_w) / ts);
    int min_ty = (int)floorf((center.y - radius + half_h) / ts);
    int max_ty = (int)floorf((center.y + radius + half_h) / ts);

    pz_circle circle = pz_circle_new(center, radius);

    for (int ty = min_ty; ty <= max_ty; ty++) {
        for (int tx = min_tx; tx <= max_tx; tx++) {
            if (!pz_map_in_bounds(map, tx, ty)) {
                return true;
            }
            // Tank can only move on tiles at its floor level
            if (pz_map_get_height(map, tx, ty) == floor_level) {
                continue;
            }

            float min_x = tx * ts - half_w;
            float min_y = ty * ts - half_h;
            float max_x = min_x + ts;
            float max_y = min_y + ts;

            pz_aabb box = pz_aabb_new(
                (pz_vec2) { min_x, min_y }, (pz_vec2) { max_x, max_y });

            if (pz_collision_circle_aabb(circle, box, NULL)) {
                return true;
            }
        }
    }

    return false;
}

static void
resolve_tank_circle_map(
    const pz_map *map, pz_vec2 *center, float radius, int8_t floor_level)
{
    if (!map || !center)
        return;

    float half_w = map->world_width / 2.0f;
    float half_h = map->world_height / 2.0f;
    float ts = map->tile_size;

    for (int iter = 0; iter < 4; iter++) {
        bool any = false;

        float min_x = -half_w + radius;
        float max_x = half_w - radius;
        float min_y = -half_h + radius;
        float max_y = half_h - radius;

        if (center->x < min_x) {
            center->x = min_x;
            any = true;
        } else if (center->x > max_x) {
            center->x = max_x;
            any = true;
        }
        if (center->y < min_y) {
            center->y = min_y;
            any = true;
        } else if (center->y > max_y) {
            center->y = max_y;
            any = true;
        }

        int min_tx = (int)floorf((center->x - radius + half_w) / ts);
        int max_tx = (int)floorf((center->x + radius + half_w) / ts);
        int min_ty = (int)floorf((center->y - radius + half_h) / ts);
        int max_ty = (int)floorf((center->y + radius + half_h) / ts);

        for (int ty = min_ty; ty <= max_ty; ty++) {
            for (int tx = min_tx; tx <= max_tx; tx++) {
                if (!pz_map_in_bounds(map, tx, ty)) {
                    continue;
                }
                // Tank can only move on tiles at its floor level
                if (pz_map_get_height(map, tx, ty) == floor_level) {
                    continue;
                }

                float tile_min_x = tx * ts - half_w;
                float tile_min_y = ty * ts - half_h;
                float tile_max_x = tile_min_x + ts;
                float tile_max_y = tile_min_y + ts;
                pz_vec2 push_out = { 0.0f, 0.0f };
                pz_circle circle = pz_circle_new(*center, radius);

                pz_aabb box = pz_aabb_new((pz_vec2) { tile_min_x, tile_min_y },
                    (pz_vec2) { tile_max_x, tile_max_y });

                if (pz_collision_circle_aabb(circle, box, &push_out)) {
                    center->x += push_out.x;
                    center->y += push_out.y;
                    any = true;
                }
            }
        }

        if (!any) {
            break;
        }
    }
}

static bool
tank_circle_hits_tanks(pz_tank_manager *mgr, pz_vec2 center, float radius,
    int exclude_id, int8_t floor_level)
{
    if (!mgr)
        return false;

    pz_circle circle = pz_circle_new(center, radius);

    for (int i = 0; i < PZ_MAX_TANKS; i++) {
        pz_tank *tank = &mgr->tanks[i];

        if (!(tank->flags & PZ_TANK_FLAG_ACTIVE))
            continue;
        if (tank->flags & PZ_TANK_FLAG_DEAD)
            continue;
        if (tank->id == exclude_id)
            continue;
        // Only collide with tanks on the same floor
        if (tank->floor_level != floor_level)
            continue;

        pz_circle other = pz_circle_new(tank->pos, mgr->collision_radius);
        if (pz_collision_circle_circle(circle, other, NULL, NULL)) {
            return true;
        }
    }

    return false;
}

// Push a tank out of overlap with other tanks, AND push other tanks (shoving)
// The moving tank pushes other tanks in its movement direction
static void
resolve_tank_circle_tanks(
    pz_tank_manager *mgr, pz_tank *tank, pz_vec2 *center, float radius)
{
    if (!mgr || !tank || !center)
        return;

    // Calculate movement direction for shoving
    pz_vec2 move_delta = pz_vec2_sub(*center, tank->pos);
    float move_len = pz_vec2_len(move_delta);
    pz_vec2 move_dir = { 0.0f, 0.0f };
    if (move_len > 0.001f) {
        move_dir = pz_vec2_scale(move_delta, 1.0f / move_len);
    }

    // Shoving strength: how much of the penetration is transferred to other
    // tank A higher value means more shoving, lower means more stopping
    const float shove_ratio = 0.6f;

    // Iterate a few times to resolve multiple overlaps
    for (int iter = 0; iter < 4; iter++) {
        bool any_push = false;

        for (int i = 0; i < PZ_MAX_TANKS; i++) {
            pz_tank *other = &mgr->tanks[i];

            if (!(other->flags & PZ_TANK_FLAG_ACTIVE))
                continue;
            if (other->flags & PZ_TANK_FLAG_DEAD)
                continue;
            if (other->id == tank->id)
                continue;
            // Only collide with tanks on the same floor
            if (other->floor_level != tank->floor_level)
                continue;

            pz_circle a = pz_circle_new(*center, radius);
            pz_circle b = pz_circle_new(other->pos, mgr->collision_radius);

            pz_vec2 normal;
            float penetration;
            if (pz_collision_circle_circle(a, b, &normal, &penetration)) {
                // Calculate how much to shove vs how much to push self back
                // If moving toward the other tank, shove more
                float toward_other = pz_vec2_dot(move_dir, normal);
                float effective_shove = shove_ratio;
                if (toward_other < 0.0f) {
                    // Moving away from other tank - no shoving
                    effective_shove = 0.0f;
                } else {
                    // Scale shove by how directly we're moving toward them
                    effective_shove = shove_ratio * toward_other;
                }

                // Push the other tank (shoving)
                float shove_amount = penetration * effective_shove;
                pz_vec2 shove_delta = pz_vec2_scale(normal, shove_amount);

                // Apply shove to other tank's position
                other->pos = pz_vec2_add(other->pos, shove_delta);

                // Push this tank out by the remaining penetration
                float self_push = penetration * (1.0f - effective_shove * 0.5f);
                center->x -= normal.x * self_push * 0.5f;
                center->y -= normal.y * self_push * 0.5f;

                any_push = true;
            }
        }

        if (!any_push)
            break;
    }
}

// Update turret color to match current weapon's projectile color
static void
update_turret_color(pz_tank *tank)
{
    if (!tank)
        return;

    int weapon_type = pz_tank_get_current_weapon(tank);
    const pz_weapon_stats *stats
        = pz_weapon_get_stats((pz_powerup_type)weapon_type);

    // Use the projectile color for the turret
    tank->turret_color = stats->projectile_color;
}

/* ============================================================================
 * Default Configuration
 * ============================================================================
 */

const pz_tank_manager_config PZ_TANK_DEFAULT_CONFIG = {
    .accel = 40.0f,
    .friction = 25.0f,
    .max_speed = 3.5f,
    .body_turn_speed = 3.5f,
    .turret_turn_speed = 5.6f,
    .collision_radius = 0.9f,
};

/* ============================================================================
 * Manager Lifecycle
 * ============================================================================
 */

pz_tank_manager *
pz_tank_manager_create(
    pz_renderer *renderer, const pz_tank_manager_config *config)
{
    pz_tank_manager *mgr = pz_calloc(1, sizeof(pz_tank_manager));

    // Use default config if none provided
    if (!config) {
        config = &PZ_TANK_DEFAULT_CONFIG;
    }

    mgr->accel = config->accel;
    mgr->friction = config->friction;
    mgr->max_speed = config->max_speed;
    mgr->body_turn_speed = config->body_turn_speed;
    mgr->turret_turn_speed = config->turret_turn_speed;
    mgr->collision_radius = config->collision_radius;
    mgr->next_id = 1;

    // Create meshes
    mgr->body_mesh = pz_mesh_create_tank_body();
    mgr->turret_mesh = pz_mesh_create_tank_turret();
    mgr->shadow_mesh = create_shadow_mesh();

    if (mgr->body_mesh) {
        pz_mesh_upload(mgr->body_mesh, renderer);
    }
    if (mgr->turret_mesh) {
        pz_mesh_upload(mgr->turret_mesh, renderer);
    }
    if (mgr->shadow_mesh) {
        pz_mesh_upload(mgr->shadow_mesh, renderer);
    }

    // Load shader
    mgr->shader = pz_renderer_load_shader(
        renderer, "shaders/entity.vert", "shaders/entity.frag", "tank");

    if (mgr->shader != PZ_INVALID_HANDLE) {
        pz_pipeline_desc desc = {
            .shader = mgr->shader,
            .vertex_layout = pz_mesh_get_vertex_layout(),
            .blend = PZ_BLEND_NONE,
            .depth = PZ_DEPTH_READ_WRITE,
            .cull = PZ_CULL_BACK,
            .primitive = PZ_PRIMITIVE_TRIANGLES,
        };
        mgr->pipeline = pz_renderer_create_pipeline(renderer, &desc);

        pz_pipeline_desc shadow_desc = desc;
        shadow_desc.blend = PZ_BLEND_ALPHA;
        shadow_desc.depth = PZ_DEPTH_READ;
        shadow_desc.cull = PZ_CULL_NONE;
        mgr->shadow_pipeline
            = pz_renderer_create_pipeline(renderer, &shadow_desc);

        mgr->render_ready = (mgr->pipeline != PZ_INVALID_HANDLE);
    }

    if (!mgr->render_ready) {
        pz_log(PZ_LOG_WARN, PZ_LOG_CAT_GAME,
            "Tank rendering not available (shader/pipeline failed)");
    }

    pz_log(PZ_LOG_INFO, PZ_LOG_CAT_GAME, "Tank manager created");
    return mgr;
}

void
pz_tank_manager_destroy(pz_tank_manager *mgr, pz_renderer *renderer)
{
    if (!mgr)
        return;

    if (mgr->pipeline != PZ_INVALID_HANDLE) {
        pz_renderer_destroy_pipeline(renderer, mgr->pipeline);
    }
    if (mgr->shadow_pipeline != PZ_INVALID_HANDLE) {
        pz_renderer_destroy_pipeline(renderer, mgr->shadow_pipeline);
    }
    if (mgr->shader != PZ_INVALID_HANDLE) {
        pz_renderer_destroy_shader(renderer, mgr->shader);
    }
    if (mgr->body_mesh) {
        pz_mesh_destroy(mgr->body_mesh, renderer);
    }
    if (mgr->turret_mesh) {
        pz_mesh_destroy(mgr->turret_mesh, renderer);
    }
    if (mgr->shadow_mesh) {
        pz_mesh_destroy(mgr->shadow_mesh, renderer);
    }

    pz_free(mgr);
    pz_log(PZ_LOG_INFO, PZ_LOG_CAT_GAME, "Tank manager destroyed");
}

/* ============================================================================
 * Tank Spawning
 * ============================================================================
 */

pz_tank *
pz_tank_spawn(pz_tank_manager *mgr, pz_vec2 pos, pz_vec4 color, bool is_player)
{
    if (!mgr)
        return NULL;

    // Find free slot
    int slot = -1;
    for (int i = 0; i < PZ_MAX_TANKS; i++) {
        if (!(mgr->tanks[i].flags & PZ_TANK_FLAG_ACTIVE)) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        pz_log(PZ_LOG_WARN, PZ_LOG_CAT_GAME, "No free tank slots (max=%d)",
            PZ_MAX_TANKS);
        return NULL;
    }

    pz_tank *tank = &mgr->tanks[slot];
    memset(tank, 0, sizeof(pz_tank));

    tank->flags = PZ_TANK_FLAG_ACTIVE;
    if (is_player) {
        tank->flags |= PZ_TANK_FLAG_PLAYER;
    }

    tank->id = mgr->next_id++;
    tank->pos = pos;
    tank->spawn_pos = pos;
    tank->vel = (pz_vec2) { 0.0f, 0.0f };
    tank->body_angle = 0.0f;
    tank->turret_angle = 0.0f;

    // Jump state
    tank->jump_state = 0;
    tank->jump_timer = 0.0f;
    tank->jump_duration = 0.0f;
    tank->jump_start_pos = pos;
    tank->jump_end_pos = pos;
    tank->jump_start_angle = 0.0f;
    tank->jump_end_angle = 0.0f;
    tank->jump_height = 0.0f;
    tank->jump_apex_height = 1.0f;
    tank->jump_pad_tile_x = -1;
    tank->jump_pad_tile_y = -1;
    tank->jump_cooldown_tile_x = -1;
    tank->jump_cooldown_tile_y = -1;
    tank->jump_cooldown_active = false;

    tank->health = DEFAULT_HEALTH;
    tank->max_health = DEFAULT_HEALTH;
    tank->fire_cooldown = 0.0f;
    tank->toxic_damage_timer = 0.0f;
    tank->toxic_grace_timer = 0.0f;
    tank->in_toxic_cloud = false;

    // Initialize loadout with default weapon
    tank->loadout[0] = PZ_POWERUP_NONE; // Default cannon
    tank->loadout_count = 1;
    tank->loadout_index = 0;

    // Initialize mines (player tanks start with max mines)
    tank->mine_count = is_player ? 2 : 0;

    tank->body_color = color;
    // Turret color matches current weapon's projectile color
    update_turret_color(tank);

    // Spawn indicator for player tanks
    if (is_player) {
        tank->spawn_indicator_timer = 1.5f;
        // Count existing player tanks to determine player number
        int player_count = 0;
        for (int i = 0; i < PZ_MAX_TANKS; i++) {
            if ((mgr->tanks[i].flags & PZ_TANK_FLAG_ACTIVE)
                && (mgr->tanks[i].flags & PZ_TANK_FLAG_PLAYER)
                && mgr->tanks[i].id != tank->id) {
                player_count++;
            }
        }
        tank->player_number = player_count + 1; // P1, P2, P3, P4
    } else {
        tank->spawn_indicator_timer = 0.0f;
        tank->player_number = 0;
    }

    mgr->tank_count++;

    pz_log(PZ_LOG_INFO, PZ_LOG_CAT_GAME,
        "Tank spawned at (%.2f, %.2f), id=%d, player=%d", pos.x, pos.y,
        tank->id, is_player);

    return tank;
}

pz_tank *
pz_tank_get_by_id(pz_tank_manager *mgr, int id)
{
    if (!mgr)
        return NULL;

    for (int i = 0; i < PZ_MAX_TANKS; i++) {
        pz_tank *tank = &mgr->tanks[i];
        if ((tank->flags & PZ_TANK_FLAG_ACTIVE) && tank->id == id) {
            return tank;
        }
    }
    return NULL;
}

pz_tank *
pz_tank_get_player(pz_tank_manager *mgr)
{
    if (!mgr)
        return NULL;

    for (int i = 0; i < PZ_MAX_TANKS; i++) {
        pz_tank *tank = &mgr->tanks[i];
        if ((tank->flags & PZ_TANK_FLAG_ACTIVE)
            && (tank->flags & PZ_TANK_FLAG_PLAYER)) {
            return tank;
        }
    }
    return NULL;
}

void
pz_tank_foreach(pz_tank_manager *mgr, pz_tank_iter_fn fn, void *user_data)
{
    if (!mgr || !fn)
        return;

    for (int i = 0; i < PZ_MAX_TANKS; i++) {
        pz_tank *tank = &mgr->tanks[i];
        if (tank->flags & PZ_TANK_FLAG_ACTIVE) {
            fn(tank, user_data);
        }
    }
}

/* ============================================================================
 * Tank Update
 * ============================================================================
 */

// Normalize angle to [-PI, PI]
static float
normalize_angle(float angle)
{
    while (angle > PZ_PI)
        angle -= 2.0f * PZ_PI;
    while (angle < -PZ_PI)
        angle += 2.0f * PZ_PI;
    return angle;
}

static bool
tank_is_in_air(const pz_tank *tank)
{
    return tank && tank->jump_state == 2;
}

bool
pz_tank_is_in_air(const pz_tank *tank)
{
    return tank_is_in_air(tank);
}

bool
pz_tank_can_fire(const pz_tank *tank)
{
    if (!tank)
        return false;
    // Firing is disabled while in-air jump animation is active
    return !tank_is_in_air(tank);
}

// Calculate the minimum apex height for a jump arc to clear all walls
// along the path from start to end.
// The arc follows: height(t) = sin(PI * t) * apex_height
// For a wall at position t with height h, we need: sin(PI*t) * apex >= h
// So: apex >= h / sin(PI*t)
static float
calculate_jump_apex_for_path(const pz_map *map, pz_vec2 start, pz_vec2 end)
{
    if (!map) {
        return 1.0f; // Default apex
    }

    // Sample points along the path to find max required apex
    float min_apex = 1.0f; // Minimum jump height even with no obstacles
    float clearance = 0.5f; // Extra clearance above walls

    pz_vec2 delta = pz_vec2_sub(end, start);
    float path_length = pz_vec2_len(delta);
    if (path_length < 0.01f) {
        return min_apex;
    }

    // Sample every 0.25 tile units
    float sample_step = 0.25f;
    int num_samples = (int)(path_length / sample_step) + 1;
    if (num_samples < 2)
        num_samples = 2;

    for (int i = 1; i < num_samples - 1; i++) {
        float t = (float)i / (float)(num_samples - 1);
        pz_vec2 sample_pos = pz_vec2_lerp(start, end, t);

        int tile_x, tile_y;
        pz_map_world_to_tile(map, sample_pos, &tile_x, &tile_y);

        int8_t wall_height = pz_map_get_height(map, tile_x, tile_y);
        if (wall_height <= 0) {
            continue; // No obstacle at this position
        }

        // Calculate required apex to clear this wall
        // height(t) = sin(PI*t) * apex >= wall_height + clearance
        // apex >= (wall_height + clearance) / sin(PI*t)
        float sin_t = sinf(PZ_PI * t);
        if (sin_t < 0.05f) {
            // Very close to start/end - use a capped value
            // This happens when there's a wall right at the edge
            sin_t = 0.05f;
        }

        float required_apex = ((float)wall_height + clearance) / sin_t;
        if (required_apex > min_apex) {
            min_apex = required_apex;
        }
    }

    return min_apex;
}

void
pz_tank_update(pz_tank_manager *mgr, pz_tank *tank, const pz_tank_input *input,
    const pz_map *map, const pz_toxic_cloud *toxic_cloud, float dt)
{
    if (!mgr || !tank || !input)
        return;

    // Skip dead tanks (they have their own update logic)
    if (tank->flags & PZ_TANK_FLAG_DEAD)
        return;

    // Update damage flash
    if (tank->damage_flash > 0.0f) {
        tank->damage_flash -= dt;
        if (tank->damage_flash < 0.0f)
            tank->damage_flash = 0.0f;
    }

    // Update fire cooldown
    if (tank->fire_cooldown > 0.0f) {
        tank->fire_cooldown -= dt;
        if (tank->fire_cooldown < 0.0f)
            tank->fire_cooldown = 0.0f;
    }

    // Get terrain properties at tank position
    float terrain_speed_mult = 1.0f;
    float terrain_friction = 1.0f;
    if (map) {
        terrain_speed_mult = pz_map_get_speed_multiplier(map, tank->pos);
        terrain_friction = pz_map_get_friction(map, tank->pos);
    }

    // Handle jump pad countdown and in-air movement
    if (tank->jump_state == 1) {
        // Countdown: player can still move, but if they leave the pad tile,
        // the jump is cancelled
        int current_tile_x = 0, current_tile_y = 0;
        if (map) {
            pz_map_world_to_tile(
                map, tank->pos, &current_tile_x, &current_tile_y);
        }

        if (current_tile_x != tank->jump_pad_tile_x
            || current_tile_y != tank->jump_pad_tile_y) {
            // Player left the jump pad - cancel the jump
            tank->jump_state = 0;
            tank->jump_timer = 0.0f;
            tank->jump_duration = 0.0f;
            tank->jump_pad_tile_x = -1;
            tank->jump_pad_tile_y = -1;
        } else {
            tank->jump_timer -= dt;
            if (tank->jump_timer <= 0.0f) {
                // Start the jump from current position
                tank->jump_state = 2;
                tank->jump_timer = 0.0f;
                tank->jump_start_pos = tank->pos;
                tank->jump_start_angle = tank->body_angle;
                pz_vec2 delta
                    = pz_vec2_sub(tank->jump_end_pos, tank->jump_start_pos);
                tank->jump_end_angle = atan2f(delta.x, delta.y);
                tank->vel = (pz_vec2) { 0.0f, 0.0f };
                tank->just_jumped = true; // Signal for sound effect

                // Calculate arc apex height to clear any walls in the path
                tank->jump_apex_height = calculate_jump_apex_for_path(
                    map, tank->jump_start_pos, tank->jump_end_pos);
            }
        }
    } else if (tank->jump_state == 2) {
        // In-air: interpolate between start and end, ignore collisions
        tank->jump_timer += dt;
        float t = (tank->jump_duration > 0.0f)
            ? pz_clampf(tank->jump_timer / tank->jump_duration, 0.0f, 1.0f)
            : 1.0f;
        tank->pos = pz_vec2_lerp(tank->jump_start_pos, tank->jump_end_pos, t);

        // Jump arc for visual height (calculated to clear obstacles)
        tank->jump_height = sinf(PZ_PI * t) * tank->jump_apex_height;

        // Interpolate body angle toward jump direction
        float angle_diff
            = normalize_angle(tank->jump_end_angle - tank->jump_start_angle);
        float angle_t = t;
        tank->body_angle
            = normalize_angle(tank->jump_start_angle + angle_diff * angle_t);

        // Zero horizontal velocity while in air
        tank->vel = (pz_vec2) { 0.0f, 0.0f };

        if (t >= 1.0f) {
            // Land at target tile, push out of overlaps
            tank->pos = tank->jump_end_pos;
            tank->jump_state = 0;
            tank->jump_timer = 0.0f;
            tank->jump_duration = 0.0f;

            // Update floor level for new position (may have changed floors)
            pz_tank_update_floor_level(tank, map);

            // Set cooldown on landing tile - pad won't reactivate until
            // player leaves this tile
            int land_tile_x = 0, land_tile_y = 0;
            if (map) {
                pz_map_world_to_tile(
                    map, tank->pos, &land_tile_x, &land_tile_y);
            }
            tank->jump_cooldown_tile_x = land_tile_x;
            tank->jump_cooldown_tile_y = land_tile_y;
            tank->jump_cooldown_active = true;

            float r = mgr->collision_radius;
            pz_vec2 pos = tank->pos;
            if (map) {
                resolve_tank_circle_map(map, &pos, r, tank->floor_level);
            }
            resolve_tank_circle_tanks(mgr, tank, &pos, r);
            tank->pos = pos;
            tank->jump_height = 0.0f;
        }
    }

    // Toxic cloud effects
    tank->in_toxic_cloud = false;
    if (toxic_cloud && toxic_cloud->config.enabled) {
        if (tank->toxic_grace_timer > 0.0f) {
            tank->toxic_grace_timer -= dt;
            if (tank->toxic_grace_timer < 0.0f) {
                tank->toxic_grace_timer = 0.0f;
            }
        }

        bool in_cloud = pz_toxic_cloud_is_inside(toxic_cloud, tank->pos);
        tank->in_toxic_cloud = in_cloud;

        float damage_interval = toxic_cloud->config.damage_interval;
        if (tank->toxic_damage_timer <= 0.0f) {
            tank->toxic_damage_timer
                = damage_interval > 0.0f ? damage_interval : 0.0f;
        }

        if (in_cloud) {
            terrain_speed_mult *= toxic_cloud->config.slowdown;

            bool damaging = pz_toxic_cloud_is_damaging(toxic_cloud, tank->pos);
            if (damaging && tank->toxic_grace_timer <= 0.0f) {
                tank->toxic_damage_timer -= dt;
                if (tank->toxic_damage_timer <= 0.0f) {
                    pz_tank_apply_damage(mgr, tank, toxic_cloud->config.damage);
                    tank->toxic_damage_timer
                        = damage_interval > 0.0f ? damage_interval : 0.0f;
                }
            } else {
                tank->toxic_damage_timer
                    = damage_interval > 0.0f ? damage_interval : 0.0f;
            }
        } else {
            tank->toxic_damage_timer
                = damage_interval > 0.0f ? damage_interval : 0.0f;
        }
    }

    // Apply acceleration in input direction (disabled while in-air)
    if (tank->jump_state != 2 && pz_vec2_len_sq(input->move_dir) > 0.0f) {
        pz_vec2 dir = pz_vec2_normalize(input->move_dir);
        tank->vel = pz_vec2_add(tank->vel, pz_vec2_scale(dir, mgr->accel * dt));

        // Rotate body towards movement direction (unless jump is controlling
        // it)
        if (tank->jump_state == 0) {
            float target_angle = atan2f(dir.x, dir.y);
            float angle_diff = normalize_angle(target_angle - tank->body_angle);
            tank->body_angle
                += angle_diff * pz_minf(1.0f, mgr->body_turn_speed * dt);
        }
    }

    // Apply friction (scaled by terrain friction - higher friction = faster
    // stop)
    float speed = pz_vec2_len(tank->vel);
    if (speed > 0.0f) {
        float friction_amount = mgr->friction * terrain_friction * dt;
        if (friction_amount > speed)
            friction_amount = speed;
        tank->vel = pz_vec2_sub(tank->vel,
            pz_vec2_scale(pz_vec2_normalize(tank->vel), friction_amount));
    }

    // Clamp to max speed (scaled by terrain speed multiplier)
    float effective_max_speed = mgr->max_speed * terrain_speed_mult;
    speed = pz_vec2_len(tank->vel);
    if (speed > effective_max_speed) {
        tank->vel
            = pz_vec2_scale(pz_vec2_normalize(tank->vel), effective_max_speed);
    }

    // Update position (skip ground collision while in-air)
    pz_vec2 new_pos = pz_vec2_add(tank->pos, pz_vec2_scale(tank->vel, dt));

    if (tank->jump_state != 2) {
        // Wall + tank collision (separate axis) using circle checks
        float r = mgr->collision_radius;
        pz_vec2 pos = tank->pos;
        int8_t floor = tank->floor_level;

        pz_vec2 test_x = { new_pos.x, pos.y };
        bool hit_map_x = map && tank_circle_hits_map(map, test_x, r, floor);
        bool hit_tank_x
            = tank_circle_hits_tanks(mgr, test_x, r, tank->id, floor);
        if (!hit_map_x && !hit_tank_x) {
            pos.x = new_pos.x;
        } else {
            tank->vel.x = 0;
        }

        pz_vec2 test_y = { pos.x, new_pos.y };
        bool hit_map_y = map && tank_circle_hits_map(map, test_y, r, floor);
        bool hit_tank_y
            = tank_circle_hits_tanks(mgr, test_y, r, tank->id, floor);
        if (!hit_map_y && !hit_tank_y) {
            pos.y = new_pos.y;
        } else {
            tank->vel.y = 0;
        }

        if (map) {
            resolve_tank_circle_map(map, &pos, r, floor);
        }

        // Resolve tank-tank overlaps
        resolve_tank_circle_tanks(mgr, tank, &pos, r);

        tank->pos = pos;
    }

    // Check for jump pad activation when on ground
    if (map && tank->jump_state == 0) {
        int tile_x = 0, tile_y = 0;
        pz_map_world_to_tile(map, tank->pos, &tile_x, &tile_y);

        // Clear cooldown if we've moved off the cooldown tile
        if (tank->jump_cooldown_active) {
            if (tile_x != tank->jump_cooldown_tile_x
                || tile_y != tank->jump_cooldown_tile_y) {
                tank->jump_cooldown_active = false;
                tank->jump_cooldown_tile_x = -1;
                tank->jump_cooldown_tile_y = -1;
            }
        }

        // Only activate jump pad if not on cooldown for this tile
        bool on_cooldown = tank->jump_cooldown_active
            && tile_x == tank->jump_cooldown_tile_x
            && tile_y == tank->jump_cooldown_tile_y;

        int target_x = 0, target_y = 0;
        if (!on_cooldown
            && pz_map_get_jump_pad_target(
                map, tile_x, tile_y, &target_x, &target_y)) {
            tank->jump_state = 1; // countdown
            tank->jump_timer = 0.5f; // seconds before jump
            tank->jump_duration = 0.6f; // in-air duration
            tank->jump_pad_tile_x = tile_x; // remember which tile triggered
            tank->jump_pad_tile_y = tile_y;
            tank->jump_start_pos = tank->pos; // start from current position
            tank->jump_end_pos = pz_map_tile_to_world(map, target_x, target_y);
            tank->jump_start_angle = tank->body_angle;
            pz_vec2 delta
                = pz_vec2_sub(tank->jump_end_pos, tank->jump_start_pos);
            tank->jump_end_angle = atan2f(delta.x, delta.y);
            tank->jump_height = 0.0f;
        }
    }

    // Turret rotation (smooth interpolation toward target)
    float turret_diff
        = normalize_angle(input->target_turret - tank->turret_angle);
    tank->turret_angle
        += turret_diff * pz_minf(1.0f, mgr->turret_turn_speed * dt);
}

void
pz_tank_update_all(pz_tank_manager *mgr, const pz_map *map,
    const pz_toxic_cloud *toxic_cloud, float dt)
{
    (void)map; // Reserved for future map-based tank updates

    if (!mgr)
        return;

    for (int i = 0; i < PZ_MAX_TANKS; i++) {
        pz_tank *tank = &mgr->tanks[i];
        if (!(tank->flags & PZ_TANK_FLAG_ACTIVE))
            continue;

        // Handle dead tanks (respawn timer for players only)
        if (tank->flags & PZ_TANK_FLAG_DEAD) {
            // Only player tanks respawn
            if (tank->flags & PZ_TANK_FLAG_PLAYER) {
                tank->respawn_timer -= dt;
                if (tank->respawn_timer <= 0.0f) {
                    pz_tank_respawn(tank);
                    if (toxic_cloud) {
                        tank->toxic_grace_timer
                            = toxic_cloud->config.grace_period;
                        tank->toxic_damage_timer
                            = toxic_cloud->config.damage_interval;
                    }
                    // Record respawn event
                    if (mgr->respawn_event_count < PZ_MAX_RESPAWN_EVENTS) {
                        pz_tank_respawn_event *event
                            = &mgr->respawn_events[mgr->respawn_event_count++];
                        event->tank_id = tank->id;
                        event->is_player
                            = (tank->flags & PZ_TANK_FLAG_PLAYER) != 0;
                    }
                }
            }
            // Non-player tanks stay dead (but remain active for cleanup)
            continue;
        }

        // Update invulnerability timer
        if (tank->invuln_timer > 0.0f) {
            tank->invuln_timer -= dt;
            if (tank->invuln_timer <= 0.0f) {
                tank->invuln_timer = 0.0f;
                tank->flags &= ~PZ_TANK_FLAG_INVULNERABLE;
            }
        }

        // Update spawn indicator timer
        if (tank->spawn_indicator_timer > 0.0f) {
            tank->spawn_indicator_timer -= dt;
            if (tank->spawn_indicator_timer < 0.0f) {
                tank->spawn_indicator_timer = 0.0f;
            }
        }

        // Update damage flash for all tanks
        if (tank->damage_flash > 0.0f) {
            tank->damage_flash -= dt;
            if (tank->damage_flash < 0.0f)
                tank->damage_flash = 0.0f;
        }

        // Decay visual recoil (spring-like decay)
        if (tank->recoil > 0.001f) {
            tank->recoil *= expf(-8.0f * dt);
        } else {
            tank->recoil = 0.0f;
        }

        // Fire cooldown
        if (tank->fire_cooldown > 0.0f) {
            tank->fire_cooldown -= dt;
            if (tank->fire_cooldown < 0.0f)
                tank->fire_cooldown = 0.0f;
        }

        // TODO: AI input for non-player tanks would go here
    }
}

/* ============================================================================
 * Combat
 * ============================================================================
 */

bool
pz_tank_damage(pz_tank *tank, int amount)
{
    if (!tank)
        return false;

    // Ignore if dead, invulnerable, or invincible
    if ((tank->flags & PZ_TANK_FLAG_DEAD)
        || (tank->flags & PZ_TANK_FLAG_INVULNERABLE)
        || (tank->flags & PZ_TANK_FLAG_INVINCIBLE)) {
        return false;
    }

    tank->health -= amount;
    tank->damage_flash = DAMAGE_FLASH_DURATION;

    pz_log(PZ_LOG_DEBUG, PZ_LOG_CAT_GAME, "Tank %d took %d damage, health=%d",
        tank->id, amount, tank->health);

    if (tank->health <= 0) {
        tank->health = 0;
        tank->flags |= PZ_TANK_FLAG_DEAD;
        tank->respawn_timer = RESPAWN_DELAY;

        pz_log(PZ_LOG_INFO, PZ_LOG_CAT_GAME, "Tank %d destroyed!", tank->id);
        return true;
    }

    return false;
}

bool
pz_tank_apply_damage(pz_tank_manager *mgr, pz_tank *tank, int amount)
{
    if (!mgr || !tank)
        return false;

    bool killed = pz_tank_damage(tank, amount);

    if (killed) {
        // Record death event
        if (mgr->death_event_count < PZ_MAX_DEATH_EVENTS) {
            pz_tank_death_event *event
                = &mgr->death_events[mgr->death_event_count++];
            event->tank_id = tank->id;
            event->pos = tank->pos;
            event->is_player = (tank->flags & PZ_TANK_FLAG_PLAYER) != 0;
            event->floor_level = tank->floor_level;
        }
    }

    return killed;
}

pz_tank *
pz_tank_check_collision(
    pz_tank_manager *mgr, pz_vec2 pos, float radius, int exclude_id)
{
    if (!mgr)
        return NULL;

    pz_circle circle = pz_circle_new(pos, radius);

    for (int i = 0; i < PZ_MAX_TANKS; i++) {
        pz_tank *tank = &mgr->tanks[i];

        // Skip inactive or dead tanks
        if (!(tank->flags & PZ_TANK_FLAG_ACTIVE))
            continue;
        if (tank->flags & PZ_TANK_FLAG_DEAD)
            continue;

        // Skip excluded tank (projectile owner)
        if (tank->id == exclude_id)
            continue;

        // Circle-circle collision
        pz_circle other = pz_circle_new(tank->pos, mgr->collision_radius);
        if (pz_collision_circle_circle(circle, other, NULL, NULL)) {
            return tank;
        }
    }

    return NULL;
}

pz_tank *
pz_tank_check_collision_on_floor(pz_tank_manager *mgr, pz_vec2 pos,
    float radius, int exclude_id, int8_t floor_level)
{
    if (!mgr)
        return NULL;

    pz_circle circle = pz_circle_new(pos, radius);

    for (int i = 0; i < PZ_MAX_TANKS; i++) {
        pz_tank *tank = &mgr->tanks[i];

        // Skip inactive or dead tanks
        if (!(tank->flags & PZ_TANK_FLAG_ACTIVE))
            continue;
        if (tank->flags & PZ_TANK_FLAG_DEAD)
            continue;

        // Skip excluded tank (projectile owner)
        if (tank->id == exclude_id)
            continue;

        // Skip tanks on different floors
        if (tank->floor_level != floor_level)
            continue;

        // Circle-circle collision
        pz_circle other = pz_circle_new(tank->pos, mgr->collision_radius);
        if (pz_collision_circle_circle(circle, other, NULL, NULL)) {
            return tank;
        }
    }

    return NULL;
}

void
pz_tank_respawn(pz_tank *tank)
{
    if (!tank)
        return;

    tank->flags &= ~PZ_TANK_FLAG_DEAD;
    tank->flags |= PZ_TANK_FLAG_INVULNERABLE;

    tank->pos = tank->spawn_pos;
    tank->floor_level = tank->spawn_floor_level;
    tank->vel = (pz_vec2) { 0.0f, 0.0f };
    tank->health = tank->max_health;
    tank->respawn_timer = 0.0f;
    tank->invuln_timer = INVULN_DURATION;
    tank->damage_flash = 0.0f;
    tank->recoil = 0.0f;
    tank->fog_timer = 0.0f;
    tank->idle_time = 0.0f;
    tank->toxic_grace_timer = 0.0f;
    tank->toxic_damage_timer = 0.0f;
    tank->in_toxic_cloud = false;

    // Show spawn indicator for player tanks
    if (tank->flags & PZ_TANK_FLAG_PLAYER) {
        tank->spawn_indicator_timer = 1.5f;
    }

    // Reset jump state on respawn
    tank->jump_state = 0;
    tank->jump_timer = 0.0f;
    tank->jump_duration = 0.0f;
    tank->jump_start_pos = tank->spawn_pos;
    tank->jump_end_pos = tank->spawn_pos;
    tank->jump_start_angle = 0.0f;
    tank->jump_end_angle = 0.0f;
    tank->jump_height = 0.0f;
    tank->jump_apex_height = 1.0f;
    tank->jump_pad_tile_x = -1;
    tank->jump_pad_tile_y = -1;
    tank->jump_cooldown_tile_x = -1;
    tank->jump_cooldown_tile_y = -1;
    tank->jump_cooldown_active = false;

    // Reset loadout to default (lose all collected weapons/powerups)
    pz_tank_reset_loadout(tank);

    pz_log(PZ_LOG_INFO, PZ_LOG_CAT_GAME, "Tank %d respawned at (%.2f, %.2f)",
        tank->id, tank->spawn_pos.x, tank->spawn_pos.y);
}

/* ============================================================================
 * Weapon Loadout
 * ============================================================================
 */

bool
pz_tank_add_weapon(pz_tank *tank, int weapon_type)
{
    if (!tank)
        return false;

    // Check if already in loadout
    for (int i = 0; i < tank->loadout_count; i++) {
        if (tank->loadout[i] == weapon_type) {
            // Already have this weapon, switch to it
            tank->loadout_index = i;
            update_turret_color(tank);
            return false;
        }
    }

    // Add to loadout if there's room
    if (tank->loadout_count >= PZ_MAX_LOADOUT_WEAPONS) {
        pz_log(PZ_LOG_WARN, PZ_LOG_CAT_GAME, "Loadout full, cannot add weapon");
        return false;
    }

    tank->loadout[tank->loadout_count] = weapon_type;
    tank->loadout_index = tank->loadout_count; // Switch to new weapon
    tank->loadout_count++;

    update_turret_color(tank);

    pz_log(PZ_LOG_INFO, PZ_LOG_CAT_GAME,
        "Added weapon to loadout, now have %d weapons", tank->loadout_count);

    return true;
}

void
pz_tank_cycle_weapon(pz_tank *tank, int scroll_delta)
{
    if (!tank || tank->loadout_count <= 1)
        return;

    // Cycle through loadout
    tank->loadout_index += scroll_delta;

    // Wrap around
    if (tank->loadout_index < 0) {
        tank->loadout_index = tank->loadout_count - 1;
    } else if (tank->loadout_index >= tank->loadout_count) {
        tank->loadout_index = 0;
    }

    update_turret_color(tank);

    int weapon = tank->loadout[tank->loadout_index];
    pz_log(PZ_LOG_DEBUG, PZ_LOG_CAT_GAME, "Switched to weapon %d (%s)",
        tank->loadout_index, pz_powerup_type_name((pz_powerup_type)weapon));
}

int
pz_tank_get_current_weapon(const pz_tank *tank)
{
    if (!tank || tank->loadout_count == 0)
        return PZ_POWERUP_NONE;

    return tank->loadout[tank->loadout_index];
}

void
pz_tank_reset_loadout(pz_tank *tank)
{
    if (!tank)
        return;

    // Reset to default weapon only
    tank->loadout[0] = PZ_POWERUP_NONE;
    tank->loadout_count = 1;
    tank->loadout_index = 0;

    // Reset barrier placer state
    tank->barrier_placer.barrier_tile[0] = '\0';
    tank->barrier_placer.barrier_health = 0.0f;
    tank->barrier_placer.max_barriers = 0;
    tank->barrier_placer.placed_count = 0;
    for (int i = 0; i < PZ_MAX_PLACED_BARRIERS; i++) {
        tank->barrier_placer.placed_barrier_ids[i] = -1;
    }

    update_turret_color(tank);

    pz_log(PZ_LOG_DEBUG, PZ_LOG_CAT_GAME, "Tank %d loadout reset to default",
        tank->id);
}

/* ============================================================================
 * Barrier Placer
 * ============================================================================
 */

void
pz_tank_set_barrier_placer(pz_tank *tank, const char *tile, float health,
    int max_count, float lifetime)
{
    if (!tank || !tile)
        return;

    strncpy(tank->barrier_placer.barrier_tile, tile,
        sizeof(tank->barrier_placer.barrier_tile) - 1);
    tank->barrier_placer
        .barrier_tile[sizeof(tank->barrier_placer.barrier_tile) - 1]
        = '\0';
    tank->barrier_placer.barrier_health = health;
    tank->barrier_placer.max_barriers = max_count;
    tank->barrier_placer.barrier_lifetime = lifetime;
    // Don't reset placed_count or placed_barrier_ids - barriers persist

    pz_log(PZ_LOG_DEBUG, PZ_LOG_CAT_GAME,
        "Tank %d barrier placer set: tile=%s, health=%.0f, max=%d, "
        "lifetime=%.1fs",
        tank->id, tile, health, max_count, lifetime);
}

bool
pz_tank_add_placed_barrier(pz_tank *tank, int barrier_id)
{
    if (!tank)
        return false;

    if (tank->barrier_placer.placed_count >= PZ_MAX_PLACED_BARRIERS) {
        pz_log(PZ_LOG_WARN, PZ_LOG_CAT_GAME,
            "Tank %d cannot track more placed barriers (max=%d)", tank->id,
            PZ_MAX_PLACED_BARRIERS);
        return false;
    }

    tank->barrier_placer.placed_barrier_ids[tank->barrier_placer.placed_count++]
        = barrier_id;
    pz_log(PZ_LOG_DEBUG, PZ_LOG_CAT_GAME, "Tank %d placed barrier %d (%d/%d)",
        tank->id, barrier_id, tank->barrier_placer.placed_count,
        tank->barrier_placer.max_barriers);
    return true;
}

void
pz_tank_on_barrier_destroyed(pz_tank *tank, int barrier_id)
{
    if (!tank)
        return;

    // Find and remove the barrier ID from tracking
    for (int i = 0; i < tank->barrier_placer.placed_count; i++) {
        if (tank->barrier_placer.placed_barrier_ids[i] == barrier_id) {
            // Shift remaining IDs down
            for (int j = i; j < tank->barrier_placer.placed_count - 1; j++) {
                tank->barrier_placer.placed_barrier_ids[j]
                    = tank->barrier_placer.placed_barrier_ids[j + 1];
            }
            tank->barrier_placer.placed_count--;
            pz_log(PZ_LOG_DEBUG, PZ_LOG_CAT_GAME,
                "Tank %d barrier %d destroyed (%d/%d remaining)", tank->id,
                barrier_id, tank->barrier_placer.placed_count,
                tank->barrier_placer.max_barriers);
            return;
        }
    }
}

bool
pz_tank_can_place_barrier(const pz_tank *tank)
{
    if (!tank)
        return false;

    // Check if holding barrier_placer
    int current_weapon = pz_tank_get_current_weapon(tank);
    if (current_weapon != PZ_POWERUP_BARRIER_PLACER)
        return false;

    // Check if under the limit
    return tank->barrier_placer.placed_count
        < tank->barrier_placer.max_barriers;
}

const pz_tank_barrier_placer *
pz_tank_get_barrier_placer(const pz_tank *tank)
{
    if (!tank)
        return NULL;

    // Only return if holding barrier_placer
    int current_weapon = pz_tank_get_current_weapon(tank);
    if (current_weapon != PZ_POWERUP_BARRIER_PLACER)
        return NULL;

    return &tank->barrier_placer;
}

/* ============================================================================
 * Rendering
 * ============================================================================
 */

void
pz_tank_render(pz_tank_manager *mgr, pz_renderer *renderer,
    const pz_mat4 *view_projection, const pz_tank_render_params *params)
{
    if (!mgr || !renderer || !view_projection)
        return;

    if (!mgr->render_ready)
        return;

    // Light parameters for directional shading
    pz_vec3 light_dir = { 0.5f, 1.0f, 0.3f };
    pz_vec3 light_color = { 0.6f, 0.55f, 0.5f };
    pz_vec3 ambient = { 0.15f, 0.18f, 0.2f };
    pz_vec4 shadow_color = { 0.0f, 0.0f, 0.0f, SHADOW_ALPHA };

    // Set shared uniforms
    pz_renderer_set_uniform_vec3(
        renderer, mgr->shader, "u_light_dir", light_dir);
    pz_renderer_set_uniform_vec3(
        renderer, mgr->shader, "u_light_color", light_color);
    pz_renderer_set_uniform_vec3(renderer, mgr->shader, "u_ambient", ambient);
    pz_renderer_set_uniform_vec2(
        renderer, mgr->shader, "u_shadow_params", (pz_vec2) { 0.0f, 0.0f });

    // Set light map uniforms
    if (params && params->light_texture != PZ_INVALID_HANDLE
        && params->light_texture != 0) {
        pz_renderer_bind_texture(renderer, 0, params->light_texture);
        pz_renderer_set_uniform_int(
            renderer, mgr->shader, "u_light_texture", 0);
        pz_renderer_set_uniform_int(renderer, mgr->shader, "u_use_lighting", 1);
        pz_renderer_set_uniform_vec2(renderer, mgr->shader, "u_light_scale",
            (pz_vec2) { params->light_scale_x, params->light_scale_z });
        pz_renderer_set_uniform_vec2(renderer, mgr->shader, "u_light_offset",
            (pz_vec2) { params->light_offset_x, params->light_offset_z });
    } else {
        pz_renderer_set_uniform_int(renderer, mgr->shader, "u_use_lighting", 0);
    }

    for (int i = 0; i < PZ_MAX_TANKS; i++) {
        pz_tank *tank = &mgr->tanks[i];

        // Skip inactive or dead tanks
        if (!(tank->flags & PZ_TANK_FLAG_ACTIVE))
            continue;
        if (tank->flags & PZ_TANK_FLAG_DEAD)
            continue;

        // Calculate colors (with damage flash)
        pz_vec4 body_color = tank->body_color;
        pz_vec4 turret_color = tank->turret_color;

        if (tank->damage_flash > 0.0f) {
            // Flash to white when damaged
            float flash_t = tank->damage_flash / DAMAGE_FLASH_DURATION;
            body_color.x = pz_lerpf(body_color.x, 1.0f, flash_t);
            body_color.y = pz_lerpf(body_color.y, 1.0f, flash_t);
            body_color.z = pz_lerpf(body_color.z, 1.0f, flash_t);
            turret_color.x = pz_lerpf(turret_color.x, 1.0f, flash_t);
            turret_color.y = pz_lerpf(turret_color.y, 1.0f, flash_t);
            turret_color.z = pz_lerpf(turret_color.z, 1.0f, flash_t);
        }

        if (tank->in_toxic_cloud && params && params->has_toxic) {
            float tint = 0.35f;
            body_color.x = pz_lerpf(body_color.x, params->toxic_color.x, tint);
            body_color.y = pz_lerpf(body_color.y, params->toxic_color.y, tint);
            body_color.z = pz_lerpf(body_color.z, params->toxic_color.z, tint);
            turret_color.x
                = pz_lerpf(turret_color.x, params->toxic_color.x, tint);
            turret_color.y
                = pz_lerpf(turret_color.y, params->toxic_color.y, tint);
            turret_color.z
                = pz_lerpf(turret_color.z, params->toxic_color.z, tint);
            body_color.w *= 0.6f;
            turret_color.w *= 0.6f;
        }

        float jump_height = tank->jump_height;

        if (mgr->shadow_pipeline != PZ_INVALID_HANDLE && mgr->shadow_mesh
            && mgr->shadow_mesh->uploaded) {
            pz_renderer_set_uniform_vec2(renderer, mgr->shader,
                "u_shadow_params", (pz_vec2) { SHADOW_SOFTNESS, 1.0f });
            pz_mat4 shadow_model = pz_mat4_identity();
            shadow_model = pz_mat4_mul(shadow_model,
                pz_mat4_translate(
                    (pz_vec3) { tank->pos.x, SHADOW_Y_OFFSET, tank->pos.y }));
            shadow_model
                = pz_mat4_mul(shadow_model, pz_mat4_rotate_y(tank->body_angle));

            pz_mat4 shadow_mvp = pz_mat4_mul(*view_projection, shadow_model);

            pz_renderer_set_uniform_mat4(
                renderer, mgr->shader, "u_mvp", &shadow_mvp);
            pz_renderer_set_uniform_mat4(
                renderer, mgr->shader, "u_model", &shadow_model);
            pz_renderer_set_uniform_vec4(
                renderer, mgr->shader, "u_color", shadow_color);

            pz_draw_cmd shadow_cmd = {
                .pipeline = mgr->shadow_pipeline,
                .vertex_buffer = mgr->shadow_mesh->buffer,
                .index_buffer = PZ_INVALID_HANDLE,
                .vertex_count = mgr->shadow_mesh->vertex_count,
                .index_count = 0,
                .vertex_offset = 0,
                .index_offset = 0,
            };
            pz_renderer_draw(renderer, &shadow_cmd);
        }

        pz_renderer_set_uniform_vec2(
            renderer, mgr->shader, "u_shadow_params", (pz_vec2) { 0.0f, 0.0f });

        // Calculate visual recoil offset (pushes backward from turret
        // direction)
        float recoil_scale = 0.25f; // How far the tank slides back visually
        float recoil_x
            = -sinf(tank->turret_angle) * tank->recoil * recoil_scale;
        float recoil_z
            = -cosf(tank->turret_angle) * tank->recoil * recoil_scale;

        // Draw body (with recoil offset)
        float body_recoil = 0.4f; // Body moves less than turret
        pz_mat4 body_model = pz_mat4_identity();
        body_model = pz_mat4_mul(body_model,
            pz_mat4_translate((pz_vec3) { tank->pos.x + recoil_x * body_recoil,
                jump_height, tank->pos.y + recoil_z * body_recoil }));
        body_model
            = pz_mat4_mul(body_model, pz_mat4_rotate_y(tank->body_angle));

        pz_mat4 body_mvp = pz_mat4_mul(*view_projection, body_model);

        pz_renderer_set_uniform_mat4(renderer, mgr->shader, "u_mvp", &body_mvp);
        pz_renderer_set_uniform_mat4(
            renderer, mgr->shader, "u_model", &body_model);
        pz_renderer_set_uniform_vec4(
            renderer, mgr->shader, "u_color", body_color);

        pz_draw_cmd body_cmd = {
            .pipeline = mgr->pipeline,
            .vertex_buffer = mgr->body_mesh->buffer,
            .index_buffer = PZ_INVALID_HANDLE,
            .vertex_count = mgr->body_mesh->vertex_count,
            .index_count = 0,
            .vertex_offset = 0,
            .index_offset = 0,
        };
        pz_renderer_draw(renderer, &body_cmd);

        // Draw turret (with full recoil offset)
        pz_mat4 turret_model = pz_mat4_identity();
        turret_model = pz_mat4_mul(turret_model,
            pz_mat4_translate((pz_vec3) { tank->pos.x + recoil_x,
                TURRET_Y_OFFSET + jump_height, tank->pos.y + recoil_z }));
        turret_model
            = pz_mat4_mul(turret_model, pz_mat4_rotate_y(tank->turret_angle));

        pz_mat4 turret_mvp = pz_mat4_mul(*view_projection, turret_model);

        pz_renderer_set_uniform_mat4(
            renderer, mgr->shader, "u_mvp", &turret_mvp);
        pz_renderer_set_uniform_mat4(
            renderer, mgr->shader, "u_model", &turret_model);
        pz_renderer_set_uniform_vec4(
            renderer, mgr->shader, "u_color", turret_color);

        pz_draw_cmd turret_cmd = {
            .pipeline = mgr->pipeline,
            .vertex_buffer = mgr->turret_mesh->buffer,
            .index_buffer = PZ_INVALID_HANDLE,
            .vertex_count = mgr->turret_mesh->vertex_count,
            .index_count = 0,
            .vertex_offset = 0,
            .index_offset = 0,
        };
        pz_renderer_draw(renderer, &turret_cmd);
    }
}

/* ============================================================================
 * Utility
 * ============================================================================
 */

pz_vec2
pz_tank_get_barrel_tip(const pz_tank *tank)
{
    if (!tank)
        return (pz_vec2) { 0.0f, 0.0f };

    float dx = sinf(tank->turret_angle) * BARREL_LENGTH;
    float dz = cosf(tank->turret_angle) * BARREL_LENGTH;

    return (pz_vec2) {
        tank->pos.x + dx,
        tank->pos.y + dz,
    };
}

pz_vec2
pz_tank_get_fire_direction(const pz_tank *tank)
{
    if (!tank)
        return (pz_vec2) { 0.0f, 1.0f };

    return (pz_vec2) {
        sinf(tank->turret_angle),
        cosf(tank->turret_angle),
    };
}

bool
pz_tank_get_fire_solution(const pz_tank *tank, const pz_map *map,
    pz_vec2 *out_pos, pz_vec2 *out_dir, int *out_bounce_cost)
{
    if (!tank) {
        return false;
    }

    pz_vec2 fire_dir = pz_tank_get_fire_direction(tank);
    pz_vec2 tip = pz_tank_get_barrel_tip(tank);

    if (out_pos) {
        *out_pos = tip;
    }
    if (out_dir) {
        *out_dir = fire_dir;
    }
    if (out_bounce_cost) {
        *out_bounce_cost = 0;
    }

    if (!map) {
        return true;
    }

    float barrel_len = pz_vec2_dist(tank->pos, tip);
    if (barrel_len < 0.0001f) {
        return true;
    }

    pz_raycast_result hit = pz_map_raycast_ex(map, tank->pos, tip);
    if (hit.hit && hit.distance < (barrel_len - BARREL_CLEAR_EPSILON)) {
        if (out_pos) {
            *out_pos = pz_vec2_add(
                hit.point, pz_vec2_scale(hit.normal, BARREL_DEFLECT_EPSILON));
        }
        if (out_dir) {
            *out_dir = pz_vec2_reflect(fire_dir, hit.normal);
        }
        if (out_bounce_cost) {
            *out_bounce_cost = 1;
        }
    }

    return true;
}

bool
pz_tank_barrel_is_clear(const pz_tank *tank, const pz_map *map)
{
    if (!tank || !map) {
        return true;
    }

    pz_vec2 tip = pz_tank_get_barrel_tip(tank);
    float barrel_len = pz_vec2_dist(tank->pos, tip);
    if (barrel_len < 0.0001f) {
        return true;
    }

    pz_raycast_result hit = pz_map_raycast_ex(map, tank->pos, tip);
    if (hit.hit && hit.distance < (barrel_len - BARREL_CLEAR_EPSILON)) {
        return false;
    }

    return true;
}

void
pz_tank_update_floor_level(pz_tank *tank, const pz_map *map)
{
    if (!tank) {
        return;
    }

    if (!map) {
        tank->floor_level = 0;
        return;
    }

    int tx, ty;
    pz_map_world_to_tile(map, tank->pos, &tx, &ty);

    if (!pz_map_in_bounds(map, tx, ty)) {
        // Out of bounds - keep current floor level
        return;
    }

    tank->floor_level = pz_map_get_height(map, tx, ty);
}

int
pz_tank_count_active(const pz_tank_manager *mgr)
{
    if (!mgr)
        return 0;

    int count = 0;
    for (int i = 0; i < PZ_MAX_TANKS; i++) {
        if ((mgr->tanks[i].flags & PZ_TANK_FLAG_ACTIVE)
            && !(mgr->tanks[i].flags & PZ_TANK_FLAG_DEAD)) {
            count++;
        }
    }
    return count;
}

int
pz_tank_count_enemies_alive(const pz_tank_manager *mgr)
{
    if (!mgr)
        return 0;

    int count = 0;
    for (int i = 0; i < PZ_MAX_TANKS; i++) {
        const pz_tank *tank = &mgr->tanks[i];
        if ((tank->flags & PZ_TANK_FLAG_ACTIVE)
            && !(tank->flags & PZ_TANK_FLAG_DEAD)
            && !(tank->flags & PZ_TANK_FLAG_PLAYER)) {
            count++;
        }
    }
    return count;
}

int
pz_tank_get_death_events(
    const pz_tank_manager *mgr, pz_tank_death_event *events, int max_events)
{
    if (!mgr || !events || max_events <= 0)
        return 0;

    int count = mgr->death_event_count;
    if (count > max_events)
        count = max_events;

    for (int i = 0; i < count; i++) {
        events[i] = mgr->death_events[i];
    }

    return count;
}

void
pz_tank_clear_death_events(pz_tank_manager *mgr)
{
    if (mgr) {
        mgr->death_event_count = 0;
    }
}

int
pz_tank_get_respawn_events(
    const pz_tank_manager *mgr, pz_tank_respawn_event *events, int max_events)
{
    if (!mgr || !events || max_events <= 0)
        return 0;

    int count = mgr->respawn_event_count;
    if (count > max_events)
        count = max_events;

    for (int i = 0; i < count; i++) {
        events[i] = mgr->respawn_events[i];
    }

    return count;
}

void
pz_tank_clear_respawn_events(pz_tank_manager *mgr)
{
    if (mgr) {
        mgr->respawn_event_count = 0;
    }
}
