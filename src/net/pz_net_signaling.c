/*
 * Tank Game - WebRTC Signaling (ntfy.sh)
 */

#include "pz_net_signaling.h"

#include "../core/pz_log.h"
#include "../core/pz_mem.h"
#include "../core/pz_platform.h"
#include "../core/pz_str.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PZ_SIGNALING_PREFIX "o57djoyt37JjRboX6vEJgg"

typedef enum {
    PZ_SIGNALING_TASK_PUBLISH,
    PZ_SIGNALING_TASK_FETCH,
} pz_signaling_task_type;

typedef struct pz_signaling_task {
    pz_signaling_task_type type;
    char *room;
    char *suffix;
    char *message;
    pz_signaling_publish_cb publish_cb;
    pz_signaling_fetch_cb fetch_cb;
    void *user_data;
    struct pz_signaling_task *next;
} pz_signaling_task;

typedef struct pz_signaling_result {
    pz_signaling_task_type type;
    bool success;
    char *message;
    pz_signaling_publish_cb publish_cb;
    pz_signaling_fetch_cb fetch_cb;
    void *user_data;
    struct pz_signaling_result *next;
} pz_signaling_result;

static pthread_mutex_t g_signal_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_signal_cond = PTHREAD_COND_INITIALIZER;
static pthread_t g_signal_thread;
static bool g_signal_thread_running = false;
static bool g_signal_thread_shutdown = false;
static pz_signaling_task *g_task_head = NULL;
static pz_signaling_task *g_task_tail = NULL;
static pz_signaling_result *g_result_head = NULL;
static pz_signaling_result *g_result_tail = NULL;

static bool
pz_signaling_is_room_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9');
}

static bool
pz_signaling_validate_room(const char *room)
{
    if (!room)
        return false;

    size_t len = strlen(room);
    if (len == 0 || len > 32)
        return false;

    for (size_t i = 0; i < len; i++) {
        if (!pz_signaling_is_room_char(room[i]))
            return false;
    }

    return true;
}

static bool
pz_signaling_validate_suffix(const char *suffix)
{
    if (!suffix || suffix[0] == '\0' || suffix[1] != '\0')
        return false;

    return pz_signaling_is_room_char(suffix[0]);
}

static char *
pz_signaling_build_topic(const char *room, const char *suffix)
{
    if (!pz_signaling_validate_room(room)
        || !pz_signaling_validate_suffix(suffix))
        return NULL;

    return pz_str_fmt("%s-%s-%s", PZ_SIGNALING_PREFIX, room, suffix);
}

static char *
pz_signaling_read_pipe(FILE *pipe)
{
    if (!pipe)
        return NULL;

    size_t capacity = 256;
    size_t size = 0;
    char *buffer = pz_alloc(capacity);

    if (!buffer)
        return NULL;

    int ch = 0;
    while ((ch = fgetc(pipe)) != EOF) {
        if (size + 1 >= capacity) {
            size_t new_capacity = capacity * 2;
            char *new_buffer = pz_realloc(buffer, new_capacity);
            if (!new_buffer) {
                pz_free(buffer);
                return NULL;
            }
            buffer = new_buffer;
            capacity = new_capacity;
        }
        buffer[size++] = (char)ch;
    }

    buffer[size] = '\0';
    return buffer;
}

static bool
pz_signaling_run_publish(
    const char *room, const char *suffix, const char *message)
{
    char *topic = pz_signaling_build_topic(room, suffix);
    if (!topic)
        return false;

    char *cmd = pz_str_fmt(
        "curl -s -X POST --data-binary @- https://ntfy.sh/%s", topic);
    pz_free(topic);

    if (!cmd)
        return false;

    FILE *pipe = popen(cmd, "w");
    pz_free(cmd);

    if (!pipe)
        return false;

    bool success = true;
    if (message && message[0] != '\0') {
        size_t len = strlen(message);
        if (fwrite(message, 1, len, pipe) != len) {
            success = false;
        }
    }

    int status = pclose(pipe);
    if (status != 0)
        success = false;

    return success;
}

static char *
pz_signaling_run_fetch(const char *room, const char *suffix)
{
    char *topic = pz_signaling_build_topic(room, suffix);
    if (!topic)
        return NULL;

    char *cmd = pz_str_fmt("curl -s https://ntfy.sh/%s/raw?poll=1", topic);
    pz_free(topic);

    if (!cmd)
        return NULL;

    FILE *pipe = popen(cmd, "r");
    pz_free(cmd);

    if (!pipe)
        return NULL;

    char *output = pz_signaling_read_pipe(pipe);
    int status = pclose(pipe);

    if (status != 0) {
        pz_free(output);
        return NULL;
    }

    if (!output)
        return NULL;

    char *trimmed = pz_str_trim(output);
    pz_free(output);

    if (!trimmed || trimmed[0] == '\0') {
        pz_free(trimmed);
        return NULL;
    }

    return trimmed;
}

static void
pz_signaling_push_result(pz_signaling_result *result)
{
    if (!result)
        return;

    pthread_mutex_lock(&g_signal_mutex);
    if (g_result_tail) {
        g_result_tail->next = result;
        g_result_tail = result;
    } else {
        g_result_head = result;
        g_result_tail = result;
    }
    pthread_mutex_unlock(&g_signal_mutex);
}

static void *
pz_signaling_worker(void *unused)
{
    (void)unused;

    while (true) {
        pthread_mutex_lock(&g_signal_mutex);
        while (!g_task_head && !g_signal_thread_shutdown) {
            pthread_cond_wait(&g_signal_cond, &g_signal_mutex);
        }

        if (g_signal_thread_shutdown) {
            pthread_mutex_unlock(&g_signal_mutex);
            break;
        }

        pz_signaling_task *task = g_task_head;
        g_task_head = task->next;
        if (!g_task_head)
            g_task_tail = NULL;
        pthread_mutex_unlock(&g_signal_mutex);

        pz_signaling_result *result
            = (pz_signaling_result *)pz_alloc(sizeof(pz_signaling_result));
        if (!result) {
            pz_free(task->room);
            pz_free(task->suffix);
            pz_free(task->message);
            pz_free(task);
            continue;
        }

        memset(result, 0, sizeof(*result));
        result->type = task->type;
        result->publish_cb = task->publish_cb;
        result->fetch_cb = task->fetch_cb;
        result->user_data = task->user_data;

        if (task->type == PZ_SIGNALING_TASK_PUBLISH) {
            result->success = pz_signaling_run_publish(
                task->room, task->suffix, task->message);
        } else {
            result->message = pz_signaling_run_fetch(task->room, task->suffix);
            result->success = result->message != NULL;
        }

        pz_free(task->room);
        pz_free(task->suffix);
        pz_free(task->message);
        pz_free(task);

        pz_signaling_push_result(result);
    }

    return NULL;
}

static void
pz_signaling_start_thread(void)
{
    if (g_signal_thread_running)
        return;

    g_signal_thread_shutdown = false;
    if (pthread_create(&g_signal_thread, NULL, pz_signaling_worker, NULL)
        == 0) {
        g_signal_thread_running = true;
    } else {
        PZ_LOG_ERROR(PZ_LOG_CAT_NET, "Failed to start signaling thread");
    }
}

static void
pz_signaling_enqueue(pz_signaling_task *task)
{
    if (!task)
        return;

    pz_signaling_start_thread();

    pthread_mutex_lock(&g_signal_mutex);
    if (g_task_tail) {
        g_task_tail->next = task;
        g_task_tail = task;
    } else {
        g_task_head = task;
        g_task_tail = task;
    }
    pthread_cond_signal(&g_signal_cond);
    pthread_mutex_unlock(&g_signal_mutex);
}

const char *
pz_signaling_generate_room(void)
{
    static char room[7];
    static bool seeded = false;
    static uint32_t rng_state = 0;

    if (!seeded) {
        rng_state = (uint32_t)pz_time_now_ms();
        if (rng_state == 0)
            rng_state = 0x12345678u;
        seeded = true;
    }

    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;

    uint32_t value = rng_state & 0xFFFFFFu;
    snprintf(room, sizeof(room), "%06x", value);
    return room;
}

void
pz_signaling_publish(const char *room, const char *suffix, const char *message,
    pz_signaling_publish_cb callback, void *user_data)
{
    if (!room || !suffix || !message) {
        if (callback)
            callback(false, user_data);
        return;
    }

    if (!pz_signaling_validate_room(room)
        || !pz_signaling_validate_suffix(suffix)) {
        if (callback)
            callback(false, user_data);
        return;
    }

    pz_signaling_task *task
        = (pz_signaling_task *)pz_alloc(sizeof(pz_signaling_task));
    if (!task) {
        if (callback)
            callback(false, user_data);
        return;
    }

    memset(task, 0, sizeof(*task));
    task->type = PZ_SIGNALING_TASK_PUBLISH;
    task->room = pz_str_dup(room);
    task->suffix = pz_str_dup(suffix);
    task->message = pz_str_dup(message);
    task->publish_cb = callback;
    task->user_data = user_data;

    if (!task->room || !task->suffix || !task->message) {
        pz_free(task->room);
        pz_free(task->suffix);
        pz_free(task->message);
        pz_free(task);
        if (callback)
            callback(false, user_data);
        return;
    }

    pz_signaling_enqueue(task);
}

void
pz_signaling_fetch(const char *room, const char *suffix,
    pz_signaling_fetch_cb callback, void *user_data)
{
    if (!room || !suffix) {
        if (callback)
            callback(NULL, user_data);
        return;
    }

    if (!pz_signaling_validate_room(room)
        || !pz_signaling_validate_suffix(suffix)) {
        if (callback)
            callback(NULL, user_data);
        return;
    }

    pz_signaling_task *task
        = (pz_signaling_task *)pz_alloc(sizeof(pz_signaling_task));
    if (!task) {
        if (callback)
            callback(NULL, user_data);
        return;
    }

    memset(task, 0, sizeof(*task));
    task->type = PZ_SIGNALING_TASK_FETCH;
    task->room = pz_str_dup(room);
    task->suffix = pz_str_dup(suffix);
    task->fetch_cb = callback;
    task->user_data = user_data;

    if (!task->room || !task->suffix) {
        pz_free(task->room);
        pz_free(task->suffix);
        pz_free(task);
        if (callback)
            callback(NULL, user_data);
        return;
    }

    pz_signaling_enqueue(task);
}

void
pz_signaling_update(void)
{
    pthread_mutex_lock(&g_signal_mutex);
    pz_signaling_result *result = g_result_head;
    g_result_head = NULL;
    g_result_tail = NULL;
    pthread_mutex_unlock(&g_signal_mutex);

    while (result) {
        pz_signaling_result *next = result->next;
        if (result->type == PZ_SIGNALING_TASK_PUBLISH) {
            if (result->publish_cb)
                result->publish_cb(result->success, result->user_data);
        } else if (result->type == PZ_SIGNALING_TASK_FETCH) {
            if (result->fetch_cb)
                result->fetch_cb(result->message, result->user_data);
        }
        pz_free(result->message);
        pz_free(result);
        result = next;
    }
}

void
pz_signaling_shutdown(void)
{
    if (!g_signal_thread_running)
        return;

    pthread_mutex_lock(&g_signal_mutex);
    g_signal_thread_shutdown = true;
    pthread_cond_signal(&g_signal_cond);
    pthread_mutex_unlock(&g_signal_mutex);

    pthread_join(g_signal_thread, NULL);
    g_signal_thread_running = false;

    pthread_mutex_lock(&g_signal_mutex);
    while (g_task_head) {
        pz_signaling_task *task = g_task_head;
        g_task_head = task->next;
        pz_free(task->room);
        pz_free(task->suffix);
        pz_free(task->message);
        pz_free(task);
    }
    g_task_tail = NULL;

    while (g_result_head) {
        pz_signaling_result *result = g_result_head;
        g_result_head = result->next;
        pz_free(result->message);
        pz_free(result);
    }
    g_result_tail = NULL;
    pthread_mutex_unlock(&g_signal_mutex);
}
