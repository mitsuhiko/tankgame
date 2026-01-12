/*
 * Tank Game - Network Protocol Implementation
 */

#include "pz_net_protocol.h"

#include "../core/pz_mem.h"
#include <string.h>

// ============================================================================
// Serialization
// ============================================================================

uint8_t *
pz_net_serialize_input(const pz_net_input *input, size_t *out_len)
{
    if (!input || !out_len) {
        return NULL;
    }

    size_t len = sizeof(pz_net_msg_input);
    uint8_t *buf = pz_alloc(len);
    if (!buf) {
        return NULL;
    }

    pz_net_msg_input *msg = (pz_net_msg_input *)buf;
    msg->header.type = PZ_NET_MSG_INPUT;
    msg->header.version = PZ_NET_PROTOCOL_VERSION;
    msg->header.length = (uint16_t)len;
    msg->header.tick = input->tick;

    msg->move_x = input->move_x;
    msg->move_y = input->move_y;
    msg->turret_angle = input->turret_angle;
    msg->fire = input->fire ? 1 : 0;
    msg->place_mine = input->place_mine ? 1 : 0;
    msg->place_barrier = input->place_barrier ? 1 : 0;
    msg->weapon_switch = (uint8_t)input->weapon_switch;

    *out_len = len;
    return buf;
}

uint8_t *
pz_net_serialize_snapshot(const pz_net_game_state *state, size_t *out_len)
{
    if (!state || !out_len) {
        return NULL;
    }

    // Calculate total size
    size_t base_size = sizeof(pz_net_msg_snapshot);
    size_t tanks_size = (size_t)state->tank_count * sizeof(pz_net_tank_state);
    size_t projectiles_size
        = (size_t)state->projectile_count * sizeof(pz_net_projectile_state);
    size_t powerups_size
        = (size_t)state->powerup_count * sizeof(pz_net_powerup_state);
    size_t mines_size = (size_t)state->mine_count * sizeof(pz_net_mine_state);

    // Barriers are variable size due to tile names
    size_t barriers_size = 0;
    for (int i = 0; i < state->barrier_count; i++) {
        barriers_size += sizeof(pz_net_barrier_state);
        barriers_size += strlen(state->barrier_tile_names[i]);
    }

    size_t total_size = base_size + tanks_size + projectiles_size
        + powerups_size + mines_size + barriers_size;

    uint8_t *buf = pz_alloc(total_size);
    if (!buf) {
        return NULL;
    }

    // Write header
    pz_net_msg_snapshot *msg = (pz_net_msg_snapshot *)buf;
    msg->header.type = PZ_NET_MSG_SNAPSHOT;
    msg->header.version = PZ_NET_PROTOCOL_VERSION;
    msg->header.length = (uint16_t)total_size;
    msg->header.tick = state->tick;

    msg->tank_count = (uint8_t)state->tank_count;
    msg->projectile_count = (uint8_t)state->projectile_count;
    msg->powerup_count = (uint8_t)state->powerup_count;
    msg->mine_count = (uint8_t)state->mine_count;
    msg->barrier_count = (uint8_t)state->barrier_count;
    msg->local_tank_id = (uint8_t)state->local_tank_id;
    msg->_pad[0] = 0;
    msg->_pad[1] = 0;

    // Write arrays
    uint8_t *ptr = buf + base_size;

    // Tanks
    memcpy(ptr, state->tanks, tanks_size);
    ptr += tanks_size;

    // Projectiles
    memcpy(ptr, state->projectiles, projectiles_size);
    ptr += projectiles_size;

    // Powerups
    memcpy(ptr, state->powerups, powerups_size);
    ptr += powerups_size;

    // Mines
    memcpy(ptr, state->mines, mines_size);
    ptr += mines_size;

    // Barriers (variable size)
    for (int i = 0; i < state->barrier_count; i++) {
        pz_net_barrier_state barrier = state->barriers[i];
        size_t name_len = strlen(state->barrier_tile_names[i]);
        barrier.tile_name_len = (uint8_t)name_len;

        memcpy(ptr, &barrier, sizeof(pz_net_barrier_state));
        ptr += sizeof(pz_net_barrier_state);

        memcpy(ptr, state->barrier_tile_names[i], name_len);
        ptr += name_len;
    }

    *out_len = total_size;
    return buf;
}

// ============================================================================
// Parsing
// ============================================================================

pz_net_msg_type
pz_net_parse_header(
    const uint8_t *data, size_t len, pz_net_msg_header *out_header)
{
    if (!data || len < sizeof(pz_net_msg_header) || !out_header) {
        return 0;
    }

    memcpy(out_header, data, sizeof(pz_net_msg_header));

    // Validate version
    if (out_header->version != PZ_NET_PROTOCOL_VERSION) {
        return 0;
    }

    // Validate length
    if (out_header->length > len) {
        return 0;
    }

    return (pz_net_msg_type)out_header->type;
}

bool
pz_net_parse_input(const uint8_t *data, size_t len, pz_net_input *out_input)
{
    if (!data || len < sizeof(pz_net_msg_input) || !out_input) {
        return false;
    }

    pz_net_msg_header header;
    if (pz_net_parse_header(data, len, &header) != PZ_NET_MSG_INPUT) {
        return false;
    }

    const pz_net_msg_input *msg = (const pz_net_msg_input *)data;

    out_input->tick = header.tick;
    out_input->move_x = msg->move_x;
    out_input->move_y = msg->move_y;
    out_input->turret_angle = msg->turret_angle;
    out_input->fire = msg->fire != 0;
    out_input->place_mine = msg->place_mine != 0;
    out_input->place_barrier = msg->place_barrier != 0;
    out_input->weapon_switch = msg->weapon_switch;

    return true;
}

bool
pz_net_parse_snapshot(
    const uint8_t *data, size_t len, pz_net_game_state *out_state)
{
    if (!data || len < sizeof(pz_net_msg_snapshot) || !out_state) {
        return false;
    }

    pz_net_msg_header header;
    if (pz_net_parse_header(data, len, &header) != PZ_NET_MSG_SNAPSHOT) {
        return false;
    }

    const pz_net_msg_snapshot *msg = (const pz_net_msg_snapshot *)data;

    // Validate counts
    if (msg->tank_count > PZ_NET_MAX_TANKS
        || msg->projectile_count > PZ_NET_MAX_PROJECTILES
        || msg->powerup_count > PZ_NET_MAX_POWERUPS
        || msg->mine_count > PZ_NET_MAX_MINES
        || msg->barrier_count > PZ_NET_MAX_BARRIERS) {
        return false;
    }

    out_state->tick = header.tick;
    out_state->local_tank_id = (int8_t)msg->local_tank_id;
    out_state->tank_count = msg->tank_count;
    out_state->projectile_count = msg->projectile_count;
    out_state->powerup_count = msg->powerup_count;
    out_state->mine_count = msg->mine_count;
    out_state->barrier_count = msg->barrier_count;

    const uint8_t *ptr = data + sizeof(pz_net_msg_snapshot);
    const uint8_t *end = data + len;

    // Read tanks
    size_t tanks_size = (size_t)msg->tank_count * sizeof(pz_net_tank_state);
    if (ptr + tanks_size > end) {
        return false;
    }
    memcpy(out_state->tanks, ptr, tanks_size);
    ptr += tanks_size;

    // Read projectiles
    size_t projectiles_size
        = (size_t)msg->projectile_count * sizeof(pz_net_projectile_state);
    if (ptr + projectiles_size > end) {
        return false;
    }
    memcpy(out_state->projectiles, ptr, projectiles_size);
    ptr += projectiles_size;

    // Read powerups
    size_t powerups_size
        = (size_t)msg->powerup_count * sizeof(pz_net_powerup_state);
    if (ptr + powerups_size > end) {
        return false;
    }
    memcpy(out_state->powerups, ptr, powerups_size);
    ptr += powerups_size;

    // Read mines
    size_t mines_size = (size_t)msg->mine_count * sizeof(pz_net_mine_state);
    if (ptr + mines_size > end) {
        return false;
    }
    memcpy(out_state->mines, ptr, mines_size);
    ptr += mines_size;

    // Read barriers (variable size)
    for (int i = 0; i < msg->barrier_count; i++) {
        if (ptr + sizeof(pz_net_barrier_state) > end) {
            return false;
        }

        memcpy(&out_state->barriers[i], ptr, sizeof(pz_net_barrier_state));
        ptr += sizeof(pz_net_barrier_state);

        size_t name_len = out_state->barriers[i].tile_name_len;
        if (name_len >= 32 || ptr + name_len > end) {
            return false;
        }

        memcpy(out_state->barrier_tile_names[i], ptr, name_len);
        out_state->barrier_tile_names[i][name_len] = '\0';
        ptr += name_len;
    }

    return true;
}
