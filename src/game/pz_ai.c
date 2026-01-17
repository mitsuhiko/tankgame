/*
 * Tank Game - Enemy AI System Implementation
 */

#include "pz_ai.h"

#include <math.h>
#include <string.h>

#include "../core/pz_log.h"
#include "../core/pz_mem.h"
#include "../core/pz_sim.h"
#include "pz_map.h"
#include "pz_mine.h"
#include "pz_powerup.h"
#include "pz_projectile.h"
#include "pz_toxic_cloud.h"

/* ============================================================================
 * Enemy Stats
 * ============================================================================
 */

static const pz_enemy_stats ENEMY_STATS[] = {
    // Index 0 (unused, placeholder)
    { .health = 10,
        .max_bounces = 1,
        .fire_cooldown_scale = 1.0f,
        .aim_speed = 1.0f,
        .aim_tolerance = 0.26f,
        .move_speed = 3.0f,
        .body_color = { 0.5f, 0.5f, 0.5f, 1.0f },
        .weapon_type = PZ_POWERUP_NONE,
        .projectile_speed_scale = 1.0f,
        .bounce_shot_range = 30.0f,
        .projectile_defense_chance = 0.0f,
        .bounce_shot_samples = 36,
        .max_shots_per_peek = 1,
        .max_projectiles_direct = 1,
        .max_projectiles_bounce = 1,
        .behavior_flags = 0 },

    // Sentry (stationary turret, fires often, uses bounce shots)
    { .health = 10,
        .max_bounces = 1,
        .fire_cooldown_scale = 1.0f,
        .aim_speed = 1.2f,
        .aim_tolerance = 0.26f,
        .move_speed = 3.0f,
        .body_color = { 0.6f, 0.25f, 0.25f, 1.0f }, // Dark red
        .weapon_type = PZ_POWERUP_NONE,
        .projectile_speed_scale = 1.0f,
        .bounce_shot_range = 30.0f,
        .projectile_defense_chance = 0.6f,
        .bounce_shot_samples = 36,
        .max_shots_per_peek = 1,
        .max_projectiles_direct = 3,
        .max_projectiles_bounce = 1,
        .behavior_flags = PZ_AI_BEHAVIOR_BOUNCE_SHOTS
            | PZ_AI_BEHAVIOR_DEFEND_PROJECTILES | PZ_AI_BEHAVIOR_TARGET_MINES
            | PZ_AI_BEHAVIOR_TOXIC_ESCAPE },

    // Skirmisher (uses cover)
    { .health = 15,
        .max_bounces = 1,
        .fire_cooldown_scale = 1.0f,
        .aim_speed = 1.3f,
        .aim_tolerance = 0.26f,
        .move_speed = 3.0f,
        .body_color = { 0.7f, 0.4f, 0.1f, 1.0f }, // Orange-brown
        .weapon_type = PZ_POWERUP_NONE,
        .projectile_speed_scale = 1.0f,
        .bounce_shot_range = 30.0f,
        .projectile_defense_chance = 0.0f,
        .bounce_shot_samples = 36,
        .max_shots_per_peek = 2,
        .max_projectiles_direct = 2,
        .max_projectiles_bounce = 1,
        .behavior_flags = PZ_AI_BEHAVIOR_MOVE | PZ_AI_BEHAVIOR_PATHFIND
            | PZ_AI_BEHAVIOR_USE_COVER | PZ_AI_BEHAVIOR_BOUNCE_SHOTS
            | PZ_AI_BEHAVIOR_TARGET_MINES | PZ_AI_BEHAVIOR_TOXIC_ESCAPE },

    // Hunter (aggressive, machine gun burst)
    { .health = 20,
        .max_bounces = 0,
        .fire_cooldown_scale = 0.8f,
        .aim_speed = 2.0f,
        .aim_tolerance = 0.52f,
        .move_speed = 6.0f,
        .body_color = { 0.2f, 0.5f, 0.2f, 1.0f }, // Dark green (hunter)
        .weapon_type = PZ_POWERUP_MACHINE_GUN,
        .projectile_speed_scale = 1.0f,
        .bounce_shot_range = 0.0f,
        .projectile_defense_chance = 0.0f,
        .bounce_shot_samples = 0,
        .max_shots_per_peek = 3,
        .max_projectiles_direct = 0,
        .max_projectiles_bounce = 0,
        .behavior_flags = PZ_AI_BEHAVIOR_MOVE | PZ_AI_BEHAVIOR_PATHFIND
            | PZ_AI_BEHAVIOR_USE_COVER | PZ_AI_BEHAVIOR_CHASE
            | PZ_AI_BEHAVIOR_FLANK | PZ_AI_BEHAVIOR_EVADE
            | PZ_AI_BEHAVIOR_STRAFE | PZ_AI_BEHAVIOR_TARGET_MINES
            | PZ_AI_BEHAVIOR_TOXIC_ESCAPE },

    // Sniper (stationary, long-range ricochet)
    { .health = 12,
        .max_bounces = 3,
        .fire_cooldown_scale = 1.0f,
        .aim_speed = 0.9f,
        .aim_tolerance = 0.14f,
        .move_speed = 3.0f,
        .body_color = { 0.55f, 0.25f, 0.7f, 1.0f }, // Purple
        .weapon_type = PZ_POWERUP_RICOCHET,
        .projectile_speed_scale = 1.4f,
        .bounce_shot_range = 60.0f,
        .projectile_defense_chance = 0.9f,
        .bounce_shot_samples = 90,
        .max_shots_per_peek = 1,
        .max_projectiles_direct = 1,
        .max_projectiles_bounce = 1,
        .behavior_flags = PZ_AI_BEHAVIOR_BOUNCE_SHOTS
            | PZ_AI_BEHAVIOR_DEFEND_PROJECTILES | PZ_AI_BEHAVIOR_TARGET_MINES
            | PZ_AI_BEHAVIOR_TOXIC_ESCAPE
            | PZ_AI_BEHAVIOR_REQUIRE_BOUNCE_SHOT },
};

const pz_enemy_stats *
pz_enemy_get_stats(pz_enemy_type type)
{
    if (type < 1 || type > PZ_ENEMY_TYPE_SNIPER) {
        return &ENEMY_STATS[1]; // Default to sentry
    }
    return &ENEMY_STATS[type];
}

const char *
pz_enemy_type_name(pz_enemy_type type)
{
    switch (type) {
    case PZ_ENEMY_TYPE_SENTRY:
        return "sentry";
    case PZ_ENEMY_TYPE_SKIRMISHER:
        return "skirmisher";
    case PZ_ENEMY_TYPE_HUNTER:
        return "hunter";
    case PZ_ENEMY_TYPE_SNIPER:
        return "sniper";
    default:
        return "sentry";
    }
}

/* ============================================================================
 * AI Manager
 * ============================================================================
 */

static bool ai_has_behavior(const pz_ai_controller *ctrl, uint32_t flag);

pz_ai_manager *
pz_ai_manager_create(pz_tank_manager *tank_mgr, const pz_map *map)
{
    pz_ai_manager *ai = pz_calloc(1, sizeof(pz_ai_manager));
    if (!ai) {
        return NULL;
    }

    ai->tank_mgr = tank_mgr;
    ai->map = map;
    ai->controller_count = 0;

    pz_log(PZ_LOG_INFO, PZ_LOG_CAT_GAME, "AI manager created");
    return ai;
}

void
pz_ai_manager_destroy(pz_ai_manager *ai_mgr)
{
    if (!ai_mgr) {
        return;
    }

    pz_log(PZ_LOG_INFO, PZ_LOG_CAT_GAME, "AI manager destroyed");
    pz_free(ai_mgr);
}

void
pz_ai_manager_set_map(pz_ai_manager *ai_mgr, const pz_map *map)
{
    if (ai_mgr) {
        ai_mgr->map = map;
    }
}

/* ============================================================================
 * Enemy Spawning
 * ============================================================================
 */

pz_tank *
pz_ai_spawn_enemy(
    pz_ai_manager *ai_mgr, pz_vec2 pos, float angle, pz_enemy_type type)
{
    if (!ai_mgr || !ai_mgr->tank_mgr) {
        return NULL;
    }

    // Check if we have room for another AI controller
    if (ai_mgr->controller_count >= PZ_MAX_AI_CONTROLLERS) {
        pz_log(PZ_LOG_WARN, PZ_LOG_CAT_GAME,
            "Cannot spawn enemy: max AI controllers reached (%d)",
            PZ_MAX_AI_CONTROLLERS);
        return NULL;
    }

    // Get enemy stats
    const pz_enemy_stats *stats = pz_enemy_get_stats(type);

    // Spawn the tank (not a player)
    pz_tank *tank
        = pz_tank_spawn(ai_mgr->tank_mgr, pos, stats->body_color, false);
    if (!tank) {
        pz_log(PZ_LOG_WARN, PZ_LOG_CAT_GAME,
            "Cannot spawn enemy: no tank slots available");
        return NULL;
    }

    // Update floor level based on spawn position
    pz_tank_update_floor_level(tank, ai_mgr->map);
    tank->spawn_floor_level = tank->floor_level;

    // Set tank properties based on enemy level
    tank->health = stats->health;
    tank->max_health = stats->health;
    tank->body_angle = angle;
    tank->turret_angle = angle;
    if (stats->weapon_type != PZ_POWERUP_NONE) {
        pz_tank_add_weapon(tank, (int)stats->weapon_type);
    }

    // Create AI controller
    pz_ai_controller *ctrl = &ai_mgr->controllers[ai_mgr->controller_count++];
    memset(ctrl, 0, sizeof(pz_ai_controller));

    ctrl->tank_id = tank->id;
    ctrl->type = type;
    ctrl->behavior_flags = stats->behavior_flags;
    ctrl->current_aim_angle = angle;
    ctrl->target_aim_angle = angle;
    // Match player behavior: allow immediate fire, then use weapon cooldown.
    ctrl->fire_timer = 0.0f;
    ctrl->can_see_player = false;
    ctrl->reaction_delay = 0.0f;
    ctrl->last_seen_time = 0.0f;
    ctrl->defending_projectile = false;
    ctrl->defense_aim_angle = angle;
    ctrl->defense_check_timer = 0.0f;
    ctrl->targeting_mine = false;
    ctrl->mine_target_pos = pos;
    ctrl->mine_target_dist = 0.0f;

    // Initialize cover behavior for level 2+
    ctrl->state = PZ_AI_STATE_IDLE;
    ctrl->cover_pos = pos;
    ctrl->peek_pos = pos;
    ctrl->move_target = pos;
    ctrl->state_timer = 0.0f;
    ctrl->cover_search_timer = 0.0f;
    ctrl->has_cover = false;
    ctrl->shots_fired = 0;
    ctrl->max_shots_per_peek = stats->max_shots_per_peek;

    // Initialize pathfinding
    pz_path_clear(&ctrl->path);
    ctrl->path_goal = pos;
    ctrl->path_update_timer = 0.0f;

    // Initialize toxic escape state
    ctrl->toxic_escaping = false;
    ctrl->toxic_escape_target = pos;
    pz_path_clear(&ctrl->toxic_escape_path);
    ctrl->toxic_check_timer = 0.0f;
    ctrl->toxic_urgency = 0.0f;
    ctrl->detour_active = false;
    ctrl->detour_target = pos;
    ctrl->detour_timer = 0.0f;
    ctrl->detour_blocked_timer = 0.0f;
    ctrl->detour_last_pos = pos;
    ctrl->detour_has_last_pos = true;

    pz_log(PZ_LOG_INFO, PZ_LOG_CAT_GAME,
        "Spawned %s enemy at (%.1f, %.1f), tank_id=%d%s",
        pz_enemy_type_name(type), pos.x, pos.y, tank->id,
        ai_has_behavior(ctrl, PZ_AI_BEHAVIOR_PATHFIND) ? " (with pathfinding)"
                                                       : "");

    return tank;
}

/* ============================================================================
 * AI Helpers
 * ============================================================================
 */

static bool
ai_has_behavior(const pz_ai_controller *ctrl, uint32_t flag)
{
    return ctrl && (ctrl->behavior_flags & flag);
}

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

// Check line-of-sight from AI to player using map raycast
static bool
check_line_of_sight(const pz_map *map, pz_vec2 from, pz_vec2 to)
{
    if (!map) {
        return true; // No map = assume can see
    }

    pz_vec2 dir = pz_vec2_sub(to, from);
    float dist = pz_vec2_len(dir);

    if (dist < 0.1f) {
        return true; // Very close = can see
    }

    dir = pz_vec2_scale(dir, 1.0f / dist); // Normalize

    // Raycast from AI to player
    bool hit_wall = false;
    pz_vec2 hit_pos = pz_map_raycast(map, from, dir, dist, &hit_wall);
    (void)hit_pos;

    // If we hit a wall before reaching player distance, no LOS
    return !hit_wall;
}

__attribute__((unused)) static bool
ai_in_toxic_cloud(const pz_toxic_cloud *toxic_cloud, pz_vec2 pos)
{
    return toxic_cloud && pz_toxic_cloud_is_inside(toxic_cloud, pos);
}

// Separation steering: compute a direction to move away from nearby tanks
// This helps prevent tanks from crowding together, especially when escaping
// toxic clouds. Returns a normalized direction vector (or zero if no tanks
// nearby).
static pz_vec2
ai_compute_separation(const pz_tank_manager *tank_mgr, pz_vec2 pos, int self_id,
    float separation_radius, float *out_urgency)
{
    if (!tank_mgr) {
        if (out_urgency) {
            *out_urgency = 0.0f;
        }
        return pz_vec2_zero();
    }

    pz_vec2 separation = pz_vec2_zero();
    float total_weight = 0.0f;
    float max_urgency = 0.0f;

    for (int i = 0; i < PZ_MAX_TANKS; i++) {
        const pz_tank *other = &tank_mgr->tanks[i];

        if (!(other->flags & PZ_TANK_FLAG_ACTIVE)) {
            continue;
        }
        if (other->flags & PZ_TANK_FLAG_DEAD) {
            continue;
        }
        if (other->id == self_id) {
            continue;
        }

        pz_vec2 to_self = pz_vec2_sub(pos, other->pos);
        float dist = pz_vec2_len(to_self);

        if (dist < 0.001f || dist > separation_radius) {
            continue;
        }

        // Weight inversely proportional to distance (closer = stronger push)
        float weight = (separation_radius - dist) / separation_radius;
        weight = weight * weight; // Quadratic falloff

        pz_vec2 away_dir = pz_vec2_scale(to_self, 1.0f / dist);
        separation = pz_vec2_add(separation, pz_vec2_scale(away_dir, weight));
        total_weight += weight;

        // Track urgency (how close is the closest tank)
        float tank_urgency = 1.0f - (dist / separation_radius);
        if (tank_urgency > max_urgency) {
            max_urgency = tank_urgency;
        }
    }

    if (out_urgency) {
        *out_urgency = max_urgency;
    }

    if (total_weight < 0.001f) {
        return pz_vec2_zero();
    }

    // Normalize the separation vector
    float len = pz_vec2_len(separation);
    if (len < 0.001f) {
        return pz_vec2_zero();
    }

    return pz_vec2_scale(separation, 1.0f / len);
}

// Blend separation steering into a movement direction
// Returns the blended direction (normalized)
static pz_vec2
ai_apply_separation(pz_vec2 move_dir, pz_vec2 separation,
    float separation_urgency, float blend_strength)
{
    if (pz_vec2_len_sq(separation) < 0.0001f) {
        return move_dir;
    }

    // Scale blend by urgency - more separation when tanks are very close
    float blend = blend_strength * separation_urgency;

    pz_vec2 result;
    if (pz_vec2_len_sq(move_dir) < 0.0001f) {
        // No primary movement - just use separation
        result = separation;
    } else {
        // Blend primary direction with separation
        result = pz_vec2_add(move_dir, pz_vec2_scale(separation, blend));
    }

    float len = pz_vec2_len(result);
    if (len < 0.001f) {
        return move_dir;
    }

    return pz_vec2_scale(result, 1.0f / len);
}

// Tank collision radius for pathfinding
#define AI_TANK_RADIUS 0.9f
// How close to waypoint before advancing
#define AI_PATH_ARRIVE_THRESHOLD 0.8f

// Forward declaration for helper used by stuck handling.
static bool is_position_valid(const pz_map *map, pz_vec2 pos, float radius);

// Detect when the AI is blocked and briefly back off to make space.
static bool
ai_hits_tank(pz_tank_manager *mgr, pz_vec2 center, float radius, int exclude_id)
{
    if (!mgr) {
        return false;
    }

    float radius_sum = radius + mgr->collision_radius;
    float radius_sq = radius_sum * radius_sum;

    for (int i = 0; i < PZ_MAX_TANKS; i++) {
        pz_tank *tank = &mgr->tanks[i];
        if (!(tank->flags & PZ_TANK_FLAG_ACTIVE)) {
            continue;
        }
        if (tank->flags & PZ_TANK_FLAG_DEAD) {
            continue;
        }
        if (tank->id == exclude_id) {
            continue;
        }

        pz_vec2 delta = pz_vec2_sub(center, tank->pos);
        if (pz_vec2_len_sq(delta) < radius_sq) {
            return true;
        }
    }

    return false;
}

static bool
ai_is_tank_blocking(pz_tank_manager *mgr, pz_vec2 pos, pz_vec2 move_dir,
    float radius, int exclude_id)
{
    if (!mgr) {
        return false;
    }

    if (pz_vec2_len_sq(move_dir) < 0.0001f) {
        return false;
    }

    pz_vec2 dir = pz_vec2_normalize(move_dir);
    float forward_limit = radius * 2.2f;
    float lateral_limit = radius * 1.2f;
    float combined_radius = radius + mgr->collision_radius;
    float combined_radius_sq = combined_radius * combined_radius;

    for (int i = 0; i < PZ_MAX_TANKS; i++) {
        pz_tank *tank = &mgr->tanks[i];
        if (!(tank->flags & PZ_TANK_FLAG_ACTIVE)) {
            continue;
        }
        if (tank->flags & PZ_TANK_FLAG_DEAD) {
            continue;
        }
        if (tank->id == exclude_id) {
            continue;
        }

        pz_vec2 delta = pz_vec2_sub(tank->pos, pos);
        float dist_sq = pz_vec2_len_sq(delta);
        if (dist_sq > combined_radius_sq * 4.0f) {
            continue;
        }

        float forward = pz_vec2_dot(delta, dir);
        if (forward <= 0.0f || forward > forward_limit) {
            continue;
        }

        pz_vec2 perp = (pz_vec2) { -dir.y, dir.x };
        float lateral = fabsf(pz_vec2_dot(delta, perp));
        if (lateral > lateral_limit) {
            continue;
        }

        return true;
    }

    return false;
}

static bool
ai_has_nearby_tank(pz_tank_manager *mgr, pz_vec2 pos, float radius,
    int exclude_id, float extra_margin)
{
    if (!mgr) {
        return false;
    }

    float radius_sum = radius + mgr->collision_radius + extra_margin;
    float radius_sq = radius_sum * radius_sum;

    for (int i = 0; i < PZ_MAX_TANKS; i++) {
        pz_tank *tank = &mgr->tanks[i];
        if (!(tank->flags & PZ_TANK_FLAG_ACTIVE)) {
            continue;
        }
        if (tank->flags & PZ_TANK_FLAG_DEAD) {
            continue;
        }
        if (tank->id == exclude_id) {
            continue;
        }

        pz_vec2 delta = pz_vec2_sub(pos, tank->pos);
        if (pz_vec2_len_sq(delta) < radius_sq) {
            return true;
        }
    }

    return false;
}

// Check if there's another AI tank blocking us that we should yield to.
// Returns the blocking tank's ID if we should yield, -1 otherwise.
// The tank with the LOWER ID yields to let the higher ID pass first.
static int
ai_should_yield_to(pz_tank_manager *tank_mgr, const pz_tank *tank,
    pz_vec2 move_dir, float check_radius)
{
    if (!tank_mgr || !tank) {
        return -1;
    }

    float dir_len = pz_vec2_len(move_dir);
    if (dir_len < 0.001f) {
        return -1;
    }

    pz_vec2 dir = pz_vec2_scale(move_dir, 1.0f / dir_len);
    float combined_radius = AI_TANK_RADIUS + tank_mgr->collision_radius;

    for (int i = 0; i < PZ_MAX_TANKS; i++) {
        pz_tank *other = &tank_mgr->tanks[i];
        if (!(other->flags & PZ_TANK_FLAG_ACTIVE)) {
            continue;
        }
        if (other->flags & PZ_TANK_FLAG_DEAD) {
            continue;
        }
        if (other->id == tank->id) {
            continue;
        }
        // Only yield to other AI tanks (skip player tanks)
        if (other->flags & PZ_TANK_FLAG_PLAYER) {
            continue;
        }

        pz_vec2 to_other = pz_vec2_sub(other->pos, tank->pos);
        float dist = pz_vec2_len(to_other);

        // Check if other tank is in our path
        if (dist > check_radius || dist < 0.01f) {
            continue;
        }

        // Check if they're roughly in front of us
        float forward = pz_vec2_dot(to_other, dir);
        if (forward < 0.0f) {
            continue; // Behind us
        }

        // Check lateral distance
        float lateral = sqrtf(dist * dist - forward * forward);
        if (lateral > combined_radius * 1.2f) {
            continue; // Not blocking our path
        }

        // We're blocking each other - lower ID yields
        if (tank->id < other->id) {
            return other->id;
        }
    }

    return -1;
}

static bool
ai_pick_detour_target(pz_ai_controller *ctrl, const pz_tank *tank,
    const pz_map *map, pz_tank_manager *tank_mgr, pz_vec2 move_dir, pz_rng *rng)
{
    const float detour_dist = AI_TANK_RADIUS * 2.0f;
    pz_vec2 base_dir = pz_vec2_normalize(move_dir);
    if (pz_vec2_len_sq(base_dir) < 0.0001f) {
        return false;
    }

    // Check if we should yield to another tank
    int yield_to
        = ai_should_yield_to(tank_mgr, tank, move_dir, AI_TANK_RADIUS * 4.0f);

    pz_vec2 candidates[3];
    if (yield_to >= 0) {
        // We should yield - prioritize backing up!
        candidates[0] = pz_vec2_scale(base_dir, -1.0f); // Backward first
        candidates[1] = pz_vec2_rotate(base_dir, PZ_PI * 0.5f);
        candidates[2] = pz_vec2_rotate(base_dir, -PZ_PI * 0.5f);
    } else {
        // Normal detour - try sides first
        candidates[0] = pz_vec2_rotate(base_dir, PZ_PI * 0.5f);
        candidates[1] = pz_vec2_rotate(base_dir, -PZ_PI * 0.5f);
        candidates[2] = pz_vec2_scale(base_dir, -1.0f);
    }

    int start = 0;
    if (rng && yield_to < 0) {
        // Only randomize if not yielding (yielding has fixed priority)
        start = pz_rng_int(rng, 0, 2);
    }

    for (int i = 0; i < 3; i++) {
        int idx = (yield_to >= 0) ? i : ((start + i) % 3);
        pz_vec2 target = pz_vec2_add(
            tank->pos, pz_vec2_scale(candidates[idx], detour_dist));

        if (map && !is_position_valid(map, target, AI_TANK_RADIUS)) {
            continue;
        }
        if (ai_hits_tank(tank_mgr, target, AI_TANK_RADIUS, tank->id)) {
            continue;
        }

        ctrl->detour_active = true;
        ctrl->detour_target = target;
        // Shorter detour when yielding - just need to make space
        ctrl->detour_timer = (yield_to >= 0) ? 0.5f : 1.0f;
        if (rng) {
            ctrl->detour_timer
                += pz_rng_float(rng) * (yield_to >= 0 ? 0.5f : 2.0f);
        }
        return true;
    }

    return false;
}

static void
ai_apply_detour(pz_ai_controller *ctrl, const pz_tank *tank, const pz_map *map,
    pz_tank_manager *tank_mgr, pz_vec2 *move_dir, pz_rng *rng, float dt)
{
    if (!ctrl || !tank || !move_dir || !tank_mgr) {
        return;
    }

    const float min_input = 0.25f;
    const float arrive_threshold = 0.4f;
    // Faster reaction when escaping toxic - can't afford to wait!
    const float blocked_time = (ctrl->toxic_urgency > 0.5f) ? 0.08f : 0.25f;
    const float step = AI_TANK_RADIUS * 1.1f;
    const float stall_dist = 0.02f;

    if (!ctrl->detour_has_last_pos) {
        ctrl->detour_last_pos = tank->pos;
        ctrl->detour_has_last_pos = true;
    }

    if (ctrl->detour_active) {
        ctrl->detour_timer -= dt;
        pz_vec2 to_target = pz_vec2_sub(ctrl->detour_target, tank->pos);
        float dist = pz_vec2_len(to_target);
        if (ctrl->detour_timer <= 0.0f || dist <= arrive_threshold) {
            ctrl->detour_active = false;
            ctrl->detour_blocked_timer = 0.0f;
        } else if (dist > 0.01f) {
            *move_dir = pz_vec2_scale(to_target, 1.0f / dist);
        }
        ctrl->detour_last_pos = tank->pos;
        return;
    }

    if (pz_vec2_len(*move_dir) < min_input) {
        ctrl->detour_blocked_timer = 0.0f;
        ctrl->detour_last_pos = tank->pos;
        return;
    }

    pz_vec2 test = pz_vec2_add(tank->pos, pz_vec2_scale(*move_dir, step));
    if (map && !is_position_valid(map, test, AI_TANK_RADIUS)) {
        ctrl->detour_blocked_timer = 0.0f;
        return;
    }

    bool blocked_by_tank = ai_is_tank_blocking(
        tank_mgr, tank->pos, *move_dir, AI_TANK_RADIUS, tank->id);
    if (!blocked_by_tank
        && ai_hits_tank(tank_mgr, test, AI_TANK_RADIUS, tank->id)) {
        blocked_by_tank = true;
    }
    if (!blocked_by_tank
        && ai_has_nearby_tank(
            tank_mgr, tank->pos, AI_TANK_RADIUS, tank->id, 0.6f)) {
        blocked_by_tank = true;
    }

    if (!blocked_by_tank) {
        ctrl->detour_blocked_timer = 0.0f;
        ctrl->detour_last_pos = tank->pos;
        return;
    }

    pz_vec2 moved = pz_vec2_sub(tank->pos, ctrl->detour_last_pos);
    if (pz_vec2_len(moved) < stall_dist) {
        ctrl->detour_blocked_timer += dt;
    } else {
        ctrl->detour_blocked_timer = 0.0f;
    }
    ctrl->detour_last_pos = tank->pos;

    if (ctrl->detour_blocked_timer < blocked_time) {
        return;
    }

    ai_pick_detour_target(ctrl, tank, map, tank_mgr, *move_dir, rng);
    if (ctrl->detour_active) {
        pz_vec2 to_target = pz_vec2_sub(ctrl->detour_target, tank->pos);
        float dist = pz_vec2_len(to_target);
        if (dist > 0.01f) {
            *move_dir = pz_vec2_scale(to_target, 1.0f / dist);
        }
    }
}

// Distance margin from boundary to consider "safe enough"
#define AI_TOXIC_SAFE_MARGIN 2.5f
// Time between toxic escape recalculations
#define AI_TOXIC_CHECK_INTERVAL 0.5f

// Calculate how urgent it is for the AI to escape the toxic cloud.
// Returns 0.0 if safe, up to 1.0+ if in immediate danger.
static float
ai_calc_toxic_urgency(const pz_toxic_cloud *toxic_cloud, pz_vec2 pos)
{
    if (!toxic_cloud || !toxic_cloud->config.enabled) {
        return 0.0f;
    }

    // Already in toxic = maximum urgency - RUN!
    if (pz_toxic_cloud_is_inside(toxic_cloud, pos)) {
        return 1.5f;
    }

    // Check if we'll be outside the FINAL safe zone when cloud fully closes
    // This is the key check - start moving early!
    bool will_be_toxic_at_end
        = pz_toxic_cloud_will_be_inside(toxic_cloud, pos, 1.0f);

    if (!will_be_toxic_at_end) {
        // We're in the final safe zone - no need to move
        return 0.0f;
    }

    // We WILL be in the toxic zone when it fully closes - need to escape!
    // Calculate urgency based on current cloud progress
    float progress = pz_toxic_cloud_get_progress(toxic_cloud);

    // Even at 0% progress, if we'll be outside the final zone, start moving
    // with moderate urgency (0.3). As progress increases, urgency increases.
    // At 50% progress -> urgency 0.6
    // At 80% progress -> urgency 0.9
    // At 100% or inside toxic -> urgency 1.5
    float urgency = 0.3f + progress * 0.7f;

    // Boost urgency if cloud is getting very close to our position
    float dist = pz_toxic_cloud_distance_to_boundary(toxic_cloud, pos);
    if (dist > -3.0f) {
        // Very close to current boundary - boost urgency
        urgency = pz_maxf(urgency, 0.8f);
    }

    return pz_clampf(urgency, 0.3f, 1.0f);
}

// Find a safe escape target and update the escape path.
// ai_index and total_ais are used to spread escape targets so AIs don't
// all converge on the same point.
// Returns true if escape is needed.
static bool
ai_update_toxic_escape(pz_ai_controller *ctrl, const pz_map *map,
    const pz_toxic_cloud *toxic_cloud, pz_vec2 current_pos, int8_t floor_level,
    float dt, int ai_index, int total_ais)
{
    if (!toxic_cloud || !toxic_cloud->config.enabled) {
        ctrl->toxic_escaping = false;
        ctrl->toxic_urgency = 0.0f;
        return false;
    }

    // Calculate current urgency
    float urgency = ai_calc_toxic_urgency(toxic_cloud, current_pos);
    float prev_urgency = ctrl->toxic_urgency;
    ctrl->toxic_urgency = urgency;

    // Already escaping with valid path? Keep following it!
    if (ctrl->toxic_escaping && ctrl->toxic_escape_path.valid
        && !pz_path_is_complete(&ctrl->toxic_escape_path)) {
        // Check if we've arrived at escape target
        float dist_to_target
            = pz_vec2_dist(current_pos, ctrl->toxic_escape_target);
        if (dist_to_target < 1.5f) {
            // Arrived - check if target is still safe AT FINAL CLOSURE
            // Use will_be_inside for consistency with urgency calculation!
            if (!pz_toxic_cloud_will_be_inside(
                    toxic_cloud, ctrl->toxic_escape_target, 1.0f)) {
                // Made it to a position that's safe even at 100% closure!
                ctrl->toxic_escaping = false;
                pz_path_clear(&ctrl->toxic_escape_path);
                return false;
            }
            // Target won't be safe at final closure, need new path (fall
            // through)
        } else {
            // Still en route - keep following current path, don't recalculate!
            return true;
        }
    }

    // If safe (low urgency), stop escaping
    if (urgency < 0.1f) {
        if (ctrl->toxic_escaping) {
            pz_log(PZ_LOG_DEBUG, PZ_LOG_CAT_GAME,
                "AI reached safety, stopping toxic escape");
            ctrl->toxic_escaping = false;
            pz_path_clear(&ctrl->toxic_escape_path);
        }
        return false;
    }

    // Log when AI first detects it needs to escape
    if (prev_urgency < 0.1f && urgency >= 0.1f && !ctrl->toxic_escaping) {
        pz_log(PZ_LOG_INFO, PZ_LOG_CAT_GAME,
            "AI at (%.1f, %.1f) detects toxic threat, urgency=%.2f, "
            "starting evacuation!",
            current_pos.x, current_pos.y, urgency);
    }

    // Update check timer - only recalculate periodically
    ctrl->toxic_check_timer -= dt;
    if (ctrl->toxic_escaping && ctrl->toxic_check_timer > 0.0f) {
        // Not time to recalculate yet, keep current escape state
        return ctrl->toxic_escaping;
    }

    // Time to find/update escape path
    ctrl->toxic_check_timer = AI_TOXIC_CHECK_INTERVAL * 2.0f; // Less frequent

    // Get a safe target position with spreading so AIs don't all converge
    // on the same point. Each AI gets a different position around the safe
    // zone. If we'll be toxic at the final closure, target the final safe
    // zone immediately so we don't stall in a temporarily safe area.
    bool will_be_toxic_at_end
        = pz_toxic_cloud_will_be_inside(toxic_cloud, current_pos, 1.0f);
    float target_progress
        = will_be_toxic_at_end ? 1.0f : toxic_cloud->closing_progress;
    pz_vec2 safe_target = pz_toxic_cloud_get_safe_position_spread_at_progress(
        toxic_cloud, current_pos, AI_TOXIC_SAFE_MARGIN, ai_index, total_ais,
        target_progress);

    // Log what we're targeting
    pz_log(PZ_LOG_DEBUG, PZ_LOG_CAT_GAME,
        "AI %d/%d at (%.1f, %.1f) seeking safe position (%.1f, %.1f)", ai_index,
        total_ais, current_pos.x, current_pos.y, safe_target.x, safe_target.y);

    // Verify target is actually safe
    if (pz_toxic_cloud_is_inside(toxic_cloud, safe_target)) {
        // Fallback: move toward center
        safe_target = toxic_cloud->config.center;
        pz_log(PZ_LOG_DEBUG, PZ_LOG_CAT_GAME,
            "Safe position was toxic, using center (%.1f, %.1f)", safe_target.x,
            safe_target.y);
    }

    // Check if target is reachable (not inside a wall)
    if (map && pz_map_is_solid(map, safe_target)) {
        // Try to find a nearby valid position
        const float offsets[] = { 0.0f, 1.0f, -1.0f, 2.0f, -2.0f, 3.0f, -3.0f };
        bool found_valid = false;
        for (int i = 0; i < 7 && !found_valid; i++) {
            for (int j = 0; j < 7 && !found_valid; j++) {
                pz_vec2 test = {
                    safe_target.x + offsets[i],
                    safe_target.y + offsets[j],
                };
                if (!pz_map_is_solid(map, test)
                    && !pz_toxic_cloud_is_inside(toxic_cloud, test)) {
                    safe_target = test;
                    found_valid = true;
                }
            }
        }
        if (found_valid) {
            pz_log(PZ_LOG_DEBUG, PZ_LOG_CAT_GAME,
                "Adjusted safe target to (%.1f, %.1f) (was in wall)",
                safe_target.x, safe_target.y);
        }
    }

    // Use A* to find path to safe target
    ctrl->toxic_escape_target = safe_target;
    ctrl->toxic_escape_path = pz_pathfind(
        map, current_pos, safe_target, AI_TANK_RADIUS, floor_level);

    if (ctrl->toxic_escape_path.valid) {
        pz_path_smooth(
            &ctrl->toxic_escape_path, map, AI_TANK_RADIUS, floor_level);
        ctrl->toxic_escaping = true;
        pz_log(PZ_LOG_INFO, PZ_LOG_CAT_GAME,
            "AI escaping toxic: path to (%.1f, %.1f), %d waypoints, "
            "urgency=%.2f",
            safe_target.x, safe_target.y, ctrl->toxic_escape_path.count,
            urgency);
        return true;
    }

    // Pathfinding failed - use direct escape as fallback
    ctrl->toxic_escaping = true;
    pz_log(PZ_LOG_WARN, PZ_LOG_CAT_GAME,
        "AI toxic escape pathfinding failed to (%.1f, %.1f), using direct",
        safe_target.x, safe_target.y);
    return true;
}

// Get movement direction for toxic escape.
// Returns zero vector if not escaping.
static pz_vec2
ai_get_toxic_escape_move(pz_ai_controller *ctrl,
    const pz_toxic_cloud *toxic_cloud, pz_vec2 current_pos)
{
    if (!ctrl->toxic_escaping) {
        return pz_vec2_zero();
    }

    // If we have a valid path, follow it
    if (ctrl->toxic_escape_path.valid
        && !pz_path_is_complete(&ctrl->toxic_escape_path)) {
        // Advance through waypoints
        while (pz_path_advance(&ctrl->toxic_escape_path, current_pos,
            AI_PATH_ARRIVE_THRESHOLD)) { }

        if (!pz_path_is_complete(&ctrl->toxic_escape_path)) {
            pz_vec2 target = pz_path_get_target(&ctrl->toxic_escape_path);
            pz_vec2 to_target = pz_vec2_sub(target, current_pos);
            float dist = pz_vec2_len(to_target);
            if (dist > 0.01f) {
                return pz_vec2_scale(to_target, 1.0f / dist);
            }
        }
    }

    // Fallback: direct movement toward escape target or safe zone
    if (ctrl->toxic_urgency > 0.5f) {
        pz_vec2 dir = pz_toxic_cloud_escape_direction(toxic_cloud, current_pos);
        if (pz_vec2_len_sq(dir) > 0.0001f) {
            return dir;
        }
        // Last resort: move toward target
        pz_vec2 to_target = pz_vec2_sub(ctrl->toxic_escape_target, current_pos);
        float dist = pz_vec2_len(to_target);
        if (dist > 0.01f) {
            return pz_vec2_scale(to_target, 1.0f / dist);
        }
    }

    return pz_vec2_zero();
}

// Look for an incoming projectile to shoot down.
// Returns true when an incoming projectile threat is found and sets aim_angle.
static bool
find_projectile_defense_target(const pz_projectile_manager *proj_mgr,
    const pz_tank *tank, float *aim_angle)
{
    if (!proj_mgr || !tank || !aim_angle) {
        return false;
    }

    const float defense_range = 8.0f;
    const float defense_lane_radius = 0.85f;
    const float defense_max_time = 0.8f;

    bool found = false;
    float best_time = defense_max_time;
    pz_vec2 best_target = tank->pos;

    for (int i = 0; i < PZ_MAX_PROJECTILES; i++) {
        const pz_projectile *proj = &proj_mgr->projectiles[i];
        if (!proj->active) {
            continue;
        }

        if (proj->owner_id == tank->id) {
            continue;
        }

        float proj_speed = pz_vec2_len(proj->velocity);
        if (proj_speed < 0.1f) {
            continue;
        }

        pz_vec2 to_tank = pz_vec2_sub(tank->pos, proj->pos);
        float dist = pz_vec2_len(to_tank);
        if (dist > defense_range) {
            continue;
        }

        pz_vec2 proj_dir = pz_vec2_scale(proj->velocity, 1.0f / proj_speed);
        float along = pz_vec2_dot(to_tank, proj_dir);
        if (along <= 0.0f) {
            continue;
        }

        float time_to_hit = along / proj_speed;
        if (time_to_hit > defense_max_time) {
            continue;
        }

        pz_vec2 closest = pz_vec2_sub(to_tank, pz_vec2_scale(proj_dir, along));
        float miss_dist = pz_vec2_len(closest);
        if (miss_dist > defense_lane_radius) {
            continue;
        }

        if (!found || time_to_hit < best_time) {
            float lead_time = pz_clampf(time_to_hit, 0.05f, 0.25f);
            best_time = time_to_hit;
            best_target = pz_vec2_add(
                proj->pos, pz_vec2_scale(proj->velocity, lead_time));
            found = true;
        }
    }

    if (!found) {
        return false;
    }

    pz_vec2 to_target = pz_vec2_sub(best_target, tank->pos);
    if (pz_vec2_len(to_target) < 0.01f) {
        return false;
    }

    *aim_angle = atan2f(to_target.x, to_target.y);
    return true;
}

// Mine avoidance behavior tuning
#define AI_MINE_AVOID_RADIUS 4.0f
#define AI_MINE_PANIC_RADIUS (PZ_MINE_DAMAGE_RADIUS + 0.4f)
#define AI_MINE_SAFE_SHOOT_RADIUS (PZ_MINE_DAMAGE_RADIUS + 0.6f)
#define AI_MINE_TARGET_RANGE 8.0f
#define AI_MINE_POSITION_SAFE_RADIUS (PZ_MINE_DAMAGE_RADIUS + 0.7f)

// Check if a position is valid (not inside a wall)
static bool
is_position_valid(const pz_map *map, pz_vec2 pos, float radius)
{
    if (!map) {
        return true;
    }

    // Check center and multiple points around the tank for robust collision
    if (pz_map_is_solid(map, pos)) {
        return false;
    }
    // Cardinal directions
    if (pz_map_is_solid(map, (pz_vec2) { pos.x + radius, pos.y })) {
        return false;
    }
    if (pz_map_is_solid(map, (pz_vec2) { pos.x - radius, pos.y })) {
        return false;
    }
    if (pz_map_is_solid(map, (pz_vec2) { pos.x, pos.y + radius })) {
        return false;
    }
    if (pz_map_is_solid(map, (pz_vec2) { pos.x, pos.y - radius })) {
        return false;
    }
    // Diagonal directions (corners)
    float diag = radius * 0.707f; // radius / sqrt(2)
    if (pz_map_is_solid(map, (pz_vec2) { pos.x + diag, pos.y + diag })) {
        return false;
    }
    if (pz_map_is_solid(map, (pz_vec2) { pos.x + diag, pos.y - diag })) {
        return false;
    }
    if (pz_map_is_solid(map, (pz_vec2) { pos.x - diag, pos.y + diag })) {
        return false;
    }
    if (pz_map_is_solid(map, (pz_vec2) { pos.x - diag, pos.y - diag })) {
        return false;
    }

    return true;
}

static bool
is_position_safe_from_mines(
    const pz_mine_manager *mine_mgr, pz_vec2 pos, float radius)
{
    if (!mine_mgr) {
        return true;
    }

    float safe_radius = AI_MINE_POSITION_SAFE_RADIUS + radius;
    float safe_radius_sq = safe_radius * safe_radius;

    for (int i = 0; i < PZ_MAX_MINES; i++) {
        const pz_mine *mine = &mine_mgr->mines[i];
        if (!mine->active) {
            continue;
        }

        float dx = pos.x - mine->pos.x;
        float dz = pos.y - mine->pos.y;
        float dist_sq = dx * dx + dz * dz;
        if (dist_sq < safe_radius_sq) {
            return false;
        }
    }

    return true;
}

static bool
is_position_safe(const pz_map *map, const pz_mine_manager *mine_mgr,
    pz_vec2 pos, float radius)
{
    if (!is_position_valid(map, pos, radius)) {
        return false;
    }

    return is_position_safe_from_mines(mine_mgr, pos, radius);
}

static pz_vec2
steer_away_from_mines(const pz_mine_manager *mine_mgr, pz_vec2 pos,
    pz_vec2 move_dir, float avoid_radius, float panic_radius)
{
    if (!mine_mgr) {
        return move_dir;
    }

    pz_vec2 repel = { 0.0f, 0.0f };
    bool panic = false;

    for (int i = 0; i < PZ_MAX_MINES; i++) {
        const pz_mine *mine = &mine_mgr->mines[i];
        if (!mine->active) {
            continue;
        }

        float dx = pos.x - mine->pos.x;
        float dz = pos.y - mine->pos.y;
        float dist = sqrtf(dx * dx + dz * dz);
        if (dist < 0.001f) {
            continue;
        }

        float armed_scale = mine->arm_timer <= 0.0f ? 1.0f : 0.35f;
        if (mine->arm_timer <= 0.0f && dist < panic_radius) {
            panic = true;
        }

        if (dist < avoid_radius) {
            float strength = (avoid_radius - dist) / avoid_radius;
            strength *= armed_scale;
            pz_vec2 away = { dx / dist, dz / dist };
            repel = pz_vec2_add(repel, pz_vec2_scale(away, strength));
        }
    }

    if (pz_vec2_len(repel) < 0.01f) {
        return move_dir;
    }

    if (panic || pz_vec2_len(move_dir) < 0.01f) {
        float repel_len = pz_vec2_len(repel);
        if (repel_len < 0.001f) {
            return move_dir;
        }
        return pz_vec2_scale(repel, 1.0f / repel_len);
    }

    pz_vec2 blended = pz_vec2_add(move_dir, pz_vec2_scale(repel, 1.4f));
    float blended_len = pz_vec2_len(blended);
    if (blended_len < 0.001f) {
        return move_dir;
    }

    return pz_vec2_scale(blended, 1.0f / blended_len);
}

static bool
ray_segment_hits_circle(pz_vec2 start, pz_vec2 dir, float seg_len,
    pz_vec2 center, float radius, float *out_dist)
{
    if (seg_len <= 0.0f) {
        return false;
    }

    pz_vec2 to_center = pz_vec2_sub(center, start);
    float along = pz_vec2_dot(to_center, dir);
    if (along < 0.0f || along > seg_len) {
        return false;
    }

    pz_vec2 closest = pz_vec2_add(start, pz_vec2_scale(dir, along));
    float miss_dist = pz_vec2_dist(closest, center);
    if (miss_dist > radius) {
        return false;
    }

    if (out_dist) {
        *out_dist = along;
    }

    return true;
}

// Simulate a ricochet path and see if it can hit the player without
// intersecting the shooter.
static bool
simulate_ricochet_shot(const pz_map *map, pz_vec2 start_pos, pz_vec2 dir,
    int max_bounces, float max_dist, pz_vec2 player_pos, float player_radius,
    pz_vec2 self_pos, float self_radius, float self_ignore_dist,
    float *out_total_dist, int *out_bounces)
{
    if (!map || max_dist <= 0.0f) {
        return false;
    }

    float dir_len = pz_vec2_len(dir);
    if (dir_len < 0.0001f) {
        return false;
    }

    dir = pz_vec2_scale(dir, 1.0f / dir_len);

    float remaining = max_dist;
    float total_dist = 0.0f;
    int bounces = 0;
    pz_vec2 pos = start_pos;

    for (int step = 0; step <= max_bounces; step++) {
        if (remaining <= 0.01f) {
            break;
        }

        pz_vec2 end = pz_vec2_add(pos, pz_vec2_scale(dir, remaining));
        pz_raycast_result ray = pz_map_raycast_ex(map, pos, end);
        float seg_len = ray.hit ? ray.distance : remaining;

        float hit_dist = 0.0f;
        bool hit_player = ray_segment_hits_circle(
            pos, dir, seg_len, player_pos, player_radius, &hit_dist);

        if (hit_player) {
            if (!ray.hit || hit_dist <= (ray.distance - player_radius)) {
                float self_hit_dist = 0.0f;
                bool hit_self = ray_segment_hits_circle(
                    pos, dir, hit_dist, self_pos, self_radius, &self_hit_dist);
                if (hit_self) {
                    bool ignore_self
                        = (step == 0 && self_hit_dist <= self_ignore_dist);
                    if (!ignore_self && self_hit_dist < hit_dist) {
                        return false;
                    }
                }

                total_dist += hit_dist;
                if (out_total_dist) {
                    *out_total_dist = total_dist;
                }
                if (out_bounces) {
                    *out_bounces = bounces;
                }
                return true;
            }
        }

        float self_hit_dist = 0.0f;
        bool hit_self = ray_segment_hits_circle(
            pos, dir, seg_len, self_pos, self_radius, &self_hit_dist);
        if (hit_self) {
            bool ignore_self = (step == 0 && self_hit_dist <= self_ignore_dist);
            if (!ignore_self) {
                return false;
            }
        }

        if (!ray.hit || bounces >= max_bounces) {
            break;
        }

        pos = pz_vec2_add(ray.point, pz_vec2_scale(ray.normal, 0.05f));
        dir = pz_vec2_reflect(dir, ray.normal);
        remaining -= seg_len;
        total_dist += seg_len;
        bounces++;
    }

    return false;
}

// Try to find a ricochet shot angle to hit the player (multi-bounce).
// Returns true if a valid shot was found, sets bounce_angle.
static bool
find_ricochet_shot(const pz_map *map, pz_vec2 ai_pos, pz_vec2 player_pos,
    float max_ray_dist, int max_bounces, int num_angles, float self_radius,
    float self_ignore_dist, float *bounce_angle)
{
    if (!map || num_angles <= 0 || max_bounces < 0) {
        return false;
    }

    const float player_hit_radius = 0.9f; // Tank collision radius

    float best_score = -1.0f;
    float best_angle = 0.0f;

    for (int i = 0; i < num_angles; i++) {
        float angle = (float)i * (2.0f * PZ_PI / (float)num_angles);
        pz_vec2 dir = { sinf(angle), cosf(angle) };

        float total_dist = 0.0f;
        int bounces = 0;
        if (!simulate_ricochet_shot(map, ai_pos, dir, max_bounces, max_ray_dist,
                player_pos, player_hit_radius, ai_pos, self_radius,
                self_ignore_dist, &total_dist, &bounces)) {
            continue;
        }

        float score = 10.0f;
        score -= (float)bounces * 1.5f;
        score -= total_dist * 0.02f;

        if (score > best_score) {
            best_score = score;
            best_angle = angle;
        }
    }

    if (best_score > 0.0f) {
        *bounce_angle = best_angle;
        return true;
    }

    return false;
}

// Find a cover position relative to the player
// Returns true if cover was found, sets cover_pos and peek_pos
// The AI will hide behind cover, then move out to peek and fire
static bool
find_cover_position(const pz_map *map, pz_vec2 ai_pos, pz_vec2 player_pos,
    const pz_mine_manager *mine_mgr, pz_vec2 *cover_pos, pz_vec2 *peek_pos)
{
    if (!map) {
        return false;
    }

    const float tank_radius = 0.9f;

    // Direction to player
    pz_vec2 to_player = pz_vec2_sub(player_pos, ai_pos);
    float dist_to_player = pz_vec2_len(to_player);
    if (dist_to_player < 0.1f) {
        return false;
    }
    pz_vec2 dir_to_player = pz_vec2_scale(to_player, 1.0f / dist_to_player);

    float best_score = -1.0f;
    pz_vec2 best_cover = ai_pos;
    pz_vec2 best_peek = ai_pos;

    // Sample positions in a grid around the AI to find good cover spots
    const float search_range = 10.0f;
    const float step = 1.0f;
    int grid_steps = (int)(search_range * 2.0f / step) + 1;

    for (int ix = 0; ix < grid_steps; ix++) {
        float dx = -search_range + ix * step;
        for (int iy = 0; iy < grid_steps; iy++) {
            float dy = -search_range + iy * step;
            pz_vec2 test_cover = { ai_pos.x + dx, ai_pos.y + dy };

            // Skip if position is in a wall
            if (!is_position_safe(map, mine_mgr, test_cover, tank_radius)) {
                continue;
            }

            // Skip if player can see this position (not good cover)
            if (check_line_of_sight(map, test_cover, player_pos)) {
                continue;
            }

            // This is a potential cover position - now find a peek position
            // The peek position should be closer to player and have LOS

            // Try stepping toward the player from cover (7 steps: 1.0 to 4.0)
            for (int peek_i = 0; peek_i < 7; peek_i++) {
                float peek_dist = 1.0f + peek_i * 0.5f;
                pz_vec2 test_peek = pz_vec2_add(
                    test_cover, pz_vec2_scale(dir_to_player, peek_dist));

                // Peek position must be valid
                if (!is_position_safe(map, mine_mgr, test_peek, tank_radius)) {
                    continue;
                }

                // Peek position must have LOS to player
                if (!check_line_of_sight(map, test_peek, player_pos)) {
                    continue;
                }

                // Found a valid cover/peek pair!
                float score = 10.0f;

                // Prefer cover closer to AI
                float cover_dist = pz_vec2_dist(ai_pos, test_cover);
                score -= cover_dist * 0.3f;

                // Prefer shorter peek distance (less time exposed)
                score -= peek_dist * 0.5f;

                // Prefer cover that's somewhat toward the player
                float toward
                    = pz_vec2_dot(pz_vec2_scale((pz_vec2) { dx, dy },
                                      1.0f / sqrtf(dx * dx + dy * dy + 0.01f)),
                        dir_to_player);
                if (toward > 0) {
                    score += toward * 3.0f;
                }

                if (score > best_score) {
                    best_score = score;
                    best_cover = test_cover;
                    best_peek = test_peek;
                }

                break; // Found a peek for this cover, try next cover
            }
        }
    }

    if (best_score > 0) {
        *cover_pos = best_cover;
        *peek_pos = best_peek;
        return true;
    }

    return false;
}

/* ============================================================================
 * Pathfinding Helpers
 * ============================================================================
 */

// How often to update paths (seconds)
#define AI_PATH_UPDATE_INTERVAL 0.5f

// Request a new path to a goal position
// Returns true if a valid path was found
static bool
ai_request_path(pz_ai_controller *ctrl, const pz_map *map, pz_vec2 start,
    pz_vec2 goal, int8_t floor_level)
{
    // Check if we already have a valid path to the same goal
    float goal_dist = pz_vec2_dist(ctrl->path_goal, goal);
    if (ctrl->path.valid && goal_dist < 1.0f) {
        // Same goal, check if path is still valid
        if (pz_path_is_valid(&ctrl->path, map, AI_TANK_RADIUS, floor_level)) {
            return true; // Keep current path
        }
    }

    // Find new path on the same floor level
    ctrl->path = pz_pathfind(map, start, goal, AI_TANK_RADIUS, floor_level);
    ctrl->path_goal = goal;

    if (ctrl->path.valid) {
        // Smooth the path to remove unnecessary waypoints
        pz_path_smooth(&ctrl->path, map, AI_TANK_RADIUS, floor_level);

        pz_log(PZ_LOG_DEBUG, PZ_LOG_CAT_GAME,
            "AI path found: %d waypoints to (%.1f, %.1f)", ctrl->path.count,
            goal.x, goal.y);
    }

    return ctrl->path.valid;
}

// Follow the current path, returns movement direction
// Returns zero vector if no valid path or at destination
static pz_vec2
ai_follow_path(pz_ai_controller *ctrl, pz_vec2 current_pos)
{
    if (!ctrl->path.valid || pz_path_is_complete(&ctrl->path)) {
        return (pz_vec2) { 0.0f, 0.0f };
    }

    // Advance through waypoints we've passed
    while (
        pz_path_advance(&ctrl->path, current_pos, AI_PATH_ARRIVE_THRESHOLD)) {
        // Keep advancing while we're close to waypoints
    }

    if (pz_path_is_complete(&ctrl->path)) {
        return (pz_vec2) { 0.0f, 0.0f };
    }

    // Get direction to current waypoint
    pz_vec2 target = pz_path_get_target(&ctrl->path);
    pz_vec2 to_target = pz_vec2_sub(target, current_pos);
    float dist = pz_vec2_len(to_target);

    if (dist < 0.01f) {
        return (pz_vec2) { 0.0f, 0.0f };
    }

    return pz_vec2_scale(to_target, 1.0f / dist);
}

// Check if we need to update the path (timer expired or goal changed
// significantly)
static bool
ai_should_repath(pz_ai_controller *ctrl, pz_vec2 new_goal, float dt)
{
    ctrl->path_update_timer -= dt;

    // Repath if timer expired
    if (ctrl->path_update_timer <= 0.0f) {
        ctrl->path_update_timer = AI_PATH_UPDATE_INTERVAL;
        return true;
    }

    // Repath if goal moved significantly
    float goal_dist = pz_vec2_dist(ctrl->path_goal, new_goal);
    if (goal_dist > 2.0f) {
        ctrl->path_update_timer = AI_PATH_UPDATE_INTERVAL;
        return true;
    }

    // Repath if current path is invalid
    if (!ctrl->path.valid) {
        return true;
    }

    return false;
}

/* ============================================================================
 * Unified AI Behavior Update
 * ============================================================================
 */

// Check if there's an incoming projectile that threatens the tank
// Returns true if evasion is needed, sets evade_dir
static bool
check_incoming_projectiles(pz_ai_controller *ctrl, pz_tank *tank,
    pz_projectile_manager *proj_mgr, pz_vec2 *evade_dir)
{
    (void)ctrl; // Reserved for future state-dependent evasion

    if (!proj_mgr) {
        return false;
    }

    const float threat_radius = 3.0f; // How close before we evade

    bool threat_found = false;
    pz_vec2 best_evade = { 0.0f, 0.0f };
    float closest_threat = threat_radius;

    // Check all active projectiles
    for (int i = 0; i < PZ_MAX_PROJECTILES; i++) {
        const pz_projectile *proj = &proj_mgr->projectiles[i];
        if (!proj->active) {
            continue;
        }

        // Ignore our own projectiles
        if (proj->owner_id == tank->id) {
            continue;
        }

        // Check distance to predicted position
        pz_vec2 to_proj = pz_vec2_sub(proj->pos, tank->pos);

        // Check if projectile is heading toward us
        float proj_speed = pz_vec2_len(proj->velocity);
        if (proj_speed < 0.1f) {
            continue;
        }

        pz_vec2 proj_dir = pz_vec2_scale(proj->velocity, 1.0f / proj_speed);

        // Calculate closest approach distance
        float dot = pz_vec2_dot(to_proj, proj_dir);
        if (dot > 0) {
            continue; // Projectile moving away from us
        }

        // Point of closest approach
        pz_vec2 closest_point
            = pz_vec2_sub(proj->pos, pz_vec2_scale(proj_dir, dot));
        float closest_dist = pz_vec2_dist(closest_point, tank->pos);

        if (closest_dist < closest_threat) {
            closest_threat = closest_dist;
            threat_found = true;

            // Calculate perpendicular evasion direction
            pz_vec2 perp = { -proj_dir.y, proj_dir.x };

            // Choose which side to evade to (away from projectile)
            if (pz_vec2_dot(perp, to_proj) > 0) {
                best_evade = perp;
            } else {
                best_evade = pz_vec2_scale(perp, -1.0f);
            }
        }
    }

    if (threat_found) {
        *evade_dir = best_evade;
    }

    return threat_found;
}

// Find a flanking position to approach player from the side
static bool
find_flank_position(const pz_map *map, const pz_mine_manager *mine_mgr,
    pz_vec2 ai_pos, pz_vec2 player_pos, pz_vec2 *flank_pos)
{
    const float tank_radius = 0.9f;
    const float flank_distance = 8.0f; // How far to the side
    const float approach_distance = 6.0f; // How close to get

    // Direction from AI to player
    pz_vec2 to_player = pz_vec2_sub(player_pos, ai_pos);
    float dist = pz_vec2_len(to_player);
    if (dist < 0.1f) {
        return false;
    }

    pz_vec2 dir_to_player = pz_vec2_scale(to_player, 1.0f / dist);

    // Perpendicular directions for flanking
    pz_vec2 perp_left = { -dir_to_player.y, dir_to_player.x };
    pz_vec2 perp_right = { dir_to_player.y, -dir_to_player.x };

    // Try both flanking directions
    pz_vec2 candidates[2];
    candidates[0] = pz_vec2_add(player_pos,
        pz_vec2_add(pz_vec2_scale(perp_left, flank_distance),
            pz_vec2_scale(dir_to_player, -approach_distance)));
    candidates[1] = pz_vec2_add(player_pos,
        pz_vec2_add(pz_vec2_scale(perp_right, flank_distance),
            pz_vec2_scale(dir_to_player, -approach_distance)));

    // Choose the closest valid flanking position
    float best_dist = 1e10f;
    bool found = false;

    for (int i = 0; i < 2; i++) {
        if (!is_position_safe(map, mine_mgr, candidates[i], tank_radius)) {
            continue;
        }

        float d = pz_vec2_dist(ai_pos, candidates[i]);
        if (d < best_dist) {
            best_dist = d;
            *flank_pos = candidates[i];
            found = true;
        }
    }

    return found;
}

static pz_vec2
ai_move_toward_goal(pz_ai_controller *ctrl, const pz_map *map,
    pz_vec2 current_pos, pz_vec2 goal, float arrive_threshold, float dt,
    bool use_path, int8_t floor_level)
{
    float dist = pz_vec2_dist(goal, current_pos);
    if (dist < arrive_threshold) {
        return pz_vec2_zero();
    }

    if (use_path && map) {
        if (ai_should_repath(ctrl, goal, dt)) {
            ai_request_path(ctrl, map, current_pos, goal, floor_level);
        }
        pz_vec2 move_dir = ai_follow_path(ctrl, current_pos);
        if (pz_vec2_len(move_dir) < 0.01f && dist > arrive_threshold) {
            pz_vec2 to_goal = pz_vec2_sub(goal, current_pos);
            return pz_vec2_scale(to_goal, 1.0f / dist);
        }
        return move_dir;
    }

    if (dist > 0.01f) {
        pz_vec2 to_goal = pz_vec2_sub(goal, current_pos);
        return pz_vec2_scale(to_goal, 1.0f / dist);
    }

    return pz_vec2_zero();
}

static void
ai_update_behavior(pz_ai_controller *ctrl, pz_tank *tank,
    pz_tank_manager *tank_mgr, const pz_map *map, pz_vec2 player_pos,
    pz_projectile_manager *proj_mgr, const pz_mine_manager *mine_mgr,
    pz_rng *rng, const pz_toxic_cloud *toxic_cloud, float dt, int ai_index,
    int total_ais, const pz_enemy_stats *stats)
{
    const float arrive_threshold = 0.5f;
    const float cover_wait_time = 1.5f;
    const float firing_time = 2.0f;
    const float cover_search_cooldown = 3.0f;
    const float engage_distance = 12.0f;
    const float chase_distance = 20.0f;
    const float too_close_distance = 3.0f;
    const float evade_duration = 0.3f;
    const float health_retreat_threshold = 0.2f;

    bool can_move = ai_has_behavior(ctrl, PZ_AI_BEHAVIOR_MOVE);
    bool can_pathfind = ai_has_behavior(ctrl, PZ_AI_BEHAVIOR_PATHFIND);
    bool can_use_cover = ai_has_behavior(ctrl, PZ_AI_BEHAVIOR_USE_COVER);
    bool can_chase = ai_has_behavior(ctrl, PZ_AI_BEHAVIOR_CHASE);
    bool can_flank = ai_has_behavior(ctrl, PZ_AI_BEHAVIOR_FLANK);
    bool can_evade = ai_has_behavior(ctrl, PZ_AI_BEHAVIOR_EVADE);
    bool can_strafe = ai_has_behavior(ctrl, PZ_AI_BEHAVIOR_STRAFE);
    bool can_escape_toxic = ai_has_behavior(ctrl, PZ_AI_BEHAVIOR_TOXIC_ESCAPE);
    bool avoid_mines = ai_has_behavior(ctrl, PZ_AI_BEHAVIOR_TARGET_MINES);

    // Timers
    if (ctrl->evade_timer > 0.0f) {
        ctrl->evade_timer -= dt;
    }
    if (ctrl->aggression_timer > 0.0f) {
        ctrl->aggression_timer -= dt;
    }
    if (ctrl->cover_search_timer > 0.0f) {
        ctrl->cover_search_timer -= dt;
    }

    float dist_to_player = pz_vec2_dist(tank->pos, player_pos);
    float health_ratio = (float)tank->health / (float)tank->max_health;

    ctrl->wants_to_fire = false;
    pz_vec2 move_dir = pz_vec2_zero();

    // a) Decide where to go (if moving)
    if (can_evade && proj_mgr) {
        pz_vec2 evade_dir = pz_vec2_zero();
        if (check_incoming_projectiles(ctrl, tank, proj_mgr, &evade_dir)
            && ctrl->evade_timer <= 0.0f) {
            ctrl->state = PZ_AI_STATE_EVADING;
            ctrl->evade_dir = evade_dir;
            ctrl->evade_timer = evade_duration;
            ctrl->wants_to_fire = false;
            pz_path_clear(&ctrl->path);
        }
    }

    switch (ctrl->state) {
    case PZ_AI_STATE_IDLE:
        if (can_chase) {
            ctrl->state = PZ_AI_STATE_CHASING;
            ctrl->aggression_timer = 1.0f + pz_rng_float(rng);
            pz_path_clear(&ctrl->path);
        } else if (can_use_cover && ctrl->cover_search_timer <= 0.0f) {
            if (find_cover_position(map, tank->pos, player_pos, mine_mgr,
                    &ctrl->cover_pos, &ctrl->peek_pos)) {
                ctrl->has_cover = true;
                ctrl->state = PZ_AI_STATE_SEEKING_COVER;
                ctrl->move_target = ctrl->cover_pos;
                if (can_pathfind) {
                    ai_request_path(ctrl, map, tank->pos, ctrl->cover_pos,
                        tank->floor_level);
                }
                pz_log(PZ_LOG_DEBUG, PZ_LOG_CAT_GAME,
                    "AI %d found cover at (%.1f, %.1f)", tank->id,
                    ctrl->cover_pos.x, ctrl->cover_pos.y);
            } else {
                ctrl->cover_search_timer = cover_search_cooldown;
            }
        } else {
            ctrl->wants_to_fire = ctrl->can_see_player || ctrl->has_bounce_shot
                || ctrl->defending_projectile || ctrl->targeting_mine;
        }
        break;

    case PZ_AI_STATE_SEEKING_COVER:
        if (!can_use_cover) {
            ctrl->state = can_chase ? PZ_AI_STATE_CHASING : PZ_AI_STATE_IDLE;
            pz_path_clear(&ctrl->path);
            break;
        }
        move_dir = ai_move_toward_goal(ctrl, map, tank->pos, ctrl->cover_pos,
            arrive_threshold, dt, can_pathfind, tank->floor_level);
        if (pz_vec2_len(move_dir) < 0.01f) {
            ctrl->state = PZ_AI_STATE_IN_COVER;
            ctrl->state_timer
                = cover_wait_time * (0.5f + 0.5f * pz_rng_float(rng));
            pz_path_clear(&ctrl->path);
            pz_log(PZ_LOG_DEBUG, PZ_LOG_CAT_GAME, "AI %d arrived at cover",
                tank->id);
        }
        break;

    case PZ_AI_STATE_IN_COVER:
        if (!can_use_cover) {
            ctrl->state = can_chase ? PZ_AI_STATE_CHASING : PZ_AI_STATE_IDLE;
            pz_path_clear(&ctrl->path);
            break;
        }
        if (check_line_of_sight(map, tank->pos, player_pos)) {
            ctrl->has_cover = false;
            ctrl->state = PZ_AI_STATE_IDLE;
            ctrl->state_timer = 0.0f;
            break;
        }
        ctrl->state_timer -= dt;
        if (ctrl->state_timer <= 0.0f) {
            if (pz_rng_int(rng, 0, 99) < 25) {
                ctrl->has_cover = false;
                ctrl->state = PZ_AI_STATE_IDLE;
                ctrl->state_timer = 0.0f;
            } else {
                ctrl->state = PZ_AI_STATE_PEEKING;
                ctrl->move_target = ctrl->peek_pos;
                ctrl->shots_fired = 0;
                if (can_pathfind) {
                    ai_request_path(ctrl, map, tank->pos, ctrl->peek_pos,
                        tank->floor_level);
                }
            }
        }
        break;

    case PZ_AI_STATE_PEEKING:
        if (!can_use_cover) {
            ctrl->state = can_chase ? PZ_AI_STATE_CHASING : PZ_AI_STATE_IDLE;
            pz_path_clear(&ctrl->path);
            break;
        }
        move_dir = ai_move_toward_goal(ctrl, map, tank->pos, ctrl->peek_pos,
            arrive_threshold, dt, can_pathfind, tank->floor_level);
        if (pz_vec2_len(move_dir) < 0.01f) {
            ctrl->state = PZ_AI_STATE_FIRING;
            ctrl->state_timer = firing_time;
            pz_path_clear(&ctrl->path);
        }
        ctrl->wants_to_fire = ctrl->can_see_player;
        break;

    case PZ_AI_STATE_FIRING:
        if (!can_use_cover) {
            ctrl->state = can_chase ? PZ_AI_STATE_CHASING : PZ_AI_STATE_IDLE;
            pz_path_clear(&ctrl->path);
            break;
        }
        ctrl->state_timer -= dt;
        ctrl->wants_to_fire = ctrl->can_see_player;
        if (ctrl->state_timer <= 0.0f
            || ctrl->shots_fired >= ctrl->max_shots_per_peek) {
            ctrl->state = PZ_AI_STATE_RETREATING;
            ctrl->move_target = ctrl->cover_pos;
            if (can_pathfind) {
                ai_request_path(
                    ctrl, map, tank->pos, ctrl->cover_pos, tank->floor_level);
            }
        }
        break;

    case PZ_AI_STATE_RETREATING:
        if (!can_use_cover) {
            ctrl->state = can_chase ? PZ_AI_STATE_CHASING : PZ_AI_STATE_IDLE;
            pz_path_clear(&ctrl->path);
            break;
        }
        move_dir = ai_move_toward_goal(ctrl, map, tank->pos, ctrl->cover_pos,
            arrive_threshold, dt, can_pathfind, tank->floor_level);
        if (pz_vec2_len(move_dir) < 0.01f) {
            ctrl->state = PZ_AI_STATE_IN_COVER;
            ctrl->state_timer
                = cover_wait_time * (0.5f + 0.5f * pz_rng_float(rng));
            pz_path_clear(&ctrl->path);
            if (pz_rng_int(rng, 0, 99) < 50) {
                ctrl->has_cover = false;
                ctrl->state = PZ_AI_STATE_IDLE;
            }
        }
        break;

    case PZ_AI_STATE_CHASING:
        if (!can_chase) {
            ctrl->state
                = can_use_cover ? PZ_AI_STATE_SEEKING_COVER : PZ_AI_STATE_IDLE;
            pz_path_clear(&ctrl->path);
            break;
        }
        move_dir = ai_move_toward_goal(ctrl, map, tank->pos, player_pos,
            arrive_threshold, dt, can_pathfind, tank->floor_level);
        if (can_strafe && dist_to_player < engage_distance
            && ctrl->can_see_player) {
            ctrl->state = PZ_AI_STATE_ENGAGING;
            ctrl->state_timer = 4.0f + pz_rng_range(rng, 0.0f, 2.0f);
            pz_path_clear(&ctrl->path);
        }
        if (can_flank && ctrl->aggression_timer <= 0.0f
            && dist_to_player < chase_distance) {
            if (find_flank_position(map, mine_mgr, tank->pos, player_pos,
                    &ctrl->flank_target)) {
                ctrl->state = PZ_AI_STATE_FLANKING;
                ctrl->aggression_timer = 2.0f;
                if (can_pathfind) {
                    ai_request_path(ctrl, map, tank->pos, ctrl->flank_target,
                        tank->floor_level);
                }
            } else {
                ctrl->aggression_timer = 1.0f;
            }
        }
        if (can_use_cover && health_ratio < health_retreat_threshold) {
            ctrl->state = PZ_AI_STATE_SEEKING_COVER;
            ctrl->has_cover = false;
            pz_path_clear(&ctrl->path);
        }
        ctrl->wants_to_fire = ctrl->can_see_player;
        break;

    case PZ_AI_STATE_FLANKING:
        if (!can_flank) {
            ctrl->state = can_chase ? PZ_AI_STATE_CHASING : PZ_AI_STATE_IDLE;
            pz_path_clear(&ctrl->path);
            break;
        }
        move_dir = ai_move_toward_goal(ctrl, map, tank->pos, ctrl->flank_target,
            arrive_threshold, dt, can_pathfind, tank->floor_level);
        if (pz_vec2_len(move_dir) < 0.01f) {
            ctrl->state
                = can_strafe ? PZ_AI_STATE_ENGAGING : PZ_AI_STATE_CHASING;
            ctrl->state_timer = 2.0f;
            pz_path_clear(&ctrl->path);
        }
        ctrl->wants_to_fire = ctrl->can_see_player;
        if (can_use_cover && health_ratio < health_retreat_threshold) {
            ctrl->state = PZ_AI_STATE_SEEKING_COVER;
            ctrl->has_cover = false;
            pz_path_clear(&ctrl->path);
        }
        break;

    case PZ_AI_STATE_ENGAGING:
        if (!can_strafe) {
            ctrl->state = can_chase ? PZ_AI_STATE_CHASING : PZ_AI_STATE_IDLE;
            pz_path_clear(&ctrl->path);
            break;
        }
        ctrl->state_timer -= dt;
        if (dist_to_player > 0.1f) {
            pz_vec2 to_player = pz_vec2_sub(player_pos, tank->pos);
            pz_vec2 dir_to_player
                = pz_vec2_scale(to_player, 1.0f / dist_to_player);
            pz_vec2 strafe = { -dir_to_player.y, dir_to_player.x };
            if (((int)(ctrl->state_timer * 2.0f)) % 2 == 0) {
                strafe = pz_vec2_scale(strafe, -1.0f);
            }

            if (dist_to_player > engage_distance * 0.7f) {
                move_dir
                    = pz_vec2_add(strafe, pz_vec2_scale(dir_to_player, 0.5f));
            } else if (dist_to_player < too_close_distance) {
                move_dir
                    = pz_vec2_add(strafe, pz_vec2_scale(dir_to_player, -0.8f));
            } else {
                move_dir = strafe;
            }

            float len = pz_vec2_len(move_dir);
            if (len > 0.1f) {
                move_dir = pz_vec2_scale(move_dir, 1.0f / len);
            }
        }

        ctrl->wants_to_fire = ctrl->can_see_player;
        if (ctrl->state_timer <= 0.0f) {
            if (can_use_cover && health_ratio < health_retreat_threshold) {
                ctrl->state = PZ_AI_STATE_SEEKING_COVER;
                ctrl->has_cover = false;
            } else if (can_chase && pz_rng_int(rng, 0, 99) < 40) {
                ctrl->state = PZ_AI_STATE_CHASING;
                ctrl->aggression_timer = 1.5f;
            } else {
                ctrl->state_timer = 2.0f + pz_rng_range(rng, 0.0f, 2.0f);
            }
        }
        if (!ctrl->can_see_player) {
            ctrl->state = can_chase ? PZ_AI_STATE_CHASING : PZ_AI_STATE_IDLE;
            pz_path_clear(&ctrl->path);
        }
        if (can_use_cover && health_ratio < health_retreat_threshold) {
            ctrl->state = PZ_AI_STATE_SEEKING_COVER;
            ctrl->has_cover = false;
        }
        break;

    case PZ_AI_STATE_EVADING:
        if (!can_evade) {
            ctrl->state = can_chase ? PZ_AI_STATE_CHASING : PZ_AI_STATE_IDLE;
            pz_path_clear(&ctrl->path);
            break;
        }
        move_dir = ctrl->evade_dir;
        ctrl->wants_to_fire = false;
        if (ctrl->evade_timer <= 0.0f) {
            if (can_use_cover && health_ratio < health_retreat_threshold) {
                ctrl->state = PZ_AI_STATE_SEEKING_COVER;
                ctrl->has_cover = false;
            } else if (can_strafe && dist_to_player < engage_distance) {
                ctrl->state = PZ_AI_STATE_ENGAGING;
            } else if (can_chase) {
                ctrl->state = PZ_AI_STATE_CHASING;
            } else {
                ctrl->state = PZ_AI_STATE_IDLE;
            }
            pz_path_clear(&ctrl->path);
        }
        break;

    default:
        ctrl->state = can_chase ? PZ_AI_STATE_CHASING : PZ_AI_STATE_IDLE;
        pz_path_clear(&ctrl->path);
        break;
    }

    // b) Move there
    // c) Consider a different goal (done inside state transitions)

    if (ctrl->targeting_mine && ctrl->state != PZ_AI_STATE_EVADING) {
        ctrl->wants_to_fire = true;
    }

    float suppress_threshold = can_strafe ? 1.2f : 1.0f;

    // d) Check evasive moves if necessary (toxic escape overrides)
    if (can_escape_toxic
        && ai_update_toxic_escape(ctrl, map, toxic_cloud, tank->pos,
            tank->floor_level, dt, ai_index, total_ais)) {
        pz_vec2 escape_dir
            = ai_get_toxic_escape_move(ctrl, toxic_cloud, tank->pos);
        if (pz_vec2_len_sq(escape_dir) > 0.001f) {
            move_dir = escape_dir;
            if (ctrl->toxic_urgency > suppress_threshold) {
                ctrl->wants_to_fire = false;
            }
        }
    }

    if (ctrl->toxic_escaping && ctrl->toxic_urgency <= suppress_threshold) {
        bool escape_fire = ctrl->can_see_player;
        if (!escape_fire
            && ai_has_behavior(ctrl, PZ_AI_BEHAVIOR_BOUNCE_SHOTS)) {
            escape_fire = ctrl->has_bounce_shot;
        }
        if (ctrl->defending_projectile || ctrl->targeting_mine) {
            escape_fire = true;
        }
        if (escape_fire) {
            ctrl->wants_to_fire = true;
        }
    }

    if (avoid_mines) {
        move_dir = steer_away_from_mines(mine_mgr, tank->pos, move_dir,
            AI_MINE_AVOID_RADIUS, AI_MINE_PANIC_RADIUS);
    }

    if (can_move || (can_escape_toxic && ctrl->toxic_escaping)
        || (avoid_mines && pz_vec2_len_sq(move_dir) > 0.0f)) {
        float sep_urgency = 0.0f;
        pz_vec2 separation = ai_compute_separation(
            tank_mgr, tank->pos, tank->id, 3.0f, &sep_urgency);
        float sep_blend = (ctrl->toxic_urgency > 0.5f) ? 1.5f : 0.8f;
        move_dir
            = ai_apply_separation(move_dir, separation, sep_urgency, sep_blend);
        ai_apply_detour(ctrl, tank, map, tank_mgr, &move_dir, rng, dt);
    }

    float current_speed = stats->move_speed;
    if (ctrl->toxic_escaping && ctrl->toxic_urgency > 0.5f) {
        current_speed *= can_strafe ? 1.3f : 1.5f;
    }

    pz_tank_input input = {
        .move_dir = pz_vec2_scale(move_dir, current_speed),
        .target_turret = ctrl->current_aim_angle,
        .fire = false,
    };

    pz_tank_update(tank_mgr, tank, &input, map, toxic_cloud, dt);
}

static bool
find_mine_target(const pz_mine_manager *mine_mgr, const pz_map *map,
    const pz_tank *tank, pz_vec2 player_pos, bool can_see_player,
    pz_vec2 *out_pos, float *out_dist)
{
    if (!mine_mgr || !tank || !out_pos) {
        return false;
    }

    float best_score = 0.0f;
    pz_vec2 best_pos = tank->pos;
    float best_dist = 0.0f;

    for (int i = 0; i < PZ_MAX_MINES; i++) {
        const pz_mine *mine = &mine_mgr->mines[i];
        if (!mine->active) {
            continue;
        }

        float dist = pz_vec2_dist(tank->pos, mine->pos);
        if (dist > AI_MINE_TARGET_RANGE) {
            continue;
        }

        if (dist < AI_MINE_SAFE_SHOOT_RADIUS) {
            continue;
        }

        if (map && !check_line_of_sight(map, tank->pos, mine->pos)) {
            continue;
        }

        float player_dist = pz_vec2_dist(player_pos, mine->pos);
        float near_player
            = pz_clampf(PZ_MINE_DAMAGE_RADIUS + 0.6f - player_dist, 0.0f, 2.0f);
        float near_self = pz_clampf(AI_MINE_AVOID_RADIUS - dist, 0.0f, 2.0f);

        float score = near_self * 1.2f + near_player * 2.2f;
        if (!can_see_player) {
            score += 0.4f;
        }

        if (score > best_score) {
            best_score = score;
            best_pos = mine->pos;
            best_dist = dist;
        }
    }

    if (best_score <= 0.0f) {
        return false;
    }

    *out_pos = best_pos;
    if (out_dist) {
        *out_dist = best_dist;
    }
    return true;
}

__attribute__((unused)) static void
update_level3_ai(pz_ai_controller *ctrl, pz_tank *tank,
    pz_tank_manager *tank_mgr, const pz_map *map, pz_vec2 player_pos,
    pz_projectile_manager *proj_mgr, const pz_mine_manager *mine_mgr,
    pz_rng *rng, const pz_toxic_cloud *toxic_cloud, float dt, int ai_index,
    int total_ais)
{
    const float move_speed = 6.0f; // Fast and aggressive
    const float arrive_threshold = 0.5f;
    const float engage_distance = 12.0f; // Distance to start shooting
    const float chase_distance = 20.0f; // Distance to start chasing
    const float too_close_distance = 3.0f; // Back off if too close
    const float evade_duration = 0.3f;
    const float health_retreat_threshold = 0.2f; // Retreat when below 20% HP

    // Update timers
    if (ctrl->evade_timer > 0.0f) {
        ctrl->evade_timer -= dt;
    }
    if (ctrl->aggression_timer > 0.0f) {
        ctrl->aggression_timer -= dt;
    }
    if (ctrl->cover_search_timer > 0.0f) {
        ctrl->cover_search_timer -= dt;
    }

    float dist_to_player = pz_vec2_dist(tank->pos, player_pos);
    float health_ratio = (float)tank->health / (float)tank->max_health;

    // Check for incoming projectiles - highest priority
    pz_vec2 evade_dir = { 0.0f, 0.0f };
    bool should_evade
        = check_incoming_projectiles(ctrl, tank, proj_mgr, &evade_dir);

    if (should_evade && ctrl->evade_timer <= 0.0f) {
        ctrl->state = PZ_AI_STATE_EVADING;
        ctrl->evade_dir = evade_dir;
        ctrl->evade_timer = evade_duration;
        ctrl->wants_to_fire = false;
        pz_path_clear(&ctrl->path); // Cancel current path when evading
    }

    // State machine
    pz_vec2 move_dir = { 0.0f, 0.0f };

    switch (ctrl->state) {
    case PZ_AI_STATE_IDLE:
        // Start chasing the player immediately
        ctrl->state = PZ_AI_STATE_CHASING;
        ctrl->aggression_timer = 1.0f + pz_rng_float(rng);
        pz_path_clear(&ctrl->path);
        break;

    case PZ_AI_STATE_EVADING:
        // Dodge incoming projectile - use direct movement, no pathfinding
        move_dir = ctrl->evade_dir;
        ctrl->wants_to_fire = false;

        if (ctrl->evade_timer <= 0.0f) {
            // Return to previous behavior based on situation
            if (health_ratio < health_retreat_threshold) {
                ctrl->state = PZ_AI_STATE_SEEKING_COVER;
                ctrl->has_cover = false;
            } else if (dist_to_player < engage_distance) {
                ctrl->state = PZ_AI_STATE_ENGAGING;
            } else {
                ctrl->state = PZ_AI_STATE_CHASING;
            }
            pz_path_clear(&ctrl->path);
        }
        break;

    case PZ_AI_STATE_CHASING: {
        // Use A* to chase player around obstacles
        // Update path periodically since player moves
        if (ai_should_repath(ctrl, player_pos, dt)) {
            ai_request_path(
                ctrl, map, tank->pos, player_pos, tank->floor_level);
        }

        // Follow the path
        move_dir = ai_follow_path(ctrl, tank->pos);

        // Fallback to direct movement if no path (or already close)
        if (pz_vec2_len(move_dir) < 0.01f
            && dist_to_player > arrive_threshold) {
            pz_vec2 to_player = pz_vec2_sub(player_pos, tank->pos);
            move_dir = pz_vec2_scale(to_player, 1.0f / dist_to_player);
        }

        // Transition to engaging when close enough and have LOS
        if (dist_to_player < engage_distance && ctrl->can_see_player) {
            ctrl->state = PZ_AI_STATE_ENGAGING;
            ctrl->state_timer = 4.0f + pz_rng_range(rng, 0.0f, 2.0f);
            pz_path_clear(&ctrl->path);
        }

        // Try to flank frequently
        if (ctrl->aggression_timer <= 0.0f && dist_to_player < chase_distance) {
            if (find_flank_position(map, mine_mgr, tank->pos, player_pos,
                    &ctrl->flank_target)) {
                ctrl->state = PZ_AI_STATE_FLANKING;
                ctrl->aggression_timer = 2.0f;
                // Request path to flank position
                ai_request_path(ctrl, map, tank->pos, ctrl->flank_target,
                    tank->floor_level);
            } else {
                ctrl->aggression_timer = 1.0f;
            }
        }

        // Only retreat if very low health
        if (health_ratio < health_retreat_threshold) {
            ctrl->state = PZ_AI_STATE_SEEKING_COVER;
            ctrl->has_cover = false;
            pz_path_clear(&ctrl->path);
        }

        // Fire while chasing if we can see the player
        ctrl->wants_to_fire = ctrl->can_see_player;
        break;
    }

    case PZ_AI_STATE_FLANKING: {
        // Use A* to move to flanking position
        float flank_dist = pz_vec2_dist(ctrl->flank_target, tank->pos);

        if (flank_dist < arrive_threshold) {
            // Reached flanking position, engage
            ctrl->state = PZ_AI_STATE_ENGAGING;
            ctrl->state_timer = 2.0f;
            pz_path_clear(&ctrl->path);
        } else {
            // Update path if needed
            if (ai_should_repath(ctrl, ctrl->flank_target, dt)) {
                ai_request_path(ctrl, map, tank->pos, ctrl->flank_target,
                    tank->floor_level);
            }
            // Follow path
            move_dir = ai_follow_path(ctrl, tank->pos);
            // Fallback to direct movement if no path
            if (pz_vec2_len(move_dir) < 0.01f
                && flank_dist > arrive_threshold) {
                pz_vec2 to_flank = pz_vec2_sub(ctrl->flank_target, tank->pos);
                move_dir = pz_vec2_scale(to_flank, 1.0f / flank_dist);
            }
        }

        // Fire while flanking if we have line of sight
        ctrl->wants_to_fire = ctrl->can_see_player;

        // Retreat if low health
        if (health_ratio < health_retreat_threshold) {
            ctrl->state = PZ_AI_STATE_SEEKING_COVER;
            ctrl->has_cover = false;
            pz_path_clear(&ctrl->path);
        }
        break;
    }

    case PZ_AI_STATE_ENGAGING: {
        // Strafe and shoot - use direct movement for responsiveness
        // (pathfinding would be too slow for strafing)
        ctrl->state_timer -= dt;

        // Calculate strafing direction (perpendicular to player)
        pz_vec2 to_player = pz_vec2_sub(player_pos, tank->pos);
        if (dist_to_player > 0.1f) {
            pz_vec2 dir_to_player
                = pz_vec2_scale(to_player, 1.0f / dist_to_player);

            // Strafe perpendicular, occasionally switching direction
            pz_vec2 strafe = { -dir_to_player.y, dir_to_player.x };
            if (((int)(ctrl->state_timer * 2.0f)) % 2 == 0) {
                strafe = pz_vec2_scale(strafe, -1.0f);
            }

            // Also move closer or farther based on distance
            if (dist_to_player > engage_distance * 0.7f) {
                // Move closer
                move_dir
                    = pz_vec2_add(strafe, pz_vec2_scale(dir_to_player, 0.5f));
            } else if (dist_to_player < too_close_distance) {
                // Back off
                move_dir
                    = pz_vec2_add(strafe, pz_vec2_scale(dir_to_player, -0.8f));
            } else {
                // Just strafe
                move_dir = strafe;
            }

            // Normalize
            float len = pz_vec2_len(move_dir);
            if (len > 0.1f) {
                move_dir = pz_vec2_scale(move_dir, 1.0f / len);
            }
        }

        ctrl->wants_to_fire = ctrl->can_see_player;

        // Transition out of engaging
        if (ctrl->state_timer <= 0.0f) {
            if (health_ratio < health_retreat_threshold) {
                ctrl->state = PZ_AI_STATE_SEEKING_COVER;
                ctrl->has_cover = false;
            } else if (pz_rng_int(rng, 0, 99) < 40) {
                ctrl->state = PZ_AI_STATE_CHASING;
                ctrl->aggression_timer = 1.5f;
            } else {
                ctrl->state_timer = 2.0f + pz_rng_range(rng, 0.0f, 2.0f);
            }
        }

        // Lost line of sight - chase with pathfinding
        if (!ctrl->can_see_player) {
            ctrl->state = PZ_AI_STATE_CHASING;
            pz_path_clear(&ctrl->path);
        }

        // Retreat if low health
        if (health_ratio < health_retreat_threshold) {
            ctrl->state = PZ_AI_STATE_SEEKING_COVER;
            ctrl->has_cover = false;
        }
        break;
    }

    case PZ_AI_STATE_SEEKING_COVER: {
        // Find cover if we don't have it
        if (!ctrl->has_cover && ctrl->cover_search_timer <= 0.0f) {
            if (find_cover_position(map, tank->pos, player_pos, mine_mgr,
                    &ctrl->cover_pos, &ctrl->peek_pos)) {
                ctrl->has_cover = true;
                ctrl->move_target = ctrl->cover_pos;
                // Request path to cover
                ai_request_path(
                    ctrl, map, tank->pos, ctrl->cover_pos, tank->floor_level);
            } else {
                ctrl->cover_search_timer = 1.0f;
                // Can't find cover, just run away
                pz_vec2 away = pz_vec2_sub(tank->pos, player_pos);
                float len = pz_vec2_len(away);
                if (len > 0.1f) {
                    move_dir = pz_vec2_scale(away, 1.0f / len);
                }
            }
        }

        if (ctrl->has_cover) {
            float cover_dist = pz_vec2_dist(ctrl->cover_pos, tank->pos);

            if (cover_dist < arrive_threshold) {
                ctrl->state = PZ_AI_STATE_IN_COVER;
                ctrl->state_timer = 1.5f + pz_rng_range(rng, 0.0f, 1.5f);
                pz_path_clear(&ctrl->path);
            } else {
                // Update path if needed
                if (ai_should_repath(ctrl, ctrl->cover_pos, dt)) {
                    ai_request_path(ctrl, map, tank->pos, ctrl->cover_pos,
                        tank->floor_level);
                }
                // Follow path
                move_dir = ai_follow_path(ctrl, tank->pos);
                // Fallback to direct movement
                if (pz_vec2_len(move_dir) < 0.01f
                    && cover_dist > arrive_threshold) {
                    pz_vec2 to_cover = pz_vec2_sub(ctrl->cover_pos, tank->pos);
                    move_dir = pz_vec2_scale(to_cover, 1.0f / cover_dist);
                }
            }
        }

        ctrl->wants_to_fire = false;
        break;
    }

    case PZ_AI_STATE_IN_COVER:
        // Wait and recover
        ctrl->state_timer -= dt;

        if (ctrl->state_timer <= 0.0f) {
            // Health recovered enough or timeout - go aggressive again
            if (health_ratio > 0.5f || ctrl->state_timer < -3.0f) {
                ctrl->state = PZ_AI_STATE_CHASING;
                ctrl->has_cover = false;
                ctrl->aggression_timer = 1.0f;
                pz_path_clear(&ctrl->path);
            } else {
                // Peek out to fire
                ctrl->state = PZ_AI_STATE_PEEKING;
                ctrl->move_target = ctrl->peek_pos;
                ctrl->shots_fired = 0;
                // Request path to peek position
                ai_request_path(
                    ctrl, map, tank->pos, ctrl->peek_pos, tank->floor_level);
            }
        }

        ctrl->wants_to_fire = false;
        break;

    case PZ_AI_STATE_PEEKING: {
        float peek_dist = pz_vec2_dist(ctrl->peek_pos, tank->pos);

        if (peek_dist < arrive_threshold) {
            ctrl->state = PZ_AI_STATE_FIRING;
            ctrl->state_timer = 1.5f;
            pz_path_clear(&ctrl->path);
        } else {
            // Update path if needed
            if (ai_should_repath(ctrl, ctrl->peek_pos, dt)) {
                ai_request_path(
                    ctrl, map, tank->pos, ctrl->peek_pos, tank->floor_level);
            }
            // Follow path
            move_dir = ai_follow_path(ctrl, tank->pos);
            // Fallback to direct movement
            if (pz_vec2_len(move_dir) < 0.01f && peek_dist > arrive_threshold) {
                pz_vec2 to_peek = pz_vec2_sub(ctrl->peek_pos, tank->pos);
                move_dir = pz_vec2_scale(to_peek, 1.0f / peek_dist);
            }
        }

        ctrl->wants_to_fire = ctrl->can_see_player;
        break;
    }

    case PZ_AI_STATE_FIRING:
        ctrl->state_timer -= dt;
        ctrl->wants_to_fire = ctrl->can_see_player;

        if (ctrl->state_timer <= 0.0f
            || ctrl->shots_fired >= ctrl->max_shots_per_peek) {
            // Go back to aggressive if health is ok
            if (health_ratio > 0.5f) {
                ctrl->state = PZ_AI_STATE_CHASING;
                ctrl->has_cover = false;
                pz_path_clear(&ctrl->path);
            } else {
                ctrl->state = PZ_AI_STATE_RETREATING;
                ctrl->move_target = ctrl->cover_pos;
                // Request path back to cover
                ai_request_path(
                    ctrl, map, tank->pos, ctrl->cover_pos, tank->floor_level);
            }
        }
        break;

    case PZ_AI_STATE_RETREATING: {
        float cover_dist = pz_vec2_dist(ctrl->cover_pos, tank->pos);

        if (cover_dist < arrive_threshold) {
            ctrl->state = PZ_AI_STATE_IN_COVER;
            ctrl->state_timer = 2.0f;
            pz_path_clear(&ctrl->path);
        } else {
            // Update path if needed
            if (ai_should_repath(ctrl, ctrl->cover_pos, dt)) {
                ai_request_path(
                    ctrl, map, tank->pos, ctrl->cover_pos, tank->floor_level);
            }
            // Follow path
            move_dir = ai_follow_path(ctrl, tank->pos);
            // Fallback to direct movement
            if (pz_vec2_len(move_dir) < 0.01f
                && cover_dist > arrive_threshold) {
                pz_vec2 to_cover = pz_vec2_sub(ctrl->cover_pos, tank->pos);
                move_dir = pz_vec2_scale(to_cover, 1.0f / cover_dist);
            }
        }

        ctrl->wants_to_fire = false;
        break;
    }

    default:
        ctrl->state = PZ_AI_STATE_CHASING;
        pz_path_clear(&ctrl->path);
        break;
    }

    if (ctrl->targeting_mine && ctrl->state != PZ_AI_STATE_EVADING) {
        ctrl->wants_to_fire = true;
    }

    // Toxic cloud escape takes priority over normal behavior
    if (ai_update_toxic_escape(ctrl, map, toxic_cloud, tank->pos,
            tank->floor_level, dt, ai_index, total_ais)) {
        pz_vec2 escape_dir
            = ai_get_toxic_escape_move(ctrl, toxic_cloud, tank->pos);
        if (pz_vec2_len_sq(escape_dir) > 0.001f) {
            move_dir = escape_dir;
            // Level 3 can still fire while escaping unless critical
            if (ctrl->toxic_urgency > 1.2f) {
                ctrl->wants_to_fire = false;
            }
        }
    }

    move_dir = steer_away_from_mines(mine_mgr, tank->pos, move_dir,
        AI_MINE_AVOID_RADIUS, AI_MINE_PANIC_RADIUS);

    // Apply separation steering to avoid crowding other tanks
    // Use stronger separation when escaping toxic - can't afford to get stuck!
    float sep_urgency = 0.0f;
    pz_vec2 separation = ai_compute_separation(
        tank_mgr, tank->pos, tank->id, 3.0f, &sep_urgency);
    float sep_blend = (ctrl->toxic_urgency > 0.5f) ? 1.5f : 0.8f;
    move_dir
        = ai_apply_separation(move_dir, separation, sep_urgency, sep_blend);
    ai_apply_detour(ctrl, tank, map, tank_mgr, &move_dir, rng, dt);

    // Apply movement - faster when escaping toxic
    float current_speed = move_speed;
    if (ctrl->toxic_escaping && ctrl->toxic_urgency > 0.5f) {
        current_speed = move_speed * 1.3f; // Hustle!
    }

    pz_tank_input input = {
        .move_dir = pz_vec2_scale(move_dir, current_speed),
        .target_turret = ctrl->current_aim_angle,
        .fire = false,
    };

    pz_tank_update(tank_mgr, tank, &input, map, toxic_cloud, dt);
}

/* ============================================================================
 * AI Update
 * ============================================================================
 */

void
pz_ai_update(pz_ai_manager *ai_mgr, pz_vec2 player_pos,
    pz_projectile_manager *proj_mgr, pz_mine_manager *mine_mgr, pz_rng *rng,
    const pz_toxic_cloud *toxic_cloud, float dt)
{
    if (!ai_mgr || !ai_mgr->tank_mgr) {
        return;
    }

    for (int i = 0; i < ai_mgr->controller_count; i++) {
        pz_ai_controller *ctrl = &ai_mgr->controllers[i];

        // Get the tank this AI controls
        pz_tank *tank = pz_tank_get_by_id(ai_mgr->tank_mgr, ctrl->tank_id);
        if (!tank) {
            continue; // Tank doesn't exist (destroyed)
        }

        // Skip dead tanks
        if (tank->flags & PZ_TANK_FLAG_DEAD) {
            continue;
        }

        // Get enemy stats for behavior parameters
        const pz_enemy_stats *stats = pz_enemy_get_stats(ctrl->type);
        bool uses_bounce_shots
            = ai_has_behavior(ctrl, PZ_AI_BEHAVIOR_BOUNCE_SHOTS);
        bool requires_bounce_shot
            = ai_has_behavior(ctrl, PZ_AI_BEHAVIOR_REQUIRE_BOUNCE_SHOT);
        bool can_defend_projectiles
            = ai_has_behavior(ctrl, PZ_AI_BEHAVIOR_DEFEND_PROJECTILES);
        bool can_target_mines
            = ai_has_behavior(ctrl, PZ_AI_BEHAVIOR_TARGET_MINES);

        // Calculate angle to player
        float dx = player_pos.x - tank->pos.x;
        float dy = player_pos.y - tank->pos.y;
        float direct_angle = atan2f(dx, dy);

        // Check line of sight
        ctrl->can_see_player
            = check_line_of_sight(ai_mgr->map, tank->pos, player_pos);

        // Check for incoming projectiles to defend against.
        if (can_defend_projectiles && stats->projectile_defense_chance > 0.0f
            && proj_mgr) {
            if (ctrl->defense_check_timer > 0.0f) {
                ctrl->defense_check_timer -= dt;
            }

            if (ctrl->defense_check_timer <= 0.0f) {
                float defense_angle = 0.0f;
                bool found_defense = find_projectile_defense_target(
                    proj_mgr, tank, &defense_angle);
                if (found_defense) {
                    float roll = pz_rng_float(rng);
                    if (roll < stats->projectile_defense_chance) {
                        ctrl->defending_projectile = true;
                        ctrl->defense_aim_angle = defense_angle;
                    } else {
                        ctrl->defending_projectile = false;
                    }
                } else {
                    ctrl->defending_projectile = false;
                }
                ctrl->defense_check_timer = 0.12f;
            }
        } else {
            ctrl->defending_projectile = false;
        }

        ctrl->targeting_mine = false;
        ctrl->mine_target_dist = 0.0f;
        if (can_target_mines && mine_mgr) {
            pz_vec2 mine_pos = tank->pos;
            float mine_dist = 0.0f;
            if (find_mine_target(mine_mgr, ai_mgr->map, tank, player_pos,
                    ctrl->can_see_player, &mine_pos, &mine_dist)) {
                ctrl->targeting_mine = true;
                ctrl->mine_target_pos = mine_pos;
                ctrl->mine_target_dist = mine_dist;
            }
        }

        // Stationary enemies use ricochet shots when player is not visible.
        if (ctrl->defending_projectile) {
            ctrl->target_aim_angle = ctrl->defense_aim_angle;
            ctrl->has_bounce_shot = false;
        } else if (ctrl->targeting_mine) {
            pz_vec2 to_mine = pz_vec2_sub(ctrl->mine_target_pos, tank->pos);
            if (pz_vec2_len(to_mine) > 0.01f) {
                ctrl->target_aim_angle = atan2f(to_mine.x, to_mine.y);
            }
            ctrl->has_bounce_shot = false;
        } else if (uses_bounce_shots) {
            // Update bounce shot search timer
            if (ctrl->bounce_shot_search_timer > 0.0f) {
                ctrl->bounce_shot_search_timer -= dt;
            }

            if (ctrl->can_see_player) {
                // Direct LOS - aim straight at player
                ctrl->target_aim_angle = direct_angle;
                ctrl->has_bounce_shot = false;
            } else {
                // No LOS - try to find a bounce shot
                if (!ctrl->has_bounce_shot
                    && ctrl->bounce_shot_search_timer <= 0.0f) {
                    float bounce_angle;
                    float bounce_range = stats->bounce_shot_range;
                    if (bounce_range <= 0.0f) {
                        bounce_range = 30.0f;
                    }
                    int sample_count = stats->bounce_shot_samples;
                    if (sample_count <= 0) {
                        sample_count = 36;
                    }
                    float self_radius = 1.0f;
                    float self_ignore_dist = 1.7f;
                    if (find_ricochet_shot(ai_mgr->map, tank->pos, player_pos,
                            bounce_range, stats->max_bounces, sample_count,
                            self_radius, self_ignore_dist, &bounce_angle)) {
                        ctrl->has_bounce_shot = true;
                        ctrl->bounce_shot_angle = bounce_angle;
                        pz_log(PZ_LOG_DEBUG, PZ_LOG_CAT_GAME,
                            "AI %d found bounce shot at angle %.1f deg",
                            tank->id, bounce_angle * 180.0f / PZ_PI);
                    } else {
                        // No bounce shot found, try again later
                        ctrl->bounce_shot_search_timer = 0.5f;
                    }
                }

                if (ctrl->has_bounce_shot) {
                    ctrl->target_aim_angle = ctrl->bounce_shot_angle;
                } else {
                    if (requires_bounce_shot) {
                        ctrl->target_aim_angle = ctrl->current_aim_angle;
                    } else {
                        // Fallback: aim at player anyway (for when they become
                        // visible)
                        ctrl->target_aim_angle = direct_angle;
                    }
                }
            }
        } else {
            // Default: aim directly at the player.
            ctrl->target_aim_angle = direct_angle;
        }

        // Smoothly rotate turret towards target
        float angle_diff
            = normalize_angle(ctrl->target_aim_angle - ctrl->current_aim_angle);
        float turret_speed = 5.0f * stats->aim_speed;
        float max_rotation = turret_speed * dt;

        if (fabsf(angle_diff) <= max_rotation) {
            ctrl->current_aim_angle = ctrl->target_aim_angle;
        } else if (angle_diff > 0) {
            ctrl->current_aim_angle += max_rotation;
        } else {
            ctrl->current_aim_angle -= max_rotation;
        }

        ctrl->current_aim_angle = normalize_angle(ctrl->current_aim_angle);

        ai_update_behavior(ctrl, tank, ai_mgr->tank_mgr, ai_mgr->map,
            player_pos, proj_mgr, mine_mgr, rng, toxic_cloud, dt, i,
            ai_mgr->controller_count, stats);

        // Update fire timer
        if (ctrl->fire_timer > 0.0f) {
            ctrl->fire_timer -= dt;
        }

        // Update hesitation timer
        if (ctrl->hesitation_timer > 0.0f) {
            ctrl->hesitation_timer -= dt;
        }

        // Calculate fire confidence for sentry and skirmisher
        // Higher confidence = more bullets allowed, lower hesitation
        if (uses_bounce_shots
            && !ai_has_behavior(ctrl, PZ_AI_BEHAVIOR_STRAFE)) {
            bool has_target = ctrl->can_see_player || ctrl->has_bounce_shot
                || ctrl->defending_projectile || ctrl->targeting_mine;

            // Trigger hesitation when acquiring a new target
            if (has_target && !ctrl->had_target_last_frame) {
                // Random hesitation: 0.15 - 0.4 seconds
                ctrl->hesitation_timer = 0.15f + 0.25f * pz_rng_float(rng);
            }
            ctrl->had_target_last_frame = has_target;

            // Calculate confidence
            if (ctrl->defending_projectile) {
                ctrl->fire_confidence = 1.0f;
                ctrl->hesitation_timer = 0.0f;
            } else if (ctrl->can_see_player) {
                // Direct LOS = high confidence
                ctrl->fire_confidence = 1.0f;
            } else if (ctrl->has_bounce_shot) {
                // Bounce shot = lower confidence (0.3 - 0.5)
                ctrl->fire_confidence = 0.3f + 0.2f * pz_rng_float(rng);
            } else {
                ctrl->fire_confidence = 0.0f;
            }
        } else {
            // Other enemy types: full confidence when they want to fire
            ctrl->fire_confidence = 1.0f;
            ctrl->hesitation_timer = 0.0f;
        }
    }
}

/* ============================================================================
 * AI Firing
 * ============================================================================
 */

// Check if firing would hit another enemy tank (friendly fire check).
// Returns true if the shot is blocked by an ally.
static bool
would_hit_friendly(const pz_ai_manager *ai_mgr, const pz_tank *shooter,
    pz_vec2 fire_dir, float max_range)
{
    if (!ai_mgr || !ai_mgr->tank_mgr || !shooter) {
        return false;
    }

    // Normalize direction
    float dir_len = pz_vec2_len(fire_dir);
    if (dir_len < 0.0001f) {
        return false;
    }
    pz_vec2 dir = pz_vec2_scale(fire_dir, 1.0f / dir_len);

    // Tank collision radius for hit detection
    const float tank_hit_radius = 0.85f;

    // Check against all other enemy tanks
    for (int i = 0; i < ai_mgr->controller_count; i++) {
        const pz_ai_controller *ctrl = &ai_mgr->controllers[i];

        // Skip self
        if (ctrl->tank_id == shooter->id) {
            continue;
        }

        // Get the other tank
        pz_tank *other = pz_tank_get_by_id(ai_mgr->tank_mgr, ctrl->tank_id);
        if (!other || (other->flags & PZ_TANK_FLAG_DEAD)) {
            continue;
        }

        // Check ray-circle intersection
        float hit_dist = 0.0f;
        if (ray_segment_hits_circle(shooter->pos, dir, max_range, other->pos,
                tank_hit_radius, &hit_dist)) {
            // Would hit a friendly - skip firing
            pz_log(PZ_LOG_DEBUG, PZ_LOG_CAT_GAME,
                "AI %d skipping shot - would hit friendly tank %d at dist %.1f",
                shooter->id, other->id, hit_dist);
            return true;
        }
    }

    return false;
}

int
pz_ai_fire(pz_ai_manager *ai_mgr, pz_projectile_manager *proj_mgr)
{
    if (!ai_mgr || !ai_mgr->tank_mgr || !proj_mgr) {
        return 0;
    }

    int fired = 0;

    for (int i = 0; i < ai_mgr->controller_count; i++) {
        pz_ai_controller *ctrl = &ai_mgr->controllers[i];

        // Get the tank
        pz_tank *tank = pz_tank_get_by_id(ai_mgr->tank_mgr, ctrl->tank_id);
        if (!tank || (tank->flags & PZ_TANK_FLAG_DEAD)) {
            continue;
        }

        const pz_enemy_stats *stats = pz_enemy_get_stats(ctrl->type);

        if (!ctrl->wants_to_fire && !ctrl->targeting_mine) {
            continue;
        }

        // Check if we can fire
        // Level 1: Can fire if we see player OR have a bounce shot
        // Others: Need to see the player
        bool can_attempt_fire = ctrl->can_see_player || ctrl->targeting_mine;
        bool uses_bounce_shots
            = ai_has_behavior(ctrl, PZ_AI_BEHAVIOR_BOUNCE_SHOTS);
        if (uses_bounce_shots && ctrl->has_bounce_shot) {
            can_attempt_fire = true;
        }
        if (ctrl->defending_projectile) {
            can_attempt_fire = true;
        }

        if (!can_attempt_fire) {
            continue;
        }

        if (ctrl->fire_timer > 0.0f) {
            continue;
        }

        // Check hesitation timer (small delay when acquiring new target)
        if (ctrl->hesitation_timer > 0.0f) {
            continue;
        }

        float aim_error = fabsf(
            normalize_angle(ctrl->target_aim_angle - ctrl->current_aim_angle));
        float aim_tolerance = stats->aim_tolerance;
        if (aim_error > aim_tolerance) {
            continue;
        }

        // Get weapon stats for this enemy
        const pz_weapon_stats *weapon = pz_weapon_get_stats(stats->weapon_type);

        // Calculate max projectiles based on confidence
        // High confidence (direct LOS): allow more bullets
        // Low confidence (bounce shot): limit to fewer bullets
        int max_projectiles = weapon->max_active_projectiles;
        if (uses_bounce_shots) {
            if (ctrl->fire_confidence >= 0.8f) {
                if (stats->max_projectiles_direct > 0) {
                    max_projectiles = stats->max_projectiles_direct;
                }
            } else if (ctrl->fire_confidence > 0.0f) {
                if (stats->max_projectiles_bounce > 0) {
                    max_projectiles = stats->max_projectiles_bounce;
                }
            }
            if (ctrl->defending_projectile) {
                max_projectiles = 1;
            }
        }

        // Check active projectile count
        int active = pz_projectile_count_by_owner(proj_mgr, tank->id);
        if (active >= max_projectiles) {
            continue;
        }

        // Calculate fire direction first to check for friendly fire
        pz_vec2 spawn_pos = { 0 };
        pz_vec2 fire_dir = { 0 };
        int bounce_cost = 0;
        pz_tank_get_fire_solution(
            tank, ai_mgr->map, &spawn_pos, &fire_dir, &bounce_cost);

        // Friendly fire check - don't shoot if we'd hit another enemy tank
        // Use a reasonable range for the check (longer for ricochet weapons)
        float ff_check_range = 15.0f;
        const pz_enemy_stats *stats_ff = pz_enemy_get_stats(ctrl->type);
        if (stats_ff->max_bounces > 0) {
            ff_check_range = 25.0f; // Longer range for ricochet weapons
        }
        if (would_hit_friendly(ai_mgr, tank, fire_dir, ff_check_range)) {
            continue; // Skip this shot - would hit ally
        }

        // Do not fire while tank is in the air on a jump pad
        if (pz_tank_is_in_air(tank)) {
            continue;
        }

        // Fire!

        float projectile_speed
            = weapon->projectile_speed * stats->projectile_speed_scale;
        pz_projectile_config proj_config = {
            .speed = projectile_speed,
            .max_bounces = stats->max_bounces,
            .lifetime = -1.0f,
            .damage = weapon->damage,
            .scale = weapon->projectile_scale,
            .color = weapon->projectile_color,
            .floor_level = tank->floor_level,
        };

        int proj_slot = pz_projectile_spawn(
            proj_mgr, spawn_pos, fire_dir, &proj_config, tank->id);
        if (proj_slot >= 0 && bounce_cost > 0) {
            pz_projectile *proj = &proj_mgr->projectiles[proj_slot];
            if (proj->bounces_remaining > 0) {
                proj->bounces_remaining -= 1;
            }
        }

        // Reset fire timer to the weapon's max fire rate (same as player).
        ctrl->fire_timer = weapon->fire_cooldown * stats->fire_cooldown_scale;
        fired++;

        // Trigger visual recoil
        tank->recoil = weapon->recoil_strength;

        // Track shots for cover behavior
        ctrl->shots_fired++;

        // After firing a bounce shot, search for a new one next time.
        if (uses_bounce_shots && ctrl->has_bounce_shot) {
            ctrl->has_bounce_shot = false;
            ctrl->bounce_shot_search_timer = 0.3f; // Brief delay before
                                                   // searching again
        }

        pz_log(PZ_LOG_DEBUG, PZ_LOG_CAT_GAME, "AI tank %d fired (%s)", tank->id,
            pz_enemy_type_name(ctrl->type));
    }

    return fired;
}

/* ============================================================================
 * AI Queries
 * ============================================================================
 */

int
pz_ai_count_alive(const pz_ai_manager *ai_mgr)
{
    if (!ai_mgr || !ai_mgr->tank_mgr) {
        return 0;
    }

    int count = 0;

    for (int i = 0; i < ai_mgr->controller_count; i++) {
        const pz_ai_controller *ctrl = &ai_mgr->controllers[i];
        pz_tank *tank = pz_tank_get_by_id(ai_mgr->tank_mgr, ctrl->tank_id);

        if (tank && !(tank->flags & PZ_TANK_FLAG_DEAD)) {
            count++;
        }
    }

    return count;
}

bool
pz_ai_has_elite_alive(const pz_ai_manager *ai_mgr)
{
    if (!ai_mgr || !ai_mgr->tank_mgr) {
        return false;
    }

    for (int i = 0; i < ai_mgr->controller_count; i++) {
        const pz_ai_controller *ctrl = &ai_mgr->controllers[i];
        if (ctrl->type == PZ_ENEMY_TYPE_HUNTER
            || ctrl->type == PZ_ENEMY_TYPE_SNIPER) {
            pz_tank *tank = pz_tank_get_by_id(ai_mgr->tank_mgr, ctrl->tank_id);
            if (tank && !(tank->flags & PZ_TANK_FLAG_DEAD)) {
                return true;
            }
        }
    }

    return false;
}

bool
pz_ai_is_controlled(const pz_ai_manager *ai_mgr, int tank_id)
{
    if (!ai_mgr) {
        return false;
    }

    for (int i = 0; i < ai_mgr->controller_count; i++) {
        if (ai_mgr->controllers[i].tank_id == tank_id) {
            return true;
        }
    }

    return false;
}

pz_ai_controller *
pz_ai_get_controller(pz_ai_manager *ai_mgr, int tank_id)
{
    if (!ai_mgr) {
        return NULL;
    }

    for (int i = 0; i < ai_mgr->controller_count; i++) {
        if (ai_mgr->controllers[i].tank_id == tank_id) {
            return &ai_mgr->controllers[i];
        }
    }

    return NULL;
}
