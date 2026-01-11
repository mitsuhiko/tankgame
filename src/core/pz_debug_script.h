/*
 * Tank Game - Debug Script Execution System
 *
 * A simple scripting system for automated testing and validation.
 * This is NOT a gameplay scripting language - it's specifically for:
 *   - Automated visual regression testing
 *   - Reproducing bugs with specific input sequences
 *   - Validating rendering and gameplay changes
 *
 * For gameplay scripting (if added later), use a proper language like Lua.
 *
 * See docs/debug-script.md for full documentation.
 */

#ifndef PZ_DEBUG_SCRIPT_H
#define PZ_DEBUG_SCRIPT_H

#include <stdbool.h>
#include <stdint.h>

// Forward declarations
typedef struct pz_renderer pz_renderer;
typedef struct pz_tank_manager pz_tank_manager;
typedef struct pz_projectile_manager pz_projectile_manager;
typedef struct pz_tank pz_tank;
typedef struct pz_ai_manager pz_ai_manager;
typedef struct pz_toxic_cloud pz_toxic_cloud;

// Script input state (directly usable by game code)
typedef struct pz_debug_script_input {
    float move_x; // -1 (left) to +1 (right)
    float move_y; // -1 (forward) to +1 (back)
    float aim_x; // World X coordinate to aim at
    float aim_y; // World Y coordinate to aim at
    bool has_aim; // Whether aim coordinates are set
    bool fire; // Fire this frame (single press, auto-clears)
    bool hold_fire; // Hold fire continuously
    int weapon_cycle; // +1 for next, -1 for prev, 0 for none (auto-clears)
    bool mouse_click_left; // Left mouse click this frame (auto-clears)
    bool mouse_click_right; // Right mouse click this frame (auto-clears)
} pz_debug_script_input;

// Script execution context
typedef struct pz_debug_script pz_debug_script;

// Create script context from file
// Returns NULL if file doesn't exist or is invalid
pz_debug_script *pz_debug_script_load(const char *path);

// Create script context from inline string
// Commands can be separated by newlines or semicolons
// Returns NULL if string is empty or invalid
pz_debug_script *pz_debug_script_create_from_string(const char *script_text);

// Inject commands from string into existing script (replaces current state)
// If script is NULL, returns a new script
// Commands can be separated by newlines or semicolons
pz_debug_script *pz_debug_script_inject(
    pz_debug_script *script, const char *commands);

// Destroy script context
void pz_debug_script_destroy(pz_debug_script *script);

// Check if script execution is complete
bool pz_debug_script_is_done(const pz_debug_script *script);

// Check if we should render this frame
bool pz_debug_script_should_render(const pz_debug_script *script);

// Check if turbo mode is enabled (skip frame timing)
bool pz_debug_script_is_turbo(const pz_debug_script *script);

// Get current input state
const pz_debug_script_input *pz_debug_script_get_input(
    const pz_debug_script *script);

// Action types returned by update
typedef enum {
    PZ_DEBUG_SCRIPT_CONTINUE,
    PZ_DEBUG_SCRIPT_QUIT,
    PZ_DEBUG_SCRIPT_LOAD_MAP,
    PZ_DEBUG_SCRIPT_SCREENSHOT,
    PZ_DEBUG_SCRIPT_DUMP,
    PZ_DEBUG_SCRIPT_SET_SEED,
    PZ_DEBUG_SCRIPT_GOD_MODE,
    PZ_DEBUG_SCRIPT_TELEPORT,
    PZ_DEBUG_SCRIPT_GIVE,
    PZ_DEBUG_SCRIPT_CURSOR,
    PZ_DEBUG_SCRIPT_MOUSE_SCREEN,
    PZ_DEBUG_SCRIPT_SPAWN_BARRIER,
    PZ_DEBUG_SCRIPT_SPAWN_POWERUP,
    PZ_DEBUG_SCRIPT_NET_HOST, // Create WebRTC offer, write to file
    PZ_DEBUG_SCRIPT_NET_JOIN, // Read offer, create answer, write to file
    PZ_DEBUG_SCRIPT_NET_ANSWER, // Read answer and apply
    PZ_DEBUG_SCRIPT_NET_WAIT, // Wait for connection
    PZ_DEBUG_SCRIPT_WAITING, // Script is waiting (for file or connection)
} pz_debug_script_action;

// Advance script state by one frame
// May return multiple actions (call repeatedly until CONTINUE)
pz_debug_script_action pz_debug_script_update(pz_debug_script *script);

// Get path for LOAD_MAP action
const char *pz_debug_script_get_map_path(const pz_debug_script *script);

// Get path for SCREENSHOT action
const char *pz_debug_script_get_screenshot_path(const pz_debug_script *script);

// Get path for DUMP action
const char *pz_debug_script_get_dump_path(const pz_debug_script *script);

// Get seed for SET_SEED action
uint32_t pz_debug_script_get_seed(const pz_debug_script *script);

// Get god mode value for GOD_MODE action (true = enable, false = disable)
bool pz_debug_script_get_god_mode(const pz_debug_script *script);

// Get teleport position for TELEPORT action
void pz_debug_script_get_teleport_pos(
    const pz_debug_script *script, float *x, float *y);

// Get give item type for GIVE action (returns item name string)
const char *pz_debug_script_get_give_item(const pz_debug_script *script);

// Get cursor position for CURSOR action
void pz_debug_script_get_cursor_pos(
    const pz_debug_script *script, float *x, float *y);

// Get spawn barrier position for SPAWN_BARRIER action
void pz_debug_script_get_spawn_barrier(
    const pz_debug_script *script, float *x, float *y);

// Get spawn powerup data for SPAWN_POWERUP action
void pz_debug_script_get_spawn_powerup(
    const pz_debug_script *script, float *x, float *y, const char **type);

// Check if physical input should be blocked (script is active)
bool pz_debug_script_blocks_input(const pz_debug_script *script);

// Dump game state to file
// This is a helper that game code can call with its state
void pz_debug_script_dump_state(const char *path, pz_tank_manager *tank_mgr,
    pz_projectile_manager *proj_mgr, pz_ai_manager *ai_mgr,
    const pz_toxic_cloud *toxic_cloud, pz_tank *player, int frame_count);

// Get offer file path for NET_HOST action
const char *pz_debug_script_get_net_offer_path(const pz_debug_script *script);

// Get offer and answer file paths for NET_JOIN action
void pz_debug_script_get_net_join_paths(const pz_debug_script *script,
    const char **offer_path, const char **answer_path);

// Get answer file path for NET_ANSWER action
const char *pz_debug_script_get_net_answer_path(const pz_debug_script *script);

// Get timeout for NET_WAIT action (in seconds, 0 = no timeout)
float pz_debug_script_get_net_wait_timeout(const pz_debug_script *script);

// Notify script that connection is established (call when data channel opens)
void pz_debug_script_notify_connected(pz_debug_script *script);

// Notify script that a file was written (call after
// NET_HOST/NET_JOIN/NET_ANSWER)
void pz_debug_script_notify_action_complete(pz_debug_script *script);

#endif // PZ_DEBUG_SCRIPT_H
