/*
 * Tank Game - WebRTC Signaling (ntfy.sh, WASM)
 */

#include "pz_net_signaling.h"

#include "../core/pz_mem.h"
#include "../core/pz_platform.h"
#include "../core/pz_str.h"

#include <emscripten/fetch.h>

#include <cstdio>
#include <cstring>

#define PZ_SIGNALING_PREFIX "o57djoyt37JjRboX6vEJgg"

struct pz_signaling_request {
    pz_signaling_publish_cb publish_cb = nullptr;
    pz_signaling_fetch_cb fetch_cb = nullptr;
    void *user_data = nullptr;
};

static char *
pz_signaling_build_url(const char *room, const char *suffix,
    const char *extra)
{
    if (!room || !suffix)
        return nullptr;

    return pz_str_fmt("https://ntfy.sh/%s-%s-%s%s", PZ_SIGNALING_PREFIX, room,
        suffix, extra ? extra : "");
}

static void
pz_signaling_complete_request(pz_signaling_request *request)
{
    if (!request)
        return;

    pz_free(request);
}

static void
pz_signaling_fetch_success(emscripten_fetch_t *fetch)
{
    pz_signaling_request *request
        = static_cast<pz_signaling_request *>(fetch->userData);

    if (request && request->fetch_cb) {
        char *message = nullptr;
        if (fetch->data && fetch->numBytes > 0) {
            message = (char *)pz_alloc(fetch->numBytes + 1);
            if (message) {
                memcpy(message, fetch->data, fetch->numBytes);
                message[fetch->numBytes] = '\0';
                char *trimmed = pz_str_trim(message);
                pz_free(message);
                message = trimmed;
            }
        }
        if (message && message[0] == '\0') {
            pz_free(message);
            message = nullptr;
        }
        request->fetch_cb(message, request->user_data);
        pz_free(message);
    }

    emscripten_fetch_close(fetch);
    pz_signaling_complete_request(request);
}

static void
pz_signaling_fetch_error(emscripten_fetch_t *fetch)
{
    pz_signaling_request *request
        = static_cast<pz_signaling_request *>(fetch->userData);
    if (request && request->fetch_cb)
        request->fetch_cb(nullptr, request->user_data);

    emscripten_fetch_close(fetch);
    pz_signaling_complete_request(request);
}

static void
pz_signaling_publish_success(emscripten_fetch_t *fetch)
{
    pz_signaling_request *request
        = static_cast<pz_signaling_request *>(fetch->userData);
    if (request && request->publish_cb)
        request->publish_cb(true, request->user_data);

    emscripten_fetch_close(fetch);
    pz_signaling_complete_request(request);
}

static void
pz_signaling_publish_error(emscripten_fetch_t *fetch)
{
    pz_signaling_request *request
        = static_cast<pz_signaling_request *>(fetch->userData);
    if (request && request->publish_cb)
        request->publish_cb(false, request->user_data);

    emscripten_fetch_close(fetch);
    pz_signaling_complete_request(request);
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

    char *url = pz_signaling_build_url(room, suffix, "");
    if (!url) {
        if (callback)
            callback(false, user_data);
        return;
    }

    pz_signaling_request *request
        = (pz_signaling_request *)pz_alloc(sizeof(pz_signaling_request));
    if (!request) {
        pz_free(url);
        if (callback)
            callback(false, user_data);
        return;
    }

    request->publish_cb = callback;
    request->user_data = user_data;

    emscripten_fetch_attr_t attr;
    emscripten_fetch_attr_init(&attr);
    strcpy(attr.requestMethod, "POST");
    attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY;
    attr.onsuccess = pz_signaling_publish_success;
    attr.onerror = pz_signaling_publish_error;
    attr.userData = request;
    attr.requestData = message;
    attr.requestDataSize = strlen(message);

    emscripten_fetch(&attr, url);
    pz_free(url);
}

void
pz_signaling_fetch(const char *room, const char *suffix,
    pz_signaling_fetch_cb callback, void *user_data)
{
    if (!room || !suffix) {
        if (callback)
            callback(nullptr, user_data);
        return;
    }

    char *url = pz_signaling_build_url(room, suffix, "/raw?poll=1");
    if (!url) {
        if (callback)
            callback(nullptr, user_data);
        return;
    }

    pz_signaling_request *request
        = (pz_signaling_request *)pz_alloc(sizeof(pz_signaling_request));
    if (!request) {
        pz_free(url);
        if (callback)
            callback(nullptr, user_data);
        return;
    }

    request->fetch_cb = callback;
    request->user_data = user_data;

    emscripten_fetch_attr_t attr;
    emscripten_fetch_attr_init(&attr);
    strcpy(attr.requestMethod, "GET");
    attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY;
    attr.onsuccess = pz_signaling_fetch_success;
    attr.onerror = pz_signaling_fetch_error;
    attr.userData = request;

    emscripten_fetch(&attr, url);
    pz_free(url);
}

void
pz_signaling_update(void)
{
}

void
pz_signaling_shutdown(void)
{
}
