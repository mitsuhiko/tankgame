/*
 * Tank Game - Main Entry Point
 */

#include <ctype.h>
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "third_party/sokol/sokol_app.h"

#include "core/pz_debug_cmd.h"
#include "core/pz_debug_script.h"
#include "core/pz_log.h"
#include "core/pz_math.h"
#include "core/pz_mem.h"
#include "core/pz_platform.h"
#include "core/pz_sim.h"
#include "core/pz_str.h"
#include "editor/pz_editor.h"
#include "engine/pz_audio.h"
#include "engine/pz_camera.h"
#include "engine/pz_cursor.h"
#include "engine/pz_debug_overlay.h"
#include "engine/pz_font.h"
#include "engine/pz_music.h"
#include "engine/render/pz_renderer.h"
#include "engine/render/pz_texture.h"
#include "game/pz_ai.h"
#include "game/pz_background.h"
#include "game/pz_barrier.h"
#include "game/pz_barrier_placer.h"
#include "game/pz_campaign.h"
#include "game/pz_game_music.h"
#include "game/pz_game_sfx.h"
#include "game/pz_lighting.h"
#include "game/pz_map.h"
#include "game/pz_map_render.h"
#include "game/pz_mesh.h"
#include "game/pz_mine.h"
#include "game/pz_particle.h"
#include "game/pz_powerup.h"
#include "game/pz_projectile.h"
#include "game/pz_spawn_indicator.h"
#include "game/pz_tank.h"
#include "game/pz_tile_registry.h"
#include "game/pz_toxic_cloud.h"
#include "game/pz_tracks.h"
#include "net/pz_net.h"
#include "net/pz_net_protocol.h"
#include "net/pz_net_signaling.h"
#include "net/pz_net_webrtc.h"

#ifdef __EMSCRIPTEN__
#    include <emscripten.h>
#endif

#define WINDOW_TITLE "Tank Game"
#define WINDOW_WIDTH 1280
#define WINDOW_HEIGHT 720
#define SAPP_KEYCODE_COUNT (SAPP_KEYCODE_MENU + 1)

#define NET_INPUT_QUEUE_SIZE 256
#define NET_SNAPSHOT_QUEUE_SIZE 8
#define NET_EVENT_QUEUE_SIZE 64
#define NET_INPUT_HISTORY_SIZE 256

typedef struct net_input_queue {
    pz_net_input entries[NET_INPUT_QUEUE_SIZE];
    atomic_uint write_index;
    atomic_uint read_index;
} net_input_queue;

typedef struct net_snapshot_queue {
    pz_net_game_state entries[NET_SNAPSHOT_QUEUE_SIZE];
    atomic_uint write_index;
    atomic_uint read_index;
} net_snapshot_queue;

typedef struct net_event_queue {
    pz_net_event entries[NET_EVENT_QUEUE_SIZE];
    atomic_uint write_index;
    atomic_uint read_index;
} net_event_queue;

typedef struct net_input_history_entry {
    uint32_t sequence;
    pz_tank_input input;
    bool valid;
} net_input_history_entry;

typedef struct net_tank_interpolation {
    int tank_id;
    bool initialized;
    pz_vec2 start_pos;
    pz_vec2 target_pos;
    float start_body_angle;
    float target_body_angle;
    float start_turret_angle;
    float target_turret_angle;
    float elapsed;
    float duration;
} net_tank_interpolation;

// Generate a timestamped screenshot filename
static char *
generate_screenshot_path(void)
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);

    char filename[128];
    snprintf(filename, sizeof(filename),
        "screenshots/screenshot_%04d%02d%02d_%02d%02d%02d.png",
        t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, t->tm_hour, t->tm_min,
        t->tm_sec);

    return pz_str_dup(filename);
}

static float
track_strength_for_tank(const pz_tank *tank)
{
    float recoil = pz_clampf(tank->recoil, 0.0f, 1.5f);
    return 1.0f + recoil * 0.35f;
}

static void
spawn_tank_fog(
    pz_particle_manager *particle_mgr, pz_tank_manager *tank_mgr, float dt)
{
    if (!particle_mgr || !tank_mgr)
        return;

    float max_speed = tank_mgr->max_speed > 0.0f ? tank_mgr->max_speed : 1.0f;

    for (int i = 0; i < PZ_MAX_TANKS; i++) {
        pz_tank *tank = &tank_mgr->tanks[i];
        if (!(tank->flags & PZ_TANK_FLAG_ACTIVE)
            || (tank->flags & PZ_TANK_FLAG_DEAD)) {
            continue;
        }

        float speed = pz_vec2_len(tank->vel);
        if (speed < 0.15f) {
            tank->idle_time = pz_minf(tank->idle_time + dt, 3.0f);
        } else {
            tank->idle_time = 0.0f;
        }

        float idle_factor = pz_clampf(tank->idle_time / 2.0f, 0.0f, 1.0f);
        float moving_factor
            = pz_clampf(speed / (max_speed * 0.75f), 0.0f, 1.0f);

        float spawn_interval = pz_lerpf(0.25f, 0.08f, moving_factor);
        if (moving_factor < 0.1f) {
            // Idle tanks produce ~30% of the smoke (longer interval)
            spawn_interval = pz_lerpf(0.25f, 0.85f, idle_factor);
        }

        tank->fog_timer -= dt;
        if (tank->fog_timer <= 0.0f) {
            pz_vec2 forward
                = { sinf(tank->body_angle), cosf(tank->body_angle) };
            // Spawn in back half of tank (tank is ~2.5 units long)
            float trail_offset = pz_lerpf(0.9f, 1.25f, moving_factor);
            pz_vec3 fog_pos = { tank->pos.x - forward.x * trail_offset, 0.35f,
                tank->pos.y - forward.y * trail_offset };

            pz_particle_spawn_fog(particle_mgr, fog_pos, idle_factor);
            tank->fog_timer = spawn_interval;
        }
    }
}

static void
spawn_projectile_fog(pz_particle_manager *particle_mgr,
    pz_projectile_manager *projectile_mgr, float dt)
{
    if (!particle_mgr || !projectile_mgr)
        return;

    for (int i = 0; i < PZ_MAX_PROJECTILES; i++) {
        pz_projectile *proj = &projectile_mgr->projectiles[i];
        if (!proj->active)
            continue;

        float speed = pz_vec2_len(proj->velocity);
        float speed_factor = pz_clampf(speed / 12.0f, 0.0f, 1.0f);
        float spawn_interval = pz_lerpf(0.07f, 0.025f, speed_factor);

        proj->fog_timer -= dt;
        if (proj->fog_timer <= 0.0f) {
            pz_vec2 forward = { 0.0f, 1.0f };
            if (speed > 0.001f) {
                forward = pz_vec2_scale(proj->velocity, 1.0f / speed);
            }

            float trail_offset = pz_lerpf(0.12f, 0.18f, speed_factor);
            pz_vec3 fog_pos = { proj->pos.x - forward.x * trail_offset, 0.85f,
                proj->pos.y - forward.y * trail_offset };

            pz_particle_spawn_bullet_fog(particle_mgr, fog_pos);
            proj->fog_timer = spawn_interval;
        }
    }
}

typedef enum {
    EXPLOSION_LIGHT_BULLET, // Bullet impact (blue-white)
    EXPLOSION_LIGHT_TANK, // Tank explosion (orange-red)
    EXPLOSION_LIGHT_MINE, // Mine explosion (yellow)
} explosion_light_type;

typedef struct {
    pz_vec2 pos;
    float timer; // Remaining time
    float duration; // Total duration
    explosion_light_type type;
    int8_t floor_level; // Floor level for multi-floor lighting
} explosion_light;

#define MAX_EXPLOSION_LIGHTS 16
#define MAX_FOG_MARKS 128
#define FOG_MARK_LIFETIME 3.0f
#define FOG_MARK_TANK_MIN_DIST 0.6f
#define FOG_MARK_PROJ_MIN_DIST 0.4f

typedef struct fog_mark {
    bool active;
    pz_vec2 pos;
    float timer;
    float duration;
    float radius;
    float strength;
} fog_mark;

// Game states
typedef enum {
    GAME_STATE_PLAYING,
    GAME_STATE_PLAYER_DEAD, // Player died, waiting for respawn
    GAME_STATE_LEVEL_COMPLETE, // All enemies defeated, waiting for transition
    GAME_STATE_GAME_OVER, // No lives left
    GAME_STATE_CAMPAIGN_COMPLETE, // All levels done
    GAME_STATE_EDITOR, // Map editor active
} game_state;

// Map session - all state that needs to be reset when loading a new map
// This struct helps ensure we don't leak state between map transitions
typedef struct map_session {
    // Map data
    pz_map *map;
    pz_map_renderer *renderer;
    pz_map_hot_reload *hot_reload;
    char map_path[256]; // Current map path for hot-reload

    // Map-specific rendering
    pz_tracks *tracks;
    pz_lighting *lighting;
    pz_toxic_cloud *toxic_cloud;

    // Entities (all cleared on map change)
    pz_tank_manager *tank_mgr;
    pz_tank *player_tank; // Convenience pointer into tank_mgr
    pz_ai_manager *ai_mgr;
    pz_projectile_manager *projectile_mgr;
    pz_particle_manager *particle_mgr;
    pz_powerup_manager *powerup_mgr;
    pz_barrier_manager *barrier_mgr;
    pz_barrier_placer_renderer *barrier_placer_renderer;
    pz_barrier_ghost barrier_ghost; // Ghost preview for barrier placement
    pz_mine_manager *mine_mgr;

    // Map gameplay state
    int initial_enemy_count;
    explosion_light explosion_lights[MAX_EXPLOSION_LIGHTS];

    // Fog disturbance trail
    fog_mark fog_marks[MAX_FOG_MARKS];
    int fog_mark_count;
    pz_vec2 fog_last_tank_pos[PZ_MAX_TANKS];
    bool fog_has_tank_pos[PZ_MAX_TANKS];
    pz_vec2 fog_last_projectile_pos[PZ_MAX_PROJECTILES];
    bool fog_has_projectile_pos[PZ_MAX_PROJECTILES];
} map_session;

typedef struct app_state {
    // Command line args
    const char *lightmap_debug_path;
    const char *map_path_arg;
    const char *campaign_path_arg;
    const char *edit_map_path_arg; // --edit-map <path>
    const char *join_payload_arg; // join <payload>
    bool show_debug_overlay;
    bool show_debug_texture_scale;
    const char *debug_script_path_arg; // --debug-script-file <file>
    const char *inline_script_arg; // --debug-script "commands"

    // Debug script execution (for automated testing, not gameplay scripting)
    // Can be loaded from file, inline string, or injected via command pipe
    pz_debug_script *debug_script;

    // Networking
    bool net_is_host;
    bool net_is_client;
    bool net_share_enabled;
    bool net_use_signaling;
    bool net_waiting_for_offer;
    bool net_waiting_for_answer;
    bool net_signaling_fetch_in_flight;
    bool net_signaling_failed;
    double net_signaling_next_poll;
    char net_room_code[16];
    const char *net_answer_payload_arg;
    atomic_bool net_channel_open;
    bool net_connection_notified;
    net_input_queue net_rx_queue;
    net_snapshot_queue net_snapshot_rx_queue;
    net_event_queue net_event_rx_queue;
    pz_net_input net_last_remote_input;
    bool net_has_remote_input;
    uint32_t net_last_remote_sequence;
    uint32_t net_last_processed_input;
    uint32_t net_last_processed_action;
    uint32_t net_next_input_sequence;
    uint32_t net_next_action_sequence;
    uint32_t net_pending_action_sequence;
    bool net_pending_fire_pressed;
    bool net_pending_place_mine;
    bool net_pending_place_barrier;
    int net_pending_weapon_switch;
    net_input_history_entry net_input_history[NET_INPUT_HISTORY_SIZE];
    net_tank_interpolation net_tank_interp[PZ_NET_MAX_TANKS];
    pz_tank *net_remote_tank;
    int net_remote_tank_id;
    uint32_t net_snapshot_tick; // Last snapshot tick sent/received
    uint32_t net_snapshot_interval; // Ticks between snapshots (host only)
    pz_net_offer *join_offer;
    char *join_answer;
    char *join_answer_json;
    pz_net_webrtc *net_webrtc;

    // Core systems (persistent across maps)
    pz_renderer *renderer;
    pz_texture_manager *tex_manager;
    pz_tile_registry *tile_registry;
    pz_camera camera;
    pz_debug_overlay *debug_overlay;
    pz_cursor *cursor;
    pz_font_manager *font_mgr;
    pz_font *font_russo;
    pz_font *font_caveat;
    pz_spawn_indicator_renderer *spawn_indicator;
    pz_sim *sim;
    pz_audio *audio;
    pz_game_music *game_music;
    pz_game_sfx *game_sfx;

    // Laser rendering (persistent)
    pz_shader_handle laser_shader;
    pz_pipeline_handle laser_pipeline;
    pz_buffer_handle laser_vb;

    // Background rendering (persistent, configured per-map)
    pz_background *background;

    // Campaign system
    pz_campaign_manager *campaign_mgr;

    // Map editor
    pz_editor *editor;

    // Current map session (all map-dependent state)
    map_session session;

    // Game state
    game_state state;
    float state_timer; // Timer for state transitions

    // Frame timing
    int frame_count;
    double last_hot_reload_check;
    double last_frame_time;
    double last_perf_log_time;
    float total_time; // Cumulative time for animations

    // Input state
    float mouse_x;
    float mouse_y;
    bool mouse_left_down;
    bool mouse_left_just_pressed;
    bool mouse_right_just_pressed;
    bool space_down;
    bool space_just_pressed;
    float scroll_accumulator;
    bool key_f_just_pressed;
    bool key_g_just_pressed;
    bool key_down[SAPP_KEYCODE_COUNT];

    // Script cursor override (world coordinates)
    float script_cursor_x;
    float script_cursor_y;
    bool script_cursor_active;
} app_state;

static app_state g_app;

static void
net_input_queue_init(net_input_queue *queue)
{
    if (!queue)
        return;
    atomic_store(&queue->write_index, 0u);
    atomic_store(&queue->read_index, 0u);
}

static bool
net_input_queue_push(net_input_queue *queue, const pz_net_input *input)
{
    if (!queue || !input)
        return false;
    unsigned int write = atomic_load(&queue->write_index);
    unsigned int read = atomic_load(&queue->read_index);
    if (write - read >= NET_INPUT_QUEUE_SIZE)
        return false;
    queue->entries[write % NET_INPUT_QUEUE_SIZE] = *input;
    atomic_store(&queue->write_index, write + 1u);
    return true;
}

static bool
net_input_queue_pop(net_input_queue *queue, pz_net_input *input)
{
    if (!queue || !input)
        return false;
    unsigned int read = atomic_load(&queue->read_index);
    unsigned int write = atomic_load(&queue->write_index);
    if (read >= write)
        return false;
    *input = queue->entries[read % NET_INPUT_QUEUE_SIZE];
    atomic_store(&queue->read_index, read + 1u);
    return true;
}

static void
net_snapshot_queue_init(net_snapshot_queue *queue)
{
    if (!queue)
        return;
    atomic_store(&queue->write_index, 0u);
    atomic_store(&queue->read_index, 0u);
}

static bool
net_snapshot_queue_push(
    net_snapshot_queue *queue, const pz_net_game_state *snapshot)
{
    if (!queue || !snapshot)
        return false;
    unsigned int write = atomic_load(&queue->write_index);
    unsigned int read = atomic_load(&queue->read_index);
    if (write - read >= NET_SNAPSHOT_QUEUE_SIZE)
        return false;
    queue->entries[write % NET_SNAPSHOT_QUEUE_SIZE] = *snapshot;
    atomic_store(&queue->write_index, write + 1u);
    return true;
}

static bool
net_snapshot_queue_pop(net_snapshot_queue *queue, pz_net_game_state *snapshot)
{
    if (!queue || !snapshot)
        return false;
    unsigned int read = atomic_load(&queue->read_index);
    unsigned int write = atomic_load(&queue->write_index);
    if (read >= write)
        return false;
    *snapshot = queue->entries[read % NET_SNAPSHOT_QUEUE_SIZE];
    atomic_store(&queue->read_index, read + 1u);
    return true;
}

static void
net_event_queue_init(net_event_queue *queue)
{
    if (!queue)
        return;
    atomic_store(&queue->write_index, 0u);
    atomic_store(&queue->read_index, 0u);
}

static bool
net_event_queue_push(net_event_queue *queue, const pz_net_event *event)
{
    if (!queue || !event)
        return false;
    unsigned int write = atomic_load(&queue->write_index);
    unsigned int read = atomic_load(&queue->read_index);
    if (write - read >= NET_EVENT_QUEUE_SIZE)
        return false;
    queue->entries[write % NET_EVENT_QUEUE_SIZE] = *event;
    atomic_store(&queue->write_index, write + 1u);
    return true;
}

static bool
net_event_queue_pop(net_event_queue *queue, pz_net_event *event)
{
    if (!queue || !event)
        return false;
    unsigned int read = atomic_load(&queue->read_index);
    unsigned int write = atomic_load(&queue->write_index);
    if (read >= write)
        return false;
    *event = queue->entries[read % NET_EVENT_QUEUE_SIZE];
    atomic_store(&queue->read_index, read + 1u);
    return true;
}

static bool
net_sequence_newer(uint32_t value, uint32_t reference)
{
    return (int32_t)(value - reference) > 0;
}

static void
net_drain_incoming_inputs(void)
{
    pz_net_input input = { 0 };
    while (net_input_queue_pop(&g_app.net_rx_queue, &input)) {
        if (g_app.net_has_remote_input
            && !net_sequence_newer(
                input.sequence, g_app.net_last_remote_sequence)) {
            continue;
        }

        input.move_x = pz_clampf(input.move_x, -1.0f, 1.0f);
        input.move_y = pz_clampf(input.move_y, -1.0f, 1.0f);
        pz_vec2 move = { input.move_x, input.move_y };
        if (pz_vec2_len_sq(move) > 1.0f) {
            move = pz_vec2_normalize(move);
            input.move_x = move.x;
            input.move_y = move.y;
        }
        input.weapon_switch = input.weapon_switch < -1
            ? -1
            : (input.weapon_switch > 1 ? 1 : input.weapon_switch);

        // Preserve edge-triggered actions if several unordered input packets
        // are drained in one frame and a newer state packet overtook them.
        if (g_app.net_has_remote_input
            && net_sequence_newer(g_app.net_last_remote_input.action_sequence,
                g_app.net_last_processed_action)) {
            input.fire_pressed |= g_app.net_last_remote_input.fire_pressed;
            input.place_mine |= g_app.net_last_remote_input.place_mine;
            input.place_barrier |= g_app.net_last_remote_input.place_barrier;
            if (input.weapon_switch == 0) {
                input.weapon_switch = g_app.net_last_remote_input.weapon_switch;
            }
        }

        g_app.net_last_remote_input = input;
        g_app.net_last_remote_sequence = input.sequence;
        g_app.net_has_remote_input = true;
    }
}

static void
net_handle_channel_state(bool open, void *user_data)
{
    (void)user_data;
    atomic_store(&g_app.net_channel_open, open);
    if (open) {
        pz_log(PZ_LOG_INFO, PZ_LOG_CAT_NET, "Data channel open");
    } else {
        pz_log(PZ_LOG_INFO, PZ_LOG_CAT_NET, "Data channel closed");
    }
}

static void
net_handle_message(const uint8_t *data, size_t len, void *user_data)
{
    (void)user_data;
    if (!data || len < sizeof(pz_net_msg_header)) {
        pz_log(
            PZ_LOG_DEBUG, PZ_LOG_CAT_NET, "Message too small: %zu bytes", len);
        return;
    }

    pz_net_msg_header header;
    pz_net_msg_type msg_type = pz_net_parse_header(data, len, &header);

    if (msg_type == PZ_NET_MSG_INPUT && g_app.net_is_host) {
        pz_net_input input;
        if (pz_net_parse_input(data, len, &input)
            && !net_input_queue_push(&g_app.net_rx_queue, &input)) {
            pz_log(PZ_LOG_WARN, PZ_LOG_CAT_NET, "Dropped remote input packet");
        }
    } else if (msg_type == PZ_NET_MSG_SNAPSHOT && g_app.net_is_client) {
        pz_net_game_state snapshot;
        if (!pz_net_parse_snapshot(data, len, &snapshot)) {
            pz_log(PZ_LOG_WARN, PZ_LOG_CAT_NET, "Rejected invalid snapshot");
        } else if (!net_snapshot_queue_push(
                       &g_app.net_snapshot_rx_queue, &snapshot)) {
            pz_log(PZ_LOG_WARN, PZ_LOG_CAT_NET, "Dropped state snapshot");
        }
    } else if (msg_type == PZ_NET_MSG_EVENT && g_app.net_is_client) {
        pz_net_event event;
        if (!pz_net_parse_event(data, len, &event)) {
            pz_log(PZ_LOG_WARN, PZ_LOG_CAT_NET, "Rejected invalid event");
        } else if (!net_event_queue_push(&g_app.net_event_rx_queue, &event)) {
            pz_log(PZ_LOG_WARN, PZ_LOG_CAT_NET, "Dropped network event");
        }
    } else {
        pz_log(PZ_LOG_DEBUG, PZ_LOG_CAT_NET,
            "Ignored message type %d (host=%d, client=%d)", msg_type,
            g_app.net_is_host, g_app.net_is_client);
    }
}

static bool
net_apply_answer_offer(pz_net_offer *answer_offer)
{
    if (!answer_offer || !g_app.net_webrtc)
        return false;

    bool ok
        = pz_net_webrtc_set_remote_answer(g_app.net_webrtc, answer_offer->sdp);
    if (!ok) {
        pz_log(PZ_LOG_ERROR, PZ_LOG_CAT_NET,
            "Failed to apply WebRTC answer payload");
    } else {
        g_app.net_waiting_for_answer = false;
        pz_log(PZ_LOG_INFO, PZ_LOG_CAT_NET, "Applied answer payload from %s",
            answer_offer->host_name);
    }

    return ok;
}

static bool
net_apply_answer_payload(const char *payload)
{
    if (!payload || !g_app.net_webrtc)
        return false;

    pz_net_offer *answer_offer = pz_net_offer_decode_url(payload);
    if (!answer_offer) {
        pz_log(PZ_LOG_ERROR, PZ_LOG_CAT_NET, "Invalid answer payload provided");
        return false;
    }

    bool ok = net_apply_answer_offer(answer_offer);
    pz_net_offer_free(answer_offer);
    return ok;
}

static bool map_session_load(map_session *session, const char *map_path);

static bool
net_setup_client_from_offer(pz_net_offer *offer)
{
    if (!offer)
        return false;
    if (offer->version != PZ_NET_PROTOCOL_VERSION) {
        pz_log(PZ_LOG_ERROR, PZ_LOG_CAT_NET,
            "Incompatible network protocol (remote=%u, local=%u)",
            offer->version, PZ_NET_PROTOCOL_VERSION);
        return false;
    }

    if (g_app.join_offer && g_app.join_offer != offer) {
        pz_net_offer_free(g_app.join_offer);
    }
    g_app.join_offer = offer;

    pz_log(PZ_LOG_INFO, PZ_LOG_CAT_NET, "Join offer loaded: host=%s map=%s",
        g_app.join_offer->host_name, g_app.join_offer->map_name);

    if (!g_app.map_path_arg && g_app.join_offer->map_name[0] != '\0') {
        g_app.map_path_arg = g_app.join_offer->map_name;
    }

    if (g_app.session.map_path[0] != '\0'
        && g_app.join_offer->map_name[0] != '\0'
        && strcmp(g_app.session.map_path, g_app.join_offer->map_name) != 0) {
        if (!map_session_load(&g_app.session, g_app.join_offer->map_name)) {
            pz_log(PZ_LOG_ERROR, PZ_LOG_CAT_NET,
                "Failed to load map from join offer");
            return false;
        }
    }

    if (!g_app.net_webrtc) {
        const char *ice_servers[] = { "stun:stun.l.google.com:19302",
            "stun:stun.cloudflare.com:3478" };
        pz_net_webrtc_config net_config = {
            .ice_servers = ice_servers,
            .ice_server_count
            = (int)(sizeof(ice_servers) / sizeof(ice_servers[0])),
            .enable_logging = true,
        };

        g_app.net_webrtc = pz_net_webrtc_create(&net_config);
        if (!g_app.net_webrtc) {
            pz_log(PZ_LOG_ERROR, PZ_LOG_CAT_NET,
                "Failed to initialize WebRTC for join offer");
            return false;
        }

        pz_net_webrtc_set_message_callback(
            g_app.net_webrtc, net_handle_message, NULL);
        pz_net_webrtc_set_channel_callback(
            g_app.net_webrtc, net_handle_channel_state, NULL);
    }

    if (!pz_net_webrtc_set_remote_offer(
            g_app.net_webrtc, g_app.join_offer->sdp)) {
        pz_log(PZ_LOG_ERROR, PZ_LOG_CAT_NET, "Failed to apply join offer");
        return false;
    }

    char *answer_sdp = pz_net_webrtc_create_answer(g_app.net_webrtc, 10000);
    if (!answer_sdp) {
        pz_log(PZ_LOG_ERROR, PZ_LOG_CAT_NET, "Failed to create WebRTC answer");
        return false;
    }

    pz_net_offer *answer_offer = pz_net_offer_create(PZ_NET_PROTOCOL_VERSION,
        "client", g_app.join_offer->map_name, answer_sdp);
    char *answer_payload = pz_net_offer_encode_url(answer_offer);
    char *answer_json = pz_net_offer_encode_json(answer_offer);
    pz_net_offer_free(answer_offer);
    pz_free(answer_sdp);

    if (!answer_payload || !answer_json) {
        pz_free(answer_payload);
        pz_free(answer_json);
        pz_log(PZ_LOG_ERROR, PZ_LOG_CAT_NET,
            "Failed to encode WebRTC answer payload");
        return false;
    }

    pz_free(g_app.join_answer);
    pz_free(g_app.join_answer_json);
    g_app.join_answer = answer_payload;
    g_app.join_answer_json = answer_json;

    pz_log(PZ_LOG_INFO, PZ_LOG_CAT_NET, "Join answer ready (send to host): %s",
        g_app.join_answer);

    return true;
}

static void
net_signaling_publish_offer_done(bool success, void *user_data)
{
    (void)user_data;
    if (success) {
        pz_log(PZ_LOG_INFO, PZ_LOG_CAT_NET, "Signaling offer published");
    } else {
        pz_log(
            PZ_LOG_ERROR, PZ_LOG_CAT_NET, "Failed to publish signaling offer");
        g_app.net_signaling_failed = true;
    }
}

static void
net_signaling_publish_answer_done(bool success, void *user_data)
{
    (void)user_data;
    if (success) {
        pz_log(PZ_LOG_INFO, PZ_LOG_CAT_NET, "Signaling answer published");
    } else {
        pz_log(
            PZ_LOG_ERROR, PZ_LOG_CAT_NET, "Failed to publish signaling answer");
        g_app.net_signaling_failed = true;
    }
}

static void
net_signaling_answer_received(const char *message, void *user_data)
{
    (void)user_data;
    g_app.net_signaling_fetch_in_flight = false;
    g_app.net_signaling_next_poll = pz_time_now() + 0.75;
    if (!message)
        return;

    pz_net_offer *answer_offer = pz_net_offer_decode_json(message);
    if (!answer_offer) {
        answer_offer = pz_net_offer_decode_url(message);
    }

    if (!answer_offer) {
        pz_log(PZ_LOG_ERROR, PZ_LOG_CAT_NET,
            "Invalid signaling answer payload provided");
        g_app.net_signaling_failed = true;
        return;
    }

    if (!net_apply_answer_offer(answer_offer))
        g_app.net_signaling_failed = true;
    pz_net_offer_free(answer_offer);
}

static void
net_signaling_offer_received(const char *message, void *user_data)
{
    (void)user_data;
    g_app.net_signaling_fetch_in_flight = false;
    g_app.net_signaling_next_poll = pz_time_now() + 0.75;
    if (!message)
        return;

    pz_log(PZ_LOG_INFO, PZ_LOG_CAT_NET, "Signaling offer received (%zu bytes)",
        strlen(message));

    pz_net_offer *offer = pz_net_offer_decode_json(message);
    if (!offer) {
        offer = pz_net_offer_decode_url(message);
    }
    if (!offer) {
        pz_log(PZ_LOG_ERROR, PZ_LOG_CAT_NET,
            "Received invalid signaling offer payload (len=%zu)",
            strlen(message));
        g_app.net_signaling_failed = true;
        return;
    }

    if (!net_setup_client_from_offer(offer)) {
        g_app.net_signaling_failed = true;
        if (g_app.join_offer == offer)
            g_app.join_offer = NULL;
        pz_net_offer_free(offer);
        return;
    }

    g_app.net_waiting_for_offer = false;
    if (g_app.net_room_code[0] != '\0' && g_app.join_answer_json) {
        pz_signaling_publish(g_app.net_room_code, "a", g_app.join_answer_json,
            net_signaling_publish_answer_done, NULL);
    } else if (g_app.net_room_code[0] != '\0') {
        pz_log(PZ_LOG_WARN, PZ_LOG_CAT_NET,
            "No signaling answer payload available to publish");
    }
}

static void
net_signaling_poll(void)
{
    if (g_app.net_signaling_failed || g_app.net_signaling_fetch_in_flight
        || g_app.net_room_code[0] == '\0'
        || pz_time_now() < g_app.net_signaling_next_poll) {
        return;
    }

    if (g_app.net_waiting_for_answer) {
        g_app.net_signaling_fetch_in_flight = true;
        pz_signaling_fetch(
            g_app.net_room_code, "a", net_signaling_answer_received, NULL);
    } else if (g_app.net_waiting_for_offer) {
        g_app.net_signaling_fetch_in_flight = true;
        pz_signaling_fetch(
            g_app.net_room_code, "o", net_signaling_offer_received, NULL);
    }
}

static bool
net_try_consume_pipe_command(const char *commands)
{
    if (!commands)
        return false;

    while (*commands == '\n' || *commands == '\r' || *commands == ' ') {
        commands++;
    }

    const char *prefix = "webrtc_answer";
    size_t prefix_len = strlen(prefix);
    if (strncmp(commands, prefix, prefix_len) != 0) {
        return false;
    }

    const char *payload = commands + prefix_len;
    while (*payload == ' ' || *payload == '\t') {
        payload++;
    }

    if (*payload == '\0') {
        pz_log(PZ_LOG_WARN, PZ_LOG_CAT_NET,
            "webrtc_answer command missing payload");
        return true;
    }

    net_apply_answer_payload(payload);
    return true;
}

static uint32_t
net_send_input(const pz_tank_input *input, pz_vec2 cursor, bool fire_pressed,
    bool place_mine, bool place_barrier, int weapon_switch)
{
    if (!input || !g_app.net_webrtc || !atomic_load(&g_app.net_channel_open)) {
        return 0;
    }

    uint32_t sequence = ++g_app.net_next_input_sequence;
    pz_net_input net_input = {
        .sequence = sequence,
        .last_host_tick = g_app.net_snapshot_tick,
        .action_sequence = g_app.net_pending_action_sequence,
        .move_x = input->move_dir.x,
        .move_y = input->move_dir.y,
        .turret_angle = input->target_turret,
        .cursor_x = cursor.x,
        .cursor_y = cursor.y,
        .fire_held = input->fire,
        .fire_pressed = fire_pressed,
        .place_mine = place_mine,
        .place_barrier = place_barrier,
        .weapon_switch = weapon_switch,
    };

    size_t len = 0;
    uint8_t *data = pz_net_serialize_input(&net_input, &len);
    bool sent = data && pz_net_webrtc_send_game(g_app.net_webrtc, data, len);
    pz_free(data);
    if (!sent)
        return 0;

    net_input_history_entry *history
        = &g_app.net_input_history[sequence % NET_INPUT_HISTORY_SIZE];
    history->sequence = sequence;
    history->input = *input;
    history->valid = true;
    return sequence;
}

static void
net_send_event(pz_net_event_type type, pz_vec2 pos, int entity_id,
    int8_t floor_level, uint8_t extra0, uint8_t extra1)
{
    if (!g_app.net_is_host || !g_app.net_webrtc
        || !atomic_load(&g_app.net_channel_open)) {
        return;
    }

    pz_net_event event = {
        .tick = (uint32_t)pz_sim_tick(g_app.sim),
        .type = type,
        .pos_x = pos.x,
        .pos_y = pos.y,
        .entity_id = entity_id,
        .floor_level = floor_level,
        .extra = { extra0, extra1 },
    };
    size_t len = 0;
    uint8_t *data = pz_net_serialize_event(&event, &len);
    if (data) {
        pz_net_webrtc_send_reliable(g_app.net_webrtc, data, len);
        pz_free(data);
    }
}

// Build a full game state snapshot from current session state
static void
net_build_snapshot(pz_net_game_state *state, uint32_t tick)
{
    memset(state, 0, sizeof(*state));
    state->tick = tick;
    state->last_processed_input = g_app.net_last_processed_input;
    state->last_processed_action = g_app.net_last_processed_action;
    state->local_tank_id = (int8_t)g_app.net_remote_tank_id;

    // Tanks
    if (g_app.session.tank_mgr) {
        pz_tank_manager *mgr = g_app.session.tank_mgr;
        for (int i = 0;
            i < PZ_MAX_TANKS && state->tank_count < PZ_NET_MAX_TANKS; i++) {
            pz_tank *tank = &mgr->tanks[i];
            if (!(tank->flags & PZ_TANK_FLAG_ACTIVE)) {
                continue;
            }
            pz_net_tank_state *ts = &state->tanks[state->tank_count++];
            ts->active = 1;
            ts->flags = (uint8_t)tank->flags;
            ts->id = (int8_t)tank->id;
            ts->health = (int8_t)tank->health;
            ts->pos_x = tank->pos.x;
            ts->pos_y = tank->pos.y;
            ts->vel_x = tank->vel.x;
            ts->vel_y = tank->vel.y;
            ts->body_angle = tank->body_angle;
            ts->turret_angle = tank->turret_angle;
            ts->floor_level = tank->floor_level;
            ts->jump_state = (uint8_t)tank->jump_state;
            ts->jump_timer = tank->jump_timer;
            ts->jump_duration = tank->jump_duration;
            ts->jump_start_x = tank->jump_start_pos.x;
            ts->jump_start_y = tank->jump_start_pos.y;
            ts->jump_end_x = tank->jump_end_pos.x;
            ts->jump_end_y = tank->jump_end_pos.y;
            ts->jump_start_angle = tank->jump_start_angle;
            ts->jump_end_angle = tank->jump_end_angle;
            ts->jump_height = tank->jump_height;
            ts->jump_apex_height = tank->jump_apex_height;
            ts->current_weapon = (uint8_t)pz_tank_get_current_weapon(tank);
            ts->mine_count = (uint8_t)tank->mine_count;
            ts->loadout_count = (uint8_t)tank->loadout_count;
            for (int j = 0; j < tank->loadout_count && j < 8; j++) {
                ts->loadout[j] = (uint8_t)tank->loadout[j];
            }
        }
    }

    // Projectiles
    if (g_app.session.projectile_mgr) {
        pz_projectile_manager *mgr = g_app.session.projectile_mgr;
        for (int i = 0; i < PZ_MAX_PROJECTILES
            && state->projectile_count < PZ_NET_MAX_PROJECTILES;
            i++) {
            pz_projectile *proj = &mgr->projectiles[i];
            if (!proj->active) {
                continue;
            }
            pz_net_projectile_state *ps
                = &state->projectiles[state->projectile_count++];
            ps->active = 1;
            ps->owner_id = (int8_t)proj->owner_id;
            ps->bounces_remaining = (int8_t)proj->bounces_remaining;
            ps->damage = (int8_t)proj->damage;
            ps->pos_x = proj->pos.x;
            ps->pos_y = proj->pos.y;
            ps->vel_x = proj->velocity.x;
            ps->vel_y = proj->velocity.y;
            ps->lifetime = proj->lifetime;
            ps->scale = proj->scale;
            ps->floor_level = proj->floor_level;
            ps->color_r = (uint8_t)(proj->color.x * 255.0f);
            ps->color_g = (uint8_t)(proj->color.y * 255.0f);
            ps->color_b = (uint8_t)(proj->color.z * 255.0f);
        }
    }

    // Powerups
    if (g_app.session.powerup_mgr) {
        pz_powerup_manager *mgr = g_app.session.powerup_mgr;
        for (int i = 0;
            i < PZ_MAX_POWERUPS && state->powerup_count < PZ_NET_MAX_POWERUPS;
            i++) {
            pz_powerup *pu = &mgr->powerups[i];
            if (!pu->active) {
                continue;
            }
            pz_net_powerup_state *pus
                = &state->powerups[state->powerup_count++];
            pus->active = 1;
            pus->collected = pu->collected ? 1 : 0;
            pus->type = (uint8_t)pu->type;
            pus->pos_x = pu->pos.x;
            pus->pos_y = pu->pos.y;
            pus->respawn_timer = pu->respawn_timer;
        }
    }

    // Mines
    if (g_app.session.mine_mgr) {
        pz_mine_manager *mgr = g_app.session.mine_mgr;
        for (int i = 0;
            i < PZ_MAX_MINES && state->mine_count < PZ_NET_MAX_MINES; i++) {
            pz_mine *mine = &mgr->mines[i];
            if (!mine->active) {
                continue;
            }
            pz_net_mine_state *ms = &state->mines[state->mine_count++];
            ms->active = 1;
            ms->owner_id = (int8_t)mine->owner_id;
            ms->armed = mine->arm_timer <= 0.0f ? 1 : 0;
            ms->pos_x = mine->pos.x;
            ms->pos_y = mine->pos.y;
        }
    }

    // Barriers
    if (g_app.session.barrier_mgr) {
        pz_barrier_manager *mgr = g_app.session.barrier_mgr;
        for (int i = 0;
            i < PZ_MAX_BARRIERS && state->barrier_count < PZ_NET_MAX_BARRIERS;
            i++) {
            pz_barrier *barrier = pz_barrier_get(mgr, i);
            if (!barrier || !barrier->active) {
                continue;
            }
            int idx = state->barrier_count++;
            pz_net_barrier_state *bs = &state->barriers[idx];
            bs->active = 1;
            bs->destroyed = barrier->destroyed ? 1 : 0;
            bs->owner_tank_id = (int8_t)barrier->owner_tank_id;
            bs->floor_level = barrier->floor_level;
            bs->pos_x = barrier->pos.x;
            bs->pos_y = barrier->pos.y;
            bs->health = barrier->health;
            bs->lifetime = barrier->lifetime;
            strncpy(state->barrier_tile_names[idx], barrier->tile_name, 31);
            state->barrier_tile_names[idx][31] = '\0';
        }
    }
}

// Send a snapshot to the client (host only)
static void
net_send_snapshot(uint32_t tick)
{
    if (!g_app.net_is_host || !g_app.net_webrtc)
        return;

    if (!atomic_load(&g_app.net_channel_open))
        return;

    pz_net_game_state state;
    net_build_snapshot(&state, tick);

    size_t len = 0;
    uint8_t *data = pz_net_serialize_snapshot(&state, &len);
    if (data) {
        pz_net_webrtc_send_game(g_app.net_webrtc, data, len);
        pz_free(data);
        g_app.net_snapshot_tick = tick;
    }
}

static net_tank_interpolation *
net_get_tank_interpolation(int tank_id)
{
    net_tank_interpolation *free_slot = NULL;
    for (int i = 0; i < PZ_NET_MAX_TANKS; i++) {
        net_tank_interpolation *interp = &g_app.net_tank_interp[i];
        if (interp->initialized && interp->tank_id == tank_id)
            return interp;
        if (!interp->initialized && !free_slot)
            free_slot = interp;
    }
    return free_slot;
}

static void
net_apply_tank_state(pz_tank *tank, const pz_net_tank_state *state)
{
    tank->flags = state->flags;
    tank->health = state->health;
    tank->vel = (pz_vec2) { state->vel_x, state->vel_y };
    tank->floor_level = state->floor_level;
    tank->jump_state = state->jump_state;
    tank->jump_timer = state->jump_timer;
    tank->jump_duration = state->jump_duration;
    tank->jump_start_pos
        = (pz_vec2) { state->jump_start_x, state->jump_start_y };
    tank->jump_end_pos = (pz_vec2) { state->jump_end_x, state->jump_end_y };
    tank->jump_start_angle = state->jump_start_angle;
    tank->jump_end_angle = state->jump_end_angle;
    tank->jump_height = state->jump_height;
    tank->jump_apex_height = state->jump_apex_height;
    tank->mine_count = state->mine_count;
    tank->loadout_count = state->loadout_count;
    for (int i = 0; i < state->loadout_count; i++)
        tank->loadout[i] = state->loadout[i];

    tank->loadout_index = 0;
    for (int i = 0; i < tank->loadout_count; i++) {
        if (tank->loadout[i] == state->current_weapon) {
            tank->loadout_index = i;
            break;
        }
    }
}

// Apply a received snapshot to local game state (client only)
static void
net_apply_snapshot(const pz_net_game_state *state)
{
    if (!g_app.net_is_client || !state)
        return;

    if (g_app.net_snapshot_tick != 0
        && !net_sequence_newer(state->tick, g_app.net_snapshot_tick)) {
        return;
    }

    // Tank IDs and spawn order are authoritative. Rebind the convenience
    // pointers from the snapshot so the client always controls the tank the
    // host assigned, independent of local spawn order.
    if (g_app.session.tank_mgr) {
        pz_tank_manager *mgr = g_app.session.tank_mgr;
        pz_tank *local_tank = pz_tank_get_by_id(mgr, state->local_tank_id);
        if (local_tank)
            g_app.session.player_tank = local_tank;

        for (int i = 0; i < state->tank_count; i++) {
            const pz_net_tank_state *ts = &state->tanks[i];
            if (!ts->active)
                continue;

            pz_tank *tank = pz_tank_get_by_id(mgr, ts->id);
            if (!tank)
                continue;

            bool is_local = ts->id == state->local_tank_id;
            pz_vec2 current_pos = tank->pos;
            float current_body = tank->body_angle;
            float current_turret = tank->turret_angle;
            net_apply_tank_state(tank, ts);

            if (is_local) {
                tank->pos = (pz_vec2) { ts->pos_x, ts->pos_y };
                tank->body_angle = ts->body_angle;
                tank->turret_angle = ts->turret_angle;
            } else {
                g_app.net_remote_tank = tank;
                g_app.net_remote_tank_id = tank->id;
                net_tank_interpolation *interp
                    = net_get_tank_interpolation(tank->id);
                if (!interp || !interp->initialized) {
                    tank->pos = (pz_vec2) { ts->pos_x, ts->pos_y };
                    tank->body_angle = ts->body_angle;
                    tank->turret_angle = ts->turret_angle;
                    current_pos = tank->pos;
                    current_body = tank->body_angle;
                    current_turret = tank->turret_angle;
                }
                if (interp) {
                    uint32_t tick_delta = g_app.net_snapshot_tick == 0
                        ? g_app.net_snapshot_interval
                        : state->tick - g_app.net_snapshot_tick;
                    interp->tank_id = tank->id;
                    interp->initialized = true;
                    interp->start_pos = current_pos;
                    interp->target_pos = (pz_vec2) { ts->pos_x, ts->pos_y };
                    interp->start_body_angle = current_body;
                    interp->target_body_angle = ts->body_angle;
                    interp->start_turret_angle = current_turret;
                    interp->target_turret_angle = ts->turret_angle;
                    interp->elapsed = 0.0f;
                    interp->duration = pz_clampf(
                        (float)tick_delta * pz_sim_dt(), 0.033f, 0.15f);
                }
            }
        }
    }

    // Apply projectile states - clear all and recreate
    if (g_app.session.projectile_mgr) {
        pz_projectile_manager *mgr = g_app.session.projectile_mgr;
        // Mark all projectiles inactive
        for (int i = 0; i < PZ_MAX_PROJECTILES; i++) {
            mgr->projectiles[i].active = false;
        }
        mgr->active_count = 0;

        // Recreate from snapshot
        for (int i = 0; i < state->projectile_count; i++) {
            const pz_net_projectile_state *ps = &state->projectiles[i];
            if (!ps->active || i >= PZ_MAX_PROJECTILES)
                continue;

            pz_projectile *proj = &mgr->projectiles[i];
            proj->active = true;
            proj->owner_id = ps->owner_id;
            proj->bounces_remaining = ps->bounces_remaining;
            proj->damage = ps->damage;
            proj->pos.x = ps->pos_x;
            proj->pos.y = ps->pos_y;
            proj->velocity.x = ps->vel_x;
            proj->velocity.y = ps->vel_y;
            proj->speed = pz_vec2_len(proj->velocity);
            proj->lifetime = ps->lifetime;
            proj->scale = ps->scale;
            proj->floor_level = ps->floor_level;
            proj->color.x = ps->color_r / 255.0f;
            proj->color.y = ps->color_g / 255.0f;
            proj->color.z = ps->color_b / 255.0f;
            proj->color.w = 1.0f;
            proj->age = 0.5f; // Past self-damage grace period
            proj->bounce_cooldown = 0.0f;
            proj->fog_timer = 0.0f;
            mgr->active_count++;
        }
    }

    // Apply powerup states
    if (g_app.session.powerup_mgr) {
        pz_powerup_manager *mgr = g_app.session.powerup_mgr;
        for (int i = 0; i < state->powerup_count && i < PZ_MAX_POWERUPS; i++) {
            const pz_net_powerup_state *pus = &state->powerups[i];
            if (!pus->active)
                continue;

            // Find matching powerup by position (powerups don't move)
            for (int j = 0; j < PZ_MAX_POWERUPS; j++) {
                pz_powerup *pu = &mgr->powerups[j];
                if (!pu->active)
                    continue;
                float dx = pu->pos.x - pus->pos_x;
                float dy = pu->pos.y - pus->pos_y;
                if (dx * dx + dy * dy < 0.1f) {
                    // Match found - apply collected state
                    pu->collected = pus->collected != 0;
                    pu->respawn_timer = pus->respawn_timer;
                    break;
                }
            }
        }
    }

    // Apply mine states - clear and recreate
    if (g_app.session.mine_mgr) {
        pz_mine_manager *mgr = g_app.session.mine_mgr;
        for (int i = 0; i < PZ_MAX_MINES; i++)
            mgr->mines[i].active = false;
        mgr->active_count = 0;

        for (int i = 0; i < state->mine_count && i < PZ_MAX_MINES; i++) {
            const pz_net_mine_state *ms = &state->mines[i];
            if (!ms->active)
                continue;

            pz_mine *mine = &mgr->mines[i];
            mine->active = true;
            mine->pos = (pz_vec2) { ms->pos_x, ms->pos_y };
            mine->owner_id = ms->owner_id;
            mine->arm_timer = ms->armed ? 0.0f : 0.1f;
            mine->owner_left_safe_zone = true;
            mine->bob_offset = (float)(i % 10) * 0.1f;
            mine->rotation = 0.0f;
            mgr->active_count++;
        }
    }

    // Barriers are authoritative too. Rebuild the compact active list without
    // calling the noisy gameplay spawn API on every snapshot.
    if (g_app.session.barrier_mgr) {
        pz_barrier_manager *mgr = g_app.session.barrier_mgr;
        pz_barrier_clear(mgr);
        for (int i = 0; i < state->barrier_count && i < PZ_MAX_BARRIERS; i++) {
            const pz_net_barrier_state *bs = &state->barriers[i];
            if (!bs->active)
                continue;

            pz_barrier *barrier = &mgr->barriers[i];
            barrier->active = true;
            barrier->destroyed = bs->destroyed != 0;
            barrier->owner_tank_id = bs->owner_tank_id;
            barrier->floor_level = bs->floor_level;
            barrier->pos = (pz_vec2) { bs->pos_x, bs->pos_y };
            barrier->health = bs->health;
            barrier->max_health = bs->health;
            barrier->lifetime = bs->lifetime;
            barrier->max_lifetime = bs->lifetime;
            barrier->tint_color = (pz_vec4) { 1, 1, 1, 1 };
            pz_tank *owner = pz_tank_get_by_id(
                g_app.session.tank_mgr, barrier->owner_tank_id);
            if (owner)
                barrier->tint_color = owner->body_color;
            strncpy(barrier->tile_name, state->barrier_tile_names[i],
                sizeof(barrier->tile_name) - 1);
            barrier->tile_name[sizeof(barrier->tile_name) - 1] = '\0';
            mgr->active_count++;
        }
    }

    if (g_app.net_pending_action_sequence != 0
        && !net_sequence_newer(
            g_app.net_pending_action_sequence, state->last_processed_action)) {
        g_app.net_pending_action_sequence = 0;
        g_app.net_pending_fire_pressed = false;
        g_app.net_pending_place_mine = false;
        g_app.net_pending_place_barrier = false;
        g_app.net_pending_weapon_switch = 0;
    }

    // Reconcile the predicted local tank: start from the acknowledged host
    // state, discard confirmed inputs, then replay only unacknowledged inputs.
    for (int i = 0; i < NET_INPUT_HISTORY_SIZE; i++) {
        net_input_history_entry *entry = &g_app.net_input_history[i];
        if (entry->valid
            && !net_sequence_newer(
                entry->sequence, state->last_processed_input)) {
            entry->valid = false;
        }
    }
    if (g_app.session.player_tank
        && !net_sequence_newer(
            state->last_processed_input, g_app.net_next_input_sequence)) {
        uint32_t pending
            = g_app.net_next_input_sequence - state->last_processed_input;
        if (pending > NET_INPUT_HISTORY_SIZE)
            pending = NET_INPUT_HISTORY_SIZE;
        uint32_t first = g_app.net_next_input_sequence - pending + 1;
        for (uint32_t sequence = first;
            pending > 0 && sequence <= g_app.net_next_input_sequence;
            sequence++) {
            net_input_history_entry *entry
                = &g_app.net_input_history[sequence % NET_INPUT_HISTORY_SIZE];
            if (entry->valid && entry->sequence == sequence) {
                pz_tank_update(g_app.session.tank_mgr,
                    g_app.session.player_tank, &entry->input, g_app.session.map,
                    NULL, pz_sim_dt());
            }
        }
    }

    g_app.net_snapshot_tick = state->tick;
    g_app.net_last_processed_input = state->last_processed_input;
    g_app.net_last_processed_action = state->last_processed_action;
}

static void
net_client_visual_update(float frame_dt)
{
    if (!g_app.net_is_client)
        return;

    for (int i = 0; i < PZ_NET_MAX_TANKS; i++) {
        net_tank_interpolation *interp = &g_app.net_tank_interp[i];
        if (!interp->initialized || interp->duration <= 0.0f)
            continue;
        pz_tank *tank
            = pz_tank_get_by_id(g_app.session.tank_mgr, interp->tank_id);
        if (!tank || tank == g_app.session.player_tank)
            continue;

        interp->elapsed = pz_minf(interp->elapsed + frame_dt, interp->duration);
        float t = interp->elapsed / interp->duration;
        tank->pos = pz_vec2_lerp(interp->start_pos, interp->target_pos, t);
        float body_delta = remainderf(
            interp->target_body_angle - interp->start_body_angle, 2.0f * PZ_PI);
        float turret_delta = remainderf(
            interp->target_turret_angle - interp->start_turret_angle,
            2.0f * PZ_PI);
        tank->body_angle = interp->start_body_angle + body_delta * t;
        tank->turret_angle = interp->start_turret_angle + turret_delta * t;
    }

    // Projectiles have no stable network ID yet; short extrapolation between
    // authoritative snapshots is smoother and is corrected every snapshot.
    if (g_app.session.projectile_mgr) {
        for (int i = 0; i < PZ_MAX_PROJECTILES; i++) {
            pz_projectile *projectile
                = &g_app.session.projectile_mgr->projectiles[i];
            if (projectile->active) {
                projectile->pos = pz_vec2_add(projectile->pos,
                    pz_vec2_scale(projectile->velocity, frame_dt));
            }
        }
    }
}

static void
net_process_received_events(void)
{
    pz_net_event event;
    while (net_event_queue_pop(&g_app.net_event_rx_queue, &event)) {
        pz_vec3 position = { event.pos_x, 0.6f, event.pos_y };
        if (event.type == PZ_NET_EVENT_PROJECTILE_HIT) {
            pz_smoke_config smoke = event.extra[0] == PZ_HIT_TANK
                    || event.extra[0] == PZ_HIT_TANK_NON_FATAL
                ? PZ_SMOKE_TANK_HIT
                : PZ_SMOKE_BULLET_IMPACT;
            smoke.position = position;
            pz_particle_spawn_smoke(g_app.session.particle_mgr, &smoke);
            if (event.extra[0] == PZ_HIT_PROJECTILE)
                pz_game_sfx_play_bullet_hit(g_app.game_sfx);
            else if (event.extra[0] == PZ_HIT_TANK_NON_FATAL)
                pz_game_sfx_play_tank_hit(g_app.game_sfx);
            else if (event.extra[0] == PZ_HIT_WALL_RICOCHET)
                pz_game_sfx_play_ricochet(g_app.game_sfx);
        } else if (event.type == PZ_NET_EVENT_MINE_EXPLOSION
            || event.type == PZ_NET_EVENT_TANK_DEATH) {
            pz_smoke_config smoke = PZ_SMOKE_TANK_EXPLOSION;
            smoke.position = position;
            pz_particle_spawn_smoke(g_app.session.particle_mgr, &smoke);
            pz_game_sfx_play_tank_explosion(g_app.game_sfx, false);
        } else if (event.type == PZ_NET_EVENT_POWERUP_COLLECT
            || event.type == PZ_NET_EVENT_BARRIER_PLACED) {
            pz_game_sfx_play_plop(g_app.game_sfx);
        } else if (event.type == PZ_NET_EVENT_GUNFIRE) {
            pz_game_sfx_play_gunfire(g_app.game_sfx);
        } else if (event.type == PZ_NET_EVENT_JUMP) {
            pz_game_sfx_play_jump_pad(g_app.game_sfx);
        }

        explosion_light_type light_type = EXPLOSION_LIGHT_BULLET;
        float duration = 0.15f;
        if (event.type == PZ_NET_EVENT_MINE_EXPLOSION) {
            light_type = EXPLOSION_LIGHT_MINE;
            duration = 0.35f;
        } else if (event.type == PZ_NET_EVENT_TANK_DEATH) {
            light_type = EXPLOSION_LIGHT_TANK;
            duration = 0.4f;
        }
        if (event.type == PZ_NET_EVENT_PROJECTILE_HIT
            || event.type == PZ_NET_EVENT_MINE_EXPLOSION
            || event.type == PZ_NET_EVENT_TANK_DEATH) {
            for (int i = 0; i < MAX_EXPLOSION_LIGHTS; i++) {
                explosion_light *light = &g_app.session.explosion_lights[i];
                if (light->timer <= 0.0f) {
                    light->pos = (pz_vec2) { event.pos_x, event.pos_y };
                    light->type = light_type;
                    light->duration = duration;
                    light->timer = duration;
                    light->floor_level = event.floor_level;
                    break;
                }
            }
        }
    }
}

static bool
app_editor_active(void)
{
    return g_app.state == GAME_STATE_EDITOR && g_app.editor;
}

static const float LASER_WIDTH = 0.08f;
static const float LASER_MAX_DIST = 50.0f;

// Forward declarations
static void map_session_unload(map_session *session);
static void fog_marks_clear(map_session *session);
static void audio_callback(
    float *buffer, int num_frames, int num_channels, void *userdata);

// ============================================================================
// Map Session Management
// ============================================================================

// Unload all map-dependent state
static void
map_session_unload(map_session *session)
{
    if (!session) {
        return;
    }

    // Destroy entity managers
    pz_ai_manager_destroy(session->ai_mgr);
    session->ai_mgr = NULL;

    pz_powerup_manager_destroy(session->powerup_mgr, g_app.renderer);
    session->powerup_mgr = NULL;

    pz_barrier_manager_destroy(session->barrier_mgr, g_app.renderer);
    session->barrier_mgr = NULL;

    pz_barrier_placer_renderer_destroy(
        session->barrier_placer_renderer, g_app.renderer);
    session->barrier_placer_renderer = NULL;

    pz_mine_manager_destroy(session->mine_mgr, g_app.renderer);
    session->mine_mgr = NULL;

    pz_particle_manager_destroy(session->particle_mgr, g_app.renderer);
    session->particle_mgr = NULL;

    pz_toxic_cloud_destroy(session->toxic_cloud);
    session->toxic_cloud = NULL;

    pz_projectile_manager_destroy(session->projectile_mgr, g_app.renderer);
    session->projectile_mgr = NULL;

    pz_tank_manager_destroy(session->tank_mgr, g_app.renderer);
    session->tank_mgr = NULL;
    session->player_tank = NULL;
    g_app.net_remote_tank = NULL;

    // Destroy map rendering
    pz_lighting_destroy(session->lighting);
    session->lighting = NULL;

    pz_tracks_destroy(session->tracks);
    session->tracks = NULL;

    pz_map_hot_reload_destroy(session->hot_reload);
    session->hot_reload = NULL;

    pz_map_renderer_destroy(session->renderer);
    session->renderer = NULL;

    pz_map_destroy(session->map);
    session->map = NULL;

    // Clear remaining state
    session->map_path[0] = '\0';
    session->initial_enemy_count = 0;
    memset(session->explosion_lights, 0, sizeof(session->explosion_lights));

    pz_log(PZ_LOG_INFO, PZ_LOG_CAT_GAME, "Map session unloaded");
}

// Load a new map and set up all map-dependent state
static bool
map_session_load(map_session *session, const char *map_path)
{
    if (!session || !map_path) {
        return false;
    }

    // Unload any existing session first
    map_session_unload(session);

    net_input_queue_init(&g_app.net_rx_queue);
    net_snapshot_queue_init(&g_app.net_snapshot_rx_queue);
    net_event_queue_init(&g_app.net_event_rx_queue);
    g_app.net_has_remote_input = false;
    g_app.net_last_remote_input = (pz_net_input) { 0 };
    g_app.net_last_remote_sequence = 0;
    g_app.net_last_processed_input = 0;
    g_app.net_last_processed_action = 0;
    g_app.net_next_input_sequence = 0;
    g_app.net_next_action_sequence = 0;
    g_app.net_pending_action_sequence = 0;
    g_app.net_pending_fire_pressed = false;
    g_app.net_pending_place_mine = false;
    g_app.net_pending_place_barrier = false;
    g_app.net_pending_weapon_switch = 0;
    memset(g_app.net_input_history, 0, sizeof(g_app.net_input_history));
    memset(g_app.net_tank_interp, 0, sizeof(g_app.net_tank_interp));

    // Store path for hot-reload
    strncpy(session->map_path, map_path, sizeof(session->map_path) - 1);
    session->map_path[sizeof(session->map_path) - 1] = '\0';

    // Load map
    session->map = pz_map_load(map_path);
    if (!session->map) {
        pz_log(
            PZ_LOG_ERROR, PZ_LOG_CAT_GAME, "Failed to load map: %s", map_path);
        return false;
    }

    // Fit camera to map
    pz_camera_fit_map(&g_app.camera, session->map->world_width,
        session->map->world_height, 20.0f);

    // Configure background from map settings
    if (g_app.background) {
        pz_background_set_from_map(g_app.background, session->map);
    }

    if (g_app.game_music) {
        if (session->map->has_music) {
            pz_game_music_load(g_app.game_music, session->map->music_name);
        } else {
            pz_game_music_stop(g_app.game_music);
        }
    }

    // Set tile registry on map for property lookups
    pz_map_set_tile_registry(session->map, g_app.tile_registry);

    if (session->map->has_toxic_cloud) {
        session->toxic_cloud
            = pz_toxic_cloud_create(&session->map->toxic_config,
                session->map->world_width, session->map->world_height);
    }

    // Create map renderer with tile registry
    session->renderer = pz_map_renderer_create(
        g_app.renderer, g_app.tex_manager, g_app.tile_registry);
    if (session->renderer) {
        pz_map_renderer_set_map(session->renderer, session->map);

        // Apply debug texture scale if requested via command line
        if (g_app.show_debug_texture_scale) {
            pz_map_renderer_set_debug_texture_scale(session->renderer, true);
        }
    }

    // Set up hot-reload
    session->hot_reload
        = pz_map_hot_reload_create(map_path, &session->map, session->renderer);

    // Create tracks system
    pz_tracks_config track_config = {
        .world_width = session->map->world_width,
        .world_height = session->map->world_height,
        .texture_size = 1024,
    };
    session->tracks
        = pz_tracks_create(g_app.renderer, g_app.tex_manager, &track_config);

    // Create lighting system
    const pz_map_lighting *map_light = pz_map_get_lighting(session->map);
    pz_lighting_config light_config = {
        .world_width = session->map->world_width,
        .world_height = session->map->world_height,
        .texture_size = 512,
        .ambient = map_light->ambient_color,
    };
    session->lighting = pz_lighting_create(g_app.renderer, &light_config);
    if (session->lighting) {
        pz_lighting_set_map_occluders(session->lighting, session->map);
    }

    // Create entity managers
    session->tank_mgr = pz_tank_manager_create(g_app.renderer, NULL);
    session->projectile_mgr = pz_projectile_manager_create(g_app.renderer);
    session->particle_mgr = pz_particle_manager_create(g_app.renderer);
    session->powerup_mgr = pz_powerup_manager_create(g_app.renderer);
    session->barrier_mgr = pz_barrier_manager_create(
        g_app.renderer, g_app.tile_registry, session->map->tile_size);
    session->barrier_placer_renderer = pz_barrier_placer_renderer_create(
        g_app.renderer, session->map->tile_size);
    memset(&session->barrier_ghost, 0, sizeof(session->barrier_ghost));
    session->mine_mgr = pz_mine_manager_create(g_app.renderer);

    bool net_active = g_app.net_is_host || g_app.net_is_client;

    // Spawn player at first spawn point
    pz_vec2 player_spawn_pos = { 0.0f, 0.0f };
    int spawn_count = pz_map_get_spawn_count(session->map);
    if (spawn_count > 0) {
        const pz_spawn_point *sp = pz_map_get_spawn(session->map, 0);
        if (sp) {
            player_spawn_pos = sp->pos;
        }
    }

    pz_vec2 remote_spawn_pos
        = { player_spawn_pos.x + 2.0f, player_spawn_pos.y + 2.0f };
    if (spawn_count > 1) {
        const pz_spawn_point *sp = pz_map_get_spawn(session->map, 1);
        if (sp) {
            remote_spawn_pos = sp->pos;
        }
    }

    g_app.net_remote_tank = NULL;
    if (net_active) {
        // Spawn in the same authoritative order on every peer: host is tank 1
        // at spawn 1, client is tank 2 at spawn 2. Convenience pointers differ
        // by role, but IDs never do.
        pz_vec4 host_color = { 0.2f, 0.4f, 0.9f, 1.0f };
        pz_vec4 client_color = { 0.9f, 0.3f, 0.2f, 1.0f };
        pz_tank *host_tank = pz_tank_spawn(
            session->tank_mgr, player_spawn_pos, host_color, true);
        pz_tank *client_tank = pz_tank_spawn(
            session->tank_mgr, remote_spawn_pos, client_color, true);

        if (host_tank) {
            pz_tank_update_floor_level(host_tank, session->map);
            host_tank->spawn_floor_level = host_tank->floor_level;
        }
        if (client_tank) {
            pz_tank_update_floor_level(client_tank, session->map);
            client_tank->spawn_floor_level = client_tank->floor_level;
        }

        session->player_tank = g_app.net_is_host ? host_tank : client_tank;
        g_app.net_remote_tank = g_app.net_is_host ? client_tank : host_tank;
        if (g_app.net_remote_tank)
            g_app.net_remote_tank_id = g_app.net_remote_tank->id;
    } else {
        session->player_tank = pz_tank_spawn(session->tank_mgr,
            player_spawn_pos, (pz_vec4) { 0.2f, 0.4f, 0.9f, 1.0f }, true);
        if (session->player_tank) {
            pz_tank_update_floor_level(session->player_tank, session->map);
            session->player_tank->spawn_floor_level
                = session->player_tank->floor_level;
        }
    }

    // Create AI manager and spawn enemies
    int enemy_count = 0;
    if (!net_active) {
        session->ai_mgr = pz_ai_manager_create(session->tank_mgr, session->map);
        enemy_count = pz_map_get_enemy_count(session->map);
        for (int i = 0; i < enemy_count; i++) {
            const pz_enemy_spawn *es = pz_map_get_enemy(session->map, i);
            if (es) {
                pz_ai_spawn_enemy(session->ai_mgr, es->pos, es->angle,
                    (pz_enemy_type)es->type);
            }
        }
    } else {
        session->ai_mgr = NULL;
        enemy_count = 0;
    }
    session->initial_enemy_count = enemy_count;

    // Spawn powerups from map data
    int powerup_count = pz_map_get_powerup_count(session->map);
    for (int i = 0; i < powerup_count; i++) {
        const pz_powerup_spawn *ps = pz_map_get_powerup(session->map, i);
        if (ps) {
            pz_powerup_type type = pz_powerup_type_from_name(ps->type_name);
            if (type == PZ_POWERUP_BARRIER_PLACER) {
                // Barrier placer needs extra config
                pz_powerup_add_barrier_placer(session->powerup_mgr, ps->pos,
                    ps->respawn_time, ps->barrier_tile, ps->barrier_health,
                    ps->barrier_count, ps->barrier_lifetime);
            } else if (type != PZ_POWERUP_NONE) {
                pz_powerup_add(
                    session->powerup_mgr, ps->pos, type, ps->respawn_time);
            } else {
                pz_log(PZ_LOG_WARN, PZ_LOG_CAT_GAME, "Unknown powerup type: %s",
                    ps->type_name);
            }
        }
    }

    // Spawn barriers from map data
    int barrier_count = pz_map_get_barrier_count(session->map);
    for (int i = 0; i < barrier_count; i++) {
        const pz_barrier_spawn *bs = pz_map_get_barrier(session->map, i);
        if (bs) {
            pz_barrier_add(session->barrier_mgr, bs->pos, bs->tile_name,
                bs->health, bs->floor_level);
        }
    }

    // Clear explosion lights
    memset(session->explosion_lights, 0, sizeof(session->explosion_lights));
    fog_marks_clear(session);

    pz_log(PZ_LOG_INFO, PZ_LOG_CAT_GAME, "Map session loaded: %s (%d enemies)",
        map_path, enemy_count);

    return true;
}

// Reset the current map (respawn enemies, reset player position)
static void
map_session_reset(map_session *session)
{
    if (!session || !session->map) {
        return;
    }

    // Clear projectiles
    if (session->projectile_mgr) {
        for (int i = 0; i < PZ_MAX_PROJECTILES; i++) {
            session->projectile_mgr->projectiles[i].active = false;
        }
        session->projectile_mgr->active_count = 0;
    }

    // Clear particles
    if (session->particle_mgr) {
        pz_particle_clear(session->particle_mgr);
    }

    // Reset player (respawn also resets loadout)
    if (session->player_tank) {
        pz_tank_respawn(session->player_tank);
        session->player_tank->mine_count = PZ_MINE_MAX_PER_TANK;
        if (session->toxic_cloud) {
            session->player_tank->toxic_grace_timer
                = session->toxic_cloud->config.grace_period;
            session->player_tank->toxic_damage_timer
                = session->toxic_cloud->config.damage_interval;
        }
    }

    if (g_app.net_remote_tank) {
        pz_tank_respawn(g_app.net_remote_tank);
        g_app.net_remote_tank->mine_count = PZ_MINE_MAX_PER_TANK;
        if (session->toxic_cloud) {
            g_app.net_remote_tank->toxic_grace_timer
                = session->toxic_cloud->config.grace_period;
            g_app.net_remote_tank->toxic_damage_timer
                = session->toxic_cloud->config.damage_interval;
        }
    }

    // Clear and respawn enemies
    if (session->ai_mgr && session->tank_mgr) {
        // Clear AI controllers
        session->ai_mgr->controller_count = 0;

        // Remove all non-player tanks
        for (int i = 0; i < PZ_MAX_TANKS; i++) {
            pz_tank *tank = &session->tank_mgr->tanks[i];
            if ((tank->flags & PZ_TANK_FLAG_ACTIVE)
                && !(tank->flags & PZ_TANK_FLAG_PLAYER)) {
                tank->flags = 0;
                session->tank_mgr->tank_count--;
            }
        }

        // Respawn enemies from map
        int enemy_count = pz_map_get_enemy_count(session->map);
        for (int i = 0; i < enemy_count; i++) {
            const pz_enemy_spawn *es = pz_map_get_enemy(session->map, i);
            if (es) {
                pz_ai_spawn_enemy(session->ai_mgr, es->pos, es->angle,
                    (pz_enemy_type)es->type);
            }
        }
    }

    // Reset powerups (clear and respawn from map)
    if (session->powerup_mgr) {
        // Clear all powerups
        for (int i = 0; i < PZ_MAX_POWERUPS; i++) {
            session->powerup_mgr->powerups[i].active = false;
        }
        session->powerup_mgr->active_count = 0;

        // Respawn from map
        int powerup_count = pz_map_get_powerup_count(session->map);
        for (int i = 0; i < powerup_count; i++) {
            const pz_powerup_spawn *ps = pz_map_get_powerup(session->map, i);
            if (ps) {
                pz_powerup_type type = pz_powerup_type_from_name(ps->type_name);
                if (type != PZ_POWERUP_NONE) {
                    pz_powerup_add(
                        session->powerup_mgr, ps->pos, type, ps->respawn_time);
                }
            }
        }
    }

    // Reset barriers (clear and respawn from map)
    if (session->barrier_mgr) {
        pz_barrier_clear(session->barrier_mgr);

        // Respawn from map
        int barrier_count = pz_map_get_barrier_count(session->map);
        for (int i = 0; i < barrier_count; i++) {
            const pz_barrier_spawn *bs = pz_map_get_barrier(session->map, i);
            if (bs) {
                pz_barrier_add(session->barrier_mgr, bs->pos, bs->tile_name,
                    bs->health, bs->floor_level);
            }
        }
    }

    // Clear mines
    if (session->mine_mgr) {
        pz_mine_clear_all(session->mine_mgr);
    }

    // Clear explosion lights
    memset(session->explosion_lights, 0, sizeof(session->explosion_lights));

    // Clear tracks
    if (session->tracks) {
        pz_tracks_clear(session->tracks);
    }

    if (session->toxic_cloud) {
        pz_toxic_cloud_destroy(session->toxic_cloud);
        session->toxic_cloud = NULL;
    }
    if (session->map->has_toxic_cloud) {
        session->toxic_cloud
            = pz_toxic_cloud_create(&session->map->toxic_config,
                session->map->world_width, session->map->world_height);
    }

    fog_marks_clear(session);

    pz_log(PZ_LOG_INFO, PZ_LOG_CAT_GAME, "Map session reset");
}

static void
fog_marks_clear(map_session *session)
{
    if (!session) {
        return;
    }

    for (int i = 0; i < MAX_FOG_MARKS; i++) {
        session->fog_marks[i].active = false;
    }
    session->fog_mark_count = 0;

    for (int i = 0; i < PZ_MAX_TANKS; i++) {
        session->fog_has_tank_pos[i] = false;
    }
    for (int i = 0; i < PZ_MAX_PROJECTILES; i++) {
        session->fog_has_projectile_pos[i] = false;
    }
}

static void
fog_marks_update(map_session *session, float dt)
{
    if (!session || session->fog_mark_count == 0) {
        return;
    }

    int active_count = 0;
    for (int i = 0; i < MAX_FOG_MARKS; i++) {
        fog_mark *mark = &session->fog_marks[i];
        if (!mark->active) {
            continue;
        }

        mark->timer -= dt;
        if (mark->timer <= 0.0f) {
            mark->active = false;
            continue;
        }

        active_count++;
    }

    session->fog_mark_count = active_count;
}

static void
fog_marks_add(map_session *session, pz_vec2 pos, float radius, float strength)
{
    if (!session) {
        return;
    }

    int slot = -1;
    for (int i = 0; i < MAX_FOG_MARKS; i++) {
        if (!session->fog_marks[i].active) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        float lowest_timer = 9999.0f;
        for (int i = 0; i < MAX_FOG_MARKS; i++) {
            if (session->fog_marks[i].timer < lowest_timer) {
                lowest_timer = session->fog_marks[i].timer;
                slot = i;
            }
        }
    }

    if (slot < 0) {
        return;
    }

    fog_mark *mark = &session->fog_marks[slot];
    if (!mark->active) {
        session->fog_mark_count++;
    }

    mark->active = true;
    mark->pos = pos;
    mark->timer = FOG_MARK_LIFETIME;
    mark->duration = FOG_MARK_LIFETIME;
    mark->radius = radius;
    mark->strength = strength;
}

static void
fog_marks_emit(map_session *session)
{
    if (!session || !session->map || !session->map->has_fog) {
        return;
    }

    if (session->map->fog_level != 0 && session->map->fog_level != 1) {
        return;
    }

    if (session->tank_mgr) {
        for (int i = 0; i < PZ_MAX_TANKS; i++) {
            pz_tank *tank = &session->tank_mgr->tanks[i];
            if (!(tank->flags & PZ_TANK_FLAG_ACTIVE)
                || (tank->flags & PZ_TANK_FLAG_DEAD)) {
                session->fog_has_tank_pos[i] = false;
                continue;
            }
            if (pz_vec2_len(tank->vel) < 0.15f) {
                continue;
            }

            pz_vec2 pos = tank->pos;
            if (!session->fog_has_tank_pos[i]
                || pz_vec2_len(pz_vec2_sub(pos, session->fog_last_tank_pos[i]))
                    >= FOG_MARK_TANK_MIN_DIST) {
                fog_marks_add(session, pos, 2.4f, 1.0f);
                session->fog_last_tank_pos[i] = pos;
                session->fog_has_tank_pos[i] = true;
            }
        }
    }

    if (session->projectile_mgr) {
        for (int i = 0; i < PZ_MAX_PROJECTILES; i++) {
            pz_projectile *proj = &session->projectile_mgr->projectiles[i];
            if (!proj->active) {
                session->fog_has_projectile_pos[i] = false;
                continue;
            }

            pz_vec2 pos = proj->pos;
            if (!session->fog_has_projectile_pos[i]
                || pz_vec2_len(
                       pz_vec2_sub(pos, session->fog_last_projectile_pos[i]))
                    >= FOG_MARK_PROJ_MIN_DIST) {
                fog_marks_add(session, pos, 1.3f, 0.85f);
                session->fog_last_projectile_pos[i] = pos;
                session->fog_has_projectile_pos[i] = true;
            }
        }
    }
}

// ============================================================================
// Argument Parsing
// ============================================================================

static bool
net_is_room_code(const char *value, char *out_code, size_t out_size)
{
    static const char alphabet[] = "23456789abcdefghjkmnpqrstuvwxyz";
    if (!value || !out_code || out_size < 9 || strlen(value) != 8)
        return false;

    for (size_t i = 0; i < 8; i++) {
        char c = (char)tolower((unsigned char)value[i]);
        if (!strchr(alphabet, c))
            return false;
        out_code[i] = c;
    }
    out_code[8] = '\0';
    return true;
}

static void
print_help(const char *program_name)
{
    printf("Tank Game\n\n");
    printf("Usage: %s [options]\n", program_name);
    printf("       %s host [--net-answer <payload>]\n", program_name);
    printf("       %s join <payload|room>\n\n", program_name);
    printf("Options:\n");
    printf("  --help                    Show this help message and exit\n");
    printf("  --map <path>              Load a specific map file\n");
    printf("  --campaign <path>         Load a specific campaign file\n");
    printf("  --edit-map <path>         Open map in editor (creates if new)\n");
    printf("  --debug                   Enable debug overlay (F2 to toggle)\n");
    printf("  --debug-script <commands> Run inline debug script commands\n");
    printf("  --debug-script-file <path> Run debug script from file\n");
    printf("  --debug-texture-scale     Enable texture scale debugging\n");
    printf("  --lightmap-debug <path>   Export lightmap to file\n");
    printf("\nNetworking:\n");
    printf("  host                      Host a WebRTC game via ntfy.sh\n");
    printf("  join <payload>            Join a WebRTC game using a join "
           "payload\n");
    printf(
        "  join <room>               Join a WebRTC game using a room code\n");
    printf(
        "  --net-answer <payload>    Apply a WebRTC answer payload (host)\n");
    printf("\nDebug Script Examples:\n");
    printf("  --debug-script \"frames 3; screenshot test.png; quit\"\n");
    printf("  --debug-script \"input +up; frames 60; screenshot moved.png; "
           "quit\"\n");
    printf("\nSee docs/debug-script.md for full debug script documentation.\n");
}

static void
parse_args(int argc, char *argv[])
{
    g_app.lightmap_debug_path = NULL;
    g_app.map_path_arg = NULL;
    g_app.campaign_path_arg = NULL;
    g_app.edit_map_path_arg = NULL;
    g_app.join_payload_arg = NULL;
    g_app.show_debug_overlay = false;
    g_app.show_debug_texture_scale = false;
    g_app.debug_script_path_arg = NULL;
    g_app.inline_script_arg = NULL;
    g_app.net_is_host = false;
    g_app.net_is_client = false;
    g_app.net_share_enabled = false;
    g_app.net_use_signaling = false;
    g_app.net_waiting_for_offer = false;
    g_app.net_waiting_for_answer = false;
    g_app.net_signaling_fetch_in_flight = false;
    g_app.net_signaling_failed = false;
    g_app.net_signaling_next_poll = 0.0;
    g_app.net_room_code[0] = '\0';
    g_app.net_answer_payload_arg = NULL;
    g_app.net_has_remote_input = false;
    g_app.net_last_remote_input = (pz_net_input) { 0 };
    g_app.net_last_remote_sequence = 0;
    g_app.net_last_processed_input = 0;
    g_app.net_last_processed_action = 0;
    g_app.net_next_input_sequence = 0;
    g_app.net_next_action_sequence = 0;
    g_app.net_pending_action_sequence = 0;
    g_app.net_pending_fire_pressed = false;
    g_app.net_pending_place_mine = false;
    g_app.net_pending_place_barrier = false;
    g_app.net_pending_weapon_switch = 0;
    g_app.net_remote_tank = NULL;
    g_app.net_remote_tank_id = -1;
    g_app.net_snapshot_tick = 0;
    g_app.net_snapshot_interval = 3; // ~20Hz at 60 ticks/s
    g_app.join_offer = NULL;
    g_app.join_answer = NULL;
    g_app.join_answer_json = NULL;
    g_app.net_webrtc = NULL;

    // Track deprecated screenshot flags for combined error message
    const char *deprecated_screenshot_path = NULL;
    const char *deprecated_screenshot_frames = NULL;
    bool has_deprecated_script = false;

    // First pass: check for --help
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_help(argv[0]);
            exit(0);
        }
    }

    // Second pass: parse arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "host") == 0) {
            g_app.net_is_host = true;
        } else if (strcmp(argv[i], "join") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: join requires a payload or room\n");
                exit(1);
            }
            g_app.net_is_client = true;
            const char *join_arg = argv[++i];
            if (net_is_room_code(join_arg, g_app.net_room_code,
                    sizeof(g_app.net_room_code))) {
                g_app.net_use_signaling = true;
            } else {
                g_app.join_payload_arg = join_arg;
            }
        } else if (strcmp(argv[i], "--share") == 0) {
            g_app.net_share_enabled = true;
            g_app.net_use_signaling = true;
        } else if (strcmp(argv[i], "--net-answer") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: --net-answer requires a payload\n");
                exit(1);
            }
            g_app.net_answer_payload_arg = argv[++i];
        } else if (strcmp(argv[i], "--lightmap-debug") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: --lightmap-debug requires a path\n");
                exit(1);
            }
            g_app.lightmap_debug_path = argv[++i];
        } else if (strcmp(argv[i], "--map") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: --map requires a path\n");
                exit(1);
            }
            g_app.map_path_arg = argv[++i];
        } else if (strcmp(argv[i], "--campaign") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: --campaign requires a path\n");
                exit(1);
            }
            g_app.campaign_path_arg = argv[++i];
        } else if (strcmp(argv[i], "--edit-map") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: --edit-map requires a path\n");
                exit(1);
            }
            g_app.edit_map_path_arg = argv[++i];
        } else if (strcmp(argv[i], "--debug") == 0) {
            g_app.show_debug_overlay = true;
        } else if (strcmp(argv[i], "--debug-texture-scale") == 0) {
            g_app.show_debug_texture_scale = true;
        } else if (strcmp(argv[i], "--debug-script-file") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr,
                    "error: --debug-script-file requires a file path\n");
                exit(1);
            }
            g_app.debug_script_path_arg = argv[++i];
        } else if (strcmp(argv[i], "--debug-script") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: --debug-script requires commands\n");
                exit(1);
            }
            g_app.inline_script_arg = argv[++i];
        }
        // Collect deprecated flags (don't exit immediately)
        else if (strcmp(argv[i], "--screenshot") == 0) {
            deprecated_screenshot_path = (i + 1 < argc && argv[i + 1][0] != '-')
                ? argv[++i]
                : "output.png";
        } else if (strcmp(argv[i], "--screenshot-frames") == 0) {
            deprecated_screenshot_frames
                = (i + 1 < argc && argv[i + 1][0] != '-') ? argv[++i] : "3";
        } else if (strcmp(argv[i], "--script") == 0) {
            has_deprecated_script = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                i++; // skip the value
            }
        }
        // Unknown argument
        else if (argv[i][0] == '-') {
            fprintf(stderr, "error: unknown option: %s\n", argv[i]);
            fprintf(stderr, "       Run with --help for usage information\n");
            exit(1);
        } else {
            fprintf(stderr, "error: unexpected argument: %s\n", argv[i]);
            fprintf(stderr, "       Run with --help for usage information\n");
            exit(1);
        }
    }

    if (g_app.net_is_host && g_app.net_is_client) {
        fprintf(stderr, "error: cannot use host and join simultaneously\n");
        exit(1);
    }

    if (g_app.net_is_host) {
        g_app.net_share_enabled = true;
        g_app.net_use_signaling = true;
    }

    // Show combined error for deprecated screenshot flags
    if (deprecated_screenshot_path || deprecated_screenshot_frames) {
        const char *path = deprecated_screenshot_path
            ? deprecated_screenshot_path
            : "output.png";
        const char *frames
            = deprecated_screenshot_frames ? deprecated_screenshot_frames : "3";

        fprintf(stderr,
            "error: --screenshot and --screenshot-frames are not "
            "supported\n");
        fprintf(stderr,
            "       Use: --debug-script \"frames %s; screenshot %s; quit\"\n",
            frames, path);
        exit(1);
    }

    // Show error for renamed --script flag
    if (has_deprecated_script) {
        fprintf(stderr, "error: --script has been renamed to --debug-script\n");
        exit(1);
    }
}

static void
app_init(void)
{
    int width = sapp_width();
    int height = sapp_height();

    printf("Tank Game - Starting...\n");

#ifdef PZ_DEBUG
    printf("Build: Debug\n");
#elif defined(PZ_DEV)
    printf("Build: Dev\n");
#elif defined(PZ_RELEASE)
    printf("Build: Release\n");
#endif

    pz_log_init();
    pz_time_init();

    net_input_queue_init(&g_app.net_rx_queue);
    net_snapshot_queue_init(&g_app.net_snapshot_rx_queue);
    net_event_queue_init(&g_app.net_event_rx_queue);
    atomic_store(&g_app.net_channel_open, false);
    g_app.net_connection_notified = false;
    g_app.net_has_remote_input = false;
    g_app.net_last_remote_input = (pz_net_input) { 0 };

    if (g_app.join_payload_arg) {
        pz_net_offer *offer = pz_net_offer_decode_url(g_app.join_payload_arg);
        if (!offer) {
            pz_log(
                PZ_LOG_ERROR, PZ_LOG_CAT_NET, "Invalid join payload provided");
            sapp_quit();
            return;
        }

        if (!net_setup_client_from_offer(offer)) {
            pz_net_offer_free(offer);
            g_app.join_offer = NULL;
            sapp_quit();
            return;
        }
    }

    if (g_app.net_use_signaling && g_app.net_is_client
        && g_app.net_room_code[0] != '\0' && !g_app.join_payload_arg) {
        g_app.net_waiting_for_offer = true;
        pz_log(PZ_LOG_INFO, PZ_LOG_CAT_NET,
            "Waiting for signaling offer in room %s", g_app.net_room_code);
        g_app.net_signaling_next_poll = 0.0;
    }

    if ((g_app.net_is_host || g_app.net_is_client) && !g_app.map_path_arg) {
        g_app.map_path_arg = "assets/maps/night_arena.map";
    }

    // Check environment variables for audio control
    // PZ_MUSIC=0 disables music, PZ_SOUNDS=0 disables sound effects
    // Debug scripts automatically disable all audio
    bool enable_music = true;
    bool enable_sounds = true;

    const char *env_music = getenv("PZ_MUSIC");
    const char *env_sounds = getenv("PZ_SOUNDS");

    if (env_music && strcmp(env_music, "0") == 0) {
        enable_music = false;
        pz_log(PZ_LOG_INFO, PZ_LOG_CAT_AUDIO, "Music disabled via PZ_MUSIC=0");
    }
    if (env_sounds && strcmp(env_sounds, "0") == 0) {
        enable_sounds = false;
        pz_log(
            PZ_LOG_INFO, PZ_LOG_CAT_AUDIO, "Sounds disabled via PZ_SOUNDS=0");
    }

    // Debug scripts run silently
    if (g_app.debug_script_path_arg || g_app.inline_script_arg) {
        enable_music = false;
        enable_sounds = false;
        pz_log(PZ_LOG_INFO, PZ_LOG_CAT_AUDIO,
            "Audio disabled for debug script execution");
    }

    g_app.audio = NULL;
    g_app.game_music = NULL;
    g_app.game_sfx = NULL;

    if (enable_music || enable_sounds) {
        g_app.audio = pz_audio_init();
        if (g_app.audio) {
            int sample_rate = pz_audio_get_sample_rate(g_app.audio);

            if (enable_music) {
                g_app.game_music
                    = pz_game_music_create("assets/sounds/soundfont.sf2");
            }
            if (enable_sounds) {
                g_app.game_sfx = pz_game_sfx_create(sample_rate);
            }

            if (g_app.game_music || g_app.game_sfx) {
                pz_audio_set_callback(g_app.audio, audio_callback, NULL);
            } else {
                pz_audio_shutdown(g_app.audio);
                g_app.audio = NULL;
            }
        }
    }

    // Initialize core systems (persistent across maps)
    pz_renderer_config renderer_config = {
        .backend = PZ_BACKEND_SOKOL,
        .window_handle = NULL,
        .viewport_width = width,
        .viewport_height = height,
    };

    g_app.renderer = pz_renderer_create(&renderer_config);
    if (!g_app.renderer) {
        pz_log(PZ_LOG_ERROR, PZ_LOG_CAT_CORE, "Failed to create renderer");
        sapp_quit();
        return;
    }

    g_app.tex_manager = pz_texture_manager_create(g_app.renderer);

    // Create and load tile registry
    g_app.tile_registry = pz_tile_registry_create();
    if (g_app.tile_registry) {
        int tiles_loaded = pz_tile_registry_load_all(
            g_app.tile_registry, g_app.tex_manager, "assets/tiles");
        pz_log(PZ_LOG_INFO, PZ_LOG_CAT_GAME,
            "Tile registry initialized with %d tiles", tiles_loaded);
    }

    pz_camera_init(&g_app.camera, width, height);

    pz_debug_cmd_init(NULL);

    g_app.debug_overlay = pz_debug_overlay_create(g_app.renderer);
    if (!g_app.debug_overlay) {
        pz_log(PZ_LOG_WARN, PZ_LOG_CAT_CORE, "Failed to create debug overlay");
    } else if (g_app.show_debug_overlay) {
        pz_debug_overlay_set_visible(g_app.debug_overlay, true);
    }

    // Hide OS cursor and create custom cursor
    sapp_show_mouse(false);
    g_app.cursor = pz_cursor_create(g_app.renderer);
    if (g_app.cursor) {
        pz_cursor_set_position(
            g_app.cursor, (float)width * 0.5f, (float)height * 0.5f);
    }

    // Initialize font system
    g_app.font_mgr = pz_font_manager_create(g_app.renderer);
    if (g_app.font_mgr) {
        g_app.font_russo
            = pz_font_load(g_app.font_mgr, "assets/fonts/RussoOne-Regular.ttf");
        if (!g_app.font_russo) {
            pz_log(
                PZ_LOG_WARN, PZ_LOG_CAT_CORE, "Failed to load Russo One font");
        }
        g_app.font_caveat = pz_font_load(
            g_app.font_mgr, "assets/fonts/CaveatBrush-Regular.ttf");
        if (!g_app.font_caveat) {
            pz_log(PZ_LOG_WARN, PZ_LOG_CAT_CORE,
                "Failed to load Caveat Brush font");
        }
    }

    // Create spawn indicator renderer
    g_app.spawn_indicator = pz_spawn_indicator_create(g_app.renderer);
    if (!g_app.spawn_indicator) {
        pz_log(PZ_LOG_WARN, PZ_LOG_CAT_CORE,
            "Failed to create spawn indicator renderer");
    }

    // Create laser rendering resources (persistent)
    g_app.laser_shader = pz_renderer_load_shader(
        g_app.renderer, "shaders/laser.vert", "shaders/laser.frag", "laser");

    g_app.laser_pipeline = PZ_INVALID_HANDLE;
    g_app.laser_vb = PZ_INVALID_HANDLE;

    if (g_app.laser_shader != PZ_INVALID_HANDLE) {
        pz_vertex_attr laser_attrs[] = {
            { .name = "a_position", .type = PZ_ATTR_FLOAT3, .offset = 0 },
            { .name = "a_texcoord",
                .type = PZ_ATTR_FLOAT2,
                .offset = 3 * sizeof(float) },
        };

        pz_pipeline_desc laser_desc = {
            .shader = g_app.laser_shader,
            .vertex_layout = { .attrs = laser_attrs,
                .attr_count = 2,
                .stride = sizeof(float) * 5 },
            .blend = PZ_BLEND_ALPHA,
            .depth = PZ_DEPTH_READ,
            .cull = PZ_CULL_NONE,
            .primitive = PZ_PRIMITIVE_TRIANGLES,
        };
        g_app.laser_pipeline
            = pz_renderer_create_pipeline(g_app.renderer, &laser_desc);

        pz_buffer_desc laser_vb_desc = {
            .type = PZ_BUFFER_VERTEX,
            .usage = PZ_BUFFER_DYNAMIC,
            .data = NULL,
            .size = 6 * sizeof(float) * 5,
        };
        g_app.laser_vb
            = pz_renderer_create_buffer(g_app.renderer, &laser_vb_desc);
    }

    // Create background renderer (persistent, configured per-map)
    g_app.background = pz_background_create(g_app.renderer);
    if (!g_app.background) {
        pz_log(PZ_LOG_WARN, PZ_LOG_CAT_CORE,
            "Failed to create background renderer");
    }

    // Initialize simulation system
    g_app.sim = pz_sim_create((uint32_t)time(NULL));

    // Create map editor
    g_app.editor = pz_editor_create(
        g_app.renderer, g_app.tex_manager, g_app.font_mgr, g_app.tile_registry);
    if (!g_app.editor) {
        pz_log(PZ_LOG_WARN, PZ_LOG_CAT_CORE, "Failed to create map editor");
    } else {
        pz_editor_set_background(g_app.editor, g_app.background);
    }

    // Check if launching directly into editor mode
    if (g_app.edit_map_path_arg) {
        pz_log(PZ_LOG_INFO, PZ_LOG_CAT_GAME, "Editor mode: %s",
            g_app.edit_map_path_arg);

        if (!pz_editor_enter_file(g_app.editor, g_app.edit_map_path_arg)) {
            pz_log(PZ_LOG_ERROR, PZ_LOG_CAT_GAME,
                "Failed to open map in editor, exiting");
            sapp_quit();
            return;
        }

        g_app.state = GAME_STATE_EDITOR;
        g_app.state_timer = 0.0f;
    } else {
        // Load campaign or single map
        g_app.campaign_mgr = pz_campaign_create();

        const char *first_map_path = NULL;

        if (g_app.map_path_arg) {
            // Single map mode (--map flag)
            first_map_path = g_app.map_path_arg;
            pz_log(PZ_LOG_INFO, PZ_LOG_CAT_GAME, "Single map mode: %s",
                first_map_path);
        } else {
            // Campaign mode
            const char *campaign_path = g_app.campaign_path_arg
                ? g_app.campaign_path_arg
                : "assets/campaigns/main.campaign";

            if (pz_campaign_load(g_app.campaign_mgr, campaign_path)) {
                pz_campaign_start(
                    g_app.campaign_mgr, 0); // Use campaign's lives
                first_map_path
                    = pz_campaign_get_current_map(g_app.campaign_mgr);
            } else {
                pz_log(PZ_LOG_WARN, PZ_LOG_CAT_GAME,
                    "Failed to load campaign, falling back to default map");
                first_map_path = "assets/maps/night_arena.map";
            }
        }

        // Load the first map
        if (!map_session_load(&g_app.session, first_map_path)) {
            pz_log(PZ_LOG_ERROR, PZ_LOG_CAT_GAME,
                "Failed to load initial map, exiting");
            sapp_quit();
            return;
        }

        if (g_app.net_is_host) {
            const char *ice_servers[] = { "stun:stun.l.google.com:19302",
                "stun:stun.cloudflare.com:3478" };
            pz_net_webrtc_config net_config = {
                .ice_servers = ice_servers,
                .ice_server_count
                = (int)(sizeof(ice_servers) / sizeof(ice_servers[0])),
                .enable_logging = true,
            };

            g_app.net_webrtc = pz_net_webrtc_create(&net_config);
            if (!g_app.net_webrtc) {
                pz_log(PZ_LOG_ERROR, PZ_LOG_CAT_NET,
                    "Failed to initialize WebRTC host");
                sapp_quit();
                return;
            }

            pz_net_webrtc_set_message_callback(
                g_app.net_webrtc, net_handle_message, NULL);
            pz_net_webrtc_set_channel_callback(
                g_app.net_webrtc, net_handle_channel_state, NULL);

            char *offer_sdp
                = pz_net_webrtc_create_offer(g_app.net_webrtc, 10000);
            if (!offer_sdp) {
                pz_log(PZ_LOG_ERROR, PZ_LOG_CAT_NET,
                    "Failed to create WebRTC offer");
                sapp_quit();
                return;
            }

            pz_net_offer *offer = pz_net_offer_create(PZ_NET_PROTOCOL_VERSION,
                "host", g_app.session.map_path, offer_sdp);
            char *offer_json = pz_net_offer_encode_json(offer);
            pz_net_offer_free(offer);
            pz_free(offer_sdp);

            if (!offer_json) {
                pz_log(PZ_LOG_ERROR, PZ_LOG_CAT_NET,
                    "Failed to encode signaling offer payload");
                sapp_quit();
                return;
            }

            const char *room = pz_signaling_generate_room();
            if (!room || room[0] == '\0') {
                pz_free(offer_json);
                sapp_quit();
                return;
            }
            strncpy(g_app.net_room_code, room, sizeof(g_app.net_room_code) - 1);
            g_app.net_room_code[sizeof(g_app.net_room_code) - 1] = '\0';
            g_app.net_waiting_for_answer = true;
            pz_log(PZ_LOG_INFO, PZ_LOG_CAT_NET, "Signaling room code: %s",
                g_app.net_room_code);
            pz_log(PZ_LOG_INFO, PZ_LOG_CAT_NET, "Join with: ./tankgame join %s",
                g_app.net_room_code);
            pz_log(PZ_LOG_INFO, PZ_LOG_CAT_NET,
                "Web join: https://mitsuhiko.github.io/tankgame/#join/%s",
                g_app.net_room_code);
            pz_signaling_publish(g_app.net_room_code, "o", offer_json,
                net_signaling_publish_offer_done, NULL);
            g_app.net_signaling_next_poll = 0.0;
            pz_free(offer_json);

            if (g_app.net_answer_payload_arg) {
                if (!net_apply_answer_payload(g_app.net_answer_payload_arg)) {
                    sapp_quit();
                    return;
                }
            }
        }

        // Initialize game state
        g_app.state = GAME_STATE_PLAYING;
        g_app.state_timer = 0.0f;
    }

    // Frame timing
    g_app.frame_count = 0;
    g_app.last_hot_reload_check = pz_time_now();
    g_app.last_frame_time = pz_time_now();
    g_app.last_perf_log_time = g_app.last_frame_time;

    // Input state
    g_app.mouse_x = (float)width * 0.5f;
    g_app.mouse_y = (float)height * 0.5f;
    g_app.mouse_left_down = false;
    g_app.mouse_left_just_pressed = false;
    g_app.mouse_right_just_pressed = false;
    g_app.space_down = false;
    g_app.space_just_pressed = false;
    g_app.scroll_accumulator = 0.0f;
    g_app.key_f_just_pressed = false;
    g_app.key_g_just_pressed = false;

    // Load debug script if specified (file or inline)
    g_app.debug_script = NULL;
    if (g_app.debug_script_path_arg) {
        g_app.debug_script = pz_debug_script_load(g_app.debug_script_path_arg);
        if (!g_app.debug_script) {
            pz_log(PZ_LOG_ERROR, PZ_LOG_CAT_CORE,
                "Failed to load debug script, exiting");
            sapp_quit();
            return;
        }
    } else if (g_app.inline_script_arg) {
        g_app.debug_script
            = pz_debug_script_create_from_string(g_app.inline_script_arg);
        if (!g_app.debug_script) {
            pz_log(PZ_LOG_ERROR, PZ_LOG_CAT_CORE,
                "Failed to parse inline script, exiting");
            sapp_quit();
            return;
        }
    }
}

// Render music debug overlay (called when debug overlay is visible)
static void
render_music_debug_overlay(void)
{
    if (!pz_debug_overlay_is_visible(g_app.debug_overlay)) {
        return;
    }
    if (!g_app.game_music) {
        return;
    }

    pz_game_music_debug_info info;
    if (!pz_game_music_get_debug_info(g_app.game_music, &info)) {
        return;
    }

    // Position music debug panel on the right side of the screen
    // Font is now 16x16 (2x scaled from 8x8)
    int fb_width, fb_height;
    pz_renderer_get_viewport(g_app.renderer, &fb_width, &fb_height);
    int panel_x = fb_width - 380; // Wider panel for larger text
    int panel_y = 16;
    int line_height = 20; // 16px font + 4px spacing
    int y = panel_y;

    pz_vec4 white = { 1.0f, 1.0f, 1.0f, 1.0f };
    pz_vec4 green = { 0.3f, 1.0f, 0.3f, 1.0f };
    pz_vec4 yellow = { 1.0f, 1.0f, 0.3f, 1.0f };
    pz_vec4 red = { 1.0f, 0.3f, 0.3f, 1.0f };
    pz_vec4 cyan = { 0.3f, 1.0f, 1.0f, 1.0f };
    pz_vec4 gray = { 0.6f, 0.6f, 0.6f, 1.0f };
    (void)red;

    // Header
    pz_debug_overlay_text_color(
        g_app.debug_overlay, panel_x, y, cyan, "-- Music Debug --");
    y += line_height + 4;

    // State
    const char *state_str
        = info.is_victory ? "VICTORY" : (info.playing ? "PLAYING" : "STOPPED");
    pz_vec4 state_color = info.playing ? green : gray;
    pz_debug_overlay_text_color(
        g_app.debug_overlay, panel_x, y, state_color, "State: %s", state_str);
    y += line_height;

    // BPM and timing
    pz_debug_overlay_text_color(
        g_app.debug_overlay, panel_x, y, white, "BPM: %.1f", info.bpm);
    y += line_height;

    // Time with beat indicator
    double beat_duration_ms = 60000.0 / info.bpm;
    double beat_progress = info.beat_pos / beat_duration_ms;
    int beat_num = (int)(info.time_ms / beat_duration_ms) % 4 + 1;
    pz_vec4 beat_color = (beat_progress < 0.1) ? yellow : white;
    pz_debug_overlay_text_color(g_app.debug_overlay, panel_x, y, beat_color,
        "Time: %.1fs [%d]", info.time_ms / 1000.0, beat_num);
    y += line_height;

    // Loop length
    pz_debug_overlay_text_color(g_app.debug_overlay, panel_x, y, white,
        "Loop: %.1fs", info.loop_length_ms / 1000.0);
    y += line_height;

    // Master volume
    pz_debug_overlay_text_color(g_app.debug_overlay, panel_x, y, white,
        "Volume: %.0f%%", info.master_volume * 100.0f);
    y += line_height + 4;

    // Intensity layers
    pz_debug_overlay_text_color(
        g_app.debug_overlay, panel_x, y, cyan, "Intensity:");
    y += line_height;

    pz_vec4 i1_color = info.intensity1_active
        ? green
        : (info.intensity1_pending ? yellow : gray);
    const char *i1_status = info.intensity1_active
        ? "ON"
        : (info.intensity1_pending ? "PENDING" : "OFF");
    pz_debug_overlay_text_color(
        g_app.debug_overlay, panel_x, y, i1_color, "  I1: %s", i1_status);
    y += line_height;

    pz_vec4 i2_color = info.intensity2_active
        ? green
        : (info.intensity2_pending ? yellow : gray);
    const char *i2_status = info.intensity2_active
        ? "ON"
        : (info.intensity2_pending ? "PENDING" : "OFF");
    pz_debug_overlay_text_color(
        g_app.debug_overlay, panel_x, y, i2_color, "  I2: %s", i2_status);
    y += line_height + 4;

    // Layer details
    pz_debug_overlay_text_color(g_app.debug_overlay, panel_x, y, cyan,
        "Layers (%d):", info.layer_count);
    y += line_height;

    for (int i = 0; i < info.layer_count && i < 6; i++) {
        pz_music_layer_info layer_info;
        if (pz_game_music_get_layer_info(g_app.game_music, i, &layer_info)) {
            pz_vec4 layer_color = layer_info.active ? green : gray;
            char status
                = layer_info.enabled ? (layer_info.active ? '+' : '~') : '-';
            pz_debug_overlay_text_color(g_app.debug_overlay, panel_x, y,
                layer_color, "[%c] ch%d v%.0f%%", status,
                layer_info.midi_channel, layer_info.volume * 100.0f);
            y += line_height;
        }
    }
}

static float
us_to_ms(uint64_t us)
{
    return (float)us / 1000.0f;
}

static void
app_frame(void)
{
    if (!g_app.renderer)
        return;

    // Poll for commands from the debug command pipe
    // Commands are injected into (or create) the debug script
    char *pipe_commands = pz_debug_cmd_poll_commands();
    if (pipe_commands) {
        if (!net_try_consume_pipe_command(pipe_commands)) {
            g_app.debug_script
                = pz_debug_script_inject(g_app.debug_script, pipe_commands);
        }
        pz_free(pipe_commands);
    }

    if (g_app.net_use_signaling) {
        pz_signaling_update();
        net_signaling_poll();
    }

    bool net_channel_open_now = atomic_load(&g_app.net_channel_open);
    if (net_channel_open_now && !g_app.net_connection_notified) {
        if (g_app.debug_script)
            pz_debug_script_notify_connected(g_app.debug_script);
        g_app.net_connection_notified = true;
    } else if (!net_channel_open_now) {
        g_app.net_connection_notified = false;
    }

    // Process debug script commands (may trigger actions like load map,
    // screenshot). Paths are copied because the script reuses action_path.
    bool script_should_screenshot = false;
    bool script_should_dump = false;
    char script_screenshot_path[256] = { 0 };
    char script_dump_path[256] = { 0 };

    if (g_app.debug_script && !pz_debug_script_is_done(g_app.debug_script)) {
        pz_debug_script_action action;
        while ((action = pz_debug_script_update(g_app.debug_script))
            != PZ_DEBUG_SCRIPT_CONTINUE) {
            switch (action) {
            case PZ_DEBUG_SCRIPT_QUIT:
                sapp_quit();
                return;

            case PZ_DEBUG_SCRIPT_LOAD_MAP: {
                const char *map_path
                    = pz_debug_script_get_map_path(g_app.debug_script);
                if (map_path) {
                    map_session_load(&g_app.session, map_path);
                }
                break;
            }

            case PZ_DEBUG_SCRIPT_SCREENSHOT: {
                script_should_screenshot = true;
                const char *path
                    = pz_debug_script_get_screenshot_path(g_app.debug_script);
                if (path) {
                    strncpy(script_screenshot_path, path,
                        sizeof(script_screenshot_path) - 1);
                }
                // Stop processing commands - let the frame render first
                goto done_script_commands;
            }

            case PZ_DEBUG_SCRIPT_DUMP: {
                script_should_dump = true;
                const char *path
                    = pz_debug_script_get_dump_path(g_app.debug_script);
                if (path) {
                    strncpy(
                        script_dump_path, path, sizeof(script_dump_path) - 1);
                }
                // Stop processing commands - let the frame render first
                goto done_script_commands;
            }

            case PZ_DEBUG_SCRIPT_SET_SEED:
                pz_sim_set_seed(
                    g_app.sim, pz_debug_script_get_seed(g_app.debug_script));
                break;

            case PZ_DEBUG_SCRIPT_GOD_MODE: {
                pz_tank *player = pz_tank_get_player(g_app.session.tank_mgr);
                if (player) {
                    bool enable
                        = pz_debug_script_get_god_mode(g_app.debug_script);
                    if (enable) {
                        player->flags |= PZ_TANK_FLAG_INVINCIBLE;
                    } else {
                        player->flags &= ~PZ_TANK_FLAG_INVINCIBLE;
                    }
                }
                break;
            }

            case PZ_DEBUG_SCRIPT_TELEPORT: {
                pz_tank *player = pz_tank_get_player(g_app.session.tank_mgr);
                if (player) {
                    float x, y;
                    pz_debug_script_get_teleport_pos(
                        g_app.debug_script, &x, &y);
                    player->pos.x = x;
                    player->pos.y = y;
                    player->vel = pz_vec2_zero();
                    pz_log(PZ_LOG_INFO, PZ_LOG_CAT_GAME,
                        "Debug: teleported player to (%.2f, %.2f)", x, y);
                }
                break;
            }

            case PZ_DEBUG_SCRIPT_GIVE: {
                pz_tank *player = pz_tank_get_player(g_app.session.tank_mgr);
                if (player) {
                    const char *item
                        = pz_debug_script_get_give_item(g_app.debug_script);
                    if (item) {
                        // Handle known item types
                        if (strcmp(item, "barrier_placer") == 0) {
                            pz_tank_set_barrier_placer(
                                player, "cobble", 15, 5, 30.0f);
                            pz_log(PZ_LOG_INFO, PZ_LOG_CAT_GAME,
                                "Debug: gave barrier_placer to player");
                        } else if (strcmp(item, "machine_gun") == 0) {
                            pz_tank_add_weapon(
                                player, (int)PZ_POWERUP_MACHINE_GUN);
                            pz_log(PZ_LOG_INFO, PZ_LOG_CAT_GAME,
                                "Debug: gave machine_gun to player");
                        } else if (strcmp(item, "ricochet") == 0) {
                            pz_tank_add_weapon(
                                player, (int)PZ_POWERUP_RICOCHET);
                            pz_log(PZ_LOG_INFO, PZ_LOG_CAT_GAME,
                                "Debug: gave ricochet to player");
                        } else {
                            pz_log(PZ_LOG_WARN, PZ_LOG_CAT_GAME,
                                "Debug: unknown item '%s'", item);
                        }
                    }
                }
                break;
            }

            case PZ_DEBUG_SCRIPT_CURSOR: {
                float x, y;
                pz_debug_script_get_cursor_pos(g_app.debug_script, &x, &y);

                // Snap to tile center if map is loaded
                if (g_app.session.map) {
                    float tile_size = g_app.session.map->tile_size;
                    int tile_x = (int)floorf(x / tile_size);
                    int tile_y = (int)floorf(y / tile_size);
                    x = (tile_x + 0.5f) * tile_size;
                    y = (tile_y + 0.5f) * tile_size;
                }

                // Convert world position to screen pixels
                // World coords are (x, 0, z) where z is our y parameter
                pz_vec3 world_pos = { x, 0.0f, y };
                pz_vec3 screen_pos
                    = pz_camera_world_to_screen(&g_app.camera, world_pos);

                // Set mouse position directly (bypasses script_cursor system)
                g_app.mouse_x = screen_pos.x;
                g_app.mouse_y = screen_pos.y;
                g_app.script_cursor_active = false; // Use mouse path now

                // Also update editor if in editor mode
                if (app_editor_active()) {
                    pz_editor_set_mouse(
                        g_app.editor, screen_pos.x, screen_pos.y);
                }

                pz_log(PZ_LOG_DEBUG, PZ_LOG_CAT_GAME,
                    "Debug: cursor at world (%.2f, %.2f) -> screen (%.0f, "
                    "%.0f)",
                    x, y, screen_pos.x, screen_pos.y);
                break;
            }

            case PZ_DEBUG_SCRIPT_MOUSE_SCREEN: {
                float x, y;
                pz_debug_script_get_cursor_pos(g_app.debug_script, &x, &y);

                // Set mouse position directly in screen coordinates
                g_app.mouse_x = x;
                g_app.mouse_y = y;

                // Update cursor visual position
                if (g_app.cursor) {
                    pz_cursor_set_position(g_app.cursor, x, y);
                }

                // Update editor if in editor mode
                if (app_editor_active()) {
                    pz_editor_set_mouse(g_app.editor, x, y);
                }

                pz_log(PZ_LOG_DEBUG, PZ_LOG_CAT_GAME,
                    "Debug: mouse_screen at (%.0f, %.0f)", x, y);
                break;
            }

            case PZ_DEBUG_SCRIPT_SPAWN_BARRIER: {
                if (g_app.session.barrier_mgr) {
                    float x, y;
                    pz_debug_script_get_spawn_barrier(
                        g_app.debug_script, &x, &y);

                    // Snap to tile center if map is loaded
                    if (g_app.session.map) {
                        float tile_size = g_app.session.map->tile_size;
                        int tile_x = (int)floorf(x / tile_size);
                        int tile_y = (int)floorf(y / tile_size);
                        x = (tile_x + 0.5f) * tile_size;
                        y = (tile_y + 0.5f) * tile_size;
                    }

                    pz_vec2 pos = { x, y };
                    // Get floor level from tile at spawn position
                    int8_t floor_level = 0;
                    if (g_app.session.map) {
                        int tx, ty;
                        pz_map_world_to_tile(g_app.session.map, pos, &tx, &ty);
                        floor_level
                            = pz_map_get_height(g_app.session.map, tx, ty);
                    }
                    pz_barrier_add(g_app.session.barrier_mgr, pos, "cobble", 20,
                        floor_level);
                    pz_log(PZ_LOG_INFO, PZ_LOG_CAT_GAME,
                        "Debug: spawned barrier at (%.2f, %.2f)", x, y);
                }
                break;
            }

            case PZ_DEBUG_SCRIPT_SPAWN_POWERUP: {
                if (g_app.session.powerup_mgr) {
                    float x, y;
                    const char *type;
                    pz_debug_script_get_spawn_powerup(
                        g_app.debug_script, &x, &y, &type);
                    pz_vec2 pos = { x, y };
                    pz_powerup_type ptype = PZ_POWERUP_BARRIER_PLACER;
                    if (type && strcmp(type, "machine_gun") == 0) {
                        ptype = PZ_POWERUP_MACHINE_GUN;
                    } else if (type && strcmp(type, "ricochet") == 0) {
                        ptype = PZ_POWERUP_RICOCHET;
                    }
                    pz_powerup_add(g_app.session.powerup_mgr, pos, ptype, 0.0f);
                    pz_log(PZ_LOG_INFO, PZ_LOG_CAT_GAME,
                        "Debug: spawned powerup '%s' at (%.2f, %.2f)",
                        type ? type : "barrier_placer", x, y);
                }
                break;
            }

            case PZ_DEBUG_SCRIPT_NET_HOST: {
                const char *offer_path
                    = pz_debug_script_get_net_offer_path(g_app.debug_script);
                if (offer_path && !g_app.net_webrtc) {
                    // Initialize WebRTC as host
                    pz_net_webrtc_config net_config = {
                        .ice_servers = NULL,
                        .ice_server_count = 0,
                        .enable_logging = true,
                    };
                    g_app.net_webrtc = pz_net_webrtc_create(&net_config);
                    if (g_app.net_webrtc) {
                        pz_net_webrtc_set_message_callback(
                            g_app.net_webrtc, net_handle_message, NULL);
                        pz_net_webrtc_set_channel_callback(
                            g_app.net_webrtc, net_handle_channel_state, NULL);
                        char *offer_sdp = pz_net_webrtc_create_offer(
                            g_app.net_webrtc, 10000);
                        if (offer_sdp) {
                            pz_net_offer *offer = pz_net_offer_create(
                                PZ_NET_PROTOCOL_VERSION, "host",
                                g_app.session.map_path[0] != '\0'
                                    ? g_app.session.map_path
                                    : "",
                                offer_sdp);
                            char *offer_url = pz_net_offer_encode_url(offer);
                            pz_net_offer_free(offer);
                            pz_free(offer_sdp);
                            if (offer_url) {
                                pz_file_write_text(offer_path, offer_url);
                                pz_log(PZ_LOG_INFO, PZ_LOG_CAT_NET,
                                    "Debug: wrote offer to '%s'", offer_path);
                                pz_free(offer_url);
                                g_app.net_is_host = true;
                                pz_campaign_destroy(g_app.campaign_mgr);
                                g_app.campaign_mgr = pz_campaign_create();

                                // Reload the map if already loaded to set up
                                // networking (create net_remote_tank)
                                if (g_app.session.map_path[0] != '\0') {
                                    pz_log(PZ_LOG_INFO, PZ_LOG_CAT_NET,
                                        "Debug: reloading map for networking");
                                    char map_path[256];
                                    strncpy(map_path, g_app.session.map_path,
                                        sizeof(map_path) - 1);
                                    map_path[sizeof(map_path) - 1] = '\0';
                                    map_session_load(&g_app.session, map_path);
                                }
                            }
                        }
                    }
                }
                pz_debug_script_notify_action_complete(g_app.debug_script);
                break;
            }

            case PZ_DEBUG_SCRIPT_NET_JOIN: {
                const char *offer_path = NULL;
                const char *answer_path = NULL;
                pz_debug_script_get_net_join_paths(
                    g_app.debug_script, &offer_path, &answer_path);
                if (offer_path && answer_path && !g_app.net_webrtc) {
                    char *offer_url = pz_file_read_text(offer_path);
                    if (offer_url) {
                        pz_net_offer *offer
                            = pz_net_offer_decode_url(offer_url);
                        pz_free(offer_url);
                        if (offer) {
                            // Save map name before freeing offer
                            char map_name[256] = { 0 };
                            if (offer->map_name && offer->map_name[0] != '\0') {
                                strncpy(map_name, offer->map_name,
                                    sizeof(map_name) - 1);
                            }

                            pz_net_webrtc_config net_config = {
                                .ice_servers = NULL,
                                .ice_server_count = 0,
                                .enable_logging = true,
                            };
                            g_app.net_webrtc
                                = pz_net_webrtc_create(&net_config);
                            if (g_app.net_webrtc) {
                                pz_net_webrtc_set_message_callback(
                                    g_app.net_webrtc, net_handle_message, NULL);
                                pz_net_webrtc_set_channel_callback(
                                    g_app.net_webrtc, net_handle_channel_state,
                                    NULL);
                                if (pz_net_webrtc_set_remote_offer(
                                        g_app.net_webrtc, offer->sdp)) {
                                    char *answer_sdp
                                        = pz_net_webrtc_create_answer(
                                            g_app.net_webrtc, 10000);
                                    if (answer_sdp) {
                                        pz_net_offer *answer
                                            = pz_net_offer_create(
                                                PZ_NET_PROTOCOL_VERSION,
                                                "client", offer->map_name,
                                                answer_sdp);
                                        char *answer_url
                                            = pz_net_offer_encode_url(answer);
                                        pz_net_offer_free(answer);
                                        pz_free(answer_sdp);
                                        if (answer_url) {
                                            pz_file_write_text(
                                                answer_path, answer_url);
                                            pz_log(PZ_LOG_INFO, PZ_LOG_CAT_NET,
                                                "Debug: wrote answer to '%s'",
                                                answer_path);
                                            pz_free(answer_url);
                                            g_app.net_is_client = true;
                                            pz_campaign_destroy(
                                                g_app.campaign_mgr);
                                            g_app.campaign_mgr
                                                = pz_campaign_create();

                                            // Load the map from the offer
                                            if (map_name[0] != '\0') {
                                                pz_log(PZ_LOG_INFO,
                                                    PZ_LOG_CAT_NET,
                                                    "Debug: loading map '%s' "
                                                    "from offer",
                                                    map_name);
                                                map_session_load(
                                                    &g_app.session, map_name);
                                            }
                                        }
                                    }
                                }
                            }
                            pz_net_offer_free(offer);
                        }
                    }
                }
                pz_debug_script_notify_action_complete(g_app.debug_script);
                break;
            }

            case PZ_DEBUG_SCRIPT_NET_ANSWER: {
                const char *answer_path
                    = pz_debug_script_get_net_answer_path(g_app.debug_script);
                if (answer_path && g_app.net_webrtc && g_app.net_is_host) {
                    char *answer_url = pz_file_read_text(answer_path);
                    if (answer_url) {
                        net_apply_answer_payload(answer_url);
                        pz_free(answer_url);
                    }
                }
                pz_debug_script_notify_action_complete(g_app.debug_script);
                break;
            }

            case PZ_DEBUG_SCRIPT_NET_WAIT:
                // Just continue - the script will poll for connection
                goto done_script_commands;

            case PZ_DEBUG_SCRIPT_WAITING:
                // Script is waiting for something, stop processing
                goto done_script_commands;

            default:
                break;
            }
        }
    }
done_script_commands:;

    // Check debug script modes
    bool script_turbo
        = g_app.debug_script && pz_debug_script_is_turbo(g_app.debug_script);
    bool script_render = !g_app.debug_script
        || pz_debug_script_should_render(g_app.debug_script);

    double current_time = pz_time_now();
    float frame_dt = (float)(current_time - g_app.last_frame_time);
    g_app.last_frame_time = current_time;

    // In turbo mode, use fixed dt for consistent simulation
    if (script_turbo) {
        frame_dt = pz_sim_dt(); // Use fixed timestep
    } else {
        if (frame_dt > 0.1f)
            frame_dt = 0.1f;
        if (frame_dt < 0.0001f)
            frame_dt = 0.0001f;
    }
    g_app.total_time += frame_dt;

    bool editor_active = app_editor_active();
    int sim_ticks = 0;
    float dt = pz_sim_dt(); // Fixed timestep for simulation
    uint64_t sim_start_us = 0;
    uint64_t sim_end_us = 0;
    uint64_t events_start_us = 0;
    uint64_t events_end_us = 0;
    uint64_t visual_start_us = 0;
    uint64_t visual_end_us = 0;

    if (editor_active) {
        // Handle debug script mouse clicks in editor
        const pz_debug_script_input *script_input = g_app.debug_script
            ? pz_debug_script_get_input(g_app.debug_script)
            : NULL;
        if (script_input) {
            if (script_input->mouse_click_left) {
                pz_editor_mouse_down(g_app.editor, 0);
                pz_editor_mouse_up(g_app.editor, 0);
            }
            if (script_input->mouse_click_right) {
                pz_editor_mouse_down(g_app.editor, 1);
                pz_editor_mouse_up(g_app.editor, 1);
            }
        }
        pz_editor_update(g_app.editor, frame_dt);
    } else {
        bool net_active = g_app.net_is_host || g_app.net_is_client;
        if (g_app.net_is_host)
            net_drain_incoming_inputs();
        if (g_app.net_is_client) {
            pz_net_game_state latest_snapshot;
            bool have_snapshot = false;
            while (net_snapshot_queue_pop(
                &g_app.net_snapshot_rx_queue, &latest_snapshot)) {
                have_snapshot = true;
            }
            if (have_snapshot)
                net_apply_snapshot(&latest_snapshot);
        }

        bool net_channel_open
            = net_active && atomic_load(&g_app.net_channel_open);

        // Determine number of simulation ticks to run this frame
        sim_ticks = script_turbo ? 1 : pz_sim_accumulate(g_app.sim, frame_dt);

        // Gather input (once per frame)
        pz_tank_input player_input = { 0 };
        pz_vec2 player_cursor = g_app.session.player_tank
            ? g_app.session.player_tank->pos
            : (pz_vec2) { 0, 0 };

        // Check if debug script is providing input
        const pz_debug_script_input *script_input = g_app.debug_script
            ? pz_debug_script_get_input(g_app.debug_script)
            : NULL;
        bool use_script_input = script_input && g_app.debug_script
            && !pz_debug_script_is_done(g_app.debug_script);

        if (use_script_input) {
            // Use script input
            player_input.move_dir.x = script_input->move_x;
            player_input.move_dir.y = script_input->move_y;

            if (script_input->has_aim && g_app.session.player_tank) {
                player_cursor
                    = (pz_vec2) { script_input->aim_x, script_input->aim_y };
                float aim_dx
                    = player_cursor.x - g_app.session.player_tank->pos.x;
                float aim_dz
                    = player_cursor.y - g_app.session.player_tank->pos.y;
                player_input.target_turret = atan2f(aim_dx, aim_dz);
            }

            player_input.fire = script_input->fire || script_input->hold_fire;
        } else {
            // Normal keyboard/mouse input
            if (g_app.key_down[SAPP_KEYCODE_W]
                || g_app.key_down[SAPP_KEYCODE_UP]) {
                player_input.move_dir.y -= 1.0f;
            }
            if (g_app.key_down[SAPP_KEYCODE_S]
                || g_app.key_down[SAPP_KEYCODE_DOWN]) {
                player_input.move_dir.y += 1.0f;
            }
            if (g_app.key_down[SAPP_KEYCODE_A]
                || g_app.key_down[SAPP_KEYCODE_LEFT]) {
                player_input.move_dir.x -= 1.0f;
            }
            if (g_app.key_down[SAPP_KEYCODE_D]
                || g_app.key_down[SAPP_KEYCODE_RIGHT]) {
                player_input.move_dir.x += 1.0f;
            }

            if (g_app.session.player_tank
                && !(g_app.session.player_tank->flags & PZ_TANK_FLAG_DEAD)) {
                pz_vec3 mouse_world = pz_camera_screen_to_world(
                    &g_app.camera, (int)g_app.mouse_x, (int)g_app.mouse_y);
                player_cursor = (pz_vec2) { mouse_world.x, mouse_world.z };
                float aim_dx
                    = player_cursor.x - g_app.session.player_tank->pos.x;
                float aim_dz
                    = player_cursor.y - g_app.session.player_tank->pos.y;
                player_input.target_turret = atan2f(aim_dx, aim_dz);
                player_input.fire = g_app.mouse_left_down || g_app.space_down;
            }
        }

        bool player_fire_pressed
            = g_app.mouse_left_just_pressed || g_app.space_just_pressed;
        bool player_place_mine
            = g_app.mouse_right_just_pressed || g_app.key_g_just_pressed;
        if (use_script_input && script_input->fire)
            player_fire_pressed = true;

        // Handle weapon cycling once per frame and include it in client input.
        int player_weapon_switch = 0;
        if (g_app.scroll_accumulator >= 3.0f) {
            player_weapon_switch = 1;
            g_app.scroll_accumulator = 0.0f;
        } else if (g_app.scroll_accumulator <= -3.0f) {
            player_weapon_switch = -1;
            g_app.scroll_accumulator = 0.0f;
        } else if (g_app.key_f_just_pressed) {
            player_weapon_switch = 1;
        } else if (script_input && script_input->weapon_cycle != 0) {
            player_weapon_switch = script_input->weapon_cycle > 0 ? 1 : -1;
        }
        if (player_weapon_switch != 0 && g_app.session.player_tank
            && !(g_app.session.player_tank->flags & PZ_TANK_FLAG_DEAD)) {
            pz_tank_cycle_weapon(
                g_app.session.player_tank, player_weapon_switch);
        }
        bool player_place_barrier = g_app.session.player_tank
            && pz_tank_get_current_weapon(g_app.session.player_tank)
                == PZ_POWERUP_BARRIER_PLACER
            && player_fire_pressed;
        if (g_app.net_is_client) {
            bool new_action = player_fire_pressed || player_place_mine
                || player_place_barrier || player_weapon_switch != 0;
            if (new_action && g_app.net_pending_action_sequence == 0) {
                g_app.net_pending_action_sequence
                    = ++g_app.net_next_action_sequence;
            }
            g_app.net_pending_fire_pressed |= player_fire_pressed;
            g_app.net_pending_place_mine |= player_place_mine;
            g_app.net_pending_place_barrier |= player_place_barrier;
            if (player_weapon_switch != 0)
                g_app.net_pending_weapon_switch = player_weapon_switch;
        }

        sim_start_us = pz_time_now_us();

        // =========================================================================
        // FIXED TIMESTEP SIMULATION LOOP
        // Run N simulation ticks at fixed dt for deterministic gameplay
        // Clients: only send input, don't simulate (host is authoritative)
        // =========================================================================

        // Clients predict only their own movement. The host remains
        // authoritative for combat and all world entities.
        if (g_app.net_is_client) {
            for (int tick = 0; tick < sim_ticks && g_app.session.map; tick++) {
                pz_sim_begin_tick(g_app.sim);
                if (net_channel_open) {
                    uint32_t sequence = net_send_input(&player_input,
                        player_cursor, g_app.net_pending_fire_pressed,
                        g_app.net_pending_place_mine,
                        g_app.net_pending_place_barrier,
                        g_app.net_pending_weapon_switch);
                    if (sequence != 0) {
                        if (g_app.session.player_tank) {
                            pz_tank_update(g_app.session.tank_mgr,
                                g_app.session.player_tank, &player_input,
                                g_app.session.map, NULL, dt);
                        }
                    }
                }
                pz_sim_end_tick(g_app.sim);
            }
            goto after_simulation;
        }

        for (int tick = 0; tick < sim_ticks && g_app.session.map; tick++) {
            uint32_t sim_tick = (uint32_t)pz_sim_tick(g_app.sim);
            pz_tank_input remote_input = { 0 };
            pz_net_input remote_net_input = { 0 };
            bool remote_new_input = false;
            bool remote_new_action = false;
            if (g_app.net_is_host && net_channel_open && g_app.net_remote_tank
                && g_app.net_has_remote_input) {
                remote_net_input = g_app.net_last_remote_input;
                remote_input.move_dir = (pz_vec2) { remote_net_input.move_x,
                    remote_net_input.move_y };
                remote_input.target_turret = remote_net_input.turret_angle;
                remote_input.fire = remote_net_input.fire_held;
                remote_new_input = net_sequence_newer(
                    remote_net_input.sequence, g_app.net_last_processed_input);
                remote_new_action = remote_net_input.action_sequence != 0
                    && net_sequence_newer(remote_net_input.action_sequence,
                        g_app.net_last_processed_action);
            }

            pz_sim_begin_tick(g_app.sim);

            if (g_app.session.toxic_cloud) {
                pz_toxic_cloud_update(g_app.session.toxic_cloud, dt);
            }

            // Player tank update
            if (g_app.session.player_tank
                && !(g_app.session.player_tank->flags & PZ_TANK_FLAG_DEAD)) {
                pz_tank_update(g_app.session.tank_mgr,
                    g_app.session.player_tank, &player_input, g_app.session.map,
                    g_app.session.toxic_cloud, dt);

                // Track marks for player
                if (g_app.session.tracks
                    && pz_vec2_len(g_app.session.player_tank->vel) > 0.1f) {
                    pz_tracks_add_mark(g_app.session.tracks,
                        g_app.session.player_tank->id,
                        g_app.session.player_tank->pos.x,
                        g_app.session.player_tank->pos.y,
                        g_app.session.player_tank->body_angle, 0.45f,
                        track_strength_for_tank(g_app.session.player_tank));
                }

                // Powerup collection
                pz_barrier_placer_data barrier_data = { 0 };
                pz_powerup_type collected
                    = pz_powerup_check_collection_ex(g_app.session.powerup_mgr,
                        g_app.session.player_tank->pos, 0.7f, &barrier_data);
                if (collected != PZ_POWERUP_NONE) {
                    pz_tank_add_weapon(
                        g_app.session.player_tank, (int)collected);
                    pz_log(PZ_LOG_INFO, PZ_LOG_CAT_GAME, "Player collected: %s",
                        pz_powerup_type_name(collected));
                    net_send_event(PZ_NET_EVENT_POWERUP_COLLECT,
                        g_app.session.player_tank->pos,
                        g_app.session.player_tank->id,
                        g_app.session.player_tank->floor_level,
                        (uint8_t)collected, 0);

                    // If barrier placer, set the barrier data on the tank
                    if (collected == PZ_POWERUP_BARRIER_PLACER) {
                        pz_tank_set_barrier_placer(g_app.session.player_tank,
                            barrier_data.barrier_tile,
                            barrier_data.barrier_health,
                            barrier_data.barrier_count,
                            barrier_data.barrier_lifetime);
                    }
                }

                // Update barrier placement ghost (uses mouse or script cursor)
                pz_vec2 cursor_2d;
                if (g_app.script_cursor_active) {
                    // Use script-controlled cursor position
                    cursor_2d.x = g_app.script_cursor_x;
                    cursor_2d.y = g_app.script_cursor_y;
                } else {
                    // Use mouse position converted to world coords
                    pz_vec3 ghost_cursor_world = pz_camera_screen_to_world(
                        &g_app.camera, (int)g_app.mouse_x, (int)g_app.mouse_y);
                    cursor_2d.x = ghost_cursor_world.x;
                    cursor_2d.y = ghost_cursor_world.z;
                }
                pz_barrier_placer_update_ghost(&g_app.session.barrier_ghost,
                    g_app.session.player_tank, g_app.session.map,
                    g_app.session.barrier_mgr, g_app.session.map->tile_size,
                    cursor_2d);

                // Player firing / barrier placement
                int current_weapon
                    = pz_tank_get_current_weapon(g_app.session.player_tank);
                const pz_weapon_stats *weapon
                    = pz_weapon_get_stats((pz_powerup_type)current_weapon);

                bool fire_held = player_input.fire;
                bool fire_pressed = player_fire_pressed;
                bool should_fire = weapon->auto_fire ? fire_held : fire_pressed;

                // Check if this is a barrier placer weapon
                if (current_weapon == PZ_POWERUP_BARRIER_PLACER) {
                    // Barrier placement instead of firing
                    if (fire_pressed
                        && g_app.session.player_tank->fire_cooldown <= 0.0f) {
                        int placed
                            = pz_barrier_placer_place(g_app.session.player_tank,
                                g_app.session.barrier_mgr, g_app.session.map,
                                &g_app.session.barrier_ghost,
                                g_app.session.map->tile_size);
                        if (placed >= 0) {
                            g_app.session.player_tank->fire_cooldown
                                = weapon->fire_cooldown;
                            // Play placement sound
                            pz_game_sfx_play_plop(g_app.game_sfx);
                            net_send_event(PZ_NET_EVENT_BARRIER_PLACED,
                                g_app.session.player_tank->pos,
                                g_app.session.player_tank->id,
                                g_app.session.player_tank->floor_level, 0, 0);
                        }
                    }
                } else {
                    // Normal weapon firing
                    int active_projectiles = pz_projectile_count_by_owner(
                        g_app.session.projectile_mgr,
                        g_app.session.player_tank->id);
                    bool can_fire
                        = active_projectiles < weapon->max_active_projectiles;

                    if (should_fire && can_fire
                        && g_app.session.player_tank->fire_cooldown <= 0.0f
                        && pz_tank_can_fire(g_app.session.player_tank)) {
                        pz_vec2 spawn_pos = { 0 };
                        pz_vec2 fire_dir = { 0 };
                        int bounce_cost = 0;
                        pz_tank_get_fire_solution(g_app.session.player_tank,
                            g_app.session.map, &spawn_pos, &fire_dir,
                            &bounce_cost);

                        pz_projectile_config proj_config = {
                            .speed = weapon->projectile_speed,
                            .max_bounces = weapon->max_bounces,
                            .lifetime = -1.0f,
                            .damage = weapon->damage,
                            .scale = weapon->projectile_scale,
                            .color = weapon->projectile_color,
                            .floor_level
                            = g_app.session.player_tank->floor_level,
                        };

                        int proj_slot = pz_projectile_spawn(
                            g_app.session.projectile_mgr, spawn_pos, fire_dir,
                            &proj_config, g_app.session.player_tank->id);
                        if (proj_slot >= 0 && bounce_cost > 0) {
                            pz_projectile *proj = &g_app.session.projectile_mgr
                                                       ->projectiles[proj_slot];
                            if (proj->bounces_remaining > 0) {
                                proj->bounces_remaining -= 1;
                            }
                        }

                        g_app.session.player_tank->fire_cooldown
                            = weapon->fire_cooldown;

                        // Trigger visual recoil
                        g_app.session.player_tank->recoil
                            = weapon->recoil_strength;

                        // Play gunfire sound
                        pz_game_sfx_play_gunfire(g_app.game_sfx);
                        net_send_event(PZ_NET_EVENT_GUNFIRE,
                            g_app.session.player_tank->pos,
                            g_app.session.player_tank->id,
                            g_app.session.player_tank->floor_level, 0, 0);
                    }
                }

                // Mine placement (right-click or G key)
                bool place_mine = player_place_mine;
                if (place_mine && g_app.session.player_tank->mine_count > 0
                    && g_app.session.mine_mgr) {
                    // Place mine behind the tank
                    float behind_dist = 1.2f;
                    pz_vec2 back_dir
                        = { -sinf(g_app.session.player_tank->body_angle),
                              -cosf(g_app.session.player_tank->body_angle) };
                    pz_vec2 mine_pos
                        = pz_vec2_add(g_app.session.player_tank->pos,
                            pz_vec2_scale(back_dir, behind_dist));

                    int slot = pz_mine_place(g_app.session.mine_mgr, mine_pos,
                        g_app.session.player_tank->id);
                    if (slot >= 0) {
                        g_app.session.player_tank->mine_count--;
                        pz_log(PZ_LOG_INFO, PZ_LOG_CAT_GAME,
                            "Mine placed, %d remaining",
                            g_app.session.player_tank->mine_count);
                    }
                }
            }

            if (g_app.net_remote_tank
                && !(g_app.net_remote_tank->flags & PZ_TANK_FLAG_DEAD)) {
                if (remote_new_action && remote_net_input.weapon_switch != 0) {
                    pz_tank_cycle_weapon(
                        g_app.net_remote_tank, remote_net_input.weapon_switch);
                }

                pz_tank_update(g_app.session.tank_mgr, g_app.net_remote_tank,
                    &remote_input, g_app.session.map, g_app.session.toxic_cloud,
                    dt);

                if (g_app.session.tracks
                    && pz_vec2_len(g_app.net_remote_tank->vel) > 0.1f) {
                    pz_tracks_add_mark(g_app.session.tracks,
                        g_app.net_remote_tank->id, g_app.net_remote_tank->pos.x,
                        g_app.net_remote_tank->pos.y,
                        g_app.net_remote_tank->body_angle, 0.45f,
                        track_strength_for_tank(g_app.net_remote_tank));
                }

                pz_barrier_placer_data barrier_data = { 0 };
                pz_powerup_type collected
                    = pz_powerup_check_collection_ex(g_app.session.powerup_mgr,
                        g_app.net_remote_tank->pos, 0.7f, &barrier_data);
                if (collected != PZ_POWERUP_NONE) {
                    pz_tank_add_weapon(g_app.net_remote_tank, (int)collected);
                    if (collected == PZ_POWERUP_BARRIER_PLACER) {
                        pz_tank_set_barrier_placer(g_app.net_remote_tank,
                            barrier_data.barrier_tile,
                            barrier_data.barrier_health,
                            barrier_data.barrier_count,
                            barrier_data.barrier_lifetime);
                    }
                    net_send_event(PZ_NET_EVENT_POWERUP_COLLECT,
                        g_app.net_remote_tank->pos, g_app.net_remote_tank->id,
                        g_app.net_remote_tank->floor_level, (uint8_t)collected,
                        0);
                }

                int current_weapon
                    = pz_tank_get_current_weapon(g_app.net_remote_tank);
                const pz_weapon_stats *weapon
                    = pz_weapon_get_stats((pz_powerup_type)current_weapon);
                bool should_fire = weapon->auto_fire
                    ? remote_net_input.fire_held
                    : (remote_new_action && remote_net_input.fire_pressed);

                if (current_weapon == PZ_POWERUP_BARRIER_PLACER) {
                    if (remote_new_action && remote_net_input.place_barrier
                        && g_app.net_remote_tank->fire_cooldown <= 0.0f) {
                        pz_barrier_ghost ghost = { 0 };
                        pz_barrier_placer_update_ghost(&ghost,
                            g_app.net_remote_tank, g_app.session.map,
                            g_app.session.barrier_mgr,
                            g_app.session.map->tile_size,
                            (pz_vec2) { remote_net_input.cursor_x,
                                remote_net_input.cursor_y });
                        int placed
                            = pz_barrier_placer_place(g_app.net_remote_tank,
                                g_app.session.barrier_mgr, g_app.session.map,
                                &ghost, g_app.session.map->tile_size);
                        if (placed >= 0) {
                            g_app.net_remote_tank->fire_cooldown
                                = weapon->fire_cooldown;
                            pz_game_sfx_play_plop(g_app.game_sfx);
                            net_send_event(PZ_NET_EVENT_BARRIER_PLACED,
                                g_app.net_remote_tank->pos,
                                g_app.net_remote_tank->id,
                                g_app.net_remote_tank->floor_level, 0, 0);
                        }
                    }
                } else {
                    int active_projectiles = pz_projectile_count_by_owner(
                        g_app.session.projectile_mgr,
                        g_app.net_remote_tank->id);
                    bool can_fire
                        = active_projectiles < weapon->max_active_projectiles;
                    if (should_fire && can_fire
                        && g_app.net_remote_tank->fire_cooldown <= 0.0f
                        && pz_tank_can_fire(g_app.net_remote_tank)) {
                        pz_vec2 spawn_pos = { 0 };
                        pz_vec2 fire_dir = { 0 };
                        int bounce_cost = 0;
                        pz_tank_get_fire_solution(g_app.net_remote_tank,
                            g_app.session.map, &spawn_pos, &fire_dir,
                            &bounce_cost);

                        pz_projectile_config proj_config = {
                            .speed = weapon->projectile_speed,
                            .max_bounces = weapon->max_bounces,
                            .lifetime = -1.0f,
                            .damage = weapon->damage,
                            .scale = weapon->projectile_scale,
                            .color = weapon->projectile_color,
                            .floor_level = g_app.net_remote_tank->floor_level,
                        };
                        int proj_slot = pz_projectile_spawn(
                            g_app.session.projectile_mgr, spawn_pos, fire_dir,
                            &proj_config, g_app.net_remote_tank->id);
                        if (proj_slot >= 0 && bounce_cost > 0) {
                            pz_projectile *proj = &g_app.session.projectile_mgr
                                                       ->projectiles[proj_slot];
                            if (proj->bounces_remaining > 0)
                                proj->bounces_remaining--;
                        }
                        g_app.net_remote_tank->fire_cooldown
                            = weapon->fire_cooldown;
                        g_app.net_remote_tank->recoil = weapon->recoil_strength;
                        pz_game_sfx_play_gunfire(g_app.game_sfx);
                        net_send_event(PZ_NET_EVENT_GUNFIRE,
                            g_app.net_remote_tank->pos,
                            g_app.net_remote_tank->id,
                            g_app.net_remote_tank->floor_level, 0, 0);
                    }
                }

                if (remote_new_action && remote_net_input.place_mine
                    && g_app.net_remote_tank->mine_count > 0
                    && g_app.session.mine_mgr) {
                    pz_vec2 back_dir
                        = { -sinf(g_app.net_remote_tank->body_angle),
                              -cosf(g_app.net_remote_tank->body_angle) };
                    pz_vec2 mine_pos = pz_vec2_add(g_app.net_remote_tank->pos,
                        pz_vec2_scale(back_dir, 1.2f));
                    if (pz_mine_place(g_app.session.mine_mgr, mine_pos,
                            g_app.net_remote_tank->id)
                        >= 0) {
                        g_app.net_remote_tank->mine_count--;
                    }
                }
            }

            if (remote_new_input)
                g_app.net_last_processed_input = remote_net_input.sequence;
            if (remote_new_action) {
                g_app.net_last_processed_action
                    = remote_net_input.action_sequence;
            }

            // Update all tanks (respawn timers, etc.)
            pz_tank_update_all(g_app.session.tank_mgr, g_app.session.map,
                g_app.session.toxic_cloud, dt);

            // Resolve tank-barrier collisions for all tanks
            if (g_app.session.barrier_mgr) {
                for (int i = 0; i < PZ_MAX_TANKS; i++) {
                    pz_tank *tank = &g_app.session.tank_mgr->tanks[i];
                    if ((tank->flags & PZ_TANK_FLAG_ACTIVE)
                        && !(tank->flags & PZ_TANK_FLAG_DEAD)) {
                        pz_barrier_resolve_collision(g_app.session.barrier_mgr,
                            &tank->pos,
                            g_app.session.tank_mgr->collision_radius,
                            tank->floor_level);
                    }
                }
            }

            // Check for jump pad sounds
            for (int i = 0; i < PZ_MAX_TANKS; i++) {
                pz_tank *tank = &g_app.session.tank_mgr->tanks[i];
                if (tank->just_jumped) {
                    pz_game_sfx_play_jump_pad(g_app.game_sfx);
                    net_send_event(PZ_NET_EVENT_JUMP, tank->pos, tank->id,
                        tank->floor_level, 0, 0);
                    tank->just_jumped = false;
                }
            }

            // AI update
            if (g_app.session.ai_mgr && g_app.session.player_tank
                && !(g_app.session.player_tank->flags & PZ_TANK_FLAG_DEAD)) {
                pz_ai_update(g_app.session.ai_mgr,
                    g_app.session.player_tank->pos,
                    g_app.session.projectile_mgr, g_app.session.mine_mgr,
                    pz_sim_rng(g_app.sim), g_app.session.toxic_cloud, dt);
                int ai_shots = pz_ai_fire(
                    g_app.session.ai_mgr, g_app.session.projectile_mgr);

                // Play gunfire sounds for AI shots
                for (int shot = 0; shot < ai_shots; shot++) {
                    pz_game_sfx_play_gunfire(g_app.game_sfx);
                }

                // Track marks for enemy tanks
                if (g_app.session.tracks) {
                    for (int i = 0; i < g_app.session.ai_mgr->controller_count;
                        i++) {
                        pz_ai_controller *ctrl
                            = &g_app.session.ai_mgr->controllers[i];
                        pz_tank *enemy = pz_tank_get_by_id(
                            g_app.session.tank_mgr, ctrl->tank_id);
                        if (enemy && !(enemy->flags & PZ_TANK_FLAG_DEAD)) {
                            if (pz_vec2_len(enemy->vel) > 0.1f) {
                                pz_tracks_add_mark(g_app.session.tracks,
                                    enemy->id, enemy->pos.x, enemy->pos.y,
                                    enemy->body_angle, 0.45f,
                                    track_strength_for_tank(enemy));
                            }
                        }
                    }
                }
            }

            // Powerup, barrier, mine, and projectile updates
            pz_powerup_update(g_app.session.powerup_mgr, dt);
            if (g_app.session.barrier_mgr) {
                pz_barrier_update(g_app.session.barrier_mgr, dt);

                // Credit expired barriers back to their owners
                int expired_count = 0;
                const pz_expired_barrier *expired = pz_barrier_get_expired(
                    g_app.session.barrier_mgr, &expired_count);
                for (int i = 0; i < expired_count; i++) {
                    if (expired[i].owner_tank_id >= 0) {
                        pz_tank *owner = pz_tank_get_by_id(
                            g_app.session.tank_mgr, expired[i].owner_tank_id);
                        if (owner) {
                            pz_tank_on_barrier_destroyed(
                                owner, expired[i].barrier_index);
                        }
                    }
                }
            }
            if (g_app.session.mine_mgr) {
                pz_mine_update(g_app.session.mine_mgr, g_app.session.tank_mgr,
                    g_app.session.projectile_mgr, dt);
            }
            pz_projectile_update(g_app.session.projectile_mgr,
                g_app.session.map, g_app.session.tank_mgr, dt);

            // Check projectile-barrier collisions
            if (g_app.session.barrier_mgr && g_app.session.projectile_mgr) {
                for (int i = 0; i < PZ_MAX_PROJECTILES; i++) {
                    pz_projectile *proj
                        = &g_app.session.projectile_mgr->projectiles[i];
                    if (!proj->active)
                        continue;

                    pz_vec2 hit_pos, hit_normal;
                    pz_barrier *barrier = NULL;

                    // Check if projectile is inside a barrier
                    // Use a small raycast from previous position to current
                    pz_vec2 prev_pos = pz_vec2_sub(
                        proj->pos, pz_vec2_scale(proj->velocity, dt));
                    if (pz_barrier_raycast(g_app.session.barrier_mgr, prev_pos,
                            proj->pos, proj->floor_level, &hit_pos, &hit_normal,
                            &barrier)) {

                        // Apply damage to barrier
                        bool destroyed = false;
                        pz_barrier_apply_damage(g_app.session.barrier_mgr,
                            hit_pos, (float)proj->damage, proj->floor_level,
                            &destroyed);

                        // Record hit for particle effects (reuse existing
                        // system)
                        if (g_app.session.projectile_mgr->hit_count
                            < PZ_MAX_PROJECTILE_HITS) {
                            pz_projectile_hit *hit
                                = &g_app.session.projectile_mgr->hits[g_app
                                        .session.projectile_mgr->hit_count++];
                            hit->type = PZ_HIT_WALL;
                            hit->pos = hit_pos;
                            hit->floor_level = proj->floor_level;
                        }

                        // Destroy projectile
                        proj->active = false;
                        g_app.session.projectile_mgr->active_count--;

                        // If barrier was destroyed, spawn larger explosion
                        if (destroyed) {
                            // Notify owner tank if this was a player-placed
                            // barrier
                            if (barrier->owner_tank_id >= 0) {
                                pz_tank *owner
                                    = pz_tank_get_by_id(g_app.session.tank_mgr,
                                        barrier->owner_tank_id);
                                if (owner) {
                                    // Find barrier index to pass to callback
                                    for (int b = 0; b < PZ_MAX_BARRIERS; b++) {
                                        pz_barrier *check = pz_barrier_get(
                                            g_app.session.barrier_mgr, b);
                                        if (check == barrier) {
                                            pz_tank_on_barrier_destroyed(
                                                owner, b);
                                            break;
                                        }
                                    }
                                }
                            }

                            pz_vec3 exp_pos
                                = { barrier->pos.x, 0.75f, barrier->pos.y };
                            pz_smoke_config explosion = PZ_SMOKE_TANK_HIT;
                            explosion.position = exp_pos;
                            explosion.count = 12;
                            explosion.spread = 1.0f;
                            explosion.scale_min = 1.5f;
                            explosion.scale_max = 2.5f;
                            pz_particle_spawn_smoke(
                                g_app.session.particle_mgr, &explosion);

                            // Add explosion light for destroyed barrier
                            for (int j = 0; j < MAX_EXPLOSION_LIGHTS; j++) {
                                if (g_app.session.explosion_lights[j].timer
                                    <= 0.0f) {
                                    g_app.session.explosion_lights[j].pos
                                        = barrier->pos;
                                    g_app.session.explosion_lights[j].type
                                        = EXPLOSION_LIGHT_BULLET;
                                    g_app.session.explosion_lights[j].duration
                                        = 0.3f;
                                    g_app.session.explosion_lights[j].timer
                                        = 0.3f;
                                    g_app.session.explosion_lights[j]
                                        .floor_level = barrier->floor_level;
                                    break;
                                }
                            }
                        }
                    }
                }
            }

            // Check projectile-mine collisions
            if (g_app.session.mine_mgr && g_app.session.projectile_mgr) {
                for (int i = 0; i < PZ_MAX_PROJECTILES; i++) {
                    pz_projectile *proj
                        = &g_app.session.projectile_mgr->projectiles[i];
                    if (!proj->active)
                        continue;

                    if (pz_mine_check_projectile_hit(g_app.session.mine_mgr,
                            proj->pos, 0.15f, g_app.session.tank_mgr)) {
                        // Projectile hit a mine - destroy the projectile
                        proj->active = false;
                        g_app.session.projectile_mgr->active_count--;
                    }
                }
            }

            // Hash game state for determinism verification
            if (g_app.session.player_tank) {
                pz_sim_hash_vec2(g_app.sim, g_app.session.player_tank->pos.x,
                    g_app.session.player_tank->pos.y);
                pz_sim_hash_float(
                    g_app.sim, g_app.session.player_tank->body_angle);
            }

            pz_sim_end_tick(g_app.sim);

            // Host: send snapshot to client periodically
            if (g_app.net_is_host && net_channel_open) {
                uint32_t interval = g_app.net_snapshot_interval;
                if (interval == 0) {
                    interval
                        = 3; // Default: every 3 ticks (~20Hz at 60 ticks/s)
                }
                if (sim_tick % interval == 0) {
                    net_send_snapshot(sim_tick);
                }
            }
        }

    after_simulation:
        sim_end_us = pz_time_now_us();
        events_start_us = sim_end_us;

        net_client_visual_update(frame_dt);
        net_process_received_events();
        fog_marks_update(&g_app.session, frame_dt);
        fog_marks_emit(&g_app.session);

        {
            pz_projectile_hit hits[PZ_MAX_PROJECTILE_HITS];
            int hit_count = pz_projectile_get_hits(
                g_app.session.projectile_mgr, hits, PZ_MAX_PROJECTILE_HITS);

            for (int i = 0; i < hit_count; i++) {
                pz_vec3 hit_pos = { hits[i].pos.x, 1.18f, hits[i].pos.y };
                net_send_event(PZ_NET_EVENT_PROJECTILE_HIT, hits[i].pos, -1,
                    hits[i].floor_level, (uint8_t)hits[i].type, 0);

                pz_smoke_config smoke = PZ_SMOKE_BULLET_IMPACT;
                smoke.position = hit_pos;

                if (hits[i].type == PZ_HIT_TANK
                    || hits[i].type == PZ_HIT_TANK_NON_FATAL) {
                    smoke = PZ_SMOKE_TANK_HIT;
                    smoke.position = hit_pos;
                }

                // Play bullet-hits-bullet sound
                if (hits[i].type == PZ_HIT_PROJECTILE) {
                    pz_game_sfx_play_bullet_hit(g_app.game_sfx);
                }

                // Play tank hit sound (non-fatal hit)
                if (hits[i].type == PZ_HIT_TANK_NON_FATAL) {
                    pz_game_sfx_play_tank_hit(g_app.game_sfx);
                }

                // Play ricochet sound (bullet bounces off wall)
                if (hits[i].type == PZ_HIT_WALL_RICOCHET) {
                    pz_game_sfx_play_ricochet(g_app.game_sfx);
                }

                pz_particle_spawn_smoke(g_app.session.particle_mgr, &smoke);

                for (int j = 0; j < MAX_EXPLOSION_LIGHTS; j++) {
                    if (g_app.session.explosion_lights[j].timer <= 0.0f) {
                        g_app.session.explosion_lights[j].pos = hits[i].pos;
                        g_app.session.explosion_lights[j].type
                            = EXPLOSION_LIGHT_BULLET;
                        g_app.session.explosion_lights[j].duration = 0.15f;
                        g_app.session.explosion_lights[j].timer
                            = g_app.session.explosion_lights[j].duration;
                        g_app.session.explosion_lights[j].floor_level
                            = hits[i].floor_level;
                        break;
                    }
                }
            }
        }

        // Process mine explosion events
        if (g_app.session.mine_mgr) {
            pz_mine_explosion explosions[PZ_MAX_MINE_EXPLOSIONS];
            int explosion_count = pz_mine_get_explosions(
                g_app.session.mine_mgr, explosions, PZ_MAX_MINE_EXPLOSIONS);

            for (int i = 0; i < explosion_count; i++) {
                pz_vec3 exp_pos
                    = { explosions[i].pos.x, 0.5f, explosions[i].pos.y };
                net_send_event(PZ_NET_EVENT_MINE_EXPLOSION, explosions[i].pos,
                    explosions[i].owner_id, 0, 0, 0);

                // Spawn explosion particles
                pz_smoke_config explosion = PZ_SMOKE_TANK_EXPLOSION;
                explosion.position = exp_pos;
                explosion.count = 15;
                explosion.spread = 1.5f;
                pz_particle_spawn_smoke(g_app.session.particle_mgr, &explosion);

                // Add explosion light (yellow for mines)
                for (int j = 0; j < MAX_EXPLOSION_LIGHTS; j++) {
                    if (g_app.session.explosion_lights[j].timer <= 0.0f) {
                        g_app.session.explosion_lights[j].pos
                            = explosions[i].pos;
                        g_app.session.explosion_lights[j].type
                            = EXPLOSION_LIGHT_MINE;
                        g_app.session.explosion_lights[j].duration = 0.35f;
                        g_app.session.explosion_lights[j].timer = 0.35f;
                        // TODO: Add floor_level to mines for multi-floor
                        g_app.session.explosion_lights[j].floor_level = 0;
                        break;
                    }
                }

                // Play explosion sound
                pz_game_sfx_play_tank_explosion(g_app.game_sfx, false);

                // Replenish mine to owner (if they're still alive)
                if (explosions[i].owner_id >= 0) {
                    pz_tank *owner = pz_tank_get_by_id(
                        g_app.session.tank_mgr, explosions[i].owner_id);
                    if (owner && !(owner->flags & PZ_TANK_FLAG_DEAD)) {
                        if (owner->mine_count < PZ_MINE_MAX_PER_TANK) {
                            owner->mine_count++;
                        }
                    }
                }
            }
        }

        // Process tank death events
        {
            pz_tank_death_event death_events[PZ_MAX_DEATH_EVENTS];
            int death_count = pz_tank_get_death_events(
                g_app.session.tank_mgr, death_events, PZ_MAX_DEATH_EVENTS);

            for (int i = 0; i < death_count; i++) {
                pz_vec3 death_pos
                    = { death_events[i].pos.x, 0.6f, death_events[i].pos.y };
                net_send_event(PZ_NET_EVENT_TANK_DEATH, death_events[i].pos,
                    death_events[i].tank_id, death_events[i].floor_level,
                    death_events[i].is_player ? 1 : 0, 0);

                // Spawn explosion particles
                pz_smoke_config explosion = PZ_SMOKE_TANK_EXPLOSION;
                explosion.position = death_pos;
                pz_particle_spawn_smoke(g_app.session.particle_mgr, &explosion);

                // Add explosion light
                for (int j = 0; j < MAX_EXPLOSION_LIGHTS; j++) {
                    if (g_app.session.explosion_lights[j].timer <= 0.0f) {
                        g_app.session.explosion_lights[j].pos
                            = death_events[i].pos;
                        g_app.session.explosion_lights[j].type
                            = EXPLOSION_LIGHT_TANK;
                        g_app.session.explosion_lights[j].duration = 0.4f;
                        g_app.session.explosion_lights[j].timer
                            = g_app.session.explosion_lights[j].duration;
                        g_app.session.explosion_lights[j].floor_level
                            = death_events[i].floor_level;
                        break;
                    }
                }

                // Check win condition (all enemies defeated)
                if (!death_events[i].is_player
                    && g_app.state == GAME_STATE_PLAYING
                    && g_app.session.initial_enemy_count > 0) {
                    int enemies_remaining
                        = pz_tank_count_enemies_alive(g_app.session.tank_mgr);
                    if (enemies_remaining == 0) {
                        // Last enemy - play big explosion
                        pz_game_sfx_play_tank_explosion(g_app.game_sfx, true);
                        g_app.state = GAME_STATE_LEVEL_COMPLETE;
                        g_app.state_timer = 0.0f;
                        pz_log(PZ_LOG_INFO, PZ_LOG_CAT_GAME,
                            "Victory! All enemies defeated.");
                    } else {
                        // Regular enemy explosion
                        pz_game_sfx_play_tank_explosion(g_app.game_sfx, false);
                    }
                } else if (!death_events[i].is_player) {
                    // Enemy died but we're not in playing state (or no enemies
                    // to track)
                    pz_game_sfx_play_tank_explosion(g_app.game_sfx, false);
                } else {
                    // Player died
                    pz_game_sfx_play_tank_explosion(g_app.game_sfx, false);
                }

                // Handle player death (lives system)
                if (death_events[i].is_player
                    && g_app.state == GAME_STATE_PLAYING) {
                    if (g_app.campaign_mgr && g_app.campaign_mgr->loaded) {
                        // Campaign mode - use lives
                        if (!pz_campaign_player_died(g_app.campaign_mgr)) {
                            // No lives left - game over
                            g_app.state = GAME_STATE_GAME_OVER;
                            g_app.state_timer = 0.0f;
                        } else {
                            // Still have lives - respawn after delay
                            // (Tank respawn is handled by tank manager)
                        }
                    }
                    // Single map mode - just respawn (handled by tank manager)
                }
            }

            // Clear events for next frame
            pz_tank_clear_death_events(g_app.session.tank_mgr);
        }

        // Process tank respawn events
        {
            pz_tank_respawn_event respawn_events[PZ_MAX_RESPAWN_EVENTS];
            int respawn_count = pz_tank_get_respawn_events(
                g_app.session.tank_mgr, respawn_events, PZ_MAX_RESPAWN_EVENTS);

            for (int i = 0; i < respawn_count; i++) {
                pz_tank *respawned = pz_tank_get_by_id(
                    g_app.session.tank_mgr, respawn_events[i].tank_id);
                if (respawned) {
                    net_send_event(PZ_NET_EVENT_TANK_RESPAWN, respawned->pos,
                        respawned->id, respawned->floor_level,
                        respawn_events[i].is_player ? 1 : 0, 0);
                }
                // Clear barriers placed by the respawned tank
                if (g_app.session.barrier_mgr) {
                    pz_barrier_clear_owned_by(
                        g_app.session.barrier_mgr, respawn_events[i].tank_id);
                }

                pz_log(PZ_LOG_DEBUG, PZ_LOG_CAT_GAME, "Tank %d respawned%s",
                    respawn_events[i].tank_id,
                    respawn_events[i].is_player ? " (player)" : "");
            }

            pz_tank_clear_respawn_events(g_app.session.tank_mgr);
        }

        events_end_us = pz_time_now_us();
        visual_start_us = events_end_us;

        // =========================================================================
        // VISUAL-ONLY UPDATES (use frame_dt for smooth animation)
        // =========================================================================
        for (int i = 0; i < MAX_EXPLOSION_LIGHTS; i++) {
            if (g_app.session.explosion_lights[i].timer > 0.0f) {
                g_app.session.explosion_lights[i].timer -= frame_dt;
            }
        }

        spawn_tank_fog(
            g_app.session.particle_mgr, g_app.session.tank_mgr, frame_dt);
        spawn_projectile_fog(
            g_app.session.particle_mgr, g_app.session.projectile_mgr, frame_dt);
        if (g_app.session.toxic_cloud && g_app.session.particle_mgr) {
            pz_toxic_cloud_spawn_particles(g_app.session.toxic_cloud,
                g_app.session.particle_mgr, frame_dt);
        }

        pz_particle_update(g_app.session.particle_mgr, frame_dt);

        // Update engine sounds for all tanks
        pz_game_sfx_update_engines(g_app.game_sfx, g_app.session.tank_mgr);

        if (g_app.game_music && g_app.session.tank_mgr) {
            int enemies_alive
                = pz_tank_count_enemies_alive(g_app.session.tank_mgr);
            bool has_elite = pz_ai_has_elite_alive(g_app.session.ai_mgr);
            bool level_complete = (g_app.state == GAME_STATE_LEVEL_COMPLETE);
            pz_game_music_update(g_app.game_music, enemies_alive, has_elite,
                level_complete, frame_dt);
        }

        visual_end_us = pz_time_now_us();
    }

    double now = pz_time_now();
    if (now - g_app.last_hot_reload_check > 0.5) {
        pz_texture_check_hot_reload(g_app.tex_manager);
        if (!editor_active) {
            bool map_reloaded
                = pz_map_hot_reload_check(g_app.session.hot_reload);
            if (map_reloaded && g_app.session.map) {
                if (g_app.background) {
                    pz_background_set_from_map(
                        g_app.background, g_app.session.map);
                }
                if (g_app.session.lighting) {
                    const pz_map_lighting *map_light
                        = pz_map_get_lighting(g_app.session.map);
                    pz_lighting_set_map_occluders(
                        g_app.session.lighting, g_app.session.map);
                    pz_lighting_set_ambient(
                        g_app.session.lighting, map_light->ambient_color);
                }
                if (g_app.session.toxic_cloud) {
                    pz_toxic_cloud_destroy(g_app.session.toxic_cloud);
                    g_app.session.toxic_cloud = NULL;
                }
                if (g_app.session.map->has_toxic_cloud) {
                    g_app.session.toxic_cloud = pz_toxic_cloud_create(
                        &g_app.session.map->toxic_config,
                        g_app.session.map->world_width,
                        g_app.session.map->world_height);
                }
            }
        }
        g_app.last_hot_reload_check = now;
    }

    uint64_t render_start_us = visual_end_us;

    pz_debug_overlay_begin_frame(g_app.debug_overlay);
    pz_renderer_begin_frame(g_app.renderer);
    pz_renderer_clear(g_app.renderer, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f);

    // Skip rendering if debug script says so (but still need begin/end frame)
    if (!script_render) {
        goto end_frame;
    }

    // Render background (sky gradient) first
    int vp_width, vp_height;
    pz_renderer_get_viewport(g_app.renderer, &vp_width, &vp_height);
    pz_background_render(g_app.background, g_app.renderer, vp_width, vp_height);

    // Editor mode rendering
    if (editor_active) {

        // Check if editor wants to close
        if (pz_editor_wants_close(g_app.editor)) {
            pz_editor_clear_close_request(g_app.editor);
            if (g_app.edit_map_path_arg) {
                // Launched with --edit-map, quit the application
                sapp_request_quit();
            } else {
                // Return to game (TODO: implement proper transition)
                // For now, just quit
                sapp_request_quit();
            }
            goto end_frame;
        }

        // Get editor camera matrices
        pz_mat4 editor_view, editor_proj;
        pz_editor_get_camera(
            g_app.editor, &editor_view, &editor_proj, vp_width, vp_height);
        pz_mat4 editor_vp = pz_mat4_mul(editor_proj, editor_view);

        // Use the fallback texture as a dummy light texture
        // (the fallback texture is always loaded at startup)
        pz_texture_handle fallback_tex = pz_texture_load(
            g_app.tex_manager, "assets/textures/fallback.png");

        // Get map dimensions for light texture UV scaling
        pz_map *editor_map = pz_editor_get_map(g_app.editor);
        float editor_map_width = editor_map ? editor_map->world_width : 1.0f;
        float editor_map_height = editor_map ? editor_map->world_height : 1.0f;

        // Render the map using editor camera
        // Use fallback texture with scale that makes it appear as white
        pz_editor_render_map(g_app.editor, &editor_vp, fallback_tex,
            1.0f / editor_map_width, 1.0f / editor_map_height, 0.5f, 0.5f);

        // Render editor overlays (grid, ghost preview, etc.)
        pz_editor_render(g_app.editor, &editor_vp);

        // Render editor UI (shortcut bar, panels)
        pz_editor_render_ui(g_app.editor, vp_width, vp_height);

        // Render cursor for editor mode
        if (g_app.cursor && !sapp_mouse_locked()) {
            pz_cursor_set_type(g_app.cursor, PZ_CURSOR_ARROW);
            pz_cursor_render(g_app.cursor);
        }

        goto end_frame;
    }

    pz_tracks_update(g_app.session.tracks);

    uint64_t lighting_start_us = pz_time_now_us();
    if (g_app.session.lighting && g_app.session.map) {
        pz_lighting_clear_dynamic_occluders(g_app.session.lighting);

        // Add barrier occluders
        if (g_app.session.barrier_mgr) {
            pz_barrier_add_occluders(
                g_app.session.barrier_mgr, g_app.session.lighting);
        }

        pz_lighting_clear_lights(g_app.session.lighting);

        for (int i = 0; i < PZ_MAX_TANKS; i++) {
            pz_tank *tank = &g_app.session.tank_mgr->tanks[i];
            if ((tank->flags & PZ_TANK_FLAG_ACTIVE)
                && !(tank->flags & PZ_TANK_FLAG_DEAD)) {

                float light_offset = 0.8f;
                pz_vec2 light_dir
                    = { sinf(tank->turret_angle), cosf(tank->turret_angle) };
                pz_vec2 light_pos = pz_vec2_add(
                    tank->pos, pz_vec2_scale(light_dir, light_offset));

                if (g_app.session.map) {
                    bool hit = false;
                    pz_vec2 hit_pos = pz_map_raycast(g_app.session.map,
                        tank->pos, light_dir, light_offset, &hit);
                    if (hit) {
                        light_pos = hit_pos;
                    }
                }

                pz_vec3 light_color;
                if (tank->flags & PZ_TANK_FLAG_PLAYER) {
                    light_color = (pz_vec3) { 0.9f, 0.95f, 1.0f };
                } else {
                    light_color = (pz_vec3) { 1.0f, 0.6f, 0.4f };
                }

                float light_dir_2d = atan2f(
                    cosf(tank->turret_angle), sinf(tank->turret_angle));

                pz_lighting_add_spotlight(g_app.session.lighting, light_pos,
                    light_dir_2d, light_color, 1.2f, 22.5f, PZ_PI * 0.35f, 0.3f,
                    tank->floor_level);
            }
        }

        for (int i = 0; i < PZ_MAX_PROJECTILES; i++) {
            pz_projectile *proj = &g_app.session.projectile_mgr->projectiles[i];
            if (proj->active) {
                pz_vec3 proj_light_color
                    = { proj->color.x, proj->color.y, proj->color.z };

                float backward_angle
                    = atan2f(-proj->velocity.y, -proj->velocity.x);

                pz_lighting_add_spotlight(g_app.session.lighting, proj->pos,
                    backward_angle, proj_light_color, 0.4f, 2.8f,
                    PZ_PI * 0.125f, 0.5f, proj->floor_level);
            }
        }

        for (int i = 0; i < MAX_EXPLOSION_LIGHTS; i++) {
            if (g_app.session.explosion_lights[i].timer > 0.0f) {
                float t = g_app.session.explosion_lights[i].timer
                    / g_app.session.explosion_lights[i].duration;
                float intensity = t * t;

                pz_vec3 exp_color;
                float exp_intensity;
                float exp_radius;

                switch (g_app.session.explosion_lights[i].type) {
                case EXPLOSION_LIGHT_TANK:
                    // Orange-red for tank explosions
                    exp_color = (pz_vec3) { 1.0f, 0.3f + 0.5f * t, 0.1f * t };
                    exp_intensity = 3.0f * intensity;
                    exp_radius = 6.0f;
                    break;
                case EXPLOSION_LIGHT_MINE:
                    // Yellow for mine explosions
                    exp_color = (pz_vec3) { 1.0f, 0.9f, 0.3f * t };
                    exp_intensity = 2.5f * intensity;
                    exp_radius = 5.0f;
                    break;
                case EXPLOSION_LIGHT_BULLET:
                default:
                    // Blue-white for bullet impacts
                    exp_color = (pz_vec3) { 0.7f, 0.8f, 1.0f };
                    exp_intensity = 2.0f * intensity;
                    exp_radius = 4.0f;
                    break;
                }

                pz_lighting_add_point_light(g_app.session.lighting,
                    g_app.session.explosion_lights[i].pos, exp_color,
                    exp_intensity, exp_radius,
                    g_app.session.explosion_lights[i].floor_level);
            }
        }

        for (int i = 0; i < PZ_MAX_POWERUPS; i++) {
            pz_powerup *powerup = &g_app.session.powerup_mgr->powerups[i];
            if (!powerup->active || powerup->collected)
                continue;

            const pz_weapon_stats *stats = pz_weapon_get_stats(powerup->type);
            pz_vec3 powerup_color = { stats->projectile_color.x,
                stats->projectile_color.y, stats->projectile_color.z };

            float flicker
                = pz_powerup_get_flicker(g_app.session.powerup_mgr, i);

            // TODO: Add floor_level to powerups for multi-floor support
            pz_lighting_add_point_light(g_app.session.lighting, powerup->pos,
                powerup_color, 1.0f * flicker, 3.5f, 0);
        }

        // Add mine lights (yellow glow)
        if (g_app.session.mine_mgr) {
            for (int i = 0; i < PZ_MAX_MINES; i++) {
                pz_mine *mine = &g_app.session.mine_mgr->mines[i];
                if (!mine->active)
                    continue;

                pz_vec3 mine_color = { 0.9f, 0.85f, 0.3f }; // Yellow
                float intensity = mine->arm_timer > 0.0f ? 0.6f : 1.0f;
                // TODO: Add floor_level to mines for multi-floor support
                pz_lighting_add_point_light(g_app.session.lighting, mine->pos,
                    mine_color, intensity, 2.5f, 0);
            }
        }

        // Add toxic cloud glow lights
        if (g_app.session.toxic_cloud
            && g_app.session.toxic_cloud->config.enabled
            && g_app.session.toxic_cloud->closing_progress > 0.0f) {
            pz_toxic_cloud *cloud = g_app.session.toxic_cloud;

            // Green glow color - slightly more saturated green than the cloud
            // particles
            pz_vec3 toxic_light_color = {
                cloud->config.color.x * 0.6f,
                cloud->config.color.y * 1.1f,
                cloud->config.color.z * 0.7f,
            };
            // Clamp to valid range
            toxic_light_color.y = pz_minf(toxic_light_color.y, 1.0f);

            // Light intensity scales with cloud progress
            float base_intensity = 0.35f * cloud->closing_progress;
            float light_radius = 6.0f;

            // Sample lights in a grid across the map
            float half_w = cloud->map_width * 0.5f;
            float half_h = cloud->map_height * 0.5f;
            float spacing = 4.0f; // Grid spacing
            int steps_x = (int)((half_w * 2.0f) / spacing);
            int steps_y = (int)((half_h * 2.0f) / spacing);

            for (int ix = 0; ix < steps_x; ix++) {
                float x = -half_w + spacing * 0.5f + ix * spacing;
                for (int iy = 0; iy < steps_y; iy++) {
                    float y = -half_h + spacing * 0.5f + iy * spacing;
                    pz_vec2 pos = { x, y };

                    // Only add light if position is in the toxic zone
                    if (pz_toxic_cloud_is_inside(cloud, pos)) {
                        // Toxic cloud affects all floors, use floor 0
                        pz_lighting_add_point_light(g_app.session.lighting, pos,
                            toxic_light_color, base_intensity, light_radius, 0);
                    }
                }
            }
        }

        pz_lighting_render(g_app.session.lighting);
    }
    uint64_t lighting_end_us = pz_time_now_us();

    const pz_mat4 *vp = pz_camera_get_view_projection(&g_app.camera);

    uint64_t map_start_us = pz_time_now_us();
    pz_map_render_params render_params = { 0 };
    if (g_app.session.tracks) {
        render_params.track_texture
            = pz_tracks_get_texture(g_app.session.tracks);
        pz_tracks_get_uv_transform(g_app.session.tracks,
            &render_params.track_scale_x, &render_params.track_scale_z,
            &render_params.track_offset_x, &render_params.track_offset_z);
    }
    if (g_app.session.lighting) {
        render_params.light_texture
            = pz_lighting_get_texture(g_app.session.lighting);
        pz_lighting_get_uv_transform(g_app.session.lighting,
            &render_params.light_scale_x, &render_params.light_scale_z,
            &render_params.light_offset_x, &render_params.light_offset_z);
    }
    if (g_app.session.map) {
        const pz_map_lighting *map_light
            = pz_map_get_lighting(g_app.session.map);
        render_params.has_sun = map_light->has_sun;
        render_params.sun_direction = map_light->sun_direction;
        render_params.sun_color = map_light->sun_color;
    }

    render_params.fog_disturb_count = 0;
    render_params.fog_disturb_strength = 1.0f;
    if (g_app.session.map && g_app.session.map->has_fog
        && (g_app.session.map->fog_level == 0
            || g_app.session.map->fog_level == 1)) {
        for (int i = 0; i < MAX_FOG_MARKS; i++) {
            fog_mark *mark = &g_app.session.fog_marks[i];
            if (!mark->active || mark->duration <= 0.0f) {
                continue;
            }

            float t = pz_clampf(mark->timer / mark->duration, 0.0f, 1.0f);
            float strength = mark->strength * t;
            if (render_params.fog_disturb_count < PZ_FOG_DISTURB_MAX) {
                int idx = render_params.fog_disturb_count++;
                render_params.fog_disturb_pos[idx]
                    = (pz_vec3) { mark->pos.x, 0.0f, mark->pos.y };
                render_params.fog_disturb_radius[idx] = mark->radius;
                render_params.fog_disturb_strengths[idx] = strength;
                continue;
            }

            int weakest = 0;
            float weakest_strength = render_params.fog_disturb_strengths[0];
            for (int j = 1; j < render_params.fog_disturb_count; j++) {
                if (render_params.fog_disturb_strengths[j] < weakest_strength) {
                    weakest = j;
                    weakest_strength = render_params.fog_disturb_strengths[j];
                }
            }

            if (strength > weakest_strength) {
                render_params.fog_disturb_pos[weakest]
                    = (pz_vec3) { mark->pos.x, 0.0f, mark->pos.y };
                render_params.fog_disturb_radius[weakest] = mark->radius;
                render_params.fog_disturb_strengths[weakest] = strength;
            }
        }
    }

    // Time for water animation
    render_params.time = g_app.total_time;

    pz_map_renderer_draw(g_app.session.renderer, vp, &render_params);

    // Draw debug texture scale grid if enabled
    pz_map_renderer_draw_debug(g_app.session.renderer, vp);
    uint64_t map_end_us = pz_time_now_us();

    // Render barriers (after map, before tanks)
    uint64_t entities_start_us = pz_time_now_us();
    if (g_app.session.barrier_mgr) {
        pz_barrier_render_params barrier_params = { 0 };
        if (g_app.session.lighting) {
            barrier_params.light_texture
                = pz_lighting_get_texture(g_app.session.lighting);
            pz_lighting_get_uv_transform(g_app.session.lighting,
                &barrier_params.light_scale_x, &barrier_params.light_scale_z,
                &barrier_params.light_offset_x, &barrier_params.light_offset_z);
        }
        if (g_app.session.map) {
            const pz_map_lighting *map_light
                = pz_map_get_lighting(g_app.session.map);
            barrier_params.has_sun = map_light->has_sun;
            barrier_params.sun_direction = map_light->sun_direction;
            barrier_params.sun_color = map_light->sun_color;
            barrier_params.ambient = map_light->ambient_color;
        }
        pz_barrier_render(
            g_app.session.barrier_mgr, g_app.renderer, vp, &barrier_params);
    }

    // Render barrier placement ghost (before tanks, semi-transparent)
    if (g_app.session.barrier_placer_renderer && g_app.session.player_tank
        && g_app.session.barrier_ghost.visible) {
        const pz_tank_barrier_placer *placer
            = pz_tank_get_barrier_placer(g_app.session.player_tank);
        if (placer) {
            pz_barrier_placer_render_ghost(
                g_app.session.barrier_placer_renderer, g_app.renderer, vp,
                &g_app.session.barrier_ghost,
                g_app.session.player_tank->body_color, g_app.tile_registry,
                placer->barrier_tile);
        }
    }

    pz_tank_render_params tank_params = { 0 };
    if (g_app.session.lighting) {
        tank_params.light_texture
            = pz_lighting_get_texture(g_app.session.lighting);
        pz_lighting_get_uv_transform(g_app.session.lighting,
            &tank_params.light_scale_x, &tank_params.light_scale_z,
            &tank_params.light_offset_x, &tank_params.light_offset_z);
    }
    if (g_app.session.toxic_cloud
        && g_app.session.toxic_cloud->config.enabled) {
        tank_params.has_toxic = true;
        tank_params.toxic_color = g_app.session.toxic_cloud->config.color;
    }
    pz_tank_render(g_app.session.tank_mgr, g_app.renderer, vp, &tank_params);

    pz_powerup_render(g_app.session.powerup_mgr, g_app.renderer, vp);

    // Render mines
    if (g_app.session.mine_mgr) {
        pz_mine_render_params mine_params = { 0 };
        if (g_app.session.lighting) {
            mine_params.light_texture
                = pz_lighting_get_texture(g_app.session.lighting);
            pz_lighting_get_uv_transform(g_app.session.lighting,
                &mine_params.light_scale_x, &mine_params.light_scale_z,
                &mine_params.light_offset_x, &mine_params.light_offset_z);
        }
        pz_mine_render(
            g_app.session.mine_mgr, g_app.renderer, vp, &mine_params);
    }

    if (g_app.laser_pipeline != PZ_INVALID_HANDLE && g_app.session.map
        && g_app.session.player_tank
        && !(g_app.session.player_tank->flags & PZ_TANK_FLAG_DEAD)) {
        pz_vec2 laser_start = { 0 };
        pz_vec2 laser_dir = { 0 };
        int bounce_cost = 0;
        pz_tank_get_fire_solution(g_app.session.player_tank, g_app.session.map,
            &laser_start, &laser_dir, &bounce_cost);

        pz_vec2 ray_start = laser_start;
        pz_vec2 ray_end
            = pz_vec2_add(ray_start, pz_vec2_scale(laser_dir, LASER_MAX_DIST));

        pz_raycast_result map_hit
            = pz_map_raycast_ex(g_app.session.map, ray_start, ray_end);
        pz_vec2 laser_end = map_hit.hit ? map_hit.point : ray_end;

        // Also check barrier collision for laser
        if (g_app.session.barrier_mgr) {
            pz_vec2 barrier_hit_pos;
            if (pz_barrier_raycast(g_app.session.barrier_mgr, ray_start,
                    ray_end, g_app.session.player_tank->floor_level,
                    &barrier_hit_pos, NULL, NULL)) {
                float barrier_dist = pz_vec2_dist(ray_start, barrier_hit_pos);
                float best_dist
                    = map_hit.hit ? map_hit.distance : LASER_MAX_DIST;
                if (barrier_dist < best_dist) {
                    laser_end = barrier_hit_pos;
                }
            }
        }

        float laser_len = pz_vec2_dist(laser_start, laser_end);
        if (laser_len > 0.01f) {
            float laser_height = 1.18f;

            pz_vec2 perp = { -laser_dir.y, laser_dir.x };
            float half_w = LASER_WIDTH * 0.5f;

            typedef struct {
                float x, y, z;
                float u, v;
            } laser_vertex;

            laser_vertex verts[6];

            pz_vec2 bl = pz_vec2_add(laser_start, pz_vec2_scale(perp, -half_w));
            pz_vec2 br = pz_vec2_add(laser_start, pz_vec2_scale(perp, half_w));
            pz_vec2 tl = pz_vec2_add(laser_end, pz_vec2_scale(perp, -half_w));
            pz_vec2 tr = pz_vec2_add(laser_end, pz_vec2_scale(perp, half_w));

            verts[0] = (laser_vertex) { bl.x, laser_height, bl.y, 0.0f, 0.0f };
            verts[1] = (laser_vertex) { br.x, laser_height, br.y, 1.0f, 0.0f };
            verts[2] = (laser_vertex) { tr.x, laser_height, tr.y, 1.0f, 1.0f };
            verts[3] = (laser_vertex) { bl.x, laser_height, bl.y, 0.0f, 0.0f };
            verts[4] = (laser_vertex) { tr.x, laser_height, tr.y, 1.0f, 1.0f };
            verts[5] = (laser_vertex) { tl.x, laser_height, tl.y, 0.0f, 1.0f };

            pz_renderer_update_buffer(
                g_app.renderer, g_app.laser_vb, 0, verts, sizeof(verts));

            pz_mat4 laser_mvp = *vp;
            pz_renderer_set_uniform_mat4(
                g_app.renderer, g_app.laser_shader, "u_mvp", &laser_mvp);
            pz_renderer_set_uniform_vec4(g_app.renderer, g_app.laser_shader,
                "u_color", (pz_vec4) { 1.0f, 0.2f, 0.2f, 0.6f });

            pz_draw_cmd laser_cmd = {
                .pipeline = g_app.laser_pipeline,
                .vertex_buffer = g_app.laser_vb,
                .vertex_count = 6,
            };
            pz_renderer_draw(g_app.renderer, &laser_cmd);
        }
    }

    pz_projectile_render_params proj_params = { 0 };
    if (g_app.session.lighting) {
        proj_params.light_texture
            = pz_lighting_get_texture(g_app.session.lighting);
        pz_lighting_get_uv_transform(g_app.session.lighting,
            &proj_params.light_scale_x, &proj_params.light_scale_z,
            &proj_params.light_offset_x, &proj_params.light_offset_z);
    }
    pz_projectile_render(
        g_app.session.projectile_mgr, g_app.renderer, vp, &proj_params);

    {
        const pz_mat4 *view = pz_camera_get_view(&g_app.camera);
        pz_vec3 cam_right = { view->m[0], view->m[4], view->m[8] };
        pz_vec3 cam_up = { view->m[1], view->m[5], view->m[9] };

        pz_particle_render_params particle_params = { 0 };
        if (g_app.session.lighting) {
            particle_params.light_texture
                = pz_lighting_get_texture(g_app.session.lighting);
            pz_lighting_get_uv_transform(g_app.session.lighting,
                &particle_params.light_scale_x, &particle_params.light_scale_z,
                &particle_params.light_offset_x,
                &particle_params.light_offset_z);
        }

        pz_particle_render(g_app.session.particle_mgr, g_app.renderer, vp,
            cam_right, cam_up, &particle_params);
    }
    uint64_t entities_end_us = pz_time_now_us();

    // Render HUD
    uint64_t hud_start_us = pz_time_now_us();
    if (g_app.font_mgr && g_app.font_russo) {
        pz_font_begin_frame(g_app.font_mgr);

        // Get logical viewport size (framebuffer / dpi_scale)
        int fb_width, fb_height;
        pz_renderer_get_viewport(g_app.renderer, &fb_width, &fb_height);
        float dpi_scale = sapp_dpi_scale();
        float vp_width = (float)fb_width / dpi_scale;
        float vp_height = (float)fb_height / dpi_scale;

        // Render spawn indicators (behind other HUD elements)
        if (g_app.spawn_indicator && g_app.session.tank_mgr) {
            pz_spawn_indicator_render(g_app.spawn_indicator, g_app.renderer,
                g_app.font_mgr, g_app.font_russo, g_app.session.tank_mgr,
                &g_app.camera, (int)vp_width, (int)vp_height);
        }

        // Font sizes and positions are in logical pixels - DPI scaling is
        // handled internally
        pz_text_style health_style
            = PZ_TEXT_STYLE_DEFAULT(g_app.font_russo, 36.0f);
        health_style.align_h = PZ_FONT_ALIGN_RIGHT;
        health_style.align_v = PZ_FONT_ALIGN_BOTTOM;

        // White text with black outline for visibility
        health_style.color = pz_vec4_new(1.0f, 1.0f, 1.0f, 1.0f);
        health_style.outline_width = 5.0f;
        health_style.outline_color = pz_vec4_new(0.0f, 0.0f, 0.0f, 1.0f);

        // Player health and mines (bottom-right)
        if (g_app.session.player_tank) {
            pz_font_drawf(g_app.font_mgr, &health_style, vp_width - 20.0f,
                vp_height - 20.0f, "HP: %d  Mines: %d",
                g_app.session.player_tank->health,
                g_app.session.player_tank->mine_count);
        }

        // Lives display (bottom-left) - only in campaign mode
        if (g_app.campaign_mgr && g_app.campaign_mgr->loaded) {
            pz_text_style lives_style
                = PZ_TEXT_STYLE_DEFAULT(g_app.font_russo, 28.0f);
            lives_style.align_h = PZ_FONT_ALIGN_LEFT;
            lives_style.align_v = PZ_FONT_ALIGN_BOTTOM;
            lives_style.color = pz_vec4_new(0.6f, 0.9f, 1.0f, 1.0f);
            lives_style.outline_width = 4.0f;
            lives_style.outline_color = pz_vec4_new(0.0f, 0.0f, 0.0f, 1.0f);

            pz_font_drawf(g_app.font_mgr, &lives_style, 20.0f,
                vp_height - 20.0f, "Lives: %d",
                pz_campaign_get_lives(g_app.campaign_mgr));

            // Level indicator (top-left)
            pz_text_style level_style
                = PZ_TEXT_STYLE_DEFAULT(g_app.font_russo, 24.0f);
            level_style.align_h = PZ_FONT_ALIGN_LEFT;
            level_style.align_v = PZ_FONT_ALIGN_TOP;
            level_style.color = pz_vec4_new(0.8f, 0.8f, 0.8f, 1.0f);
            level_style.outline_width = 4.0f;
            level_style.outline_color = pz_vec4_new(0.0f, 0.0f, 0.0f, 1.0f);

            pz_font_drawf(g_app.font_mgr, &level_style, 20.0f, 20.0f,
                "Level %d/%d", pz_campaign_get_level_number(g_app.campaign_mgr),
                pz_campaign_get_level_count(g_app.campaign_mgr));
        }

        // Enemies remaining (top-right)
        if (g_app.session.initial_enemy_count > 0) {
            int enemies_alive
                = pz_tank_count_enemies_alive(g_app.session.tank_mgr);

            pz_text_style enemy_style
                = PZ_TEXT_STYLE_DEFAULT(g_app.font_russo, 28.0f);
            enemy_style.align_h = PZ_FONT_ALIGN_RIGHT;
            enemy_style.align_v = PZ_FONT_ALIGN_TOP;
            enemy_style.color = pz_vec4_new(1.0f, 0.8f, 0.6f, 1.0f);
            enemy_style.outline_width = 4.0f;
            enemy_style.outline_color = pz_vec4_new(0.0f, 0.0f, 0.0f, 1.0f);

            pz_font_drawf(g_app.font_mgr, &enemy_style, vp_width - 20.0f, 20.0f,
                "Enemies: %d", enemies_alive);
        }

        // State-based overlays
        pz_text_style title_style
            = PZ_TEXT_STYLE_DEFAULT(g_app.font_russo, 64.0f);
        title_style.align_h = PZ_FONT_ALIGN_CENTER;
        title_style.align_v = PZ_FONT_ALIGN_MIDDLE;
        title_style.outline_width = 6.0f;

        pz_text_style subtitle_style
            = PZ_TEXT_STYLE_DEFAULT(g_app.font_russo, 28.0f);
        subtitle_style.align_h = PZ_FONT_ALIGN_CENTER;
        subtitle_style.align_v = PZ_FONT_ALIGN_MIDDLE;
        subtitle_style.color = pz_vec4_new(0.9f, 0.9f, 0.9f, 1.0f);
        subtitle_style.outline_width = 4.0f;
        subtitle_style.outline_color = pz_vec4_new(0.0f, 0.0f, 0.0f, 1.0f);

        if (g_app.state == GAME_STATE_LEVEL_COMPLETE) {
            g_app.state_timer += frame_dt;

            title_style.color = pz_vec4_new(1.0f, 0.9f, 0.3f, 1.0f);
            title_style.outline_color = pz_vec4_new(0.2f, 0.15f, 0.0f, 1.0f);

            pz_font_draw(g_app.font_mgr, &title_style, vp_width * 0.5f,
                vp_height * 0.4f, "LEVEL COMPLETE!");

            if (g_app.state_timer > 1.5f) {
                // Check if there are more levels
                bool has_next = g_app.campaign_mgr && g_app.campaign_mgr->loaded
                    && (pz_campaign_get_level_number(g_app.campaign_mgr)
                        < pz_campaign_get_level_count(g_app.campaign_mgr));

                if (has_next) {
                    pz_font_draw(g_app.font_mgr, &subtitle_style,
                        vp_width * 0.5f, vp_height * 0.55f,
                        "Press SPACE for next level, R to replay");
                } else if (g_app.campaign_mgr && g_app.campaign_mgr->loaded) {
                    // Last level of campaign - SPACE finishes campaign
                    pz_font_draw(g_app.font_mgr, &subtitle_style,
                        vp_width * 0.5f, vp_height * 0.55f,
                        "Press SPACE to finish, R to replay");
                } else {
                    // Single map mode
                    pz_font_draw(g_app.font_mgr, &subtitle_style,
                        vp_width * 0.5f, vp_height * 0.55f,
                        "Press R to replay");
                }
            }
        } else if (g_app.state == GAME_STATE_CAMPAIGN_COMPLETE) {
            g_app.state_timer += frame_dt;

            title_style.color = pz_vec4_new(1.0f, 0.9f, 0.3f, 1.0f);
            title_style.outline_color = pz_vec4_new(0.2f, 0.15f, 0.0f, 1.0f);

            pz_font_draw(g_app.font_mgr, &title_style, vp_width * 0.5f,
                vp_height * 0.4f, "CAMPAIGN COMPLETE!");

            if (g_app.state_timer > 1.5f) {
                pz_font_draw(g_app.font_mgr, &subtitle_style, vp_width * 0.5f,
                    vp_height * 0.55f, "Congratulations! Press R to restart");
            }
        } else if (g_app.state == GAME_STATE_GAME_OVER) {
            g_app.state_timer += frame_dt;

            title_style.color = pz_vec4_new(1.0f, 0.3f, 0.3f, 1.0f);
            title_style.outline_color = pz_vec4_new(0.3f, 0.0f, 0.0f, 1.0f);

            pz_font_draw(g_app.font_mgr, &title_style, vp_width * 0.5f,
                vp_height * 0.4f, "GAME OVER");

            if (g_app.state_timer > 1.5f) {
                pz_font_draw(g_app.font_mgr, &subtitle_style, vp_width * 0.5f,
                    vp_height * 0.55f, "Press R to restart campaign");
            }
        }

        pz_font_end_frame(g_app.font_mgr);
    }
    uint64_t hud_end_us = pz_time_now_us();

    uint64_t render_end_us = pz_time_now_us();
    float sim_ms = us_to_ms(sim_end_us - sim_start_us);
    float events_ms = us_to_ms(events_end_us - events_start_us);
    float visual_ms = us_to_ms(visual_end_us - visual_start_us);
    float lighting_ms = us_to_ms(lighting_end_us - lighting_start_us);
    float map_ms = us_to_ms(map_end_us - map_start_us);
    float entities_ms = us_to_ms(entities_end_us - entities_start_us);
    float hud_ms = us_to_ms(hud_end_us - hud_start_us);
    float render_ms = us_to_ms(render_end_us - render_start_us);
    int light_count = pz_lighting_get_light_count(g_app.session.lighting);
    int occluder_count = pz_lighting_get_occluder_count(g_app.session.lighting);
    int edge_count = pz_lighting_get_edge_count(g_app.session.lighting);
    int projectile_count = pz_projectile_count(g_app.session.projectile_mgr);
    int particle_count = pz_particle_count(g_app.session.particle_mgr);
    int enemies_alive = 0;
    if (g_app.session.tank_mgr) {
        enemies_alive = pz_tank_count_enemies_alive(g_app.session.tank_mgr);
    }

    if (pz_debug_overlay_is_visible(g_app.debug_overlay)) {
        int x = 10;
        int y = 10;
        int line_height = 16;

        pz_debug_overlay_text(g_app.debug_overlay, x, y, "Perf (ms)");
        y += line_height;
        pz_debug_overlay_text(g_app.debug_overlay, x, y, "Sim: %.2f (ticks %d)",
            sim_ms, sim_ticks);
        y += line_height;
        pz_debug_overlay_text(
            g_app.debug_overlay, x, y, "Events: %.2f", events_ms);
        y += line_height;
        pz_debug_overlay_text(
            g_app.debug_overlay, x, y, "Visual: %.2f", visual_ms);
        y += line_height;
        pz_debug_overlay_text(g_app.debug_overlay, x, y,
            "Lighting: %.2f (L%d O%d E%d)", lighting_ms, light_count,
            occluder_count, edge_count);
        y += line_height;
        pz_debug_overlay_text(g_app.debug_overlay, x, y, "Map: %.2f", map_ms);
        y += line_height;
        pz_debug_overlay_text(
            g_app.debug_overlay, x, y, "Entities: %.2f", entities_ms);
        y += line_height;
        pz_debug_overlay_text(g_app.debug_overlay, x, y, "HUD: %.2f", hud_ms);
        y += line_height;
        pz_debug_overlay_text(
            g_app.debug_overlay, x, y, "Render: %.2f", render_ms);
        y += line_height;
        pz_debug_overlay_text(g_app.debug_overlay, x, y,
            "Projectiles: %d  Particles: %d", projectile_count, particle_count);
    }

    if (current_time - g_app.last_perf_log_time >= 5.0) {
        float fps = pz_debug_overlay_get_fps(g_app.debug_overlay);
        float frame_ms
            = pz_debug_overlay_get_frame_time_ms(g_app.debug_overlay);
        const char *map_name
            = g_app.session.map ? g_app.session.map->name : "none";
        pz_log(PZ_LOG_INFO, PZ_LOG_CAT_GAME,
            "Perf %s: fps=%.1f frame=%.2fms sim=%.2fms events=%.2fms "
            "visual=%.2fms lighting=%.2fms map=%.2fms entities=%.2fms "
            "hud=%.2fms render=%.2fms L=%d O=%d E=%d proj=%d particles=%d "
            "enemies=%d ticks=%d",
            map_name, fps, frame_ms, sim_ms, events_ms, visual_ms, lighting_ms,
            map_ms, entities_ms, hud_ms, render_ms, light_count, occluder_count,
            edge_count, projectile_count, particle_count, enemies_alive,
            sim_ticks);
        g_app.last_perf_log_time = current_time;
    }

    // Render debug overlay on top of everything
    render_music_debug_overlay();
    pz_debug_overlay_render(g_app.debug_overlay);
    pz_debug_overlay_end_frame(g_app.debug_overlay);

    // Render custom cursor (on top of everything including debug overlay)
    // Don't render when mouse is locked (fullscreen mode)
    if (g_app.cursor && !sapp_mouse_locked()) {
        // Use crosshair during gameplay, arrow during menus/overlays
        if (g_app.state == GAME_STATE_PLAYING) {
            pz_cursor_set_type(g_app.cursor, PZ_CURSOR_CROSSHAIR);
        } else {
            pz_cursor_set_type(g_app.cursor, PZ_CURSOR_ARROW);
        }
        pz_cursor_render(g_app.cursor);
    }

end_frame:;
    bool should_quit = false;
    g_app.frame_count++;

    // Handle debug script screenshot/dump requests
    if (script_should_screenshot && script_screenshot_path[0]) {
        pz_renderer_save_screenshot(g_app.renderer, script_screenshot_path);
    }
    if (script_should_dump && script_dump_path[0]) {
        pz_debug_script_dump_state(script_dump_path, g_app.session.tank_mgr,
            g_app.session.projectile_mgr, g_app.session.ai_mgr,
            g_app.session.toxic_cloud, g_app.session.player_tank,
            g_app.frame_count);

        // Append networking state to the dump
        bool net_active = g_app.net_is_host || g_app.net_is_client;
        if (net_active) {
            FILE *f = fopen(script_dump_path, "a");
            if (f) {
                fprintf(f, "[networking]\n");
                fprintf(f, "mode: %s\n", g_app.net_is_host ? "host" : "client");
                fprintf(f, "channel_open: %s\n",
                    atomic_load(&g_app.net_channel_open) ? "yes" : "no");
                fprintf(f, "snapshot_tick: %u\n", g_app.net_snapshot_tick);
                fprintf(f, "next_input_sequence: %u\n",
                    g_app.net_next_input_sequence);
                fprintf(f, "last_processed_input: %u\n",
                    g_app.net_last_processed_input);
                fprintf(f, "last_processed_action: %u\n",
                    g_app.net_last_processed_action);
                fprintf(f, "has_remote_input: %s\n",
                    g_app.net_has_remote_input ? "yes" : "no");

                if (g_app.net_has_remote_input) {
                    fprintf(f, "last_remote_input:\n");
                    fprintf(f, "  sequence: %u\n",
                        g_app.net_last_remote_input.sequence);
                    fprintf(f, "  action_sequence: %u\n",
                        g_app.net_last_remote_input.action_sequence);
                    fprintf(f, "  move_dir: %.3f %.3f\n",
                        g_app.net_last_remote_input.move_x,
                        g_app.net_last_remote_input.move_y);
                    fprintf(f, "  target_turret: %.3f\n",
                        g_app.net_last_remote_input.turret_angle);
                    fprintf(f, "  fire: %s\n",
                        g_app.net_last_remote_input.fire_held ? "yes" : "no");
                }

                if (g_app.net_remote_tank) {
                    fprintf(f, "remote_tank:\n");
                    fprintf(f, "  id: %d\n", g_app.net_remote_tank->id);
                    fprintf(f, "  pos: %.3f %.3f\n",
                        g_app.net_remote_tank->pos.x,
                        g_app.net_remote_tank->pos.y);
                    fprintf(f, "  vel: %.3f %.3f\n",
                        g_app.net_remote_tank->vel.x,
                        g_app.net_remote_tank->vel.y);
                    fprintf(f, "  body_angle: %.3f\n",
                        g_app.net_remote_tank->body_angle);
                    fprintf(f, "  turret_angle: %.3f\n",
                        g_app.net_remote_tank->turret_angle);
                    fprintf(f, "  health: %d\n", g_app.net_remote_tank->health);
                    fprintf(
                        f, "  flags: 0x%08x\n", g_app.net_remote_tank->flags);
                } else {
                    fprintf(f, "remote_tank: none\n");
                }

                fprintf(f, "\n");
                fclose(f);
            }
        }
    }

    // Save lightmap debug image on first frame if requested
    if (g_app.lightmap_debug_path && g_app.frame_count >= 1) {
        pz_lighting_save_debug(
            g_app.session.lighting, g_app.lightmap_debug_path);
        g_app.lightmap_debug_path = NULL;
    }

    pz_renderer_end_frame(g_app.renderer);

    if (should_quit) {
        sapp_quit();
    }

    g_app.mouse_left_just_pressed = false;
    g_app.mouse_right_just_pressed = false;
    g_app.space_just_pressed = false;
    g_app.key_f_just_pressed = false;
    g_app.key_g_just_pressed = false;
}

static void
app_event(const sapp_event *event)
{
    if (!event)
        return;

    // Block physical input when debug script is active
    bool block_input = pz_debug_script_blocks_input(g_app.debug_script);
    bool editor_active = app_editor_active();

    switch (event->type) {
    case SAPP_EVENTTYPE_KEY_DOWN:
        // Allow F-keys and escape even during scripts for debugging/emergencies
        if (block_input && event->key_code != SAPP_KEYCODE_ESCAPE
            && event->key_code != SAPP_KEYCODE_F2
            && event->key_code != SAPP_KEYCODE_F3
            && event->key_code != SAPP_KEYCODE_F11
            && event->key_code != SAPP_KEYCODE_F12) {
            return;
        }
        if (event->key_code >= 0 && event->key_code < SAPP_KEYCODE_COUNT) {
            g_app.key_down[event->key_code] = true;
        }
        if (editor_active && pz_editor_event(g_app.editor, event)) {
            return;
        }
        if (!event->key_repeat) {
            if (event->key_code == SAPP_KEYCODE_F2) {
                pz_debug_overlay_toggle(g_app.debug_overlay);
            } else if (event->key_code == SAPP_KEYCODE_ENTER
                && (event->modifiers
                    & (SAPP_MODIFIER_SUPER | SAPP_MODIFIER_ALT))) {
                // Cmd+Enter or Alt+Enter toggles fullscreen
                sapp_toggle_fullscreen();
            } else if (event->key_code == SAPP_KEYCODE_F3) {
                // Toggle texture scale debug visualization
                if (g_app.session.renderer) {
                    bool enabled = pz_map_renderer_get_debug_texture_scale(
                        g_app.session.renderer);
                    pz_map_renderer_set_debug_texture_scale(
                        g_app.session.renderer, !enabled);
                }
            } else if (event->key_code == SAPP_KEYCODE_F11) {
                if (g_app.session.lighting) {
                    pz_lighting_save_debug(g_app.session.lighting,
                        "screenshots/lightmap_debug.png");
                }
            } else if (event->key_code == SAPP_KEYCODE_F12) {
                char *path = generate_screenshot_path();
                if (path) {
                    pz_renderer_save_screenshot(g_app.renderer, path);
                    pz_free(path);
                }
            }
        }
        if (editor_active) {
            return;
        }
        if (!event->key_repeat) {
            if (event->key_code == SAPP_KEYCODE_ESCAPE) {
#ifndef __EMSCRIPTEN__
                sapp_quit();
#endif
            } else if (event->key_code == SAPP_KEYCODE_F) {
                g_app.key_f_just_pressed = true;
            } else if (event->key_code == SAPP_KEYCODE_G) {
                g_app.key_g_just_pressed = true;
            } else if (event->key_code == SAPP_KEYCODE_SPACE) {
                // SPACE fires during gameplay, advances level when complete
                g_app.space_down = true;
                g_app.space_just_pressed = true;
                if (g_app.state == GAME_STATE_LEVEL_COMPLETE
                    && g_app.state_timer > 1.5f) {
                    // Consume the space press so it doesn't fire on new level
                    g_app.space_just_pressed = false;
                    if (g_app.campaign_mgr && g_app.campaign_mgr->loaded) {
                        if (pz_campaign_advance(g_app.campaign_mgr)) {
                            // Load next map
                            const char *next_map = pz_campaign_get_current_map(
                                g_app.campaign_mgr);
                            if (next_map
                                && map_session_load(&g_app.session, next_map)) {
                                g_app.state = GAME_STATE_PLAYING;
                                g_app.state_timer = 0.0f;
                            } else {
                                pz_log(PZ_LOG_ERROR, PZ_LOG_CAT_GAME,
                                    "Failed to load next map");
                            }
                        } else {
                            // Campaign complete!
                            g_app.state = GAME_STATE_CAMPAIGN_COMPLETE;
                            g_app.state_timer = 0.0f;
                        }
                    }
                }
            } else if (event->key_code == SAPP_KEYCODE_R) {
                // R key behavior depends on current state
                if (g_app.state == GAME_STATE_LEVEL_COMPLETE
                    && g_app.state_timer > 1.5f) {
                    // Replay current level
                    map_session_reset(&g_app.session);
                    g_app.state = GAME_STATE_PLAYING;
                    g_app.state_timer = 0.0f;
                    pz_log(PZ_LOG_INFO, PZ_LOG_CAT_GAME, "Level restarted");
                } else if (g_app.state == GAME_STATE_GAME_OVER
                    && g_app.state_timer > 1.5f) {
                    // Restart entire campaign
                    if (g_app.campaign_mgr && g_app.campaign_mgr->loaded) {
                        pz_campaign_start(g_app.campaign_mgr, 0);
                        const char *first_map
                            = pz_campaign_get_current_map(g_app.campaign_mgr);
                        if (first_map
                            && map_session_load(&g_app.session, first_map)) {
                            g_app.state = GAME_STATE_PLAYING;
                            g_app.state_timer = 0.0f;
                            pz_log(PZ_LOG_INFO, PZ_LOG_CAT_GAME,
                                "Campaign restarted");
                        }
                    } else {
                        // Single map mode - just reset
                        map_session_reset(&g_app.session);
                        g_app.state = GAME_STATE_PLAYING;
                        g_app.state_timer = 0.0f;
                    }
                } else if (g_app.state == GAME_STATE_CAMPAIGN_COMPLETE
                    && g_app.state_timer > 1.5f) {
                    // Restart campaign from beginning
                    if (g_app.campaign_mgr && g_app.campaign_mgr->loaded) {
                        pz_campaign_start(g_app.campaign_mgr, 0);
                        const char *first_map
                            = pz_campaign_get_current_map(g_app.campaign_mgr);
                        if (first_map
                            && map_session_load(&g_app.session, first_map)) {
                            g_app.state = GAME_STATE_PLAYING;
                            g_app.state_timer = 0.0f;
                            pz_log(PZ_LOG_INFO, PZ_LOG_CAT_GAME,
                                "Campaign restarted");
                        }
                    }
                }
            }
        }
        break;
    case SAPP_EVENTTYPE_KEY_UP:
        if (block_input)
            break;
        if (event->key_code >= 0 && event->key_code < SAPP_KEYCODE_COUNT) {
            g_app.key_down[event->key_code] = false;
        }
        if (editor_active) {
            pz_editor_event(g_app.editor, event);
            break;
        }
        if (event->key_code == SAPP_KEYCODE_SPACE) {
            g_app.space_down = false;
        }
        break;
    case SAPP_EVENTTYPE_CHAR:
        if (block_input)
            break;
        if (editor_active && pz_editor_event(g_app.editor, event)) {
            break;
        }
        break;
    case SAPP_EVENTTYPE_MOUSE_MOVE:
        if (block_input)
            break;
        g_app.mouse_x = event->mouse_x;
        g_app.mouse_y = event->mouse_y;
        pz_cursor_set_position(g_app.cursor, g_app.mouse_x, g_app.mouse_y);
        if (editor_active) {
            pz_editor_event(g_app.editor, event);
        }
        break;
    case SAPP_EVENTTYPE_MOUSE_DOWN:
        if (block_input)
            break;
        if (editor_active && pz_editor_event(g_app.editor, event)) {
            break;
        }
        if (event->mouse_button == SAPP_MOUSEBUTTON_LEFT) {
            g_app.mouse_left_down = true;
            g_app.mouse_left_just_pressed = true;
        } else if (event->mouse_button == SAPP_MOUSEBUTTON_RIGHT) {
            g_app.mouse_right_just_pressed = true;
        }
        break;
    case SAPP_EVENTTYPE_MOUSE_UP:
        if (block_input)
            break;
        if (editor_active && pz_editor_event(g_app.editor, event)) {
            break;
        }
        if (event->mouse_button == SAPP_MOUSEBUTTON_LEFT) {
            g_app.mouse_left_down = false;
        }
        break;
    case SAPP_EVENTTYPE_MOUSE_SCROLL:
        if (block_input)
            break;
        if (editor_active && pz_editor_event(g_app.editor, event)) {
            break;
        }
        g_app.scroll_accumulator += event->scroll_y;
        break;
    case SAPP_EVENTTYPE_RESIZED: {
        int width = sapp_width();
        int height = sapp_height();
        pz_renderer_set_viewport(g_app.renderer, width, height);
        pz_camera_set_viewport(&g_app.camera, width, height);
        pz_log(PZ_LOG_INFO, PZ_LOG_CAT_CORE, "Window resized: %dx%d", width,
            height);
    } break;
    case SAPP_EVENTTYPE_FOCUSED:
    case SAPP_EVENTTYPE_RESTORED:
        // Re-hide OS cursor when window regains focus or is restored
        // macOS can reset cursor visibility in these cases
        // Toggle state to force sokol to re-apply the hide
        sapp_show_mouse(true);
        sapp_show_mouse(false);
        break;
    default:
        break;
    }
}

static void
app_cleanup(void)
{
    // Unload map session (all map-dependent state)
    map_session_unload(&g_app.session);

    // Destroy editor
    pz_editor_destroy(g_app.editor);

    // Destroy campaign manager
    pz_campaign_destroy(g_app.campaign_mgr);

    // Destroy persistent systems
    pz_spawn_indicator_destroy(g_app.spawn_indicator, g_app.renderer);
    pz_font_manager_destroy(g_app.font_mgr);
    pz_debug_overlay_destroy(g_app.debug_overlay);
    pz_cursor_destroy(g_app.cursor);
    pz_debug_cmd_shutdown();

    pz_free(g_app.join_answer);
    pz_free(g_app.join_answer_json);
    pz_net_webrtc_destroy(g_app.net_webrtc);
    pz_net_offer_free(g_app.join_offer);
    pz_signaling_shutdown();

    if (g_app.laser_vb != PZ_INVALID_HANDLE) {
        pz_renderer_destroy_buffer(g_app.renderer, g_app.laser_vb);
    }
    if (g_app.laser_pipeline != PZ_INVALID_HANDLE) {
        pz_renderer_destroy_pipeline(g_app.renderer, g_app.laser_pipeline);
    }
    if (g_app.laser_shader != PZ_INVALID_HANDLE) {
        pz_renderer_destroy_shader(g_app.renderer, g_app.laser_shader);
    }

    pz_background_destroy(g_app.background, g_app.renderer);

    pz_sim_destroy(g_app.sim);

    pz_tile_registry_destroy(g_app.tile_registry);
    pz_texture_manager_destroy(g_app.tex_manager);
    pz_renderer_destroy(g_app.renderer);

    if (g_app.audio) {
        pz_audio_set_callback(g_app.audio, NULL, NULL);
        pz_audio_shutdown(g_app.audio);
    }
    pz_game_sfx_destroy(g_app.game_sfx);
    pz_game_music_destroy(g_app.game_music);

    pz_log_shutdown();
    pz_mem_dump_leaks();

    printf("Tank Game - Exiting.\n");
}

static void
audio_callback(float *buffer, int num_frames, int num_channels, void *userdata)
{
    (void)userdata;

    // Render music first (fills buffer)
    pz_game_music_render(g_app.game_music, buffer, num_frames, num_channels);

    // Render SFX on top (adds to buffer)
    pz_game_sfx_render(g_app.game_sfx, buffer, num_frames, num_channels);
}

// ============================================================================
// Web hosting API (Emscripten only)
// ============================================================================

#ifdef __EMSCRIPTEN__

// Start hosting a game with the specified map
// Returns 1 on success, 0 on failure
EMSCRIPTEN_KEEPALIVE int
pz_web_start_host(const char *map_path)
{
    if (!map_path || map_path[0] == '\0') {
        pz_log(PZ_LOG_ERROR, PZ_LOG_CAT_NET, "web_start_host: no map path");
        return 0;
    }

    // Already hosting or joining?
    if (g_app.net_is_host || g_app.net_is_client) {
        pz_log(PZ_LOG_ERROR, PZ_LOG_CAT_NET,
            "web_start_host: already in network session");
        return 0;
    }

    // Multiplayer is a standalone match, never a campaign level.
    pz_campaign_destroy(g_app.campaign_mgr);
    g_app.campaign_mgr = pz_campaign_create();

    // Load the map first
    if (!map_session_load(&g_app.session, map_path)) {
        pz_log(PZ_LOG_ERROR, PZ_LOG_CAT_NET,
            "web_start_host: failed to load map '%s'", map_path);
        return 0;
    }

    // Set up WebRTC
    const char *ice_servers[]
        = { "stun:stun.l.google.com:19302", "stun:stun.cloudflare.com:3478" };
    pz_net_webrtc_config net_config = {
        .ice_servers = ice_servers,
        .ice_server_count = (int)(sizeof(ice_servers) / sizeof(ice_servers[0])),
        .enable_logging = true,
    };

    g_app.net_webrtc = pz_net_webrtc_create(&net_config);
    if (!g_app.net_webrtc) {
        pz_log(PZ_LOG_ERROR, PZ_LOG_CAT_NET,
            "web_start_host: failed to create WebRTC");
        return 0;
    }

    pz_net_webrtc_set_message_callback(
        g_app.net_webrtc, net_handle_message, NULL);
    pz_net_webrtc_set_channel_callback(
        g_app.net_webrtc, net_handle_channel_state, NULL);

    char *offer_sdp = pz_net_webrtc_create_offer(g_app.net_webrtc, 10000);
    if (!offer_sdp) {
        pz_log(PZ_LOG_ERROR, PZ_LOG_CAT_NET,
            "web_start_host: failed to create offer");
        pz_net_webrtc_destroy(g_app.net_webrtc);
        g_app.net_webrtc = NULL;
        return 0;
    }

    pz_net_offer *offer = pz_net_offer_create(
        PZ_NET_PROTOCOL_VERSION, "host", g_app.session.map_path, offer_sdp);
    char *offer_json = pz_net_offer_encode_json(offer);
    pz_net_offer_free(offer);
    pz_free(offer_sdp);

    if (!offer_json) {
        pz_log(PZ_LOG_ERROR, PZ_LOG_CAT_NET,
            "web_start_host: failed to encode offer");
        pz_net_webrtc_destroy(g_app.net_webrtc);
        g_app.net_webrtc = NULL;
        return 0;
    }

    // Generate room code and publish
    const char *room = pz_signaling_generate_room();
    if (!room || room[0] == '\0') {
        pz_free(offer_json);
        pz_net_webrtc_destroy(g_app.net_webrtc);
        g_app.net_webrtc = NULL;
        return 0;
    }
    strncpy(g_app.net_room_code, room, sizeof(g_app.net_room_code) - 1);
    g_app.net_room_code[sizeof(g_app.net_room_code) - 1] = '\0';
    g_app.net_waiting_for_answer = true;
    g_app.net_is_host = true;
    g_app.net_use_signaling = true;

    pz_log(PZ_LOG_INFO, PZ_LOG_CAT_NET, "Web host: room code %s",
        g_app.net_room_code);
    pz_log(PZ_LOG_INFO, PZ_LOG_CAT_NET, "Web host: offer ready (%zu bytes)",
        strlen(offer_json));

    pz_signaling_publish(g_app.net_room_code, "o", offer_json,
        net_signaling_publish_offer_done, NULL);
    g_app.net_signaling_next_poll = 0.0;
    pz_free(offer_json);

    // Reload map to set up networking (create net_remote_tank)
    // Note: must copy path first since map_session_unload clears it
    char reload_path[256];
    strncpy(reload_path, g_app.session.map_path, sizeof(reload_path) - 1);
    reload_path[sizeof(reload_path) - 1] = '\0';
    map_session_load(&g_app.session, reload_path);

    g_app.state = GAME_STATE_PLAYING;
    g_app.state_timer = 0.0f;

    return 1;
}

// Get the current room code (returns empty string if not hosting)
EMSCRIPTEN_KEEPALIVE const char *
pz_web_get_room_code(void)
{
    return g_app.net_room_code;
}

// Check if currently hosting
EMSCRIPTEN_KEEPALIVE int
pz_web_is_hosting(void)
{
    return g_app.net_is_host ? 1 : 0;
}

// Check if peer is connected
EMSCRIPTEN_KEEPALIVE int
pz_web_is_peer_connected(void)
{
    return (g_app.net_is_host || g_app.net_is_client) && g_app.net_webrtc
            && atomic_load(&g_app.net_channel_open)
        ? 1
        : 0;
}

EMSCRIPTEN_KEEPALIVE int
pz_web_net_failed(void)
{
    return g_app.net_signaling_failed ? 1 : 0;
}

#endif // __EMSCRIPTEN__

sapp_desc
sokol_main(int argc, char *argv[])
{
    parse_args(argc, argv);

    return (sapp_desc) {
        .init_cb = app_init,
        .frame_cb = app_frame,
        .cleanup_cb = app_cleanup,
        .event_cb = app_event,
        .width = WINDOW_WIDTH,
        .height = WINDOW_HEIGHT,
        .sample_count = 4,
        .high_dpi = true,
        .window_title = WINDOW_TITLE,
    };
}
