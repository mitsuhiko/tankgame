/*
 * Map System Implementation
 *
 * Supports map format v2 with combined height+terrain cells.
 */

#include "pz_map.h"
#include "pz_tile_registry.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/pz_log.h"
#include "../core/pz_mem.h"
#include "../core/pz_platform.h"
#include "../core/pz_str.h"

static pz_vec2
pz_map_tile_to_world_float(const pz_map *map, float tile_x, float tile_y)
{
    float half_w = map->world_width / 2.0f;
    float half_h = map->world_height / 2.0f;

    return (pz_vec2) {
        .x = tile_x * map->tile_size + map->tile_size / 2.0f - half_w,
        .y = tile_y * map->tile_size + map->tile_size / 2.0f - half_h,
    };
}

static pz_vec2
pz_map_world_to_tile_float(const pz_map *map, pz_vec2 world_pos)
{
    float half_w = map->world_width / 2.0f;
    float half_h = map->world_height / 2.0f;

    return (pz_vec2) {
        .x = (world_pos.x + half_w - map->tile_size / 2.0f) / map->tile_size,
        .y = (world_pos.y + half_h - map->tile_size / 2.0f) / map->tile_size,
    };
}

// Default tile definitions
static void
init_default_tile_defs(pz_map *map)
{
    // Ground - standard passable terrain (wood floor)
    pz_map_add_tile_def(map, '.', "wood_oak_brown");

    // Stone - wall material (dark rustic wood)
    pz_map_add_tile_def(map, '#', "wood_rustic_dark");
}

pz_map *
pz_map_create(int width, int height, float tile_size)
{
    if (width <= 0 || height <= 0 || width > PZ_MAP_MAX_SIZE
        || height > PZ_MAP_MAX_SIZE) {
        pz_log(PZ_LOG_ERROR, PZ_LOG_CAT_GAME,
            "Invalid map size: %dx%d (max %d)", width, height, PZ_MAP_MAX_SIZE);
        return NULL;
    }

    pz_map *map = pz_calloc(1, sizeof(pz_map));
    if (!map) {
        return NULL;
    }

    map->version = 2;
    map->width = width;
    map->height = height;
    map->tile_size = tile_size;
    map->world_width = width * tile_size;
    map->world_height = height * tile_size;
    map->water_level = -100; // No water by default (far below any tile)
    map->has_water = false;
    map->water_color = (pz_vec3) { 0.2f, 0.4f, 0.6f }; // Default blue
    map->wave_strength = 1.0f; // Default wave strength
    map->wind_direction = 0.0f; // Default wind direction (+X)
    map->wind_strength = 1.0f; // Default wind strength
    map->fog_level = -100; // No fog by default (far below any tile)
    map->has_fog = false;
    map->fog_color = (pz_vec3) { 0.6f, 0.6f, 0.7f }; // Default cool gray

    // Allocate cells
    int num_cells = width * height;
    map->cells = pz_calloc(num_cells, sizeof(pz_map_cell));
    if (!map->cells) {
        pz_map_destroy(map);
        return NULL;
    }

    // Initialize default tile definitions
    init_default_tile_defs(map);

    // Default all cells to ground at height 0
    for (int i = 0; i < num_cells; i++) {
        map->cells[i].height = 0;
        map->cells[i].tile_index = 0; // ground
    }

    // Default lighting: night mode (no sun, dark ambient)
    map->lighting.has_sun = false;
    map->lighting.sun_direction = (pz_vec3) { 0.4f, -0.8f, 0.3f };
    map->lighting.sun_color = (pz_vec3) { 1.0f, 0.95f, 0.85f };
    map->lighting.ambient_color = (pz_vec3) { 0.12f, 0.12f, 0.15f };
    map->lighting.ambient_darkness = 0.85f;

    // Default background: dark gray (fallback, maps should override)
    map->background.type = PZ_BACKGROUND_COLOR;
    map->background.color = (pz_vec3) { 0.2f, 0.2f, 0.25f };
    map->background.color_end = (pz_vec3) { 0.1f, 0.1f, 0.15f };
    map->background.gradient_dir = PZ_GRADIENT_VERTICAL;
    map->background.texture_path[0] = '\0';

    map->has_music = false;
    map->music_name[0] = '\0';
    map->has_toxic_cloud = false;
    map->toxic_config = pz_toxic_cloud_config_default(pz_vec2_zero());

    pz_log(PZ_LOG_INFO, PZ_LOG_CAT_GAME, "Created map %dx%d (%.1f unit tiles)",
        width, height, tile_size);

    return map;
}

void
pz_map_destroy(pz_map *map)
{
    if (!map) {
        return;
    }

    pz_free(map->cells);
    pz_free(map);
}

void
pz_map_set_tile_registry(pz_map *map, const pz_tile_registry *registry)
{
    if (map) {
        map->tile_registry = registry;
    }
}

// Build a hardcoded 16x16 test map
pz_map *
pz_map_create_test(void)
{
    pz_map *map = pz_map_create(16, 16, 2.0f);
    if (!map) {
        return NULL;
    }

    snprintf(map->name, sizeof(map->name), "Test Arena");

    // Add tile definitions
    pz_map_add_tile_def(map, ':', "mud_wet");
    pz_map_add_tile_def(map, '*', "carpet_gray");

    // Define the map layout with heights
    // Format: height, tile_symbol
    // 2# = height 2, stone wall
    // 0. = height 0, ground
    // 0: = height 0, mud
    // 0* = height 0, ice
    const char *layout[] = {
        "2# 2# 2# 2# 2# 2# 2# 2# 2# 2# 2# 2# 2# 2# 2# 2#",
        "2# 0. 0. 0. 0. 0. 0. 0. 0. 0. 0. 0. 0. 0. 0. 2#",
        "2# 0. 0. 2# 2# 0. 0. 0. 0. 2# 2# 0. 0. 0. 0. 2#",
        "2# 0. 0. 2# 2# 0. 0. 0. 0. 2# 2# 0. 0. 0* 0. 2#",
        "2# 0. 0. 0. 0. 0. 0. 0. 0. 0. 0. 0. 0* 0* 0* 2#",
        "2# 0. 0. 0. 0. 0: 0: 0. 0. 0. 0. 0. 0* 0. 0. 2#",
        "2# 0. 0. 0. 0. 0: 0: 0. 0. 0. 0. 0. 0. 0. 0. 2#",
        "2# 0. 0. 0. 0. 0. 0. 0. 0. 0. 0. 0. 0. 0. 0. 2#",
        "2# 0. 0. 0. 0. 0. 0. 0. 0. 0. 0. 0. 0. 0. 0. 2#",
        "2# 0. 0. 0. 0. 0. 0. 0. 0. 0. 0. 0. 0. 0. 0. 2#",
        "2# 0. 0. 0. 0. 0. 0. 0. 0. 0. 0. 0. 0. 0. 0. 2#",
        "2# 0. 0. 2# 2# 0. 0. 0. 0. 0. 0. 0. 0. 0. 0. 2#",
        "2# 0. 0. 2# 2# 0. 0. 0. 0. 2# 2# 0. 0. 0. 0. 2#",
        "2# 0. 0. 0. 0. 0. 0. 0. 0. 2# 2# 0. 0. 0. 0. 2#",
        "2# 0. 0. 0. 0. 0. 0. 0. 0. 0. 0. 0. 0. 0. 0. 2#",
        "2# 2# 2# 2# 2# 2# 2# 2# 2# 2# 2# 2# 2# 2# 2# 2#",
    };

    // Parse layout
    for (int y = 0; y < 16; y++) {
        const char *row = layout[15 - y]; // Flip Y (file is top-down)
        const char *p = row;

        for (int x = 0; x < 16; x++) {
            // Skip whitespace
            while (*p && isspace((unsigned char)*p)) {
                p++;
            }

            if (!*p)
                break;

            // Parse height (can be negative)
            int height = 0;
            bool negative = false;
            if (*p == '-') {
                negative = true;
                p++;
            }
            while (*p && isdigit((unsigned char)*p)) {
                height = height * 10 + (*p - '0');
                p++;
            }
            if (negative) {
                height = -height;
            }

            // Parse tile symbol
            char tile_symbol = *p++;

            // Set cell
            int tile_idx = pz_map_find_tile_def(map, tile_symbol);
            if (tile_idx < 0) {
                tile_idx = 0; // fallback to ground
            }

            pz_map_cell cell
                = { .height = (int8_t)height, .tile_index = (uint8_t)tile_idx };
            pz_map_set_cell(map, x, y, cell);
        }
    }

    // Add spawn points (4 corners for FFA)
    map->spawn_count = 4;

    // Bottom-left
    map->spawns[0] = (pz_spawn_point) {
        .pos = { -12.0f, -12.0f },
        .angle = 0.785f,
        .team = 0,
        .team_spawn = false,
    };

    // Bottom-right
    map->spawns[1] = (pz_spawn_point) {
        .pos = { 12.0f, -12.0f },
        .angle = 2.356f,
        .team = 0,
        .team_spawn = false,
    };

    // Top-left
    map->spawns[2] = (pz_spawn_point) {
        .pos = { -12.0f, 12.0f },
        .angle = -0.785f,
        .team = 0,
        .team_spawn = false,
    };

    // Top-right
    map->spawns[3] = (pz_spawn_point) {
        .pos = { 12.0f, 12.0f },
        .angle = -2.356f,
        .team = 0,
        .team_spawn = false,
    };

    pz_log(PZ_LOG_INFO, PZ_LOG_CAT_GAME, "Created test map: %s", map->name);

    return map;
}

// ============================================================================
// Cell Access
// ============================================================================

pz_map_cell
pz_map_get_cell(const pz_map *map, int x, int y)
{
    if (!map || !pz_map_in_bounds(map, x, y)) {
        // Out of bounds returns a solid wall
        return (pz_map_cell) { .height = 99, .tile_index = 1 };
    }
    return map->cells[y * map->width + x];
}

void
pz_map_set_cell(pz_map *map, int x, int y, pz_map_cell cell)
{
    if (!map || !pz_map_in_bounds(map, x, y)) {
        return;
    }
    map->cells[y * map->width + x] = cell;
}

int8_t
pz_map_get_height(const pz_map *map, int x, int y)
{
    if (!map || !pz_map_in_bounds(map, x, y)) {
        return 99; // Out of bounds is very high wall
    }
    return map->cells[y * map->width + x].height;
}

void
pz_map_set_height(pz_map *map, int x, int y, int8_t height)
{
    if (!map || !pz_map_in_bounds(map, x, y)) {
        return;
    }
    map->cells[y * map->width + x].height = height;
}

uint8_t
pz_map_get_tile_index(const pz_map *map, int x, int y)
{
    if (!map || !pz_map_in_bounds(map, x, y)) {
        return 1; // Out of bounds returns stone
    }
    return map->cells[y * map->width + x].tile_index;
}

const pz_tile_def *
pz_map_get_tile_def(const pz_map *map, int x, int y)
{
    if (!map)
        return NULL;
    uint8_t idx = pz_map_get_tile_index(map, x, y);
    return pz_map_get_tile_def_by_index(map, idx);
}

const pz_tile_def *
pz_map_get_tile_def_by_index(const pz_map *map, uint8_t index)
{
    if (!map || index >= map->tile_def_count) {
        return NULL;
    }
    return &map->tile_defs[index];
}

int
pz_map_add_tile_def(pz_map *map, char symbol, const char *name)
{
    if (!map || !name || map->tile_def_count >= PZ_MAP_MAX_TILE_DEFS) {
        return -1;
    }

    // Check if symbol already exists
    for (int i = 0; i < map->tile_def_count; i++) {
        if (map->tile_defs[i].symbol == symbol) {
            // Update existing
            strncpy(map->tile_defs[i].name, name,
                sizeof(map->tile_defs[i].name) - 1);
            map->tile_defs[i].name[sizeof(map->tile_defs[i].name) - 1] = '\0';
            return i;
        }
    }

    int idx = map->tile_def_count++;
    pz_tile_def *def = &map->tile_defs[idx];

    def->symbol = symbol;
    strncpy(def->name, name, sizeof(def->name) - 1);
    def->name[sizeof(def->name) - 1] = '\0';

    return idx;
}

int
pz_map_find_tile_def(const pz_map *map, char symbol)
{
    if (!map)
        return -1;

    for (int i = 0; i < map->tile_def_count; i++) {
        if (map->tile_defs[i].symbol == symbol) {
            return i;
        }
    }
    return -1;
}

int
pz_map_find_tag_def(const pz_map *map, const char *name)
{
    if (!map || !name || !name[0]) {
        return -1;
    }

    for (int i = 0; i < map->tag_def_count; i++) {
        if (strcmp(map->tag_defs[i].name, name) == 0) {
            return i;
        }
    }

    return -1;
}

int
pz_map_add_tag_def(pz_map *map, const pz_tag_def *def)
{
    if (!map || !def || !def->name[0]) {
        return -1;
    }

    int existing = pz_map_find_tag_def(map, def->name);
    if (existing >= 0) {
        return existing;
    }

    if (map->tag_def_count >= PZ_MAP_MAX_TAG_DEFS) {
        return -1;
    }

    int idx = map->tag_def_count++;
    map->tag_defs[idx] = *def;
    return idx;
}

bool
pz_map_remove_tag_def(pz_map *map, int index)
{
    if (!map || index < 0 || index >= map->tag_def_count) {
        return false;
    }

    char removed_name[32];
    strncpy(removed_name, map->tag_defs[index].name, sizeof(removed_name) - 1);
    removed_name[sizeof(removed_name) - 1] = '\0';

    for (int i = 0; i < map->tag_placement_count;) {
        if (map->tag_placements[i].tag_index == index) {
            pz_map_remove_tag_placement(map, i);
            continue;
        }
        if (map->tag_placements[i].tag_index > index) {
            map->tag_placements[i].tag_index--;
        }
        i++;
    }

    for (int i = index; i < map->tag_def_count - 1; i++) {
        map->tag_defs[i] = map->tag_defs[i + 1];
    }
    map->tag_def_count--;

    for (int i = 0; i < map->tag_def_count; i++) {
        pz_tag_def *def = &map->tag_defs[i];
        if (def->type == PZ_TAG_POWERUP
            && strcmp(def->data.powerup.barrier_tag, removed_name) == 0) {
            def->data.powerup.barrier_tag[0] = '\0';
        }
    }

    return true;
}

int
pz_map_add_tag_placement(pz_map *map, int tag_index, int tile_x, int tile_y)
{
    if (!map || tag_index < 0 || tag_index >= map->tag_def_count) {
        return -1;
    }
    if (map->tag_placement_count >= PZ_MAP_MAX_TAG_PLACEMENTS) {
        return -1;
    }

    int idx = map->tag_placement_count++;
    map->tag_placements[idx] = (pz_tag_placement) {
        .tag_index = tag_index,
        .tile_x = tile_x,
        .tile_y = tile_y,
    };
    return idx;
}

bool
pz_map_remove_tag_placement(pz_map *map, int placement_index)
{
    if (!map || placement_index < 0
        || placement_index >= map->tag_placement_count) {
        return false;
    }

    for (int i = placement_index; i < map->tag_placement_count - 1; i++) {
        map->tag_placements[i] = map->tag_placements[i + 1];
    }
    map->tag_placement_count--;
    return true;
}

int
pz_map_find_tag_placement(
    const pz_map *map, int tile_x, int tile_y, int tag_index)
{
    if (!map) {
        return -1;
    }

    for (int i = 0; i < map->tag_placement_count; i++) {
        const pz_tag_placement *placement = &map->tag_placements[i];
        if (placement->tile_x != tile_x || placement->tile_y != tile_y) {
            continue;
        }
        if (tag_index >= 0 && placement->tag_index != tag_index) {
            continue;
        }
        return i;
    }

    return -1;
}

int
pz_map_count_tag_placements(const pz_map *map, int tag_index)
{
    if (!map) {
        return 0;
    }

    int count = 0;
    for (int i = 0; i < map->tag_placement_count; i++) {
        if (map->tag_placements[i].tag_index == tag_index) {
            count++;
        }
    }
    return count;
}

// ============================================================================
// Collision and Movement
// ============================================================================

bool
pz_map_is_solid(const pz_map *map, pz_vec2 world_pos)
{
    int tx, ty;
    pz_map_world_to_tile(map, world_pos, &tx, &ty);

    if (!pz_map_in_bounds(map, tx, ty)) {
        return true; // Out of bounds is solid
    }

    int8_t height = pz_map_get_height(map, tx, ty);

    // Height > 0 = wall, height < 0 = pit (both block movement)
    return height != 0;
}

bool
pz_map_is_passable(const pz_map *map, pz_vec2 world_pos)
{
    return !pz_map_is_solid(map, world_pos);
}

bool
pz_map_blocks_bullets(const pz_map *map, pz_vec2 world_pos)
{
    int tx, ty;
    pz_map_world_to_tile(map, world_pos, &tx, &ty);

    if (!pz_map_in_bounds(map, tx, ty)) {
        return true;
    }

    int8_t height = pz_map_get_height(map, tx, ty);

    // Only walls (height > 0) block bullets
    // Pits (height < 0) don't block bullets - they fly over
    return height > 0;
}

float
pz_map_get_speed_multiplier(const pz_map *map, pz_vec2 world_pos)
{
    int tx, ty;
    pz_map_world_to_tile(map, world_pos, &tx, &ty);

    if (!pz_map_in_bounds(map, tx, ty)) {
        return 0.0f;
    }

    // Note: We don't check height here because multi-floor gameplay allows
    // tanks to move on any floor level. The collision system handles blocking
    // movement into tiles at the wrong height.

    // Get tile definition and look up properties from registry
    const pz_tile_def *def = pz_map_get_tile_def(map, tx, ty);
    if (def && map->tile_registry) {
        const pz_tile_config *config
            = pz_tile_registry_get(map->tile_registry, def->name);
        if (config) {
            return config->speed_multiplier;
        }
    }

    return 1.0f;
}

float
pz_map_get_friction(const pz_map *map, pz_vec2 world_pos)
{
    int tx, ty;
    pz_map_world_to_tile(map, world_pos, &tx, &ty);

    if (!pz_map_in_bounds(map, tx, ty)) {
        return 1.0f;
    }

    // Note: We don't check height here because multi-floor gameplay allows
    // tanks to move on any floor level.

    // Get tile definition and look up properties from registry
    const pz_tile_def *def = pz_map_get_tile_def(map, tx, ty);
    if (def && map->tile_registry) {
        const pz_tile_config *config
            = pz_tile_registry_get(map->tile_registry, def->name);
        if (config) {
            return config->friction;
        }
    }

    return 1.0f;
}

// ============================================================================
// Coordinate Conversion
// ============================================================================

pz_vec2
pz_map_tile_to_world(const pz_map *map, int tile_x, int tile_y)
{
    // Center of the map is at world origin (0,0)
    // Tile (0,0) is at top-left in world space
    float half_w = map->world_width / 2.0f;
    float half_h = map->world_height / 2.0f;

    pz_vec2 result = {
        .x = tile_x * map->tile_size + map->tile_size / 2.0f - half_w,
        .y = tile_y * map->tile_size + map->tile_size / 2.0f - half_h,
    };

    return result;
}

void
pz_map_world_to_tile(
    const pz_map *map, pz_vec2 world_pos, int *tile_x, int *tile_y)
{
    float half_w = map->world_width / 2.0f;
    float half_h = map->world_height / 2.0f;

    *tile_x = (int)((world_pos.x + half_w) / map->tile_size);
    *tile_y = (int)((world_pos.y + half_h) / map->tile_size);
}

bool
pz_map_in_bounds(const pz_map *map, int tile_x, int tile_y)
{
    return tile_x >= 0 && tile_x < map->width && tile_y >= 0
        && tile_y < map->height;
}

bool
pz_map_in_bounds_world(const pz_map *map, pz_vec2 world_pos)
{
    float half_w = map->world_width / 2.0f;
    float half_h = map->world_height / 2.0f;

    return world_pos.x >= -half_w && world_pos.x < half_w
        && world_pos.y >= -half_h && world_pos.y < half_h;
}

bool
pz_map_get_jump_pad_target(
    const pz_map *map, int tile_x, int tile_y, int *out_x, int *out_y)
{
    if (!map) {
        return false;
    }

    for (int i = 0; i < map->jump_pad_count; i++) {
        const struct pz_jump_pad_link *link = &map->jump_pads[i];
        if (link->start_x == tile_x && link->start_y == tile_y) {
            if (out_x)
                *out_x = link->landing_x;
            if (out_y)
                *out_y = link->landing_y;
            return true;
        }
        if (link->landing_x == tile_x && link->landing_y == tile_y) {
            if (out_x)
                *out_x = link->start_x;
            if (out_y)
                *out_y = link->start_y;
            return true;
        }
    }

    return false;
}

bool
pz_map_get_jump_pad_target_world(
    const pz_map *map, pz_vec2 world_pos, pz_vec2 *out_target_world)
{
    if (!map || !out_target_world) {
        return false;
    }

    int tx, ty;
    pz_map_world_to_tile(map, world_pos, &tx, &ty);
    int target_x = 0, target_y = 0;
    if (!pz_map_get_jump_pad_target(map, tx, ty, &target_x, &target_y)) {
        return false;
    }

    *out_target_world = pz_map_tile_to_world(map, target_x, target_y);
    return true;
}

// ============================================================================
// Spawn/Enemy Helpers
// ============================================================================

const pz_spawn_point *
pz_map_get_spawn(const pz_map *map, int index)
{
    if (!map || index < 0 || index >= map->spawn_count) {
        return NULL;
    }
    return &map->spawns[index];
}

int
pz_map_get_spawn_count(const pz_map *map)
{
    return map ? map->spawn_count : 0;
}

const pz_enemy_spawn *
pz_map_get_enemy(const pz_map *map, int index)
{
    if (!map || index < 0 || index >= map->enemy_count) {
        return NULL;
    }
    return &map->enemies[index];
}

int
pz_map_get_enemy_count(const pz_map *map)
{
    return map ? map->enemy_count : 0;
}

const pz_powerup_spawn *
pz_map_get_powerup(const pz_map *map, int index)
{
    if (!map || index < 0 || index >= map->powerup_count) {
        return NULL;
    }
    return &map->powerups[index];
}

int
pz_map_get_powerup_count(const pz_map *map)
{
    return map ? map->powerup_count : 0;
}

const pz_barrier_spawn *
pz_map_get_barrier(const pz_map *map, int index)
{
    if (!map || index < 0 || index >= map->barrier_count) {
        return NULL;
    }
    return &map->barriers[index];
}

int
pz_map_get_barrier_count(const pz_map *map)
{
    return map ? map->barrier_count : 0;
}

const pz_map_lighting *
pz_map_get_lighting(const pz_map *map)
{
    return map ? &map->lighting : NULL;
}

const pz_map_background *
pz_map_get_background(const pz_map *map)
{
    return map ? &map->background : NULL;
}

// ============================================================================
// Debug
// ============================================================================

void
pz_map_print(const pz_map *map)
{
    if (!map) {
        return;
    }

    printf("Map: %s (%dx%d, tile_size=%.1f, version=%d)\n", map->name,
        map->width, map->height, map->tile_size, map->version);
    printf("World size: %.1f x %.1f\n", map->world_width, map->world_height);
    printf("Tile definitions: %d\n", map->tile_def_count);
    for (int i = 0; i < map->tile_def_count; i++) {
        printf(
            "  '%c' = %s\n", map->tile_defs[i].symbol, map->tile_defs[i].name);
    }
    printf("Spawns: %d, Enemies: %d\n", map->spawn_count, map->enemy_count);

    printf("\nGrid (height + tile):\n");
    for (int y = 0; y < map->height; y++) {
        printf("  ");
        for (int x = 0; x < map->width; x++) {
            pz_map_cell cell = pz_map_get_cell(map, x, y);
            const pz_tile_def *def
                = pz_map_get_tile_def_by_index(map, cell.tile_index);
            char symbol = def ? def->symbol : '?';
            printf("%2d%c ", cell.height, symbol);
        }
        printf("\n");
    }
}

// ============================================================================
// Raycast
// ============================================================================

pz_vec2
pz_map_raycast(const pz_map *map, pz_vec2 start, pz_vec2 direction,
    float max_dist, bool *hit)
{
    if (hit)
        *hit = false;

    if (!map || max_dist <= 0.0f)
        return start;

    // Normalize direction
    float dir_len = pz_vec2_len(direction);
    if (dir_len < 0.0001f)
        return start;
    direction = pz_vec2_scale(direction, 1.0f / dir_len);

    pz_vec2 end = pz_vec2_add(start, pz_vec2_scale(direction, max_dist));
    pz_raycast_result result = pz_map_raycast_ex(map, start, end);
    if (result.hit) {
        if (hit)
            *hit = true;
        return result.point;
    }

    return end;
}

pz_raycast_result
pz_map_raycast_ex(const pz_map *map, pz_vec2 start, pz_vec2 end)
{
    pz_raycast_result result = { 0 };
    result.point = end;
    result.normal = (pz_vec2) { 0, 0 };
    result.hit = false;
    result.distance = pz_vec2_len(pz_vec2_sub(end, start));

    if (!map)
        return result;

    pz_vec2 delta = pz_vec2_sub(end, start);
    float total_dist = pz_vec2_len(delta);
    if (total_dist < 0.0001f)
        return result;

    // Check if start is already in a wall (shouldn't happen, but handle it)
    if (pz_map_blocks_bullets(map, start)) {
        result.hit = true;
        result.point = start;
        result.distance = 0;
        // Try to determine normal from direction
        pz_vec2 dir = pz_vec2_normalize(delta);
        if (fabsf(dir.x) > fabsf(dir.y)) {
            result.normal = (pz_vec2) { dir.x > 0 ? -1.0f : 1.0f, 0.0f };
        } else {
            result.normal = (pz_vec2) { 0.0f, dir.y > 0 ? -1.0f : 1.0f };
        }
        return result;
    }

    // DDA (Digital Differential Analyzer) algorithm
    // Step through grid cells along the ray

    float half_w = map->world_width / 2.0f;
    float half_h = map->world_height / 2.0f;
    float ts = map->tile_size;

    // Convert to grid-relative coordinates (0,0 at bottom-left of map)
    float rx = start.x + half_w;
    float ry = start.y + half_h;

    // Direction
    pz_vec2 dir = pz_vec2_normalize(delta);

    // Current tile
    int tile_x = (int)(rx / ts);
    int tile_y = (int)(ry / ts);

    // Step direction (+1 or -1)
    int step_x = (dir.x >= 0) ? 1 : -1;
    int step_y = (dir.y >= 0) ? 1 : -1;

    // Distance to next tile boundary (in terms of t along ray)
    // t_max_x = distance along ray to next vertical grid line
    // t_max_y = distance along ray to next horizontal grid line
    float t_max_x, t_max_y;
    float t_delta_x, t_delta_y;

    if (fabsf(dir.x) < 0.0001f) {
        t_max_x = 1e30f;
        t_delta_x = 1e30f;
    } else {
        float next_x = (step_x > 0) ? (tile_x + 1) * ts : tile_x * ts;
        t_max_x = (next_x - rx) / dir.x;
        t_delta_x = ts / fabsf(dir.x);
    }

    if (fabsf(dir.y) < 0.0001f) {
        t_max_y = 1e30f;
        t_delta_y = 1e30f;
    } else {
        float next_y = (step_y > 0) ? (tile_y + 1) * ts : tile_y * ts;
        t_max_y = (next_y - ry) / dir.y;
        t_delta_y = ts / fabsf(dir.y);
    }

    // Track which edge we crossed to enter each cell
    // 0 = horizontal edge (normal is Y), 1 = vertical edge (normal is X)
    int last_step = -1;

    // Maximum iterations to prevent infinite loops
    int max_iters = (int)(total_dist / ts) + map->width + map->height + 10;

    const float corner_epsilon = 0.00001f;

    for (int i = 0; i < max_iters; i++) {
        // Check if we've traveled past the end point
        float current_t = (t_max_x < t_max_y) ? (t_max_x - t_delta_x)
                                              : (t_max_y - t_delta_y);
        if (current_t > total_dist) {
            // No hit within range
            return result;
        }

        if (fabsf(t_max_x - t_max_y) < corner_epsilon) {
            int next_x = tile_x + step_x;
            int next_y = tile_y + step_y;

            bool hit_x = false;
            if (!pz_map_in_bounds(map, next_x, tile_y)) {
                hit_x = true;
            } else if (pz_map_get_height(map, next_x, tile_y) > 0) {
                hit_x = true;
            }

            bool hit_y = false;
            if (!pz_map_in_bounds(map, tile_x, next_y)) {
                hit_y = true;
            } else if (pz_map_get_height(map, tile_x, next_y) > 0) {
                hit_y = true;
            }

            if (hit_x || hit_y) {
                float hit_t = t_max_x;
                if (hit_t < 0)
                    hit_t = 0;
                if (hit_t > total_dist)
                    hit_t = total_dist;

                result.hit = true;
                result.distance = hit_t;
                result.point = pz_vec2_add(start, pz_vec2_scale(dir, hit_t));

                if (hit_x && !hit_y) {
                    result.normal
                        = (pz_vec2) { (step_x > 0) ? -1.0f : 1.0f, 0.0f };
                } else if (hit_y && !hit_x) {
                    result.normal
                        = (pz_vec2) { 0.0f, (step_y > 0) ? -1.0f : 1.0f };
                } else {
                    if (fabsf(dir.x) >= fabsf(dir.y)) {
                        result.normal
                            = (pz_vec2) { (step_x > 0) ? -1.0f : 1.0f, 0.0f };
                    } else {
                        result.normal
                            = (pz_vec2) { 0.0f, (step_y > 0) ? -1.0f : 1.0f };
                    }
                }

                return result;
            }
        }

        // Check if current tile is solid
        if (pz_map_in_bounds(map, tile_x, tile_y)) {
            if (pz_map_get_height(map, tile_x, tile_y) > 0) {
                // Hit a wall! Calculate exact intersection point
                result.hit = true;

                // The intersection is at the edge we just crossed
                float hit_t;
                if (last_step == 0) {
                    // Crossed horizontal edge (Y boundary)
                    float edge_y
                        = (step_y > 0) ? tile_y * ts : (tile_y + 1) * ts;
                    hit_t = (edge_y - ry) / dir.y;
                    result.normal
                        = (pz_vec2) { 0.0f, (step_y > 0) ? -1.0f : 1.0f };
                } else if (last_step == 1) {
                    // Crossed vertical edge (X boundary)
                    float edge_x
                        = (step_x > 0) ? tile_x * ts : (tile_x + 1) * ts;
                    hit_t = (edge_x - rx) / dir.x;
                    result.normal
                        = (pz_vec2) { (step_x > 0) ? -1.0f : 1.0f, 0.0f };
                } else {
                    // First tile (shouldn't be solid, but handle it)
                    hit_t = 0;
                    result.normal = pz_vec2_scale(dir, -1.0f);
                }

                // Clamp hit_t to valid range
                if (hit_t < 0)
                    hit_t = 0;
                if (hit_t > total_dist)
                    hit_t = total_dist;

                result.distance = hit_t;
                result.point = pz_vec2_add(start, pz_vec2_scale(dir, hit_t));
                return result;
            }
        } else {
            // Out of bounds - treat as hit
            result.hit = true;
            float hit_t = (last_step == 0) ? (t_max_y - t_delta_y)
                                           : (t_max_x - t_delta_x);
            if (hit_t < 0)
                hit_t = 0;
            if (hit_t > total_dist)
                hit_t = total_dist;
            result.distance = hit_t;
            result.point = pz_vec2_add(start, pz_vec2_scale(dir, hit_t));
            // Normal points back toward the map
            if (tile_x < 0)
                result.normal = (pz_vec2) { 1.0f, 0.0f };
            else if (tile_x >= map->width)
                result.normal = (pz_vec2) { -1.0f, 0.0f };
            else if (tile_y < 0)
                result.normal = (pz_vec2) { 0.0f, 1.0f };
            else
                result.normal = (pz_vec2) { 0.0f, -1.0f };
            return result;
        }

        // Step to next tile
        if (t_max_x < t_max_y) {
            t_max_x += t_delta_x;
            tile_x += step_x;
            last_step = 1; // Crossed vertical edge
        } else {
            t_max_y += t_delta_y;
            tile_y += step_y;
            last_step = 0; // Crossed horizontal edge
        }
    }

    // Shouldn't reach here, but return no hit
    return result;
}

// ============================================================================
// Map Loading/Saving
// ============================================================================

// Skip whitespace
static const char *
skip_whitespace(const char *s)
{
    while (*s && isspace((unsigned char)*s)) {
        s++;
    }
    return s;
}

// Read a line from text buffer, advance pointer
static const char *
read_line(const char **text, char *buf, size_t buf_size)
{
    const char *p = *text;
    if (!p || !*p) {
        return NULL;
    }

    size_t i = 0;
    while (*p && *p != '\n' && *p != '\r' && i < buf_size - 1) {
        buf[i++] = *p++;
    }
    buf[i] = '\0';

    while (*p && (*p == '\n' || *p == '\r')) {
        p++;
    }

    *text = p;
    return buf;
}

// Parse a single cell from string: "[-]<height><tile>[|tags]"
// Returns pointer past the parsed cell, or NULL on error
static const char *
parse_cell(const char *p, int8_t *out_height, char *out_tile, char *out_tags,
    size_t tags_size)
{
    *out_height = 0;
    *out_tile = '.';
    if (out_tags)
        out_tags[0] = '\0';

    // Skip leading whitespace
    while (*p && isspace((unsigned char)*p)) {
        p++;
    }

    if (!*p)
        return NULL;

    // Parse height (can be negative)
    bool negative = false;
    if (*p == '-') {
        negative = true;
        p++;
    }

    int height = 0;
    while (*p && isdigit((unsigned char)*p)) {
        height = height * 10 + (*p - '0');
        p++;
    }
    if (negative) {
        height = -height;
    }
    *out_height = (int8_t)height;

    // Parse tile symbol (single char)
    if (!*p || isspace((unsigned char)*p)) {
        return NULL; // Missing tile symbol
    }
    *out_tile = *p++;

    // Parse optional tags after |
    if (*p == '|') {
        p++; // skip |
        size_t i = 0;
        while (*p && !isspace((unsigned char)*p) && i < tags_size - 1) {
            out_tags[i++] = *p++;
        }
        out_tags[i] = '\0';
    }

    return p;
}

// Parse spawn tag: "spawn angle=<f> team=<i>"
static bool
parse_spawn_tag(const char *params, pz_spawn_point *spawn)
{
    spawn->angle = 0.0f;
    spawn->team = 0;
    spawn->team_spawn = false;

    // Parse key=value pairs
    const char *p = params;
    while (*p) {
        while (*p && (isspace((unsigned char)*p) || *p == ',')) {
            p++;
        }
        if (!*p)
            break;

        if (strncmp(p, "angle=", 6) == 0) {
            spawn->angle = (float)atof(p + 6);
        } else if (strncmp(p, "team=", 5) == 0) {
            spawn->team = atoi(p + 5);
        } else if (strncmp(p, "team_spawn=", 11) == 0) {
            spawn->team_spawn = atoi(p + 11) != 0;
        }

        // Skip to next param
        while (*p && !isspace((unsigned char)*p) && *p != ',') {
            p++;
        }
    }
    return true;
}

static int
parse_enemy_type_value(const char *value)
{
    if (!value || !*value) {
        return 1;
    }

    if (isdigit((unsigned char)value[0]) || value[0] == '-') {
        return atoi(value);
    }

    if (pz_str_casecmp(value, "sentry") == 0) {
        return 1;
    }
    if (pz_str_casecmp(value, "skirmisher") == 0) {
        return 2;
    }
    if (pz_str_casecmp(value, "hunter") == 0) {
        return 3;
    }
    if (pz_str_casecmp(value, "sniper") == 0) {
        return 4;
    }

    return 1;
}

static const char *
enemy_type_name(int type)
{
    switch (type) {
    case 1:
        return "sentry";
    case 2:
        return "skirmisher";
    case 3:
        return "hunter";
    case 4:
        return "sniper";
    default:
        return "sentry";
    }
}

// Parse enemy tag: "enemy angle=<f> level=<i>" or "enemy angle=<f> type=<name>"
static bool
parse_enemy_tag(const char *params, pz_enemy_spawn *enemy)
{
    enemy->angle = 0.0f;
    enemy->type = 1;

    const char *p = params;
    while (*p) {
        while (*p && (isspace((unsigned char)*p) || *p == ',')) {
            p++;
        }
        if (!*p)
            break;

        if (strncmp(p, "angle=", 6) == 0) {
            enemy->angle = (float)atof(p + 6);
        } else if (strncmp(p, "level=", 6) == 0) {
            const char *start = p + 6;
            const char *end = start;
            while (*end && !isspace((unsigned char)*end) && *end != ',') {
                end++;
            }
            char value[32];
            size_t len = (size_t)(end - start);
            if (len >= sizeof(value)) {
                len = sizeof(value) - 1;
            }
            strncpy(value, start, len);
            value[len] = '\0';
            enemy->type = parse_enemy_type_value(value);
            p = end;
            continue;
        } else if (strncmp(p, "type=", 5) == 0) {
            const char *start = p + 5;
            const char *end = start;
            while (*end && !isspace((unsigned char)*end) && *end != ',') {
                end++;
            }
            char value[32];
            size_t len = (size_t)(end - start);
            if (len >= sizeof(value)) {
                len = sizeof(value) - 1;
            }
            strncpy(value, start, len);
            value[len] = '\0';
            enemy->type = parse_enemy_type_value(value);
            p = end;
            continue;
        }

        while (*p && !isspace((unsigned char)*p) && *p != ',') {
            p++;
        }
    }
    return true;
}

// Parse powerup tag: "powerup type=<name> respawn=<f> barrier=<tag>
// barrier_count=<n>" For barrier_placer type, barrier and barrier_count specify
// the barrier template
static bool
parse_powerup_tag(const char *params, pz_powerup_spawn *powerup,
    char *barrier_tag_out, size_t barrier_tag_size)
{
    powerup->type_name[0] = '\0';
    powerup->respawn_time = 15.0f; // Default respawn time
    powerup->barrier_tile[0] = '\0';
    powerup->barrier_health = 20.0f;
    powerup->barrier_count = 2; // Default barrier count
    powerup->barrier_lifetime = 0.0f; // Default: no lifetime (infinite)

    if (barrier_tag_out && barrier_tag_size > 0) {
        barrier_tag_out[0] = '\0';
    }

    const char *p = params;
    while (*p) {
        while (*p && (isspace((unsigned char)*p) || *p == ',')) {
            p++;
        }
        if (!*p)
            break;

        if (strncmp(p, "type=", 5) == 0) {
            // Extract type name
            const char *start = p + 5;
            const char *end = start;
            while (*end && !isspace((unsigned char)*end) && *end != ',') {
                end++;
            }
            size_t len = end - start;
            if (len >= sizeof(powerup->type_name)) {
                len = sizeof(powerup->type_name) - 1;
            }
            strncpy(powerup->type_name, start, len);
            powerup->type_name[len] = '\0';
            p = end;
            continue;
        } else if (strncmp(p, "respawn=", 8) == 0) {
            powerup->respawn_time = (float)atof(p + 8);
        } else if (strncmp(p, "barrier=", 8) == 0) {
            // Extract barrier tag name (to be resolved later)
            const char *start = p + 8;
            const char *end = start;
            while (*end && !isspace((unsigned char)*end) && *end != ',') {
                end++;
            }
            if (barrier_tag_out && barrier_tag_size > 0) {
                size_t len = end - start;
                if (len >= barrier_tag_size) {
                    len = barrier_tag_size - 1;
                }
                strncpy(barrier_tag_out, start, len);
                barrier_tag_out[len] = '\0';
            }
            p = end;
            continue;
        } else if (strncmp(p, "barrier_count=", 14) == 0) {
            powerup->barrier_count = atoi(p + 14);
            if (powerup->barrier_count < 1)
                powerup->barrier_count = 1;
            if (powerup->barrier_count > 8)
                powerup->barrier_count = 8;
        } else if (strncmp(p, "barrier_lifetime=", 17) == 0) {
            powerup->barrier_lifetime = (float)atof(p + 17);
            if (powerup->barrier_lifetime < 0.0f)
                powerup->barrier_lifetime = 0.0f;
        }

        while (*p && !isspace((unsigned char)*p) && *p != ',') {
            p++;
        }
    }
    return powerup->type_name[0] != '\0';
}

// Parse barrier tag: "barrier tile=<name> health=<f>"
static bool
parse_barrier_tag(const char *params, pz_barrier_spawn *barrier)
{
    barrier->tile_name[0] = '\0';
    barrier->health = 20.0f; // Default health

    const char *p = params;
    while (*p) {
        while (*p && (isspace((unsigned char)*p) || *p == ',')) {
            p++;
        }
        if (!*p)
            break;

        if (strncmp(p, "tile=", 5) == 0) {
            // Extract tile name
            const char *start = p + 5;
            const char *end = start;
            while (*end && !isspace((unsigned char)*end) && *end != ',') {
                end++;
            }
            size_t len = end - start;
            if (len >= sizeof(barrier->tile_name)) {
                len = sizeof(barrier->tile_name) - 1;
            }
            strncpy(barrier->tile_name, start, len);
            barrier->tile_name[len] = '\0';
            p = end;
            continue;
        } else if (strncmp(p, "health=", 7) == 0) {
            barrier->health = (float)atof(p + 7);
        }

        while (*p && !isspace((unsigned char)*p) && *p != ',') {
            p++;
        }
    }
    return barrier->tile_name[0] != '\0';
}

static bool
tag_type_from_string(const char *type, pz_tag_type *out_type)
{
    if (!type || !out_type) {
        return false;
    }

    if (pz_str_casecmp(type, "spawn") == 0) {
        *out_type = PZ_TAG_SPAWN;
        return true;
    }
    if (pz_str_casecmp(type, "enemy") == 0) {
        *out_type = PZ_TAG_ENEMY;
        return true;
    }
    if (pz_str_casecmp(type, "powerup") == 0) {
        *out_type = PZ_TAG_POWERUP;
        return true;
    }
    if (pz_str_casecmp(type, "barrier") == 0) {
        *out_type = PZ_TAG_BARRIER;
        return true;
    }
    if (pz_str_casecmp(type, "jump_pad") == 0) {
        *out_type = PZ_TAG_JUMP_PAD;
        return true;
    }

    return false;
}

static const char *
tag_type_name(pz_tag_type type)
{
    switch (type) {
    case PZ_TAG_SPAWN:
        return "spawn";
    case PZ_TAG_ENEMY:
        return "enemy";
    case PZ_TAG_POWERUP:
        return "powerup";
    case PZ_TAG_BARRIER:
        return "barrier";
    case PZ_TAG_JUMP_PAD:
        return "jump_pad";
    default:
        return "spawn";
    }
}

static void
tag_def_defaults(pz_tag_def *def, pz_tag_type type)
{
    if (!def) {
        return;
    }

    memset(def, 0, sizeof(*def));
    def->type = type;

    switch (type) {
    case PZ_TAG_SPAWN:
        def->data.spawn.angle = 0.0f;
        def->data.spawn.team = 0;
        def->data.spawn.team_spawn = false;
        break;
    case PZ_TAG_ENEMY:
        def->data.enemy.angle = 0.0f;
        def->data.enemy.type = 1;
        break;
    case PZ_TAG_POWERUP:
        strncpy(def->data.powerup.type_name, "machine_gun",
            sizeof(def->data.powerup.type_name) - 1);
        def->data.powerup.type_name[sizeof(def->data.powerup.type_name) - 1]
            = '\0';
        def->data.powerup.respawn_time = 15.0f;
        def->data.powerup.barrier_tag[0] = '\0';
        def->data.powerup.barrier_count = 2;
        def->data.powerup.barrier_lifetime = 0.0f;
        break;
    case PZ_TAG_BARRIER:
        def->data.barrier.tile_name[0] = '\0';
        def->data.barrier.health = 20.0f;
        break;
    case PZ_TAG_JUMP_PAD:
        def->data.jump_pad.id[0] = '\0';
        def->data.jump_pad.role = PZ_JUMP_PAD_START;
        break;
    default:
        break;
    }
}

static bool
parse_tag_definition(const char *name, const char *type_str, const char *params,
    pz_tag_def *out_def)
{
    if (!name || !name[0] || !type_str || !out_def) {
        return false;
    }

    pz_tag_type type;
    if (!tag_type_from_string(type_str, &type)) {
        return false;
    }

    tag_def_defaults(out_def, type);
    strncpy(out_def->name, name, sizeof(out_def->name) - 1);
    out_def->name[sizeof(out_def->name) - 1] = '\0';

    if (!params || !params[0]) {
        return true;
    }

    if (type == PZ_TAG_SPAWN) {
        pz_spawn_point temp = { 0 };
        parse_spawn_tag(params, &temp);
        out_def->data.spawn.angle = temp.angle;
        out_def->data.spawn.team = temp.team;
        out_def->data.spawn.team_spawn = temp.team_spawn;
    } else if (type == PZ_TAG_ENEMY) {
        pz_enemy_spawn temp = { 0 };
        parse_enemy_tag(params, &temp);
        out_def->data.enemy.angle = temp.angle;
        out_def->data.enemy.type = temp.type;
    } else if (type == PZ_TAG_POWERUP) {
        pz_powerup_spawn temp = { 0 };
        char barrier_tag[32] = { 0 };
        parse_powerup_tag(params, &temp, barrier_tag, sizeof(barrier_tag));
        if (temp.type_name[0] != '\0') {
            strncpy(out_def->data.powerup.type_name, temp.type_name,
                sizeof(out_def->data.powerup.type_name) - 1);
            out_def->data.powerup
                .type_name[sizeof(out_def->data.powerup.type_name) - 1]
                = '\0';
        }
        out_def->data.powerup.respawn_time = temp.respawn_time;
        out_def->data.powerup.barrier_count = temp.barrier_count;
        out_def->data.powerup.barrier_lifetime = temp.barrier_lifetime;
        if (barrier_tag[0]) {
            strncpy(out_def->data.powerup.barrier_tag, barrier_tag,
                sizeof(out_def->data.powerup.barrier_tag) - 1);
            out_def->data.powerup
                .barrier_tag[sizeof(out_def->data.powerup.barrier_tag) - 1]
                = '\0';
        }
    } else if (type == PZ_TAG_BARRIER) {
        pz_barrier_spawn temp = { 0 };
        parse_barrier_tag(params, &temp);
        if (temp.tile_name[0]) {
            strncpy(out_def->data.barrier.tile_name, temp.tile_name,
                sizeof(out_def->data.barrier.tile_name) - 1);
            out_def->data.barrier
                .tile_name[sizeof(out_def->data.barrier.tile_name) - 1]
                = '\0';
        }
        out_def->data.barrier.health = temp.health;
    } else if (type == PZ_TAG_JUMP_PAD) {
        // Params: id=<id> role=start|landing
        const char *p = params;
        while (p && *p) {
            while (*p && (isspace((unsigned char)*p) || *p == ',')) {
                p++;
            }
            if (!*p)
                break;

            if (strncmp(p, "id=", 3) == 0) {
                const char *start = p + 3;
                const char *end = start;
                while (*end && !isspace((unsigned char)*end) && *end != ',') {
                    end++;
                }
                size_t len = (size_t)(end - start);
                if (len >= sizeof(out_def->data.jump_pad.id)) {
                    len = sizeof(out_def->data.jump_pad.id) - 1;
                }
                strncpy(out_def->data.jump_pad.id, start, len);
                out_def->data.jump_pad.id[len] = '\0';
                p = end;
                continue;
            } else if (strncmp(p, "role=", 5) == 0) {
                const char *start = p + 5;
                const char *end = start;
                while (*end && !isspace((unsigned char)*end) && *end != ',') {
                    end++;
                }
                char value[16];
                size_t len = (size_t)(end - start);
                if (len >= sizeof(value)) {
                    len = sizeof(value) - 1;
                }
                strncpy(value, start, len);
                value[len] = '\0';
                if (pz_str_casecmp(value, "start") == 0) {
                    out_def->data.jump_pad.role = PZ_JUMP_PAD_START;
                } else if (pz_str_casecmp(value, "landing") == 0) {
                    out_def->data.jump_pad.role = PZ_JUMP_PAD_LANDING;
                }
                p = end;
                continue;
            }

            while (*p && !isspace((unsigned char)*p) && *p != ',') {
                p++;
            }
        }
    }

    return true;
}

// Pending tag placement (to resolve after grid is parsed)
typedef struct tag_placement_pending {
    char tag_name[32];
    int tile_x;
    int tile_y;
} tag_placement_pending;

// Count cells in a row string
static int
count_row_cells(const char *row)
{
    int count = 0;
    const char *p = row;

    while (*p) {
        // Skip whitespace
        while (*p && isspace((unsigned char)*p)) {
            p++;
        }
        if (!*p)
            break;

        // Parse height (can be negative)
        if (*p == '-') {
            p++;
        }
        while (*p && isdigit((unsigned char)*p)) {
            p++;
        }

        // Parse tile symbol
        if (!*p || isspace((unsigned char)*p)) {
            break; // Malformed
        }
        p++;

        // Skip optional tags
        if (*p == '|') {
            p++;
            while (*p && !isspace((unsigned char)*p)) {
                p++;
            }
        }

        count++;
    }

    return count;
}

void
pz_map_rebuild_spawns_from_tags(pz_map *map)
{
    if (!map) {
        return;
    }

    map->spawn_count = 0;
    map->enemy_count = 0;
    map->powerup_count = 0;
    map->barrier_count = 0;
    map->jump_pad_count = 0;

    // Temporary table keyed by jump pad id
    typedef struct jump_pad_accum {
        char id[32];
        bool has_start;
        int start_x, start_y;
        bool has_landing;
        int landing_x, landing_y;
    } jump_pad_accum;

    jump_pad_accum jump_accums[32];
    int jump_accum_count = 0;

    for (int i = 0; i < map->tag_placement_count; i++) {
        const pz_tag_placement *placement = &map->tag_placements[i];
        if (placement->tag_index < 0
            || placement->tag_index >= map->tag_def_count) {
            continue;
        }

        const pz_tag_def *def = &map->tag_defs[placement->tag_index];
        pz_vec2 pos
            = pz_map_tile_to_world(map, placement->tile_x, placement->tile_y);

        switch (def->type) {
        case PZ_TAG_SPAWN: {
            if (map->spawn_count >= PZ_MAP_MAX_SPAWNS) {
                break;
            }
            pz_spawn_point *sp = &map->spawns[map->spawn_count++];
            sp->pos = pos;
            sp->angle = def->data.spawn.angle;
            sp->team = def->data.spawn.team;
            sp->team_spawn = def->data.spawn.team_spawn;
        } break;
        case PZ_TAG_ENEMY: {
            if (map->enemy_count >= PZ_MAP_MAX_ENEMIES) {
                break;
            }
            pz_enemy_spawn *es = &map->enemies[map->enemy_count++];
            es->pos = pos;
            es->angle = def->data.enemy.angle;
            es->type = def->data.enemy.type;
        } break;
        case PZ_TAG_POWERUP: {
            if (map->powerup_count >= PZ_MAP_MAX_POWERUPS) {
                break;
            }
            pz_powerup_spawn *ps = &map->powerups[map->powerup_count++];
            memset(ps, 0, sizeof(*ps));
            ps->pos = pos;
            strncpy(ps->type_name, def->data.powerup.type_name,
                sizeof(ps->type_name) - 1);
            ps->type_name[sizeof(ps->type_name) - 1] = '\0';
            if (ps->type_name[0] == '\0') {
                strncpy(
                    ps->type_name, "machine_gun", sizeof(ps->type_name) - 1);
                ps->type_name[sizeof(ps->type_name) - 1] = '\0';
            }
            ps->respawn_time = def->data.powerup.respawn_time;
            ps->barrier_count = def->data.powerup.barrier_count;
            ps->barrier_lifetime = def->data.powerup.barrier_lifetime;

            if (strcmp(def->data.powerup.type_name, "barrier_placer") == 0
                && def->data.powerup.barrier_tag[0]) {
                int barrier_idx
                    = pz_map_find_tag_def(map, def->data.powerup.barrier_tag);
                if (barrier_idx >= 0
                    && map->tag_defs[barrier_idx].type == PZ_TAG_BARRIER) {
                    const pz_tag_def *barrier_def = &map->tag_defs[barrier_idx];
                    strncpy(ps->barrier_tile,
                        barrier_def->data.barrier.tile_name,
                        sizeof(ps->barrier_tile) - 1);
                    ps->barrier_tile[sizeof(ps->barrier_tile) - 1] = '\0';
                    ps->barrier_health = barrier_def->data.barrier.health;
                } else {
                    pz_log(PZ_LOG_WARN, PZ_LOG_CAT_GAME,
                        "Barrier placer references unknown barrier tag: %s",
                        def->data.powerup.barrier_tag);
                }
            }
        } break;
        case PZ_TAG_BARRIER: {
            if (map->barrier_count >= PZ_MAP_MAX_BARRIERS) {
                break;
            }
            pz_barrier_spawn *bs = &map->barriers[map->barrier_count++];
            bs->pos = pos;
            strncpy(bs->tile_name, def->data.barrier.tile_name,
                sizeof(bs->tile_name) - 1);
            bs->tile_name[sizeof(bs->tile_name) - 1] = '\0';
            bs->health = def->data.barrier.health;
            // Set floor level from the tile height where barrier is placed
            bs->floor_level
                = pz_map_get_height(map, placement->tile_x, placement->tile_y);
        } break;
        case PZ_TAG_JUMP_PAD: {
            // Accumulate start/landing tiles per id
            if (!def->data.jump_pad.id[0]) {
                break;
            }
            int idx = -1;
            for (int j = 0; j < jump_accum_count; j++) {
                if (strcmp(jump_accums[j].id, def->data.jump_pad.id) == 0) {
                    idx = j;
                    break;
                }
            }
            if (idx < 0
                && jump_accum_count
                    < (int)(sizeof(jump_accums) / sizeof(jump_accums[0]))) {
                idx = jump_accum_count++;
                memset(&jump_accums[idx], 0, sizeof(jump_accums[idx]));
                strncpy(jump_accums[idx].id, def->data.jump_pad.id,
                    sizeof(jump_accums[idx].id) - 1);
                jump_accums[idx].id[sizeof(jump_accums[idx].id) - 1] = '\0';
            }
            if (idx >= 0) {
                if (def->data.jump_pad.role == PZ_JUMP_PAD_START) {
                    jump_accums[idx].has_start = true;
                    jump_accums[idx].start_x = placement->tile_x;
                    jump_accums[idx].start_y = placement->tile_y;
                } else if (def->data.jump_pad.role == PZ_JUMP_PAD_LANDING) {
                    jump_accums[idx].has_landing = true;
                    jump_accums[idx].landing_x = placement->tile_x;
                    jump_accums[idx].landing_y = placement->tile_y;
                }
            }
        } break;
        default:
            break;
        }
    }

    // Build runtime jump pad links from accumulated ids
    for (int i = 0; i < jump_accum_count; i++) {
        const jump_pad_accum *acc = &jump_accums[i];
        if (!acc->has_start || !acc->has_landing) {
            pz_log(PZ_LOG_WARN, PZ_LOG_CAT_GAME,
                "Jump pad id '%s' has incomplete pair (start=%d, landing=%d)",
                acc->id, acc->has_start ? 1 : 0, acc->has_landing ? 1 : 0);
            continue;
        }
        if (map->jump_pad_count
            >= (int)(sizeof(map->jump_pads) / sizeof(map->jump_pads[0]))) {
            pz_log(PZ_LOG_WARN, PZ_LOG_CAT_GAME,
                "Too many jump pad pairs (max=%zu)",
                sizeof(map->jump_pads) / sizeof(map->jump_pads[0]));
            break;
        }
        map->jump_pads[map->jump_pad_count].start_x = acc->start_x;
        map->jump_pads[map->jump_pad_count].start_y = acc->start_y;
        map->jump_pads[map->jump_pad_count].landing_x = acc->landing_x;
        map->jump_pads[map->jump_pad_count].landing_y = acc->landing_y;
        map->jump_pad_count++;
    }
}

pz_map *
pz_map_load(const char *path)
{
    char *file_data = pz_file_read_text(path);
    if (!file_data) {
        pz_log(
            PZ_LOG_ERROR, PZ_LOG_CAT_GAME, "Failed to read map file: %s", path);
        return NULL;
    }

    pz_map *map = NULL;
    const char *text = file_data;
    char line[1024];

    char name[64] = "Unnamed";
    float tile_size = 2.0f;
    char music_name[64] = "";
    bool has_music = false;

    // Tag definitions
    pz_tag_def tag_defs[PZ_MAP_MAX_TAG_DEFS];
    int tag_def_count = 0;

    // Tag placements (resolved after grid)
    tag_placement_pending placements[PZ_MAP_MAX_TAG_PLACEMENTS];
    int placement_count = 0;

    // Temporary tile defs (before map creation)
    char tile_symbols[PZ_MAP_MAX_TILE_DEFS];
    char tile_names[PZ_MAP_MAX_TILE_DEFS][32];
    int tile_def_count = 0;

    // Grid rows stored temporarily until we know the size
    char grid_rows[PZ_MAP_MAX_SIZE][1024];
    int grid_row_count = 0;
    int grid_width = -1; // Will be set from first row

    // Post-grid directives stored for later parsing
    char post_grid_lines[256][256];
    int post_grid_count = 0;

    bool in_grid = false;

    // Parse file
    while (read_line(&text, line, sizeof(line))) {
        const char *p = skip_whitespace(line);

        // Handle grid section specially
        if (in_grid) {
            // Check for end of grid
            if (strcmp(p, "</grid>") == 0) {
                in_grid = false;
                continue;
            }

            // Skip empty/whitespace-only lines inside grid
            if (!*p) {
                continue;
            }

            // Store grid row
            if (grid_row_count >= PZ_MAP_MAX_SIZE) {
                pz_log(PZ_LOG_ERROR, PZ_LOG_CAT_GAME,
                    "Map too tall (max %d rows)", PZ_MAP_MAX_SIZE);
                pz_free(file_data);
                return NULL;
            }

            // Count cells in this row
            int row_cells = count_row_cells(p);

            if (grid_width < 0) {
                // First row sets the width
                grid_width = row_cells;
                if (grid_width <= 0 || grid_width > PZ_MAP_MAX_SIZE) {
                    pz_log(PZ_LOG_ERROR, PZ_LOG_CAT_GAME,
                        "Invalid grid width: %d", grid_width);
                    pz_free(file_data);
                    return NULL;
                }
            } else if (row_cells != grid_width) {
                // All rows must have same width
                pz_log(PZ_LOG_ERROR, PZ_LOG_CAT_GAME,
                    "Row %d has %d cells, expected %d (first row width)",
                    grid_row_count + 1, row_cells, grid_width);
                pz_free(file_data);
                return NULL;
            }

            strncpy(grid_rows[grid_row_count], p, sizeof(grid_rows[0]) - 1);
            grid_rows[grid_row_count][sizeof(grid_rows[0]) - 1] = '\0';
            grid_row_count++;
            continue;
        }

        // Outside grid section
        if (!*p || *p == '#') {
            continue;
        }

        if (strncmp(p, "name ", 5) == 0) {
            strncpy(name, p + 5, sizeof(name) - 1);
            name[sizeof(name) - 1] = '\0';
        } else if (strncmp(p, "tile_size ", 10) == 0) {
            tile_size = (float)atof(p + 10);
        } else if (strncmp(p, "music ", 6) == 0) {
            if (sscanf(p + 6, "%63s", music_name) == 1) {
                has_music = true;
            }
        } else if (strncmp(p, "tile ", 5) == 0) {
            // Parse: tile <symbol> <name>
            char sym;
            char tname[32];
            if (sscanf(p + 5, " %c %31s", &sym, tname) == 2) {
                if (tile_def_count < PZ_MAP_MAX_TILE_DEFS) {
                    tile_symbols[tile_def_count] = sym;
                    strncpy(tile_names[tile_def_count], tname, 31);
                    tile_def_count++;
                }
            }
        } else if (strncmp(p, "tag ", 4) == 0) {
            // Parse: tag <name> <type> <params...>
            char tname[32], ttype[16], tparams[128] = "";
            int n = sscanf(p + 4, "%31s %15s %127[^\n]", tname, ttype, tparams);
            if (n >= 2 && tag_def_count < PZ_MAP_MAX_TAG_DEFS) {
                if (parse_tag_definition(
                        tname, ttype, tparams, &tag_defs[tag_def_count])) {
                    tag_def_count++;
                } else {
                    pz_log(PZ_LOG_WARN, PZ_LOG_CAT_GAME,
                        "Unknown tag type '%s' for tag '%s'", ttype, tname);
                }
            }
        } else if (strcmp(p, "<grid>") == 0) {
            in_grid = true;
        } else {
            // Store for post-grid processing
            if (post_grid_count < 256) {
                strncpy(post_grid_lines[post_grid_count], p,
                    sizeof(post_grid_lines[0]) - 1);
                post_grid_lines[post_grid_count][sizeof(post_grid_lines[0]) - 1]
                    = '\0';
                post_grid_count++;
            }
        }
    }

    // Validate we got a grid
    if (grid_row_count <= 0 || grid_width <= 0) {
        pz_log(PZ_LOG_ERROR, PZ_LOG_CAT_GAME,
            "No valid grid found in map file: %s", path);
        pz_free(file_data);
        return NULL;
    }

    int width = grid_width;
    int height = grid_row_count;

    // Create the map
    map = pz_map_create(width, height, tile_size);
    if (!map) {
        pz_free(file_data);
        return NULL;
    }
    strncpy(map->name, name, sizeof(map->name) - 1);
    if (has_music) {
        strncpy(map->music_name, music_name, sizeof(map->music_name) - 1);
        map->music_name[sizeof(map->music_name) - 1] = '\0';
        map->has_music = true;
    }

    // Add tile definitions from file
    for (int i = 0; i < tile_def_count; i++) {
        pz_map_add_tile_def(map, tile_symbols[i], tile_names[i]);
    }

    // Add tag definitions from file
    for (int i = 0; i < tag_def_count; i++) {
        if (pz_map_add_tag_def(map, &tag_defs[i]) < 0) {
            pz_log(PZ_LOG_WARN, PZ_LOG_CAT_GAME,
                "Skipping tag def '%s' (limit reached?)", tag_defs[i].name);
        }
    }

    // Parse grid rows (file rows are top-down, y=0 is top)
    for (int row = 0; row < height; row++) {
        const char *p = grid_rows[row];
        int y = row;
        int x = 0;

        while (*p && x < width) {
            int8_t cell_height;
            char cell_tile;
            char cell_tags[64] = "";

            p = parse_cell(
                p, &cell_height, &cell_tile, cell_tags, sizeof(cell_tags));
            if (!p)
                break;

            // Find tile definition
            int tile_idx = pz_map_find_tile_def(map, cell_tile);
            if (tile_idx < 0) {
                pz_log(PZ_LOG_WARN, PZ_LOG_CAT_GAME,
                    "Unknown tile '%c' at (%d,%d), using ground", cell_tile, x,
                    y);
                tile_idx = 0;
            }

            // Set cell
            pz_map_cell cell
                = { .height = cell_height, .tile_index = (uint8_t)tile_idx };
            pz_map_set_cell(map, x, y, cell);

            // Record tag placements
            if (cell_tags[0]) {
                // Can have multiple tags: "P1,E1"
                char *tag = cell_tags;
                char *comma;
                while ((comma = strchr(tag, ',')) != NULL || tag[0] != '\0') {
                    char single_tag[32];
                    if (comma) {
                        size_t len = comma - tag;
                        if (len > 31)
                            len = 31;
                        strncpy(single_tag, tag, len);
                        single_tag[len] = '\0';
                        tag = comma + 1;
                    } else {
                        strncpy(single_tag, tag, 31);
                        single_tag[31] = '\0';
                        tag += strlen(tag); // end loop
                    }

                    if (single_tag[0]
                        && placement_count < PZ_MAP_MAX_TAG_PLACEMENTS) {
                        strncpy(placements[placement_count].tag_name,
                            single_tag, 31);
                        placements[placement_count].tile_x = x;
                        placements[placement_count].tile_y = y;
                        placement_count++;
                    }

                    if (!comma)
                        break;
                }
            }

            x++;
        }
    }

    // Resolve tag placements into map storage
    for (int i = 0; i < placement_count; i++) {
        int tag_index = pz_map_find_tag_def(map, placements[i].tag_name);
        if (tag_index < 0) {
            pz_log(PZ_LOG_WARN, PZ_LOG_CAT_GAME, "Unknown tag: %s",
                placements[i].tag_name);
            continue;
        }
        if (pz_map_add_tag_placement(
                map, tag_index, placements[i].tile_x, placements[i].tile_y)
            < 0) {
            pz_log(PZ_LOG_WARN, PZ_LOG_CAT_GAME,
                "Too many tag placements (max=%d)", PZ_MAP_MAX_TAG_PLACEMENTS);
            break;
        }
    }

    pz_map_rebuild_spawns_from_tags(map);

    // Process post-grid directives
    for (int i = 0; i < post_grid_count; i++) {
        const char *p = post_grid_lines[i];

        // Legacy spawn format: spawn x y angle team team_spawn
        if (strncmp(p, "spawn ", 6) == 0) {
            float sx, sy, angle;
            int team, team_spawn;
            if (sscanf(p + 6, "%f %f %f %d %d", &sx, &sy, &angle, &team,
                    &team_spawn)
                == 5) {
                if (map->spawn_count < PZ_MAP_MAX_SPAWNS) {
                    map->spawns[map->spawn_count++] = (pz_spawn_point) {
                        .pos = { sx, sy },
                        .angle = angle,
                        .team = team,
                        .team_spawn = team_spawn != 0,
                    };
                }
            }
        }
        // Legacy enemy format: enemy x y angle level|type
        else if (strncmp(p, "enemy ", 6) == 0) {
            float ex, ey, angle;
            char type_value[32];
            if (sscanf(p + 6, "%f %f %f %31s", &ex, &ey, &angle, type_value)
                == 4) {
                int type = parse_enemy_type_value(type_value);
                if (map->enemy_count < PZ_MAP_MAX_ENEMIES) {
                    map->enemies[map->enemy_count++] = (pz_enemy_spawn) {
                        .pos = { ex, ey },
                        .angle = angle,
                        .type = type,
                    };
                }
            }
        }
        // Lighting
        else if (strncmp(p, "sun_direction ", 14) == 0) {
            float x, y, z;
            if (sscanf(p + 14, "%f %f %f", &x, &y, &z) == 3) {
                map->lighting.sun_direction = (pz_vec3) { x, y, z };
                map->lighting.has_sun = true;
            }
        } else if (strncmp(p, "sun_color ", 10) == 0) {
            float r, g, b;
            if (sscanf(p + 10, "%f %f %f", &r, &g, &b) == 3) {
                map->lighting.sun_color = (pz_vec3) { r, g, b };
            }
        } else if (strncmp(p, "ambient_color ", 14) == 0) {
            float r, g, b;
            if (sscanf(p + 14, "%f %f %f", &r, &g, &b) == 3) {
                map->lighting.ambient_color = (pz_vec3) { r, g, b };
            }
        } else if (strncmp(p, "ambient_darkness ", 17) == 0) {
            map->lighting.ambient_darkness = (float)atof(p + 17);
        } else if (strncmp(p, "music ", 6) == 0) {
            if (sscanf(p + 6, "%63s", map->music_name) == 1) {
                map->has_music = true;
            }
        } else if (strncmp(p, "water_level ", 12) == 0) {
            map->water_level = atoi(p + 12);
            map->has_water = true;
        } else if (strncmp(p, "water_color ", 12) == 0) {
            float r, g, b;
            if (sscanf(p + 12, "%f %f %f", &r, &g, &b) == 3) {
                map->water_color = (pz_vec3) { r, g, b };
            }
        } else if (strncmp(p, "wave_strength ", 14) == 0) {
            map->wave_strength = (float)atof(p + 14);
        } else if (strncmp(p, "wind_direction ", 15) == 0) {
            map->wind_direction = (float)atof(p + 15);
        } else if (strncmp(p, "wind_strength ", 14) == 0) {
            map->wind_strength = (float)atof(p + 14);
        } else if (strncmp(p, "fog_level ", 10) == 0) {
            map->fog_level = atoi(p + 10);
            map->has_fog = true;
        } else if (strncmp(p, "fog_color ", 10) == 0) {
            float r, g, b;
            if (sscanf(p + 10, "%f %f %f", &r, &g, &b) == 3) {
                map->fog_color = (pz_vec3) { r, g, b };
            }
        }
        // Toxic cloud
        else if (strncmp(p, "toxic_cloud ", 12) == 0) {
            map->has_toxic_cloud = true;
            if (strncmp(p + 12, "enabled", 7) == 0) {
                map->toxic_config.enabled = true;
            } else if (strncmp(p + 12, "disabled", 8) == 0) {
                map->toxic_config.enabled = false;
            }
        } else if (strncmp(p, "toxic_delay ", 12) == 0) {
            map->has_toxic_cloud = true;
            map->toxic_config.delay = (float)atof(p + 12);
        } else if (strncmp(p, "toxic_duration ", 15) == 0) {
            map->has_toxic_cloud = true;
            map->toxic_config.duration = (float)atof(p + 15);
        } else if (strncmp(p, "toxic_safe_zone ", 16) == 0) {
            map->has_toxic_cloud = true;
            map->toxic_config.safe_zone_ratio = (float)atof(p + 16);
        } else if (strncmp(p, "toxic_damage ", 13) == 0) {
            map->has_toxic_cloud = true;
            map->toxic_config.damage = atoi(p + 13);
        } else if (strncmp(p, "toxic_interval ", 15) == 0) {
            map->has_toxic_cloud = true;
            map->toxic_config.damage_interval = (float)atof(p + 15);
        } else if (strncmp(p, "toxic_slowdown ", 15) == 0) {
            map->has_toxic_cloud = true;
            map->toxic_config.slowdown = (float)atof(p + 15);
        } else if (strncmp(p, "toxic_color ", 12) == 0) {
            float r, g, b;
            if (sscanf(p + 12, "%f %f %f", &r, &g, &b) == 3) {
                map->has_toxic_cloud = true;
                map->toxic_config.color = (pz_vec3) { r, g, b };
            }
        } else if (strncmp(p, "toxic_center ", 13) == 0) {
            float x, y;
            if (sscanf(p + 13, "%f %f", &x, &y) == 2) {
                map->has_toxic_cloud = true;
                map->toxic_config.center
                    = pz_map_tile_to_world_float(map, x, y);
            }
        }
        // Background settings
        else if (strncmp(p, "background_color ", 17) == 0) {
            float r, g, b;
            if (sscanf(p + 17, "%f %f %f", &r, &g, &b) == 3) {
                map->background.type = PZ_BACKGROUND_COLOR;
                map->background.color = (pz_vec3) { r, g, b };
            }
        } else if (strncmp(p, "background_gradient ", 20) == 0) {
            // Format: background_gradient <direction> r1 g1 b1 r2 g2 b2
            // direction: vertical or radial
            char dir[16];
            float r1, g1, b1, r2, g2, b2;
            if (sscanf(p + 20, "%15s %f %f %f %f %f %f", dir, &r1, &g1, &b1,
                    &r2, &g2, &b2)
                == 7) {
                map->background.type = PZ_BACKGROUND_GRADIENT;
                map->background.color = (pz_vec3) { r1, g1, b1 };
                map->background.color_end = (pz_vec3) { r2, g2, b2 };
                if (strcmp(dir, "radial") == 0) {
                    map->background.gradient_dir = PZ_GRADIENT_RADIAL;
                } else {
                    map->background.gradient_dir = PZ_GRADIENT_VERTICAL;
                }
            }
        } else if (strncmp(p, "background_texture ", 19) == 0) {
            // Future: background_texture <path>
            map->background.type = PZ_BACKGROUND_TEXTURE;
            sscanf(p + 19, "%63s", map->background.texture_path);
            pz_log(PZ_LOG_WARN, PZ_LOG_CAT_GAME,
                "Background textures not yet implemented: %s",
                map->background.texture_path);
        }
    }

    pz_free(file_data);

    pz_log(
        PZ_LOG_INFO, PZ_LOG_CAT_GAME, "Loaded map: %s (%s)", map->name, path);

    return map;
}

pz_map *
pz_map_load_with_registry(const char *path, const pz_tile_registry *registry)
{
    pz_map *map = pz_map_load(path);
    if (map && registry) {
        pz_map_set_tile_registry(map, registry);
    }
    return map;
}

static int
map_build_cell_tag_list(
    const pz_map *map, int x, int y, char *out, size_t out_size)
{
    if (!map || !out || out_size == 0) {
        return 0;
    }

    out[0] = '\0';
    int count = 0;

    for (int i = 0; i < map->tag_placement_count; i++) {
        const pz_tag_placement *placement = &map->tag_placements[i];
        if (placement->tile_x != x || placement->tile_y != y) {
            continue;
        }
        if (placement->tag_index < 0
            || placement->tag_index >= map->tag_def_count) {
            continue;
        }

        const char *name = map->tag_defs[placement->tag_index].name;
        size_t len = strlen(out);
        size_t name_len = strlen(name);
        size_t extra = name_len + (count > 0 ? 1 : 0) + 1;
        if (len + extra > out_size) {
            break;
        }
        if (count > 0) {
            out[len++] = ',';
            out[len] = '\0';
        }
        strncat(out, name, out_size - len - 1);
        count++;
    }

    return count;
}

bool
pz_map_save(const pz_map *map, const char *path)
{
    if (!map || !path) {
        return false;
    }

    // Calculate buffer size (generous to avoid realloc)
    size_t buf_size = 4096 + (size_t)(map->width * 40 + 8) * (size_t)map->height
        + (size_t)map->tile_def_count * 64 + (size_t)map->tag_def_count * 128;
    if (map->tag_def_count == 0) {
        buf_size += (size_t)(map->spawn_count + map->enemy_count) * 128;
    }
    char *buf = pz_alloc(buf_size);
    if (!buf) {
        return false;
    }

    char *p = buf;
    int remaining = (int)buf_size;
    int written;

    // Header
    written = snprintf(p, remaining,
        "# Tank Game Map\n"
        "name %s\n"
        "tile_size %.1f\n",
        map->name, map->tile_size);
    p += written;
    remaining -= written;

    if (map->has_music && map->music_name[0]) {
        written = snprintf(p, remaining, "music %s\n", map->music_name);
        p += written;
        remaining -= written;
    }

    written = snprintf(p, remaining, "\n");
    p += written;
    remaining -= written;

    // Tile definitions
    written = snprintf(p, remaining, "# Tile definitions\n");
    p += written;
    remaining -= written;

    for (int i = 0; i < map->tile_def_count; i++) {
        written = snprintf(p, remaining, "tile %c %s\n",
            map->tile_defs[i].symbol, map->tile_defs[i].name);
        p += written;
        remaining -= written;
    }

    // Tag definitions
    if (map->tag_def_count > 0) {
        written = snprintf(p, remaining, "\n# Object tags\n");
        p += written;
        remaining -= written;

        for (int i = 0; i < map->tag_def_count; i++) {
            const pz_tag_def *def = &map->tag_defs[i];
            if (def->type == PZ_TAG_SPAWN) {
                written = snprintf(p, remaining,
                    "tag %s %s angle=%.3f team=%d team_spawn=%d\n", def->name,
                    tag_type_name(def->type), def->data.spawn.angle,
                    def->data.spawn.team, def->data.spawn.team_spawn ? 1 : 0);
            } else if (def->type == PZ_TAG_ENEMY) {
                written = snprintf(p, remaining,
                    "tag %s %s angle=%.3f type=%s\n", def->name,
                    tag_type_name(def->type), def->data.enemy.angle,
                    enemy_type_name(def->data.enemy.type));
            } else if (def->type == PZ_TAG_POWERUP) {
                const char *powerup_type = def->data.powerup.type_name[0]
                    ? def->data.powerup.type_name
                    : "machine_gun";
                written
                    = snprintf(p, remaining, "tag %s %s type=%s respawn=%.1f",
                        def->name, tag_type_name(def->type), powerup_type,
                        def->data.powerup.respawn_time);
                p += written;
                remaining -= written;

                if (strcmp(powerup_type, "barrier_placer") == 0) {
                    if (def->data.powerup.barrier_tag[0]) {
                        written = snprintf(p, remaining, " barrier=%s",
                            def->data.powerup.barrier_tag);
                        p += written;
                        remaining -= written;
                    }
                    written = snprintf(p, remaining,
                        " barrier_count=%d barrier_lifetime=%.1f",
                        def->data.powerup.barrier_count,
                        def->data.powerup.barrier_lifetime);
                    p += written;
                    remaining -= written;
                }

                written = snprintf(p, remaining, "\n");
            } else if (def->type == PZ_TAG_BARRIER) {
                const char *tile_name = def->data.barrier.tile_name[0]
                    ? def->data.barrier.tile_name
                    : (map->tile_def_count > 0 ? map->tile_defs[0].name
                                               : "wood_rustic_dark");
                written
                    = snprintf(p, remaining, "tag %s %s tile=%s health=%.1f\n",
                        def->name, tag_type_name(def->type), tile_name,
                        def->data.barrier.health);
            } else if (def->type == PZ_TAG_JUMP_PAD) {
                const char *role_str
                    = (def->data.jump_pad.role == PZ_JUMP_PAD_LANDING)
                    ? "landing"
                    : "start";
                const char *id = def->data.jump_pad.id[0]
                    ? def->data.jump_pad.id
                    : def->name;
                written = snprintf(p, remaining, "tag %s %s id=%s role=%s\n",
                    def->name, tag_type_name(def->type), id, role_str);
            } else {
                written = 0;
            }

            p += written;
            remaining -= written;
        }
    }

    // Water level
    if (map->has_water) {
        written
            = snprintf(p, remaining, "\nwater_level %d\n", map->water_level);
        p += written;
        remaining -= written;

        written = snprintf(p, remaining, "water_color %.2f %.2f %.2f\n",
            map->water_color.x, map->water_color.y, map->water_color.z);
        p += written;
        remaining -= written;

        // Only write wave_strength if not default (1.0)
        if (map->wave_strength != 1.0f) {
            written = snprintf(
                p, remaining, "wave_strength %.1f\n", map->wave_strength);
            p += written;
            remaining -= written;
        }

        // Only write wind settings if not default
        if (map->wind_direction != 0.0f || map->wind_strength != 1.0f) {
            written = snprintf(
                p, remaining, "wind_direction %.2f\n", map->wind_direction);
            p += written;
            remaining -= written;
            written = snprintf(
                p, remaining, "wind_strength %.1f\n", map->wind_strength);
            p += written;
            remaining -= written;
        }
    }

    // Fog level
    if (map->has_fog) {
        written = snprintf(p, remaining, "\nfog_level %d\n", map->fog_level);
        p += written;
        remaining -= written;

        written = snprintf(p, remaining, "fog_color %.2f %.2f %.2f\n",
            map->fog_color.x, map->fog_color.y, map->fog_color.z);
        p += written;
        remaining -= written;
    }

    // Toxic cloud settings
    if (map->has_toxic_cloud) {
        const char *state = map->toxic_config.enabled ? "enabled" : "disabled";
        written = snprintf(p, remaining, "\ntoxic_cloud %s\n", state);
        p += written;
        remaining -= written;

        written = snprintf(
            p, remaining, "toxic_delay %.1f\n", map->toxic_config.delay);
        p += written;
        remaining -= written;

        written = snprintf(
            p, remaining, "toxic_duration %.1f\n", map->toxic_config.duration);
        p += written;
        remaining -= written;

        written = snprintf(p, remaining, "toxic_safe_zone %.2f\n",
            map->toxic_config.safe_zone_ratio);
        p += written;
        remaining -= written;

        written = snprintf(
            p, remaining, "toxic_damage %d\n", map->toxic_config.damage);
        p += written;
        remaining -= written;

        written = snprintf(p, remaining, "toxic_interval %.1f\n",
            map->toxic_config.damage_interval);
        p += written;
        remaining -= written;

        written = snprintf(
            p, remaining, "toxic_slowdown %.2f\n", map->toxic_config.slowdown);
        p += written;
        remaining -= written;

        written = snprintf(p, remaining, "toxic_color %.2f %.2f %.2f\n",
            map->toxic_config.color.x, map->toxic_config.color.y,
            map->toxic_config.color.z);
        p += written;
        remaining -= written;

        pz_vec2 toxic_tile
            = pz_map_world_to_tile_float(map, map->toxic_config.center);
        written = snprintf(p, remaining, "toxic_center %.2f %.2f\n",
            toxic_tile.x, toxic_tile.y);
        p += written;
        remaining -= written;
    }

    // If tags are present, serialize them inline in the grid. Otherwise, fall
    // back to legacy spawn/enemy blocks after the grid.

    // Grid
    written = snprintf(p, remaining, "\n<grid>\n");
    p += written;
    remaining -= written;

    // Determine cell width for alignment
    int max_cell_width = 2; // minimum "0."
    for (int y = 0; y < map->height; y++) {
        for (int x = 0; x < map->width; x++) {
            pz_map_cell cell = pz_map_get_cell(map, x, y);
            char tmp[16];
            int len = snprintf(tmp, sizeof(tmp), "%d", cell.height);
            len += 1; // tile char
            char tag_buf[128];
            if (map_build_cell_tag_list(map, x, y, tag_buf, sizeof(tag_buf))
                > 0) {
                len += 1 + (int)strlen(tag_buf);
            }
            if (len > max_cell_width)
                max_cell_width = len;
        }
    }

    // Write grid rows (top to bottom in file = low Y to high Y)
    for (int y = 0; y < map->height; y++) {
        for (int x = 0; x < map->width; x++) {
            pz_map_cell cell = pz_map_get_cell(map, x, y);
            const pz_tile_def *def
                = pz_map_get_tile_def_by_index(map, cell.tile_index);
            char symbol = def ? def->symbol : '.';

            char cell_str[256];
            char tag_buf[128];
            int tag_count
                = map_build_cell_tag_list(map, x, y, tag_buf, sizeof(tag_buf));
            if (tag_count > 0) {
                snprintf(cell_str, sizeof(cell_str), "%d%c|%s", cell.height,
                    symbol, tag_buf);
            } else {
                snprintf(
                    cell_str, sizeof(cell_str), "%d%c", cell.height, symbol);
            }

            // Pad to max_cell_width
            written = snprintf(p, remaining, "%*s ", max_cell_width, cell_str);
            p += written;
            remaining -= written;
        }
        *p++ = '\n';
        remaining--;
    }

    written = snprintf(p, remaining, "</grid>\n");
    p += written;
    remaining -= written;

    // Spawn/enemy blocks are only emitted when no tags are defined.
    if (map->tag_def_count == 0) {
        if (map->spawn_count > 0) {
            written = snprintf(p, remaining, "\n# Spawn points\n");
            p += written;
            remaining -= written;

            for (int i = 0; i < map->spawn_count; i++) {
                const pz_spawn_point *sp = &map->spawns[i];
                written = snprintf(p, remaining, "spawn %.2f %.2f %.3f %d %d\n",
                    sp->pos.x, sp->pos.y, sp->angle, sp->team,
                    sp->team_spawn ? 1 : 0);
                p += written;
                remaining -= written;
            }
        }

        if (map->enemy_count > 0) {
            written = snprintf(p, remaining, "\n# Enemy spawns\n");
            p += written;
            remaining -= written;

            for (int i = 0; i < map->enemy_count; i++) {
                const pz_enemy_spawn *es = &map->enemies[i];
                written = snprintf(p, remaining, "enemy %.2f %.2f %.3f %s\n",
                    es->pos.x, es->pos.y, es->angle, enemy_type_name(es->type));
                p += written;
                remaining -= written;
            }
        }
    }

    // Lighting
    written = snprintf(p, remaining, "\n# Lighting\n");
    p += written;
    remaining -= written;

    if (map->lighting.has_sun) {
        written = snprintf(p, remaining, "sun_direction %.2f %.2f %.2f\n",
            map->lighting.sun_direction.x, map->lighting.sun_direction.y,
            map->lighting.sun_direction.z);
        p += written;
        remaining -= written;

        written = snprintf(p, remaining, "sun_color %.2f %.2f %.2f\n",
            map->lighting.sun_color.x, map->lighting.sun_color.y,
            map->lighting.sun_color.z);
        p += written;
        remaining -= written;
    }

    written = snprintf(p, remaining, "ambient_color %.2f %.2f %.2f\n",
        map->lighting.ambient_color.x, map->lighting.ambient_color.y,
        map->lighting.ambient_color.z);
    p += written;
    remaining -= written;

    written = snprintf(p, remaining, "ambient_darkness %.2f\n",
        map->lighting.ambient_darkness);
    p += written;

    *p = '\0';

    bool success = pz_file_write_text(path, buf);
    pz_free(buf);

    if (success) {
        pz_log(PZ_LOG_INFO, PZ_LOG_CAT_GAME, "Saved map v2: %s", path);
    } else {
        pz_log(PZ_LOG_ERROR, PZ_LOG_CAT_GAME, "Failed to save map: %s", path);
    }

    return success;
}

int64_t
pz_map_file_mtime(const char *path)
{
    return pz_file_mtime(path);
}
