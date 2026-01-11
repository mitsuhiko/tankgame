/*
 * Tank Game - Debug Script Execution System Implementation
 */

#include "pz_debug_script.h"
#include "../game/pz_ai.h"
#include "../game/pz_projectile.h"
#include "../game/pz_tank.h"
#include "../game/pz_toxic_cloud.h"
#include "pz_log.h"
#include "pz_mem.h"
#include "pz_platform.h"
#include "pz_str.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Command types
typedef enum {
    CMD_NONE,
    CMD_TURBO,
    CMD_RENDER,
    CMD_FRAMES,
    CMD_MAP,
    CMD_SEED,
    CMD_INPUT,
    CMD_AIM,
    CMD_FIRE,
    CMD_HOLD_FIRE,
    CMD_SCREENSHOT,
    CMD_DUMP,
    CMD_QUIT,
    CMD_GOD,
    CMD_WEAPON,
    CMD_TELEPORT,
    CMD_GIVE,
    CMD_CURSOR,
    CMD_MOUSE_SCREEN,
    CMD_SPAWN_BARRIER,
    CMD_SPAWN_POWERUP,
    CMD_MOUSE_CLICK,
    CMD_WAIT_FILE,
    CMD_NET_HOST,
    CMD_NET_JOIN,
    CMD_NET_ANSWER,
    CMD_NET_WAIT,
    CMD_LOG,
    CMD_DELETE_FILE,
} script_cmd_type;

// Parsed command
typedef struct {
    script_cmd_type type;
    union {
        bool bool_val; // turbo, render, hold_fire
        int int_val; // frames
        uint32_t seed_val; // seed
        char str_val[256]; // map, screenshot, dump paths, give item
        struct {
            float x, y;
        } aim_val; // aim
        struct {
            float x, y;
            int mode; // 0=replace (stop), 1=add, -1=subtract
        } input_val; // input (move direction)
        struct {
            float x, y;
        } pos_val; // teleport, cursor, spawn_barrier
        struct {
            float x, y;
            char type[64];
        } spawn_powerup_val; // spawn_powerup
        int mouse_button; // 0=left, 1=right, 2=middle
        struct {
            char path[256];
            float timeout_sec; // 0 = no timeout
        } wait_file_val; // wait_file, net_wait
        struct {
            char offer_path[256];
        } net_host_val; // net_host
        struct {
            char offer_path[256];
            char answer_path[256];
        } net_join_val; // net_join
        struct {
            char answer_path[256];
        } net_answer_val; // net_answer
        struct {
            float timeout_sec;
        } net_wait_val; // net_wait
        char log_msg[256]; // log
    };
} script_cmd;

// Wait state for blocking operations
typedef enum {
    WAIT_NONE,
    WAIT_FILE,
    WAIT_NET_CONNECTED,
    WAIT_ACTION_COMPLETE, // waiting for main.c to complete an action
} wait_state;

// Script context
struct pz_debug_script {
    script_cmd *commands;
    int command_count;
    int current_cmd;

    // Execution state
    int frames_remaining; // For 'frames N' command
    bool done;

    // Mode flags
    bool turbo;
    bool render;

    // Input state
    pz_debug_script_input input;

    // Action data (for returning to caller)
    char action_path[256];
    char action_path2[256]; // second path for net_join
    uint32_t action_seed;
    bool action_god_mode;
    float action_x, action_y;
    char action_item[64];
    char action_powerup_type[64];
    float action_timeout;

    // Wait state
    wait_state waiting;
    char wait_path[256];
    float wait_timeout_sec;
    uint64_t wait_start_ms;
    bool net_connected; // set by notify_connected
};

// Parse a single line into a command
static bool
parse_command(const char *line, script_cmd *cmd)
{
    // Skip leading whitespace
    while (*line && isspace(*line))
        line++;

    // Skip empty lines and comments
    if (*line == '\0' || *line == '#') {
        cmd->type = CMD_NONE;
        return true;
    }

    char keyword[64] = { 0 };
    char arg1[256] = { 0 };
    char arg2[64] = { 0 };

    int n = sscanf(line, "%63s %255s %63s", keyword, arg1, arg2);
    if (n < 1) {
        cmd->type = CMD_NONE;
        return true;
    }

    // Convert keyword to lowercase for comparison
    for (char *p = keyword; *p; p++)
        *p = (char)tolower(*p);

    if (strcmp(keyword, "turbo") == 0) {
        cmd->type = CMD_TURBO;
        cmd->bool_val = (strcmp(arg1, "on") == 0 || strcmp(arg1, "1") == 0);
    } else if (strcmp(keyword, "render") == 0) {
        cmd->type = CMD_RENDER;
        cmd->bool_val = (strcmp(arg1, "on") == 0 || strcmp(arg1, "1") == 0);
    } else if (strcmp(keyword, "frames") == 0) {
        cmd->type = CMD_FRAMES;
        cmd->int_val = atoi(arg1);
        if (cmd->int_val <= 0) {
            pz_log(PZ_LOG_WARN, PZ_LOG_CAT_CORE,
                "Debug script: invalid frame count '%s', using 1", arg1);
            cmd->int_val = 1;
        }
    } else if (strcmp(keyword, "map") == 0) {
        cmd->type = CMD_MAP;
        strncpy(cmd->str_val, arg1, sizeof(cmd->str_val) - 1);
    } else if (strcmp(keyword, "seed") == 0) {
        cmd->type = CMD_SEED;
        cmd->seed_val = (uint32_t)strtoul(arg1, NULL, 10);
    } else if (strcmp(keyword, "input") == 0) {
        cmd->type = CMD_INPUT;

        // Check for +/- prefix (additive mode)
        const char *dir = arg1;
        int mode = 1; // default: add

        if (dir[0] == '+') {
            mode = 1;
            dir++;
        } else if (dir[0] == '-') {
            mode = -1;
            dir++;
        }

        // Convert direction to lowercase
        char dir_lower[64] = { 0 };
        for (int i = 0; dir[i] && i < 63; i++)
            dir_lower[i] = (char)tolower(dir[i]);

        cmd->input_val.x = 0.0f;
        cmd->input_val.y = 0.0f;
        cmd->input_val.mode = mode;

        if (strcmp(dir_lower, "up") == 0) {
            cmd->input_val.y = -1.0f; // W key, toward top of screen
        } else if (strcmp(dir_lower, "down") == 0) {
            cmd->input_val.y = 1.0f; // S key, toward bottom of screen
        } else if (strcmp(dir_lower, "left") == 0) {
            cmd->input_val.x = -1.0f; // A key
        } else if (strcmp(dir_lower, "right") == 0) {
            cmd->input_val.x = 1.0f; // D key
        } else if (strcmp(dir_lower, "stop") == 0) {
            cmd->input_val.mode = 0; // replace with zero
        } else {
            pz_log(PZ_LOG_WARN, PZ_LOG_CAT_CORE,
                "Debug script: unknown input direction '%s'", arg1);
        }
    } else if (strcmp(keyword, "aim") == 0) {
        cmd->type = CMD_AIM;
        cmd->aim_val.x = (float)atof(arg1);
        cmd->aim_val.y = (float)atof(arg2);
    } else if (strcmp(keyword, "fire") == 0) {
        cmd->type = CMD_FIRE;
    } else if (strcmp(keyword, "hold_fire") == 0) {
        cmd->type = CMD_HOLD_FIRE;
        cmd->bool_val = (strcmp(arg1, "on") == 0 || strcmp(arg1, "1") == 0);
    } else if (strcmp(keyword, "screenshot") == 0) {
        cmd->type = CMD_SCREENSHOT;
        strncpy(cmd->str_val, arg1, sizeof(cmd->str_val) - 1);
    } else if (strcmp(keyword, "dump") == 0) {
        cmd->type = CMD_DUMP;
        strncpy(cmd->str_val, arg1, sizeof(cmd->str_val) - 1);
    } else if (strcmp(keyword, "quit") == 0) {
        cmd->type = CMD_QUIT;
    } else if (strcmp(keyword, "god") == 0) {
        cmd->type = CMD_GOD;
        cmd->bool_val = (strcmp(arg1, "on") == 0 || strcmp(arg1, "1") == 0);
    } else if (strcmp(keyword, "weapon") == 0) {
        cmd->type = CMD_WEAPON;
        // weapon next (default) or weapon prev
        cmd->int_val = (strcmp(arg1, "prev") == 0) ? -1 : 1;
    } else if (strcmp(keyword, "teleport") == 0) {
        cmd->type = CMD_TELEPORT;
        cmd->pos_val.x = (float)atof(arg1);
        cmd->pos_val.y = (float)atof(arg2);
    } else if (strcmp(keyword, "give") == 0) {
        cmd->type = CMD_GIVE;
        strncpy(cmd->str_val, arg1, sizeof(cmd->str_val) - 1);
    } else if (strcmp(keyword, "cursor") == 0) {
        cmd->type = CMD_CURSOR;
        cmd->pos_val.x = (float)atof(arg1);
        cmd->pos_val.y = (float)atof(arg2);
    } else if (strcmp(keyword, "mouse_screen") == 0) {
        cmd->type = CMD_MOUSE_SCREEN;
        cmd->pos_val.x = (float)atof(arg1);
        cmd->pos_val.y = (float)atof(arg2);
    } else if (strcmp(keyword, "spawn_barrier") == 0) {
        cmd->type = CMD_SPAWN_BARRIER;
        cmd->pos_val.x = (float)atof(arg1);
        cmd->pos_val.y = (float)atof(arg2);
    } else if (strcmp(keyword, "spawn_powerup") == 0) {
        cmd->type = CMD_SPAWN_POWERUP;
        cmd->spawn_powerup_val.x = (float)atof(arg1);
        cmd->spawn_powerup_val.y = (float)atof(arg2);
        // Third argument is the type - need to re-parse
        char arg3[64] = { 0 };
        sscanf(line, "%*s %*s %*s %63s", arg3);
        strncpy(cmd->spawn_powerup_val.type, arg3,
            sizeof(cmd->spawn_powerup_val.type) - 1);
    } else if (strcmp(keyword, "mouse_click") == 0) {
        cmd->type = CMD_MOUSE_CLICK;
        if (strcmp(arg1, "right") == 0) {
            cmd->mouse_button = 1;
        } else if (strcmp(arg1, "middle") == 0) {
            cmd->mouse_button = 2;
        } else {
            cmd->mouse_button = 0; // default: left
        }
    } else if (strcmp(keyword, "wait_file") == 0) {
        cmd->type = CMD_WAIT_FILE;
        strncpy(
            cmd->wait_file_val.path, arg1, sizeof(cmd->wait_file_val.path) - 1);
        cmd->wait_file_val.timeout_sec
            = arg2[0] ? (float)atof(arg2) : 30.0f; // default 30s
    } else if (strcmp(keyword, "net_host") == 0) {
        cmd->type = CMD_NET_HOST;
        strncpy(cmd->net_host_val.offer_path, arg1,
            sizeof(cmd->net_host_val.offer_path) - 1);
    } else if (strcmp(keyword, "net_join") == 0) {
        cmd->type = CMD_NET_JOIN;
        strncpy(cmd->net_join_val.offer_path, arg1,
            sizeof(cmd->net_join_val.offer_path) - 1);
        strncpy(cmd->net_join_val.answer_path, arg2,
            sizeof(cmd->net_join_val.answer_path) - 1);
    } else if (strcmp(keyword, "net_answer") == 0) {
        cmd->type = CMD_NET_ANSWER;
        strncpy(cmd->net_answer_val.answer_path, arg1,
            sizeof(cmd->net_answer_val.answer_path) - 1);
    } else if (strcmp(keyword, "net_wait") == 0) {
        cmd->type = CMD_NET_WAIT;
        cmd->net_wait_val.timeout_sec
            = arg1[0] ? (float)atof(arg1) : 30.0f; // default 30s
    } else if (strcmp(keyword, "log") == 0) {
        cmd->type = CMD_LOG;
        // Get everything after "log " as the message
        const char *msg = line;
        while (*msg && !isspace(*msg))
            msg++; // skip "log"
        while (*msg && isspace(*msg))
            msg++; // skip whitespace
        strncpy(cmd->log_msg, msg, sizeof(cmd->log_msg) - 1);
    } else if (strcmp(keyword, "delete_file") == 0) {
        cmd->type = CMD_DELETE_FILE;
        strncpy(cmd->str_val, arg1, sizeof(cmd->str_val) - 1);
    } else {
        pz_log(PZ_LOG_WARN, PZ_LOG_CAT_CORE,
            "Debug script: unknown command '%s'", keyword);
        cmd->type = CMD_NONE;
    }

    return true;
}

// Internal: count commands in text (newlines and semicolons are separators)
static int
count_commands(const char *text)
{
    int count = 1;
    for (const char *p = text; *p; p++) {
        if (*p == '\n' || *p == ';')
            count++;
    }
    return count;
}

// Internal: parse script text into commands array
// Returns number of commands parsed
static int
parse_script_text(const char *text, script_cmd *commands, int max_commands)
{
    // Make a mutable copy
    char *content = pz_str_dup(text);
    if (!content)
        return 0;

    int command_count = 0;
    char *pos = content;

    while (pos && *pos && command_count < max_commands) {
        // Find next separator (newline or semicolon)
        char *next_newline = strchr(pos, '\n');
        char *next_semi = strchr(pos, ';');
        char *next = NULL;

        if (next_newline && next_semi) {
            next = (next_newline < next_semi) ? next_newline : next_semi;
        } else {
            next = next_newline ? next_newline : next_semi;
        }

        if (next) {
            *next = '\0';
            next++;
        }

        script_cmd cmd = { 0 };
        if (parse_command(pos, &cmd) && cmd.type != CMD_NONE) {
            commands[command_count++] = cmd;
        }

        pos = next;
    }

    pz_free(content);
    return command_count;
}

// Internal: create a new script with allocated command array
static pz_debug_script *
create_script(int max_commands)
{
    pz_debug_script *script = pz_alloc(sizeof(pz_debug_script));
    memset(script, 0, sizeof(*script));

    script->commands = pz_alloc(sizeof(script_cmd) * max_commands);
    script->command_count = 0;
    script->current_cmd = 0;
    script->frames_remaining = 0;
    script->done = false;

    // Default modes for script execution
    script->turbo = true; // Fast by default
    script->render = true; // Render by default

    return script;
}

pz_debug_script *
pz_debug_script_load(const char *path)
{
    char *content = pz_file_read_text(path);
    if (!content) {
        pz_log(PZ_LOG_ERROR, PZ_LOG_CAT_CORE,
            "Debug script: failed to load '%s'", path);
        return NULL;
    }

    int max_commands = count_commands(content);
    pz_debug_script *script = create_script(max_commands);

    script->command_count
        = parse_script_text(content, script->commands, max_commands);

    pz_free(content);

    pz_log(PZ_LOG_INFO, PZ_LOG_CAT_CORE,
        "Debug script: loaded '%s' with %d commands", path,
        script->command_count);

    return script;
}

pz_debug_script *
pz_debug_script_create_from_string(const char *script_text)
{
    if (!script_text || !*script_text) {
        return NULL;
    }

    int max_commands = count_commands(script_text);
    pz_debug_script *script = create_script(max_commands);

    script->command_count
        = parse_script_text(script_text, script->commands, max_commands);

    pz_log(PZ_LOG_INFO, PZ_LOG_CAT_CORE,
        "Debug script: created from string with %d commands",
        script->command_count);

    return script;
}

pz_debug_script *
pz_debug_script_inject(pz_debug_script *script, const char *commands)
{
    if (!commands || !*commands) {
        return script;
    }

    // If no existing script, create a new one
    if (!script) {
        return pz_debug_script_create_from_string(commands);
    }

    // Replace the script's commands
    int max_commands = count_commands(commands);

    // Reallocate command array if needed
    pz_free(script->commands);
    script->commands = pz_alloc(sizeof(script_cmd) * max_commands);

    script->command_count
        = parse_script_text(commands, script->commands, max_commands);
    script->current_cmd = 0;
    script->frames_remaining = 0;
    script->done = false;

    // Keep existing turbo/render settings, but reset input
    script->input.move_x = 0.0f;
    script->input.move_y = 0.0f;
    script->input.aim_x = 0.0f;
    script->input.aim_y = 0.0f;
    script->input.has_aim = false;
    script->input.fire = false;
    script->input.hold_fire = false;

    pz_log(PZ_LOG_INFO, PZ_LOG_CAT_CORE, "Debug script: injected %d commands",
        script->command_count);

    return script;
}

void
pz_debug_script_destroy(pz_debug_script *script)
{
    if (!script)
        return;

    pz_free(script->commands);
    pz_free(script);
}

bool
pz_debug_script_is_done(const pz_debug_script *script)
{
    return !script || script->done;
}

bool
pz_debug_script_should_render(const pz_debug_script *script)
{
    return !script || script->render;
}

bool
pz_debug_script_is_turbo(const pz_debug_script *script)
{
    return script && script->turbo;
}

const pz_debug_script_input *
pz_debug_script_get_input(const pz_debug_script *script)
{
    return script ? &script->input : NULL;
}

pz_debug_script_action
pz_debug_script_update(pz_debug_script *script)
{
    if (!script || script->done) {
        return PZ_DEBUG_SCRIPT_CONTINUE;
    }

    // Clear single-frame inputs
    script->input.fire = false;
    script->input.weapon_cycle = 0;
    script->input.mouse_click_left = false;
    script->input.mouse_click_right = false;

    // Handle waiting states
    if (script->waiting != WAIT_NONE) {
        uint64_t elapsed_ms = pz_time_now_ms() - script->wait_start_ms;
        float elapsed_sec = (float)elapsed_ms / 1000.0f;

        switch (script->waiting) {
        case WAIT_FILE: {
            // Check if file exists
            if (pz_file_exists(script->wait_path)) {
                pz_log(PZ_LOG_INFO, PZ_LOG_CAT_CORE,
                    "Debug script: file '%s' found after %.1fs",
                    script->wait_path, elapsed_sec);
                script->waiting = WAIT_NONE;
                // Continue to process next commands
                break;
            }
            // Check timeout
            if (script->wait_timeout_sec > 0
                && elapsed_sec >= script->wait_timeout_sec) {
                pz_log(PZ_LOG_ERROR, PZ_LOG_CAT_CORE,
                    "Debug script: wait_file '%s' timed out after %.0fs",
                    script->wait_path, script->wait_timeout_sec);
                script->done = true;
                return PZ_DEBUG_SCRIPT_QUIT;
            }
            // Still waiting
            return PZ_DEBUG_SCRIPT_WAITING;
        }

        case WAIT_NET_CONNECTED:
            // Check if connected
            if (script->net_connected) {
                pz_log(PZ_LOG_INFO, PZ_LOG_CAT_CORE,
                    "Debug script: connected after %.1fs", elapsed_sec);
                script->waiting = WAIT_NONE;
                // Continue to process next commands
                break;
            }
            // Check timeout
            if (script->wait_timeout_sec > 0
                && elapsed_sec >= script->wait_timeout_sec) {
                pz_log(PZ_LOG_ERROR, PZ_LOG_CAT_CORE,
                    "Debug script: net_wait timed out after %.0fs",
                    script->wait_timeout_sec);
                script->done = true;
                return PZ_DEBUG_SCRIPT_QUIT;
            }
            // Still waiting
            return PZ_DEBUG_SCRIPT_WAITING;

        case WAIT_ACTION_COMPLETE:
            // Still waiting for main.c to complete the action
            return PZ_DEBUG_SCRIPT_WAITING;

        case WAIT_NONE:
            break;
        }
    }

    // If we're counting down frames, just continue
    if (script->frames_remaining > 0) {
        script->frames_remaining--;
        return PZ_DEBUG_SCRIPT_CONTINUE;
    }

    // Process commands until we hit one that needs a frame
    while (script->current_cmd < script->command_count) {
        script_cmd *cmd = &script->commands[script->current_cmd];
        script->current_cmd++;

        switch (cmd->type) {
        case CMD_NONE:
            // Skip
            break;

        case CMD_TURBO:
            script->turbo = cmd->bool_val;
            pz_log(PZ_LOG_DEBUG, PZ_LOG_CAT_CORE, "Debug script: turbo %s",
                cmd->bool_val ? "on" : "off");
            break;

        case CMD_RENDER:
            script->render = cmd->bool_val;
            pz_log(PZ_LOG_DEBUG, PZ_LOG_CAT_CORE, "Debug script: render %s",
                cmd->bool_val ? "on" : "off");
            break;

        case CMD_FRAMES:
            script->frames_remaining
                = cmd->int_val - 1; // -1 because this frame counts
            pz_log(PZ_LOG_DEBUG, PZ_LOG_CAT_CORE,
                "Debug script: advancing %d frames", cmd->int_val);
            return PZ_DEBUG_SCRIPT_CONTINUE;

        case CMD_MAP:
            strncpy(script->action_path, cmd->str_val,
                sizeof(script->action_path) - 1);
            pz_log(PZ_LOG_INFO, PZ_LOG_CAT_CORE,
                "Debug script: loading map '%s'", cmd->str_val);
            return PZ_DEBUG_SCRIPT_LOAD_MAP;

        case CMD_SEED:
            script->action_seed = cmd->seed_val;
            pz_log(PZ_LOG_INFO, PZ_LOG_CAT_CORE,
                "Debug script: setting seed %u", cmd->seed_val);
            return PZ_DEBUG_SCRIPT_SET_SEED;

        case CMD_INPUT:
            if (cmd->input_val.mode == 0) {
                // Replace mode (stop)
                script->input.move_x = 0.0f;
                script->input.move_y = 0.0f;
            } else if (cmd->input_val.mode == 1) {
                // Add mode
                script->input.move_x += cmd->input_val.x;
                script->input.move_y += cmd->input_val.y;
            } else {
                // Subtract mode
                script->input.move_x -= cmd->input_val.x;
                script->input.move_y -= cmd->input_val.y;
            }
            // Clamp to valid range
            if (script->input.move_x > 1.0f)
                script->input.move_x = 1.0f;
            if (script->input.move_x < -1.0f)
                script->input.move_x = -1.0f;
            if (script->input.move_y > 1.0f)
                script->input.move_y = 1.0f;
            if (script->input.move_y < -1.0f)
                script->input.move_y = -1.0f;
            pz_log(PZ_LOG_DEBUG, PZ_LOG_CAT_CORE,
                "Debug script: input now (%.1f, %.1f)", script->input.move_x,
                script->input.move_y);
            break;

        case CMD_AIM:
            script->input.aim_x = cmd->aim_val.x;
            script->input.aim_y = cmd->aim_val.y;
            script->input.has_aim = true;
            pz_log(PZ_LOG_DEBUG, PZ_LOG_CAT_CORE,
                "Debug script: aim at (%.2f, %.2f)", cmd->aim_val.x,
                cmd->aim_val.y);
            break;

        case CMD_FIRE:
            script->input.fire = true;
            pz_log(PZ_LOG_DEBUG, PZ_LOG_CAT_CORE, "Debug script: fire");
            break;

        case CMD_HOLD_FIRE:
            script->input.hold_fire = cmd->bool_val;
            pz_log(PZ_LOG_DEBUG, PZ_LOG_CAT_CORE, "Debug script: hold_fire %s",
                cmd->bool_val ? "on" : "off");
            break;

        case CMD_SCREENSHOT:
            strncpy(script->action_path, cmd->str_val,
                sizeof(script->action_path) - 1);
            pz_log(PZ_LOG_INFO, PZ_LOG_CAT_CORE,
                "Debug script: screenshot '%s'", cmd->str_val);
            return PZ_DEBUG_SCRIPT_SCREENSHOT;

        case CMD_DUMP:
            strncpy(script->action_path, cmd->str_val,
                sizeof(script->action_path) - 1);
            pz_log(PZ_LOG_INFO, PZ_LOG_CAT_CORE,
                "Debug script: dump state to '%s'", cmd->str_val);
            return PZ_DEBUG_SCRIPT_DUMP;

        case CMD_QUIT:
            pz_log(PZ_LOG_INFO, PZ_LOG_CAT_CORE, "Debug script: quit");
            script->done = true;
            return PZ_DEBUG_SCRIPT_QUIT;

        case CMD_GOD:
            script->action_god_mode = cmd->bool_val;
            pz_log(PZ_LOG_INFO, PZ_LOG_CAT_CORE, "Debug script: god mode %s",
                cmd->bool_val ? "on" : "off");
            return PZ_DEBUG_SCRIPT_GOD_MODE;

        case CMD_WEAPON:
            script->input.weapon_cycle = cmd->int_val;
            pz_log(PZ_LOG_DEBUG, PZ_LOG_CAT_CORE, "Debug script: weapon %s",
                cmd->int_val > 0 ? "next" : "prev");
            break;

        case CMD_TELEPORT:
            script->action_x = cmd->pos_val.x;
            script->action_y = cmd->pos_val.y;
            pz_log(PZ_LOG_INFO, PZ_LOG_CAT_CORE,
                "Debug script: teleport to (%.2f, %.2f)", cmd->pos_val.x,
                cmd->pos_val.y);
            return PZ_DEBUG_SCRIPT_TELEPORT;

        case CMD_GIVE:
            strncpy(script->action_item, cmd->str_val,
                sizeof(script->action_item) - 1);
            pz_log(PZ_LOG_INFO, PZ_LOG_CAT_CORE, "Debug script: give '%s'",
                cmd->str_val);
            return PZ_DEBUG_SCRIPT_GIVE;

        case CMD_CURSOR:
            script->action_x = cmd->pos_val.x;
            script->action_y = cmd->pos_val.y;
            pz_log(PZ_LOG_INFO, PZ_LOG_CAT_CORE,
                "Debug script: cursor at (%.2f, %.2f)", cmd->pos_val.x,
                cmd->pos_val.y);
            return PZ_DEBUG_SCRIPT_CURSOR;

        case CMD_MOUSE_SCREEN:
            script->action_x = cmd->pos_val.x;
            script->action_y = cmd->pos_val.y;
            pz_log(PZ_LOG_INFO, PZ_LOG_CAT_CORE,
                "Debug script: mouse_screen at (%.0f, %.0f)", cmd->pos_val.x,
                cmd->pos_val.y);
            return PZ_DEBUG_SCRIPT_MOUSE_SCREEN;

        case CMD_SPAWN_BARRIER:
            script->action_x = cmd->pos_val.x;
            script->action_y = cmd->pos_val.y;
            pz_log(PZ_LOG_INFO, PZ_LOG_CAT_CORE,
                "Debug script: spawn_barrier at (%.2f, %.2f)", cmd->pos_val.x,
                cmd->pos_val.y);
            return PZ_DEBUG_SCRIPT_SPAWN_BARRIER;

        case CMD_SPAWN_POWERUP:
            script->action_x = cmd->spawn_powerup_val.x;
            script->action_y = cmd->spawn_powerup_val.y;
            strncpy(script->action_powerup_type, cmd->spawn_powerup_val.type,
                sizeof(script->action_powerup_type) - 1);
            pz_log(PZ_LOG_INFO, PZ_LOG_CAT_CORE,
                "Debug script: spawn_powerup '%s' at (%.2f, %.2f)",
                cmd->spawn_powerup_val.type, cmd->spawn_powerup_val.x,
                cmd->spawn_powerup_val.y);
            return PZ_DEBUG_SCRIPT_SPAWN_POWERUP;

        case CMD_MOUSE_CLICK:
            if (cmd->mouse_button == 1) {
                script->input.mouse_click_right = true;
                pz_log(PZ_LOG_DEBUG, PZ_LOG_CAT_CORE,
                    "Debug script: mouse_click right");
            } else {
                script->input.mouse_click_left = true;
                pz_log(PZ_LOG_DEBUG, PZ_LOG_CAT_CORE,
                    "Debug script: mouse_click left");
            }
            break;

        case CMD_WAIT_FILE:
            strncpy(script->wait_path, cmd->wait_file_val.path,
                sizeof(script->wait_path) - 1);
            script->wait_timeout_sec = cmd->wait_file_val.timeout_sec;
            script->wait_start_ms = pz_time_now_ms();
            script->waiting = WAIT_FILE;
            pz_log(PZ_LOG_INFO, PZ_LOG_CAT_CORE,
                "Debug script: waiting for file '%s' (timeout %.0fs)",
                cmd->wait_file_val.path, cmd->wait_file_val.timeout_sec);
            return PZ_DEBUG_SCRIPT_WAITING;

        case CMD_NET_HOST:
            strncpy(script->action_path, cmd->net_host_val.offer_path,
                sizeof(script->action_path) - 1);
            script->waiting = WAIT_ACTION_COMPLETE;
            pz_log(PZ_LOG_INFO, PZ_LOG_CAT_CORE,
                "Debug script: net_host -> '%s'", cmd->net_host_val.offer_path);
            return PZ_DEBUG_SCRIPT_NET_HOST;

        case CMD_NET_JOIN:
            strncpy(script->action_path, cmd->net_join_val.offer_path,
                sizeof(script->action_path) - 1);
            strncpy(script->action_path2, cmd->net_join_val.answer_path,
                sizeof(script->action_path2) - 1);
            script->waiting = WAIT_ACTION_COMPLETE;
            pz_log(PZ_LOG_INFO, PZ_LOG_CAT_CORE,
                "Debug script: net_join '%s' -> '%s'",
                cmd->net_join_val.offer_path, cmd->net_join_val.answer_path);
            return PZ_DEBUG_SCRIPT_NET_JOIN;

        case CMD_NET_ANSWER:
            strncpy(script->action_path, cmd->net_answer_val.answer_path,
                sizeof(script->action_path) - 1);
            script->waiting = WAIT_ACTION_COMPLETE;
            pz_log(PZ_LOG_INFO, PZ_LOG_CAT_CORE,
                "Debug script: net_answer <- '%s'",
                cmd->net_answer_val.answer_path);
            return PZ_DEBUG_SCRIPT_NET_ANSWER;

        case CMD_NET_WAIT:
            script->action_timeout = cmd->net_wait_val.timeout_sec;
            script->wait_timeout_sec = cmd->net_wait_val.timeout_sec;
            script->wait_start_ms = pz_time_now_ms();
            script->waiting = WAIT_NET_CONNECTED;
            pz_log(PZ_LOG_INFO, PZ_LOG_CAT_CORE,
                "Debug script: net_wait (timeout %.0fs)",
                cmd->net_wait_val.timeout_sec);
            return PZ_DEBUG_SCRIPT_NET_WAIT;

        case CMD_LOG:
            pz_log(
                PZ_LOG_INFO, PZ_LOG_CAT_CORE, "Debug script: %s", cmd->log_msg);
            break;

        case CMD_DELETE_FILE:
            if (remove(cmd->str_val) == 0) {
                pz_log(PZ_LOG_DEBUG, PZ_LOG_CAT_CORE,
                    "Debug script: deleted '%s'", cmd->str_val);
            } else {
                // Not an error if file doesn't exist
                pz_log(PZ_LOG_DEBUG, PZ_LOG_CAT_CORE,
                    "Debug script: delete '%s' (already gone or failed)",
                    cmd->str_val);
            }
            break;
        }
    }

    // Reached end of script
    pz_log(PZ_LOG_INFO, PZ_LOG_CAT_CORE, "Debug script: completed");
    script->done = true;
    return PZ_DEBUG_SCRIPT_QUIT;
}

const char *
pz_debug_script_get_map_path(const pz_debug_script *script)
{
    return script ? script->action_path : NULL;
}

const char *
pz_debug_script_get_screenshot_path(const pz_debug_script *script)
{
    return script ? script->action_path : NULL;
}

const char *
pz_debug_script_get_dump_path(const pz_debug_script *script)
{
    return script ? script->action_path : NULL;
}

uint32_t
pz_debug_script_get_seed(const pz_debug_script *script)
{
    return script ? script->action_seed : 0;
}

bool
pz_debug_script_get_god_mode(const pz_debug_script *script)
{
    return script ? script->action_god_mode : false;
}

void
pz_debug_script_get_teleport_pos(
    const pz_debug_script *script, float *x, float *y)
{
    if (script) {
        if (x)
            *x = script->action_x;
        if (y)
            *y = script->action_y;
    }
}

const char *
pz_debug_script_get_give_item(const pz_debug_script *script)
{
    return script ? script->action_item : NULL;
}

void
pz_debug_script_get_cursor_pos(
    const pz_debug_script *script, float *x, float *y)
{
    if (script) {
        if (x)
            *x = script->action_x;
        if (y)
            *y = script->action_y;
    }
}

void
pz_debug_script_get_spawn_barrier(
    const pz_debug_script *script, float *x, float *y)
{
    if (script) {
        if (x)
            *x = script->action_x;
        if (y)
            *y = script->action_y;
    }
}

void
pz_debug_script_get_spawn_powerup(
    const pz_debug_script *script, float *x, float *y, const char **type)
{
    if (script) {
        if (x)
            *x = script->action_x;
        if (y)
            *y = script->action_y;
        if (type)
            *type = script->action_powerup_type;
    }
}

bool
pz_debug_script_blocks_input(const pz_debug_script *script)
{
    // Block physical input when a script is active and not done
    return script && !script->done;
}

const char *
pz_debug_script_get_net_offer_path(const pz_debug_script *script)
{
    return script ? script->action_path : NULL;
}

void
pz_debug_script_get_net_join_paths(const pz_debug_script *script,
    const char **offer_path, const char **answer_path)
{
    if (script) {
        if (offer_path)
            *offer_path = script->action_path;
        if (answer_path)
            *answer_path = script->action_path2;
    }
}

const char *
pz_debug_script_get_net_answer_path(const pz_debug_script *script)
{
    return script ? script->action_path : NULL;
}

float
pz_debug_script_get_net_wait_timeout(const pz_debug_script *script)
{
    return script ? script->action_timeout : 0.0f;
}

void
pz_debug_script_notify_connected(pz_debug_script *script)
{
    if (script) {
        script->net_connected = true;
    }
}

void
pz_debug_script_notify_action_complete(pz_debug_script *script)
{
    if (script && script->waiting == WAIT_ACTION_COMPLETE) {
        script->waiting = WAIT_NONE;
    }
}

static const char *
pz_debug_ai_state_name(pz_ai_state state)
{
    switch (state) {
    case PZ_AI_STATE_IDLE:
        return "idle";
    case PZ_AI_STATE_SEEKING_COVER:
        return "seeking_cover";
    case PZ_AI_STATE_IN_COVER:
        return "in_cover";
    case PZ_AI_STATE_PEEKING:
        return "peeking";
    case PZ_AI_STATE_FIRING:
        return "firing";
    case PZ_AI_STATE_RETREATING:
        return "retreating";
    case PZ_AI_STATE_CHASING:
        return "chasing";
    case PZ_AI_STATE_FLANKING:
        return "flanking";
    case PZ_AI_STATE_EVADING:
        return "evading";
    case PZ_AI_STATE_ENGAGING:
        return "engaging";
    default:
        return "unknown";
    }
}

static pz_tank *
pz_debug_find_tank_by_id(pz_tank_manager *tank_mgr, int tank_id)
{
    if (!tank_mgr) {
        return NULL;
    }

    for (int i = 0; i < PZ_MAX_TANKS; i++) {
        pz_tank *tank = &tank_mgr->tanks[i];
        if (!(tank->flags & PZ_TANK_FLAG_ACTIVE)) {
            continue;
        }
        if (tank->id == tank_id) {
            return tank;
        }
    }

    return NULL;
}

void
pz_debug_script_dump_state(const char *path, pz_tank_manager *tank_mgr,
    pz_projectile_manager *proj_mgr, pz_ai_manager *ai_mgr,
    const pz_toxic_cloud *toxic_cloud, pz_tank *player, int frame_count)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        pz_log(PZ_LOG_ERROR, PZ_LOG_CAT_CORE,
            "Debug script: failed to open dump file '%s'", path);
        return;
    }

    fprintf(f, "# Tank Game State Dump\n");
    fprintf(f, "frame: %d\n\n", frame_count);

    // Player state
    if (player) {
        fprintf(f, "[player]\n");
        fprintf(f, "pos: %.3f %.3f\n", player->pos.x, player->pos.y);
        fprintf(f, "vel: %.3f %.3f\n", player->vel.x, player->vel.y);
        fprintf(f, "body_angle: %.3f\n", player->body_angle);
        fprintf(f, "turret_angle: %.3f\n", player->turret_angle);
        fprintf(f, "health: %d\n", player->health);
        fprintf(f, "flags: 0x%08x\n", player->flags);
        fprintf(f, "fire_cooldown: %.3f\n", player->fire_cooldown);
        fprintf(f, "\n");
    }

    // Tank counts
    if (tank_mgr) {
        int alive_enemies = 0;
        int dead_enemies = 0;

        for (int i = 0; i < PZ_MAX_TANKS; i++) {
            pz_tank *tank = &tank_mgr->tanks[i];
            if (!(tank->flags & PZ_TANK_FLAG_ACTIVE))
                continue;
            if (tank->flags & PZ_TANK_FLAG_PLAYER)
                continue;

            if (tank->flags & PZ_TANK_FLAG_DEAD)
                dead_enemies++;
            else
                alive_enemies++;
        }

        fprintf(f, "[tanks]\n");
        fprintf(f, "total: %d\n", tank_mgr->tank_count);
        fprintf(f, "enemies_alive: %d\n", alive_enemies);
        fprintf(f, "enemies_dead: %d\n", dead_enemies);
        fprintf(f, "\n");

        // Individual enemy positions
        fprintf(f, "[enemies]\n");
        int enemy_idx = 0;
        for (int i = 0; i < PZ_MAX_TANKS; i++) {
            pz_tank *tank = &tank_mgr->tanks[i];
            if (!(tank->flags & PZ_TANK_FLAG_ACTIVE))
                continue;
            if (tank->flags & PZ_TANK_FLAG_PLAYER)
                continue;

            const char *status
                = (tank->flags & PZ_TANK_FLAG_DEAD) ? "dead" : "alive";
            fprintf(f, "%d: pos=(%.3f, %.3f) health=%d status=%s\n", enemy_idx,
                tank->pos.x, tank->pos.y, tank->health, status);
            enemy_idx++;
        }
        fprintf(f, "\n");
    }

    // AI controller state
    if (ai_mgr) {
        fprintf(f, "[ai]\n");
        for (int i = 0; i < ai_mgr->controller_count; i++) {
            pz_ai_controller *ctrl = &ai_mgr->controllers[i];
            pz_tank *tank = pz_debug_find_tank_by_id(tank_mgr, ctrl->tank_id);
            const char *type_name = pz_enemy_type_name(ctrl->type);
            bool in_toxic = false;
            bool toxic_at_end = false;
            bool target_in_toxic = false;
            pz_vec2 path_target = ctrl->toxic_escape_target;
            pz_vec2 path_goal = ctrl->toxic_escape_target;
            float path_target_dist = 0.0f;
            float path_goal_dist = 0.0f;
            pz_vec2 path_dir = pz_vec2_zero();
            bool detour_active = ctrl->detour_active;
            float detour_timer = ctrl->detour_timer;
            pz_vec2 detour_target = ctrl->detour_target;
            float detour_blocked_timer = ctrl->detour_blocked_timer;
            if (tank && toxic_cloud) {
                in_toxic = pz_toxic_cloud_is_inside(toxic_cloud, tank->pos);
                toxic_at_end = pz_toxic_cloud_will_be_inside(
                    toxic_cloud, tank->pos, 1.0f);
                target_in_toxic = pz_toxic_cloud_is_inside(
                    toxic_cloud, ctrl->toxic_escape_target);
            }
            if (tank && ctrl->toxic_escape_path.valid) {
                path_target = pz_path_get_target(&ctrl->toxic_escape_path);
                path_goal = pz_path_get_goal(&ctrl->toxic_escape_path);
                path_target_dist = pz_vec2_dist(tank->pos, path_target);
                path_goal_dist = pz_vec2_dist(tank->pos, path_goal);
                if (path_target_dist > 0.01f) {
                    pz_vec2 to_target = pz_vec2_sub(path_target, tank->pos);
                    path_dir
                        = pz_vec2_scale(to_target, 1.0f / path_target_dist);
                }
            }

            fprintf(f,
                "tank_id=%d type=%s state=%s pos=(%.3f, %.3f) "
                "toxic_escaping=%d toxic_urgency=%.2f in_toxic=%d "
                "toxic_at_end=%d target=(%.3f, %.3f) target_in_toxic=%d "
                "path_valid=%d path_count=%d path_current=%d "
                "path_complete=%d path_target=(%.3f, %.3f) "
                "path_target_dist=%.3f path_goal=(%.3f, %.3f) "
                "path_goal_dist=%.3f move_dir=(%.3f, %.3f) detour=%d "
                "detour_timer=%.2f detour_blocked=%.2f detour_target=(%.3f, "
                "%.3f)\n",
                ctrl->tank_id, type_name ? type_name : "unknown",
                pz_debug_ai_state_name(ctrl->state), tank ? tank->pos.x : 0.0f,
                tank ? tank->pos.y : 0.0f, ctrl->toxic_escaping ? 1 : 0,
                ctrl->toxic_urgency, in_toxic ? 1 : 0, toxic_at_end ? 1 : 0,
                ctrl->toxic_escape_target.x, ctrl->toxic_escape_target.y,
                target_in_toxic ? 1 : 0, ctrl->toxic_escape_path.valid ? 1 : 0,
                ctrl->toxic_escape_path.count, ctrl->toxic_escape_path.current,
                pz_path_is_complete(&ctrl->toxic_escape_path) ? 1 : 0,
                path_target.x, path_target.y, path_target_dist, path_goal.x,
                path_goal.y, path_goal_dist, path_dir.x, path_dir.y,
                detour_active ? 1 : 0, detour_timer, detour_blocked_timer,
                detour_target.x, detour_target.y);
        }
        fprintf(f, "\n");
    }

    // Toxic cloud summary
    if (toxic_cloud && toxic_cloud->config.enabled) {
        float left = 0.0f;
        float right = 0.0f;
        float top = 0.0f;
        float bottom = 0.0f;
        float radius = 0.0f;
        pz_toxic_cloud_get_boundary(
            toxic_cloud, &left, &right, &top, &bottom, &radius);
        fprintf(f, "[toxic_cloud]\n");
        fprintf(f, "progress: %.3f\n", toxic_cloud->closing_progress);
        fprintf(f, "boundary: left=%.3f right=%.3f top=%.3f bottom=%.3f\n",
            left, right, top, bottom);
        fprintf(f, "corner_radius: %.3f\n", radius);
        fprintf(f, "\n");
    }

    // Projectile state
    if (proj_mgr) {
        fprintf(f, "[projectiles]\n");
        fprintf(f, "active: %d\n", proj_mgr->active_count);

        for (int i = 0; i < PZ_MAX_PROJECTILES; i++) {
            pz_projectile *proj = &proj_mgr->projectiles[i];
            if (!proj->active)
                continue;

            fprintf(f, "  pos=(%.3f, %.3f) vel=(%.3f, %.3f) bounces=%d\n",
                proj->pos.x, proj->pos.y, proj->velocity.x, proj->velocity.y,
                proj->bounces_remaining);
        }
        fprintf(f, "\n");
    }

    fclose(f);
    pz_log(PZ_LOG_INFO, PZ_LOG_CAT_CORE, "Debug script: dumped state to '%s'",
        path);
}
