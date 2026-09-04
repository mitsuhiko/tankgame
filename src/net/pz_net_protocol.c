/*
 * Tank Game - Network Protocol Implementation
 */

#include "pz_net_protocol.h"

#include "../core/pz_mem.h"

#include <limits.h>
#include <math.h>
#include <string.h>

static size_t
pz_net_bounded_strlen(const char *value, size_t max_len)
{
    if (!value)
        return max_len;

    size_t len = 0;
    while (len < max_len && value[len] != '\0')
        len++;
    return len;
}

static bool
pz_net_valid_input_floats(const pz_net_input *input)
{
    return isfinite(input->move_x) && isfinite(input->move_y)
        && isfinite(input->turret_angle) && isfinite(input->cursor_x)
        && isfinite(input->cursor_y);
}

uint8_t *
pz_net_serialize_input(const pz_net_input *input, size_t *out_len)
{
    if (!input || !out_len || !pz_net_valid_input_floats(input)
        || input->weapon_switch < INT8_MIN || input->weapon_switch > INT8_MAX) {
        return NULL;
    }

    const size_t len = sizeof(pz_net_msg_input);
    pz_net_msg_input *msg = pz_calloc(1, len);
    if (!msg)
        return NULL;

    msg->header.type = PZ_NET_MSG_INPUT;
    msg->header.version = PZ_NET_PROTOCOL_VERSION;
    msg->header.length = (uint16_t)len;
    msg->header.tick = input->sequence;
    msg->last_host_tick = input->last_host_tick;
    msg->action_sequence = input->action_sequence;
    msg->move_x = input->move_x;
    msg->move_y = input->move_y;
    msg->turret_angle = input->turret_angle;
    msg->cursor_x = input->cursor_x;
    msg->cursor_y = input->cursor_y;
    msg->fire_held = input->fire_held ? 1 : 0;
    msg->fire_pressed = input->fire_pressed ? 1 : 0;
    msg->place_mine = input->place_mine ? 1 : 0;
    msg->place_barrier = input->place_barrier ? 1 : 0;
    msg->weapon_switch = (int8_t)input->weapon_switch;

    *out_len = len;
    return (uint8_t *)msg;
}

static bool
pz_net_valid_snapshot_counts(const pz_net_game_state *state)
{
    return state->tank_count >= 0 && state->tank_count <= PZ_NET_MAX_TANKS
        && state->projectile_count >= 0
        && state->projectile_count <= PZ_NET_MAX_PROJECTILES
        && state->powerup_count >= 0
        && state->powerup_count <= PZ_NET_MAX_POWERUPS && state->mine_count >= 0
        && state->mine_count <= PZ_NET_MAX_MINES && state->barrier_count >= 0
        && state->barrier_count <= PZ_NET_MAX_BARRIERS;
}

uint8_t *
pz_net_serialize_snapshot(const pz_net_game_state *state, size_t *out_len)
{
    if (!state || !out_len || !pz_net_valid_snapshot_counts(state))
        return NULL;

    size_t barriers_size = 0;
    size_t barrier_name_lens[PZ_NET_MAX_BARRIERS] = { 0 };
    for (int i = 0; i < state->barrier_count; i++) {
        size_t name_len = pz_net_bounded_strlen(
            state->barrier_tile_names[i], PZ_NET_MAX_BARRIER_NAME + 1);
        if (name_len > PZ_NET_MAX_BARRIER_NAME)
            return NULL;
        barrier_name_lens[i] = name_len;
        barriers_size += sizeof(pz_net_barrier_state) + name_len;
    }

    size_t tanks_size = (size_t)state->tank_count * sizeof(pz_net_tank_state);
    size_t projectiles_size
        = (size_t)state->projectile_count * sizeof(pz_net_projectile_state);
    size_t powerups_size
        = (size_t)state->powerup_count * sizeof(pz_net_powerup_state);
    size_t mines_size = (size_t)state->mine_count * sizeof(pz_net_mine_state);
    size_t total_size = sizeof(pz_net_msg_snapshot) + tanks_size
        + projectiles_size + powerups_size + mines_size + barriers_size;
    if (total_size > UINT16_MAX)
        return NULL;

    uint8_t *buf = pz_calloc(1, total_size);
    if (!buf)
        return NULL;

    pz_net_msg_snapshot *msg = (pz_net_msg_snapshot *)buf;
    msg->header.type = PZ_NET_MSG_SNAPSHOT;
    msg->header.version = PZ_NET_PROTOCOL_VERSION;
    msg->header.length = (uint16_t)total_size;
    msg->header.tick = state->tick;
    msg->last_processed_input = state->last_processed_input;
    msg->last_processed_action = state->last_processed_action;
    msg->tank_count = (uint8_t)state->tank_count;
    msg->projectile_count = (uint8_t)state->projectile_count;
    msg->powerup_count = (uint8_t)state->powerup_count;
    msg->mine_count = (uint8_t)state->mine_count;
    msg->barrier_count = (uint8_t)state->barrier_count;
    msg->local_tank_id = (uint8_t)state->local_tank_id;

    uint8_t *ptr = buf + sizeof(*msg);
    memcpy(ptr, state->tanks, tanks_size);
    ptr += tanks_size;
    memcpy(ptr, state->projectiles, projectiles_size);
    ptr += projectiles_size;
    memcpy(ptr, state->powerups, powerups_size);
    ptr += powerups_size;
    memcpy(ptr, state->mines, mines_size);
    ptr += mines_size;

    for (int i = 0; i < state->barrier_count; i++) {
        pz_net_barrier_state barrier = state->barriers[i];
        size_t name_len = barrier_name_lens[i];
        barrier.tile_name_len = (uint8_t)name_len;
        memcpy(ptr, &barrier, sizeof(barrier));
        ptr += sizeof(barrier);
        memcpy(ptr, state->barrier_tile_names[i], name_len);
        ptr += name_len;
    }

    *out_len = total_size;
    return buf;
}

uint8_t *
pz_net_serialize_event(const pz_net_event *event, size_t *out_len)
{
    if (!event || !out_len || event->type < PZ_NET_EVENT_TANK_DEATH
        || event->type > PZ_NET_EVENT_BARRIER_PLACED
        || event->entity_id < INT8_MIN || event->entity_id > INT8_MAX
        || !isfinite(event->pos_x) || !isfinite(event->pos_y)) {
        return NULL;
    }

    pz_net_msg_event *msg = pz_calloc(1, sizeof(*msg));
    if (!msg)
        return NULL;

    msg->header.type = PZ_NET_MSG_EVENT;
    msg->header.version = PZ_NET_PROTOCOL_VERSION;
    msg->header.length = (uint16_t)sizeof(*msg);
    msg->header.tick = event->tick;
    msg->event_type = (uint8_t)event->type;
    msg->floor_level = event->floor_level;
    msg->extra[0] = event->extra[0];
    msg->extra[1] = event->extra[1];
    msg->pos_x = event->pos_x;
    msg->pos_y = event->pos_y;
    msg->entity_id = (int8_t)event->entity_id;

    *out_len = sizeof(*msg);
    return (uint8_t *)msg;
}

pz_net_msg_type
pz_net_parse_header(
    const uint8_t *data, size_t len, pz_net_msg_header *out_header)
{
    if (!data || len < sizeof(*out_header) || len > UINT16_MAX || !out_header)
        return 0;

    memcpy(out_header, data, sizeof(*out_header));
    if (out_header->version != PZ_NET_PROTOCOL_VERSION
        || out_header->length != len || out_header->type < PZ_NET_MSG_INPUT
        || out_header->type > PZ_NET_MSG_EVENT) {
        return 0;
    }

    return (pz_net_msg_type)out_header->type;
}

bool
pz_net_parse_input(const uint8_t *data, size_t len, pz_net_input *out_input)
{
    if (!data || len != sizeof(pz_net_msg_input) || !out_input)
        return false;

    pz_net_msg_header header;
    if (pz_net_parse_header(data, len, &header) != PZ_NET_MSG_INPUT)
        return false;

    pz_net_msg_input msg;
    memcpy(&msg, data, sizeof(msg));
    pz_net_input parsed = {
        .sequence = header.tick,
        .last_host_tick = msg.last_host_tick,
        .action_sequence = msg.action_sequence,
        .move_x = msg.move_x,
        .move_y = msg.move_y,
        .turret_angle = msg.turret_angle,
        .cursor_x = msg.cursor_x,
        .cursor_y = msg.cursor_y,
        .fire_held = msg.fire_held != 0,
        .fire_pressed = msg.fire_pressed != 0,
        .place_mine = msg.place_mine != 0,
        .place_barrier = msg.place_barrier != 0,
        .weapon_switch = msg.weapon_switch,
    };
    if (!pz_net_valid_input_floats(&parsed))
        return false;

    *out_input = parsed;
    return true;
}

static bool
pz_net_valid_tank_state(const pz_net_tank_state *tank)
{
    return tank->loadout_count <= 8 && isfinite(tank->pos_x)
        && isfinite(tank->pos_y) && isfinite(tank->vel_x)
        && isfinite(tank->vel_y) && isfinite(tank->body_angle)
        && isfinite(tank->turret_angle) && isfinite(tank->jump_timer)
        && isfinite(tank->jump_duration) && isfinite(tank->jump_start_x)
        && isfinite(tank->jump_start_y) && isfinite(tank->jump_end_x)
        && isfinite(tank->jump_end_y) && isfinite(tank->jump_start_angle)
        && isfinite(tank->jump_end_angle) && isfinite(tank->jump_height)
        && isfinite(tank->jump_apex_height);
}

static bool
pz_net_valid_projectile_state(const pz_net_projectile_state *projectile)
{
    return isfinite(projectile->pos_x) && isfinite(projectile->pos_y)
        && isfinite(projectile->vel_x) && isfinite(projectile->vel_y)
        && isfinite(projectile->lifetime) && isfinite(projectile->scale);
}

bool
pz_net_parse_snapshot(
    const uint8_t *data, size_t len, pz_net_game_state *out_state)
{
    if (!data || len < sizeof(pz_net_msg_snapshot) || !out_state)
        return false;

    memset(out_state, 0, sizeof(*out_state));

    pz_net_msg_header header;
    if (pz_net_parse_header(data, len, &header) != PZ_NET_MSG_SNAPSHOT)
        return false;

    pz_net_msg_snapshot msg;
    memcpy(&msg, data, sizeof(msg));
    if (msg.tank_count > PZ_NET_MAX_TANKS
        || msg.projectile_count > PZ_NET_MAX_PROJECTILES
        || msg.powerup_count > PZ_NET_MAX_POWERUPS
        || msg.mine_count > PZ_NET_MAX_MINES
        || msg.barrier_count > PZ_NET_MAX_BARRIERS) {
        return false;
    }

    out_state->tick = header.tick;
    out_state->last_processed_input = msg.last_processed_input;
    out_state->last_processed_action = msg.last_processed_action;
    out_state->local_tank_id = (int8_t)msg.local_tank_id;
    out_state->tank_count = msg.tank_count;
    out_state->projectile_count = msg.projectile_count;
    out_state->powerup_count = msg.powerup_count;
    out_state->mine_count = msg.mine_count;
    out_state->barrier_count = msg.barrier_count;

    const uint8_t *ptr = data + sizeof(msg);
    const uint8_t *end = data + len;

#define PZ_NET_READ_ARRAY(destination, count, type)                            \
    do {                                                                       \
        size_t bytes = (size_t)(count) * sizeof(type);                         \
        if ((size_t)(end - ptr) < bytes)                                       \
            return false;                                                      \
        memcpy((destination), ptr, bytes);                                     \
        ptr += bytes;                                                          \
    } while (0)

    PZ_NET_READ_ARRAY(out_state->tanks, msg.tank_count, pz_net_tank_state);
    PZ_NET_READ_ARRAY(
        out_state->projectiles, msg.projectile_count, pz_net_projectile_state);
    PZ_NET_READ_ARRAY(
        out_state->powerups, msg.powerup_count, pz_net_powerup_state);
    PZ_NET_READ_ARRAY(out_state->mines, msg.mine_count, pz_net_mine_state);

#undef PZ_NET_READ_ARRAY

    for (int i = 0; i < msg.tank_count; i++) {
        if (!pz_net_valid_tank_state(&out_state->tanks[i]))
            return false;
    }
    for (int i = 0; i < msg.projectile_count; i++) {
        if (!pz_net_valid_projectile_state(&out_state->projectiles[i]))
            return false;
    }
    for (int i = 0; i < msg.powerup_count; i++) {
        const pz_net_powerup_state *powerup = &out_state->powerups[i];
        if (!isfinite(powerup->pos_x) || !isfinite(powerup->pos_y)
            || !isfinite(powerup->respawn_timer)) {
            return false;
        }
    }
    for (int i = 0; i < msg.mine_count; i++) {
        const pz_net_mine_state *mine = &out_state->mines[i];
        if (!isfinite(mine->pos_x) || !isfinite(mine->pos_y))
            return false;
    }

    for (int i = 0; i < msg.barrier_count; i++) {
        if ((size_t)(end - ptr) < sizeof(pz_net_barrier_state))
            return false;

        memcpy(&out_state->barriers[i], ptr, sizeof(pz_net_barrier_state));
        ptr += sizeof(pz_net_barrier_state);

        pz_net_barrier_state *barrier = &out_state->barriers[i];
        size_t name_len = barrier->tile_name_len;
        if (name_len > PZ_NET_MAX_BARRIER_NAME || (size_t)(end - ptr) < name_len
            || !isfinite(barrier->pos_x) || !isfinite(barrier->pos_y)
            || !isfinite(barrier->health) || !isfinite(barrier->lifetime)) {
            return false;
        }

        memcpy(out_state->barrier_tile_names[i], ptr, name_len);
        out_state->barrier_tile_names[i][name_len] = '\0';
        ptr += name_len;
    }

    return ptr == end;
}

bool
pz_net_parse_event(const uint8_t *data, size_t len, pz_net_event *out_event)
{
    if (!data || len != sizeof(pz_net_msg_event) || !out_event)
        return false;

    pz_net_msg_header header;
    if (pz_net_parse_header(data, len, &header) != PZ_NET_MSG_EVENT)
        return false;

    pz_net_msg_event msg;
    memcpy(&msg, data, sizeof(msg));
    if (msg.event_type < PZ_NET_EVENT_TANK_DEATH
        || msg.event_type > PZ_NET_EVENT_BARRIER_PLACED || !isfinite(msg.pos_x)
        || !isfinite(msg.pos_y)) {
        return false;
    }

    *out_event = (pz_net_event) {
        .tick = header.tick,
        .type = (pz_net_event_type)msg.event_type,
        .pos_x = msg.pos_x,
        .pos_y = msg.pos_y,
        .entity_id = msg.entity_id,
        .floor_level = msg.floor_level,
        .extra = { msg.extra[0], msg.extra[1] },
    };
    return true;
}
