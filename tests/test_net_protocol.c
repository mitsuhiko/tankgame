/*
 * Tank Game - Network Protocol Tests
 */

#include "test_framework.h"

#include "../src/core/pz_mem.h"
#include "../src/net/pz_net_protocol.h"

#include <math.h>
#include <string.h>

TEST(net_input_round_trip)
{
    pz_mem_init();

    pz_net_input input = {
        .sequence = 42,
        .last_host_tick = 99,
        .action_sequence = 7,
        .move_x = -0.5f,
        .move_y = 1.0f,
        .turret_angle = 2.25f,
        .cursor_x = 12.0f,
        .cursor_y = -7.0f,
        .fire_held = true,
        .fire_pressed = true,
        .place_mine = true,
        .place_barrier = false,
        .weapon_switch = -1,
    };

    size_t len = 0;
    uint8_t *data = pz_net_serialize_input(&input, &len);
    ASSERT_NOT_NULL(data);
    ASSERT_EQ(sizeof(pz_net_msg_input), len);

    pz_net_input parsed = { 0 };
    ASSERT(pz_net_parse_input(data, len, &parsed));
    ASSERT_EQ(42, parsed.sequence);
    ASSERT_EQ(99, parsed.last_host_tick);
    ASSERT_EQ(7, parsed.action_sequence);
    ASSERT_NEAR(-0.5f, parsed.move_x, 0.0001f);
    ASSERT_NEAR(1.0f, parsed.move_y, 0.0001f);
    ASSERT_NEAR(2.25f, parsed.turret_angle, 0.0001f);
    ASSERT_NEAR(12.0f, parsed.cursor_x, 0.0001f);
    ASSERT_NEAR(-7.0f, parsed.cursor_y, 0.0001f);
    ASSERT(parsed.fire_held && parsed.fire_pressed && parsed.place_mine);
    ASSERT(!parsed.place_barrier);
    ASSERT_EQ(-1, parsed.weapon_switch);

    pz_free(data);
    ASSERT(!pz_mem_has_leaks());
    pz_mem_shutdown();
}

TEST(net_snapshot_round_trip)
{
    pz_mem_init();

    pz_net_game_state state = { 0 };
    state.tick = 120;
    state.last_processed_input = 77;
    state.last_processed_action = 5;
    state.local_tank_id = 2;
    state.tank_count = 1;
    state.tanks[0] = (pz_net_tank_state) {
        .active = 1,
        .flags = 9,
        .id = 2,
        .health = 8,
        .floor_level = 4,
        .jump_state = 2,
        .current_weapon = 1,
        .mine_count = 2,
        .loadout_count = 2,
        .loadout = { 0, 1 },
        .pos_x = 3.0f,
        .pos_y = 4.0f,
        .vel_x = 1.0f,
        .vel_y = -1.0f,
        .body_angle = 0.5f,
        .turret_angle = 1.5f,
        .jump_timer = 0.2f,
        .jump_duration = 0.6f,
        .jump_start_x = 1.0f,
        .jump_start_y = 2.0f,
        .jump_end_x = 5.0f,
        .jump_end_y = 6.0f,
        .jump_start_angle = 0.1f,
        .jump_end_angle = 0.9f,
        .jump_height = 1.2f,
        .jump_apex_height = 2.2f,
    };
    state.projectile_count = 1;
    state.projectiles[0] = (pz_net_projectile_state) {
        .active = 1,
        .owner_id = 2,
        .bounces_remaining = 1,
        .damage = 3,
        .floor_level = 4,
        .color_r = 10,
        .color_g = 20,
        .color_b = 30,
        .pos_x = 7.0f,
        .pos_y = 8.0f,
        .vel_x = 9.0f,
        .vel_y = 10.0f,
        .lifetime = 2.0f,
        .scale = 0.5f,
    };
    state.barrier_count = 1;
    state.barriers[0] = (pz_net_barrier_state) {
        .active = 1,
        .owner_tank_id = 2,
        .floor_level = 4,
        .pos_x = 11.0f,
        .pos_y = 12.0f,
        .health = 13.0f,
        .lifetime = 14.0f,
    };
    strcpy(state.barrier_tile_names[0], "cobble");

    size_t len = 0;
    uint8_t *data = pz_net_serialize_snapshot(&state, &len);
    ASSERT_NOT_NULL(data);

    pz_net_game_state parsed;
    ASSERT(pz_net_parse_snapshot(data, len, &parsed));
    ASSERT_EQ(120, parsed.tick);
    ASSERT_EQ(77, parsed.last_processed_input);
    ASSERT_EQ(5, parsed.last_processed_action);
    ASSERT_EQ(2, parsed.local_tank_id);
    ASSERT_EQ(1, parsed.tank_count);
    ASSERT_EQ(4, parsed.tanks[0].floor_level);
    ASSERT_EQ(2, parsed.tanks[0].jump_state);
    ASSERT_NEAR(1.2f, parsed.tanks[0].jump_height, 0.0001f);
    ASSERT_EQ(1, parsed.projectile_count);
    ASSERT_EQ(4, parsed.projectiles[0].floor_level);
    ASSERT_EQ(1, parsed.barrier_count);
    ASSERT_EQ(4, parsed.barriers[0].floor_level);
    ASSERT_STR_EQ("cobble", parsed.barrier_tile_names[0]);

    pz_free(data);
    ASSERT(!pz_mem_has_leaks());
    pz_mem_shutdown();
}

TEST(net_protocol_rejects_malformed_messages)
{
    pz_mem_init();

    pz_net_input input = { .sequence = 1 };
    size_t len = 0;
    uint8_t *data = pz_net_serialize_input(&input, &len);
    ASSERT_NOT_NULL(data);

    pz_net_input parsed = { 0 };
    ASSERT(!pz_net_parse_input(data, len - 1, &parsed));

    pz_net_msg_header *header = (pz_net_msg_header *)data;
    header->length--;
    ASSERT(!pz_net_parse_input(data, len, &parsed));
    header->length++;

    pz_net_msg_input *wire_input = (pz_net_msg_input *)data;
    wire_input->move_x = NAN;
    ASSERT(!pz_net_parse_input(data, len, &parsed));
    pz_free(data);

    pz_net_game_state invalid_state = { .tank_count = -1 };
    ASSERT_NULL(pz_net_serialize_snapshot(&invalid_state, &len));

    ASSERT(!pz_mem_has_leaks());
    pz_mem_shutdown();
}

TEST(net_event_round_trip)
{
    pz_mem_init();

    pz_net_event event = {
        .tick = 88,
        .type = PZ_NET_EVENT_PROJECTILE_HIT,
        .pos_x = 1.5f,
        .pos_y = -2.5f,
        .entity_id = 2,
        .floor_level = 3,
        .extra = { 4, 5 },
    };
    size_t len = 0;
    uint8_t *data = pz_net_serialize_event(&event, &len);
    ASSERT_NOT_NULL(data);

    pz_net_event parsed = { 0 };
    ASSERT(pz_net_parse_event(data, len, &parsed));
    ASSERT_EQ(88, parsed.tick);
    ASSERT_EQ(PZ_NET_EVENT_PROJECTILE_HIT, parsed.type);
    ASSERT_NEAR(1.5f, parsed.pos_x, 0.0001f);
    ASSERT_EQ(3, parsed.floor_level);
    ASSERT_EQ(4, parsed.extra[0]);

    pz_free(data);
    ASSERT(!pz_mem_has_leaks());
    pz_mem_shutdown();
}

TEST_MAIN()
