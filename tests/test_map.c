/*
 * Map system tests
 */

#include "../src/core/pz_platform.h"
#include "../src/game/pz_map.h"
#include "test_framework.h"

TEST(map_create)
{
    pz_map *map = pz_map_create(16, 16, 2.0f);
    ASSERT_NOT_NULL(map);
    ASSERT_EQ(16, map->width);
    ASSERT_EQ(16, map->height);
    ASSERT_NEAR(2.0f, map->tile_size, 0.01f);
    ASSERT_NEAR(32.0f, map->world_width, 0.01f);
    ASSERT_NEAR(32.0f, map->world_height, 0.01f);

    // Should have default tile definitions (ground, stone)
    ASSERT(map->tile_def_count >= 2);

    // Default cells should be ground at height 0
    pz_map_cell cell = pz_map_get_cell(map, 0, 0);
    ASSERT_EQ(0, cell.height);
    ASSERT_EQ(0, cell.tile_index); // ground

    cell = pz_map_get_cell(map, 8, 8);
    ASSERT_EQ(0, cell.height);

    pz_map_destroy(map);
}

TEST(map_cell_access)
{
    pz_map *map = pz_map_create(8, 8, 1.0f);
    ASSERT_NOT_NULL(map);

    // Add tile definitions (symbol → texture name)
    int mud_idx = pz_map_add_tile_def(map, ':', "mud_wet");
    int carpet_idx = pz_map_add_tile_def(map, '*', "carpet_gray");
    ASSERT(mud_idx >= 0);
    ASSERT(carpet_idx >= 0);

    // Set and get cells
    pz_map_set_cell(
        map, 0, 0, (pz_map_cell) { .height = 2, .tile_index = 1 }); // wall
    pz_map_set_cell(map, 1, 1,
        (pz_map_cell) { .height = 0, .tile_index = (uint8_t)mud_idx });
    pz_map_set_cell(
        map, 2, 2, (pz_map_cell) { .height = -1, .tile_index = 0 }); // pit
    pz_map_set_cell(map, 3, 3,
        (pz_map_cell) { .height = 0, .tile_index = (uint8_t)carpet_idx });

    pz_map_cell cell = pz_map_get_cell(map, 0, 0);
    ASSERT_EQ(2, cell.height);
    ASSERT_EQ(1, cell.tile_index);

    cell = pz_map_get_cell(map, 1, 1);
    ASSERT_EQ(0, cell.height);
    ASSERT_EQ(mud_idx, cell.tile_index);

    cell = pz_map_get_cell(map, 2, 2);
    ASSERT_EQ(-1, cell.height);

    cell = pz_map_get_cell(map, 3, 3);
    ASSERT_EQ(0, cell.height);
    ASSERT_EQ(carpet_idx, cell.tile_index);

    // Out of bounds returns high wall
    cell = pz_map_get_cell(map, -1, 0);
    ASSERT(cell.height > 0); // Should be solid

    pz_map_destroy(map);
}

TEST(map_height)
{
    pz_map *map = pz_map_create(8, 8, 1.0f);
    ASSERT_NOT_NULL(map);

    // Default height is 0
    ASSERT_EQ(0, pz_map_get_height(map, 0, 0));

    // Set and get height
    pz_map_set_height(map, 0, 0, 2);
    pz_map_set_height(map, 1, 1, 5);
    pz_map_set_height(map, 2, 2, -1); // pit

    ASSERT_EQ(2, pz_map_get_height(map, 0, 0));
    ASSERT_EQ(5, pz_map_get_height(map, 1, 1));
    ASSERT_EQ(-1, pz_map_get_height(map, 2, 2));

    pz_map_destroy(map);
}

TEST(map_tile_defs)
{
    pz_map *map = pz_map_create(8, 8, 1.0f);
    ASSERT_NOT_NULL(map);

    // Check default tile defs
    int ground_idx = pz_map_find_tile_def(map, '.');
    int stone_idx = pz_map_find_tile_def(map, '#');
    ASSERT(ground_idx >= 0);
    ASSERT(stone_idx >= 0);

    const pz_tile_def *ground = pz_map_get_tile_def_by_index(map, ground_idx);
    const pz_tile_def *stone = pz_map_get_tile_def_by_index(map, stone_idx);
    ASSERT_NOT_NULL(ground);
    ASSERT_NOT_NULL(stone);
    ASSERT_EQ('.', ground->symbol);
    ASSERT_EQ('#', stone->symbol);

    // Add custom tile def
    int lava_idx = pz_map_add_tile_def(map, 'L', "lava");
    ASSERT(lava_idx >= 0);
    ASSERT_EQ(lava_idx, pz_map_find_tile_def(map, 'L'));

    // Unknown tile returns -1
    ASSERT_EQ(-1, pz_map_find_tile_def(map, 'X'));

    pz_map_destroy(map);
}

TEST(map_coordinate_conversion)
{
    // 8x8 map with 2.0 unit tiles = 16x16 world centered at origin
    pz_map *map = pz_map_create(8, 8, 2.0f);
    ASSERT_NOT_NULL(map);

    // Tile (0,0) center should be at (-7, -7) = bottom-left quadrant
    pz_vec2 world = pz_map_tile_to_world(map, 0, 0);
    ASSERT_NEAR(-7.0f, world.x, 0.01f);
    ASSERT_NEAR(-7.0f, world.y, 0.01f);

    // Tile (4,4) center should be at (1, 1) = near center
    world = pz_map_tile_to_world(map, 4, 4);
    ASSERT_NEAR(1.0f, world.x, 0.01f);
    ASSERT_NEAR(1.0f, world.y, 0.01f);

    // World-to-tile conversion
    int tx, ty;
    pz_map_world_to_tile(map, (pz_vec2) { -7.0f, -7.0f }, &tx, &ty);
    ASSERT_EQ(0, tx);
    ASSERT_EQ(0, ty);

    pz_map_world_to_tile(map, (pz_vec2) { 0.0f, 0.0f }, &tx, &ty);
    ASSERT_EQ(4, tx);
    ASSERT_EQ(4, ty);

    pz_map_destroy(map);
}

TEST(map_solid_check)
{
    pz_map *map = pz_map_create(8, 8, 2.0f);
    ASSERT_NOT_NULL(map);

    pz_vec2 center = pz_map_tile_to_world(map, 4, 4);

    // Height 0 is passable
    pz_map_set_height(map, 4, 4, 0);
    ASSERT(!pz_map_is_solid(map, center));
    ASSERT(pz_map_is_passable(map, center));

    // Height > 0 (wall) is solid
    pz_map_set_height(map, 4, 4, 2);
    ASSERT(pz_map_is_solid(map, center));
    ASSERT(!pz_map_is_passable(map, center));

    // Height < 0 (pit) is also solid for tanks
    pz_map_set_height(map, 4, 4, -1);
    ASSERT(pz_map_is_solid(map, center));

    // But bullets can fly over pits
    ASSERT(!pz_map_blocks_bullets(map, center));

    // Walls block bullets
    pz_map_set_height(map, 4, 4, 2);
    ASSERT(pz_map_blocks_bullets(map, center));

    pz_map_destroy(map);
}

// NOTE: Tile properties are now loaded from .tile files via pz_tile_registry.
// The old pz_get_texture_props function has been removed.
// Tile property tests should be added to a separate tile registry test file.

// NOTE: Speed multiplier tests require a tile registry to be set on the map.
// These tests would need to be integrated with the tile registry for full
// coverage. For now, we test the basic structure without registry (returns
// defaults).
TEST(map_speed_multiplier_without_registry)
{
    pz_map *map = pz_map_create(8, 8, 2.0f);
    ASSERT_NOT_NULL(map);

    pz_vec2 center = pz_map_tile_to_world(map, 4, 4);

    // Without tile registry, speed multiplier returns 1.0 for passable tiles
    pz_map_set_cell(map, 4, 4, (pz_map_cell) { .height = 0, .tile_index = 0 });
    ASSERT_NEAR(1.0f, pz_map_get_speed_multiplier(map, center), 0.01f);

    // Non-zero height tiles also return 1.0 (multi-floor gameplay allows
    // movement on any floor level - collision system handles blocking)
    pz_map_set_cell(map, 4, 4, (pz_map_cell) { .height = 2, .tile_index = 1 });
    ASSERT_NEAR(1.0f, pz_map_get_speed_multiplier(map, center), 0.01f);

    pz_map_destroy(map);
}

TEST(map_bounds)
{
    pz_map *map = pz_map_create(8, 8, 2.0f);
    ASSERT_NOT_NULL(map);

    // Tile bounds
    ASSERT(pz_map_in_bounds(map, 0, 0));
    ASSERT(pz_map_in_bounds(map, 7, 7));
    ASSERT(!pz_map_in_bounds(map, -1, 0));
    ASSERT(!pz_map_in_bounds(map, 8, 0));
    ASSERT(!pz_map_in_bounds(map, 0, 8));

    // World bounds (16x16 centered at origin = -8 to +8)
    ASSERT(pz_map_in_bounds_world(map, (pz_vec2) { 0, 0 }));
    ASSERT(pz_map_in_bounds_world(map, (pz_vec2) { -7.9f, -7.9f }));
    ASSERT(pz_map_in_bounds_world(map, (pz_vec2) { 7.9f, 7.9f }));
    ASSERT(!pz_map_in_bounds_world(map, (pz_vec2) { -8.1f, 0 }));
    ASSERT(!pz_map_in_bounds_world(map, (pz_vec2) { 8.1f, 0 }));

    pz_map_destroy(map);
}

TEST(map_test_creation)
{
    pz_map *map = pz_map_create_test();
    ASSERT_NOT_NULL(map);
    ASSERT_EQ(16, map->width);
    ASSERT_EQ(16, map->height);

    // Border should be walls (height > 0)
    ASSERT(pz_map_get_height(map, 0, 0) > 0);
    ASSERT(pz_map_get_height(map, 15, 0) > 0);
    ASSERT(pz_map_get_height(map, 0, 15) > 0);
    ASSERT(pz_map_get_height(map, 15, 15) > 0);

    // Interior should have some ground (height = 0)
    ASSERT_EQ(0, pz_map_get_height(map, 1, 1));

    // Should have spawn points
    ASSERT_EQ(4, map->spawn_count);

    // Print for visual verification
    printf("\n");
    pz_map_print(map);

    pz_map_destroy(map);
}

TEST(map_v2_format)
{
    // Test loading a v2 format map
    pz_map *map = pz_map_load("assets/maps/test_arena.map");
    if (!map) {
        printf(
            "Note: Skipping v2 format test (map file not found in test env)\n");
        return;
    }

    ASSERT_EQ(2, map->version);
    ASSERT_EQ(24, map->width);
    ASSERT_EQ(14, map->height);

    // Check tile definitions were loaded
    ASSERT(map->tile_def_count >= 2);
    ASSERT(pz_map_find_tile_def(map, '.') >= 0);
    ASSERT(pz_map_find_tile_def(map, '#') >= 0);

    // Border should be walls
    ASSERT(pz_map_get_height(map, 0, 0) > 0);
    ASSERT(pz_map_get_height(map, 23, 0) > 0);

    // Should have spawns from tags
    ASSERT(map->spawn_count > 0);

    // Should have enemies from tags
    ASSERT(map->enemy_count > 0);

    pz_map_destroy(map);
}

TEST(map_toxic_config)
{
    const char *path = "/tmp/tankgame_toxic_test.map";
    const char *content = "name Toxic Test\n"
                          "tile_size 2.0\n"
                          "\n"
                          "<grid>\n"
                          "0.\n"
                          "</grid>\n"
                          "\n"
                          "toxic_cloud enabled\n"
                          "toxic_delay 12.0\n"
                          "toxic_duration 80.0\n"
                          "toxic_safe_zone 0.25\n"
                          "toxic_damage 2\n"
                          "toxic_interval 3.5\n"
                          "toxic_slowdown 0.65\n"
                          "toxic_color 0.1 0.2 0.3\n"
                          "toxic_center 0 0\n";

    ASSERT(pz_file_write_text(path, content));

    pz_map *map = pz_map_load(path);
    ASSERT_NOT_NULL(map);
    ASSERT(map->has_toxic_cloud);
    ASSERT(map->toxic_config.enabled);
    ASSERT_NEAR(12.0f, map->toxic_config.delay, 0.001f);
    ASSERT_NEAR(80.0f, map->toxic_config.duration, 0.001f);
    ASSERT_NEAR(0.25f, map->toxic_config.safe_zone_ratio, 0.001f);
    ASSERT_EQ(2, map->toxic_config.damage);
    ASSERT_NEAR(3.5f, map->toxic_config.damage_interval, 0.001f);
    ASSERT_NEAR(0.65f, map->toxic_config.slowdown, 0.001f);
    ASSERT_NEAR(0.1f, map->toxic_config.color.x, 0.001f);
    ASSERT_NEAR(0.2f, map->toxic_config.color.y, 0.001f);
    ASSERT_NEAR(0.3f, map->toxic_config.color.z, 0.001f);
    ASSERT_NEAR(0.0f, map->toxic_config.center.x, 0.001f);
    ASSERT_NEAR(0.0f, map->toxic_config.center.y, 0.001f);

    pz_map_destroy(map);
    pz_file_delete(path);
}

TEST_MAIN()
