/*
 * Tank Game - Network Protocol
 *
 * Binary protocol for game state replication.
 * Host is authoritative - sends state snapshots to clients.
 * Clients send inputs to host.
 */

#ifndef PZ_NET_PROTOCOL_H
#define PZ_NET_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../core/pz_math.h"

// Protocol version (bump when breaking changes)
#define PZ_NET_PROTOCOL_VERSION 1

// Maximum entities per snapshot
#define PZ_NET_MAX_TANKS 8
#define PZ_NET_MAX_PROJECTILES 64
#define PZ_NET_MAX_POWERUPS 16
#define PZ_NET_MAX_MINES 32
#define PZ_NET_MAX_BARRIERS 32

// Message types
typedef enum pz_net_msg_type {
    PZ_NET_MSG_INPUT = 1, // Client -> Host: player input
    PZ_NET_MSG_SNAPSHOT = 2, // Host -> Client: full state snapshot
    PZ_NET_MSG_EVENT = 3, // Bidirectional: one-shot events (sounds, etc)
} pz_net_msg_type;

// Event types (for PZ_NET_MSG_EVENT)
typedef enum pz_net_event_type {
    PZ_NET_EVENT_TANK_DEATH = 1,
    PZ_NET_EVENT_TANK_RESPAWN = 2,
    PZ_NET_EVENT_EXPLOSION = 3,
    PZ_NET_EVENT_POWERUP_COLLECT = 4,
} pz_net_event_type;

// ============================================================================
// Wire formats (packed, little-endian)
// ============================================================================

#pragma pack(push, 1)

// Message header (all messages start with this)
typedef struct pz_net_msg_header {
    uint8_t type; // pz_net_msg_type
    uint8_t version; // Protocol version
    uint16_t length; // Total message length including header
    uint32_t tick; // Game tick this message relates to
} pz_net_msg_header;

// Input message (client -> host)
typedef struct pz_net_msg_input {
    pz_net_msg_header header;
    float move_x; // Movement direction X
    float move_y; // Movement direction Y
    float turret_angle; // Target turret angle
    uint8_t fire; // Fire button pressed
    uint8_t place_mine; // Place mine button pressed
    uint8_t place_barrier; // Place barrier button pressed
    uint8_t weapon_switch; // 0 = no switch, 1+ = switch to weapon index
} pz_net_msg_input;

// Tank state in snapshot
typedef struct pz_net_tank_state {
    uint8_t active;
    uint8_t flags; // Dead, invulnerable, etc.
    int8_t id;
    int8_t health;
    float pos_x;
    float pos_y;
    float vel_x;
    float vel_y;
    float body_angle;
    float turret_angle;
    uint8_t current_weapon; // pz_powerup_type
    uint8_t mine_count;
    uint8_t loadout_count;
    uint8_t loadout[8]; // Weapon types in loadout
} pz_net_tank_state;

// Projectile state in snapshot
typedef struct pz_net_projectile_state {
    uint8_t active;
    int8_t owner_id;
    int8_t bounces_remaining;
    int8_t damage;
    float pos_x;
    float pos_y;
    float vel_x;
    float vel_y;
    float lifetime;
    float scale;
    uint8_t color_r;
    uint8_t color_g;
    uint8_t color_b;
    uint8_t _pad;
} pz_net_projectile_state;

// Powerup state in snapshot
typedef struct pz_net_powerup_state {
    uint8_t active;
    uint8_t collected;
    uint8_t type; // pz_powerup_type
    uint8_t _pad;
    float pos_x;
    float pos_y;
    float respawn_timer;
} pz_net_powerup_state;

// Mine state in snapshot
typedef struct pz_net_mine_state {
    uint8_t active;
    int8_t owner_id;
    uint8_t armed; // arm_timer <= 0
    uint8_t _pad;
    float pos_x;
    float pos_y;
} pz_net_mine_state;

// Barrier state in snapshot
typedef struct pz_net_barrier_state {
    uint8_t active;
    uint8_t destroyed;
    int8_t owner_tank_id;
    uint8_t tile_name_len;
    float pos_x;
    float pos_y;
    float health;
    float lifetime;
    // tile_name follows (tile_name_len bytes, no null terminator in wire
    // format)
} pz_net_barrier_state;

// Full state snapshot (host -> client)
// Variable length - contains counts then arrays
typedef struct pz_net_msg_snapshot {
    pz_net_msg_header header;
    uint8_t tank_count;
    uint8_t projectile_count;
    uint8_t powerup_count;
    uint8_t mine_count;
    uint8_t barrier_count;
    uint8_t local_tank_id; // Which tank ID the client controls
    uint8_t _pad[2];
    // Followed by:
    // - pz_net_tank_state[tank_count]
    // - pz_net_projectile_state[projectile_count]
    // - pz_net_powerup_state[powerup_count]
    // - pz_net_mine_state[mine_count]
    // - pz_net_barrier_state[barrier_count] (variable size due to tile_name)
} pz_net_msg_snapshot;

// Event message
typedef struct pz_net_msg_event {
    pz_net_msg_header header;
    uint8_t event_type; // pz_net_event_type
    uint8_t _pad[3];
    float pos_x;
    float pos_y;
    int8_t entity_id; // Tank ID, etc.
    uint8_t extra[3]; // Event-specific data
} pz_net_msg_event;

#pragma pack(pop)

// ============================================================================
// Unpacked state (for game use)
// ============================================================================

typedef struct pz_net_game_state {
    uint32_t tick;
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
    char barrier_tile_names[PZ_NET_MAX_BARRIERS][32];
} pz_net_game_state;

typedef struct pz_net_input {
    uint32_t tick;
    float move_x;
    float move_y;
    float turret_angle;
    bool fire;
    bool place_mine;
    bool place_barrier;
    int weapon_switch;
} pz_net_input;

// ============================================================================
// API
// ============================================================================

// Serialize input to wire format
// Returns allocated buffer (caller must free), sets *out_len
uint8_t *pz_net_serialize_input(const pz_net_input *input, size_t *out_len);

// Serialize game state to snapshot message
// Returns allocated buffer (caller must free), sets *out_len
uint8_t *pz_net_serialize_snapshot(
    const pz_net_game_state *state, size_t *out_len);

// Parse message header (returns message type, or 0 on error)
// Validates version and length
pz_net_msg_type pz_net_parse_header(
    const uint8_t *data, size_t len, pz_net_msg_header *out_header);

// Parse input message (returns true on success)
bool pz_net_parse_input(
    const uint8_t *data, size_t len, pz_net_input *out_input);

// Parse snapshot message (returns true on success)
bool pz_net_parse_snapshot(
    const uint8_t *data, size_t len, pz_net_game_state *out_state);

#endif // PZ_NET_PROTOCOL_H
