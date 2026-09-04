/*
 * Tank Game - Network Protocol
 *
 * Binary protocol for host-authoritative state replication. All current
 * targets are little-endian IEEE-754 platforms; parsers still validate every
 * size/count before touching payload data.
 */

#ifndef PZ_NET_PROTOCOL_H
#define PZ_NET_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Protocol version (bump when breaking wire compatibility)
#define PZ_NET_PROTOCOL_VERSION 2

// Maximum entities per snapshot
#define PZ_NET_MAX_TANKS 8
#define PZ_NET_MAX_PROJECTILES 64
#define PZ_NET_MAX_POWERUPS 16
#define PZ_NET_MAX_MINES 32
#define PZ_NET_MAX_BARRIERS 32
#define PZ_NET_MAX_BARRIER_NAME 31

// Message types
typedef enum pz_net_msg_type {
    PZ_NET_MSG_INPUT = 1, // Client -> Host: player input
    PZ_NET_MSG_SNAPSHOT = 2, // Host -> Client: full state snapshot
    PZ_NET_MSG_EVENT = 3, // Host -> Client: reliable one-shot event
} pz_net_msg_type;

// Event types (for PZ_NET_MSG_EVENT)
typedef enum pz_net_event_type {
    PZ_NET_EVENT_TANK_DEATH = 1,
    PZ_NET_EVENT_TANK_RESPAWN = 2,
    PZ_NET_EVENT_PROJECTILE_HIT = 3,
    PZ_NET_EVENT_MINE_EXPLOSION = 4,
    PZ_NET_EVENT_POWERUP_COLLECT = 5,
    PZ_NET_EVENT_GUNFIRE = 6,
    PZ_NET_EVENT_JUMP = 7,
    PZ_NET_EVENT_BARRIER_PLACED = 8,
} pz_net_event_type;

// ============================================================================
// Wire formats (packed, little-endian)
// ============================================================================

#pragma pack(push, 1)

typedef struct pz_net_msg_header {
    uint8_t type; // pz_net_msg_type
    uint8_t version; // Protocol version
    uint16_t length; // Exact message length including header
    uint32_t tick; // Input sequence or host simulation tick
} pz_net_msg_header;

typedef struct pz_net_msg_input {
    pz_net_msg_header header;
    uint32_t last_host_tick;
    uint32_t action_sequence;
    float move_x;
    float move_y;
    float turret_angle;
    float cursor_x;
    float cursor_y;
    uint8_t fire_held;
    uint8_t fire_pressed;
    uint8_t place_mine;
    uint8_t place_barrier;
    int8_t weapon_switch;
    uint8_t _pad[3];
} pz_net_msg_input;

typedef struct pz_net_tank_state {
    uint8_t active;
    uint8_t flags;
    int8_t id;
    int8_t health;
    int8_t floor_level;
    uint8_t jump_state;
    uint8_t current_weapon;
    uint8_t mine_count;
    uint8_t loadout_count;
    uint8_t loadout[8];
    float pos_x;
    float pos_y;
    float vel_x;
    float vel_y;
    float body_angle;
    float turret_angle;
    float jump_timer;
    float jump_duration;
    float jump_start_x;
    float jump_start_y;
    float jump_end_x;
    float jump_end_y;
    float jump_start_angle;
    float jump_end_angle;
    float jump_height;
    float jump_apex_height;
} pz_net_tank_state;

typedef struct pz_net_projectile_state {
    uint8_t active;
    int8_t owner_id;
    int8_t bounces_remaining;
    int8_t damage;
    int8_t floor_level;
    uint8_t color_r;
    uint8_t color_g;
    uint8_t color_b;
    float pos_x;
    float pos_y;
    float vel_x;
    float vel_y;
    float lifetime;
    float scale;
} pz_net_projectile_state;

typedef struct pz_net_powerup_state {
    uint8_t active;
    uint8_t collected;
    uint8_t type;
    uint8_t _pad;
    float pos_x;
    float pos_y;
    float respawn_timer;
} pz_net_powerup_state;

typedef struct pz_net_mine_state {
    uint8_t active;
    int8_t owner_id;
    uint8_t armed;
    uint8_t _pad;
    float pos_x;
    float pos_y;
} pz_net_mine_state;

typedef struct pz_net_barrier_state {
    uint8_t active;
    uint8_t destroyed;
    int8_t owner_tank_id;
    int8_t floor_level;
    uint8_t tile_name_len;
    uint8_t _pad[3];
    float pos_x;
    float pos_y;
    float health;
    float lifetime;
    // tile_name follows (tile_name_len bytes, no null terminator)
} pz_net_barrier_state;

typedef struct pz_net_msg_snapshot {
    pz_net_msg_header header;
    uint32_t last_processed_input;
    uint32_t last_processed_action;
    uint8_t tank_count;
    uint8_t projectile_count;
    uint8_t powerup_count;
    uint8_t mine_count;
    uint8_t barrier_count;
    uint8_t local_tank_id; // Tank controlled by the receiving client
    uint8_t _pad[2];
    // Followed by fixed-size arrays, then variable-size barriers.
} pz_net_msg_snapshot;

typedef struct pz_net_msg_event {
    pz_net_msg_header header;
    uint8_t event_type;
    int8_t floor_level;
    uint8_t extra[2];
    float pos_x;
    float pos_y;
    int8_t entity_id;
    uint8_t _pad[3];
} pz_net_msg_event;

#pragma pack(pop)

// ============================================================================
// Unpacked state (for game use)
// ============================================================================

typedef struct pz_net_game_state {
    uint32_t tick;
    uint32_t last_processed_input;
    uint32_t last_processed_action;
    int8_t local_tank_id;

    int tank_count;
    pz_net_tank_state tanks[PZ_NET_MAX_TANKS];

    int projectile_count;
    pz_net_projectile_state projectiles[PZ_NET_MAX_PROJECTILES];

    int powerup_count;
    pz_net_powerup_state powerups[PZ_NET_MAX_POWERUPS];

    int mine_count;
    pz_net_mine_state mines[PZ_NET_MAX_MINES];

    int barrier_count;
    pz_net_barrier_state barriers[PZ_NET_MAX_BARRIERS];
    char barrier_tile_names[PZ_NET_MAX_BARRIERS][PZ_NET_MAX_BARRIER_NAME + 1];
} pz_net_game_state;

typedef struct pz_net_input {
    uint32_t sequence;
    uint32_t last_host_tick;
    uint32_t action_sequence;
    float move_x;
    float move_y;
    float turret_angle;
    float cursor_x;
    float cursor_y;
    bool fire_held;
    bool fire_pressed;
    bool place_mine;
    bool place_barrier;
    int weapon_switch;
} pz_net_input;

typedef struct pz_net_event {
    uint32_t tick;
    pz_net_event_type type;
    float pos_x;
    float pos_y;
    int entity_id;
    int8_t floor_level;
    uint8_t extra[2];
} pz_net_event;

uint8_t *pz_net_serialize_input(const pz_net_input *input, size_t *out_len);
uint8_t *pz_net_serialize_snapshot(
    const pz_net_game_state *state, size_t *out_len);
uint8_t *pz_net_serialize_event(const pz_net_event *event, size_t *out_len);

pz_net_msg_type pz_net_parse_header(
    const uint8_t *data, size_t len, pz_net_msg_header *out_header);
bool pz_net_parse_input(
    const uint8_t *data, size_t len, pz_net_input *out_input);
bool pz_net_parse_snapshot(
    const uint8_t *data, size_t len, pz_net_game_state *out_state);
bool pz_net_parse_event(
    const uint8_t *data, size_t len, pz_net_event *out_event);

#endif // PZ_NET_PROTOCOL_H
