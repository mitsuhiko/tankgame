/*
 * Tank Game - WebRTC Wrapper (libdatachannel)
 */

#include "pz_net_webrtc.h"

#include "../core/pz_log.h"
#include "../core/pz_mem.h"
#include "../core/pz_platform.h"

#include <limits.h>
#include <stdatomic.h>
#include <string.h>

#ifdef PZ_ENABLE_WEBRTC
#    include <rtc/rtc.h>

#    define PZ_NET_MAX_GAME_BUFFERED (256 * 1024)
#    define PZ_NET_MAX_RELIABLE_BUFFERED (1024 * 1024)

typedef struct pz_net_webrtc {
    int pc;
    int game_dc;
    int reliable_dc;
    atomic_bool gathering_complete;
    atomic_bool have_remote_offer;
    atomic_bool game_open;
    atomic_bool reliable_open;
    atomic_bool reported_open;
    pz_net_webrtc_message_callback message_callback;
    pz_net_webrtc_channel_callback channel_callback;
    void *callback_user_data;
} pz_net_webrtc;

static bool g_pz_net_webrtc_logger_initialized = false;

static char *pz_net_webrtc_get_local_description(pz_net_webrtc *net);

static void RTC_API
pz_net_webrtc_log_callback(rtcLogLevel level, const char *message)
{
    switch (level) {
    case RTC_LOG_FATAL:
    case RTC_LOG_ERROR:
        PZ_LOG_ERROR(PZ_LOG_CAT_NET, "%s", message);
        break;
    case RTC_LOG_WARNING:
        PZ_LOG_WARN(PZ_LOG_CAT_NET, "%s", message);
        break;
    case RTC_LOG_INFO:
        PZ_LOG_INFO(PZ_LOG_CAT_NET, "%s", message);
        break;
    case RTC_LOG_DEBUG:
    case RTC_LOG_VERBOSE:
        PZ_LOG_DEBUG(PZ_LOG_CAT_NET, "%s", message);
        break;
    case RTC_LOG_NONE:
    default:
        break;
    }
}

static void
pz_net_webrtc_report_channel_state(pz_net_webrtc *net)
{
    if (!net)
        return;

    bool open
        = atomic_load(&net->game_open) && atomic_load(&net->reliable_open);
    bool previous = atomic_exchange(&net->reported_open, open);
    if (open != previous && net->channel_callback)
        net->channel_callback(open, net->callback_user_data);
}

static void RTC_API
pz_net_webrtc_on_gathering_state(int pc, rtcGatheringState state, void *ptr)
{
    (void)pc;
    pz_net_webrtc *net = ptr;
    if (net && state == RTC_GATHERING_COMPLETE)
        atomic_store(&net->gathering_complete, true);
}

static void RTC_API
pz_net_webrtc_on_signaling_state(int pc, rtcSignalingState state, void *ptr)
{
    (void)pc;
    pz_net_webrtc *net = ptr;
    if (net && state == RTC_SIGNALING_HAVE_REMOTE_OFFER)
        atomic_store(&net->have_remote_offer, true);
}

static void RTC_API
pz_net_webrtc_on_channel_open(int id, void *ptr)
{
    pz_net_webrtc *net = ptr;
    if (!net)
        return;

    if (id == net->game_dc)
        atomic_store(&net->game_open, true);
    if (id == net->reliable_dc)
        atomic_store(&net->reliable_open, true);
    pz_net_webrtc_report_channel_state(net);
}

static void RTC_API
pz_net_webrtc_on_channel_closed(int id, void *ptr)
{
    pz_net_webrtc *net = ptr;
    if (!net)
        return;

    if (id == net->game_dc)
        atomic_store(&net->game_open, false);
    if (id == net->reliable_dc)
        atomic_store(&net->reliable_open, false);
    pz_net_webrtc_report_channel_state(net);
}

static void RTC_API
pz_net_webrtc_on_channel_message(
    int id, const char *message, int size, void *ptr)
{
    (void)id;
    pz_net_webrtc *net = ptr;
    if (!net || !net->message_callback || !message || size <= 0)
        return;

    net->message_callback(
        (const uint8_t *)message, (size_t)size, net->callback_user_data);
}

static bool
pz_net_webrtc_channel_is_game(int dc)
{
    char label[32] = { 0 };
    int rc = rtcGetDataChannelLabel(dc, label, (int)sizeof(label));
    return rc >= 0 && strcmp(label, "game") == 0;
}

static void
pz_net_webrtc_attach_data_channel(pz_net_webrtc *net, int dc, bool game)
{
    if (!net || dc < 0)
        return;

    if (game)
        net->game_dc = dc;
    else
        net->reliable_dc = dc;

    rtcSetUserPointer(dc, net);
    rtcSetOpenCallback(dc, pz_net_webrtc_on_channel_open);
    rtcSetClosedCallback(dc, pz_net_webrtc_on_channel_closed);
    rtcSetMessageCallback(dc, pz_net_webrtc_on_channel_message);

    if (rtcIsOpen(dc)) {
        if (game)
            atomic_store(&net->game_open, true);
        else
            atomic_store(&net->reliable_open, true);
        pz_net_webrtc_report_channel_state(net);
    }
}

static void RTC_API
pz_net_webrtc_on_data_channel(int pc, int dc, void *ptr)
{
    (void)pc;
    pz_net_webrtc *net = ptr;
    if (!net)
        return;

    bool game = pz_net_webrtc_channel_is_game(dc);
    pz_net_webrtc_attach_data_channel(net, dc, game);
}

static bool
pz_net_webrtc_wait_for_gathering(pz_net_webrtc *net, uint32_t timeout_ms)
{
    uint64_t start = pz_time_now_ms();
    const uint32_t min_wait_ms = 2000;

    while (!atomic_load(&net->gathering_complete)) {
        uint64_t elapsed = pz_time_now_ms() - start;
        if (elapsed >= min_wait_ms) {
            char *desc = pz_net_webrtc_get_local_description(net);
            if (desc) {
                bool has_candidates = strstr(desc, "candidate:") != NULL;
                pz_free(desc);
                if (has_candidates)
                    return true;
            }
        }

        if (timeout_ms > 0 && elapsed > timeout_ms)
            return false;
        pz_time_sleep_ms(10);
    }

    return true;
}

static bool
pz_net_webrtc_wait_for_remote_offer(pz_net_webrtc *net, uint32_t timeout_ms)
{
    uint64_t start = pz_time_now_ms();
    while (!atomic_load(&net->have_remote_offer)) {
        if (timeout_ms > 0 && pz_time_now_ms() - start > timeout_ms)
            return false;
        pz_time_sleep_ms(10);
    }
    return true;
}

static char *
pz_net_webrtc_get_local_description(pz_net_webrtc *net)
{
    int size = 4096;
    for (int attempt = 0; attempt < 6; attempt++) {
        char *buffer = pz_alloc((size_t)size);
        if (!buffer)
            return NULL;

        int rc = rtcGetLocalDescription(net->pc, buffer, size);
        if (rc == RTC_ERR_TOO_SMALL) {
            pz_free(buffer);
            size *= 2;
            continue;
        }
        if (rc < 0) {
            PZ_LOG_ERROR(
                PZ_LOG_CAT_NET, "rtcGetLocalDescription failed (%d)", rc);
            pz_free(buffer);
            return NULL;
        }
        if (rc >= size) {
            pz_free(buffer);
            size = rc + 1;
            continue;
        }

        buffer[rc] = '\0';
        return buffer;
    }

    PZ_LOG_ERROR(
        PZ_LOG_CAT_NET, "rtcGetLocalDescription exceeded buffer limits");
    return NULL;
}

pz_net_webrtc *
pz_net_webrtc_create(const pz_net_webrtc_config *config)
{
    pz_net_webrtc *net = pz_calloc(1, sizeof(*net));
    if (!net)
        return NULL;

    net->pc = -1;
    net->game_dc = -1;
    net->reliable_dc = -1;

    if (config && config->enable_logging
        && !g_pz_net_webrtc_logger_initialized) {
        rtcInitLogger(RTC_LOG_INFO, pz_net_webrtc_log_callback);
        g_pz_net_webrtc_logger_initialized = true;
    }

    rtcConfiguration rtc_config;
    memset(&rtc_config, 0, sizeof(rtc_config));
    if (config) {
        rtc_config.iceServers = config->ice_servers;
        rtc_config.iceServersCount = config->ice_server_count;
    }
    rtc_config.disableAutoNegotiation = true;

    net->pc = rtcCreatePeerConnection(&rtc_config);
    if (net->pc < 0) {
        PZ_LOG_ERROR(
            PZ_LOG_CAT_NET, "rtcCreatePeerConnection failed (%d)", net->pc);
        pz_free(net);
        return NULL;
    }

    rtcSetUserPointer(net->pc, net);
    rtcSetGatheringStateChangeCallback(
        net->pc, pz_net_webrtc_on_gathering_state);
    rtcSetSignalingStateChangeCallback(
        net->pc, pz_net_webrtc_on_signaling_state);
    rtcSetDataChannelCallback(net->pc, pz_net_webrtc_on_data_channel);
    return net;
}

void
pz_net_webrtc_destroy(pz_net_webrtc *net)
{
    if (!net)
        return;

    net->message_callback = NULL;
    net->channel_callback = NULL;
    if (net->game_dc >= 0) {
        rtcSetUserPointer(net->game_dc, NULL);
        rtcDeleteDataChannel(net->game_dc);
    }
    if (net->reliable_dc >= 0 && net->reliable_dc != net->game_dc) {
        rtcSetUserPointer(net->reliable_dc, NULL);
        rtcDeleteDataChannel(net->reliable_dc);
    }
    if (net->pc >= 0) {
        rtcSetUserPointer(net->pc, NULL);
        rtcClosePeerConnection(net->pc);
        rtcDeletePeerConnection(net->pc);
    }
    pz_free(net);
}

char *
pz_net_webrtc_create_offer(pz_net_webrtc *net, uint32_t timeout_ms)
{
    if (!net)
        return NULL;

    atomic_store(&net->gathering_complete, false);

    if (net->game_dc < 0) {
        rtcDataChannelInit init;
        memset(&init, 0, sizeof(init));
        init.reliability.unordered = true;
        init.reliability.unreliable = true;
        init.reliability.maxRetransmits = 0;
        int dc = rtcCreateDataChannelEx(net->pc, "game", &init);
        if (dc < 0) {
            PZ_LOG_ERROR(
                PZ_LOG_CAT_NET, "rtcCreateDataChannelEx(game) failed (%d)", dc);
            return NULL;
        }
        pz_net_webrtc_attach_data_channel(net, dc, true);
    }

    if (net->reliable_dc < 0) {
        int dc = rtcCreateDataChannel(net->pc, "reliable");
        if (dc < 0) {
            PZ_LOG_ERROR(PZ_LOG_CAT_NET,
                "rtcCreateDataChannel(reliable) failed (%d)", dc);
            return NULL;
        }
        pz_net_webrtc_attach_data_channel(net, dc, false);
    }

    int rc = rtcSetLocalDescription(net->pc, "offer");
    if (rc < 0) {
        PZ_LOG_ERROR(
            PZ_LOG_CAT_NET, "rtcSetLocalDescription(offer) failed (%d)", rc);
        return NULL;
    }

    if (!pz_net_webrtc_wait_for_gathering(net, timeout_ms)) {
        PZ_LOG_WARN(PZ_LOG_CAT_NET, "ICE gathering timed out for offer");
        return NULL;
    }
    return pz_net_webrtc_get_local_description(net);
}

bool
pz_net_webrtc_set_remote_offer(pz_net_webrtc *net, const char *sdp)
{
    if (!net || !sdp)
        return false;

    atomic_store(&net->have_remote_offer, false);
    int rc = rtcSetRemoteDescription(net->pc, sdp, "offer");
    if (rc < 0) {
        PZ_LOG_ERROR(
            PZ_LOG_CAT_NET, "rtcSetRemoteDescription(offer) failed (%d)", rc);
        return false;
    }
    return true;
}

bool
pz_net_webrtc_set_remote_answer(pz_net_webrtc *net, const char *sdp)
{
    if (!net || !sdp)
        return false;

    int rc = rtcSetRemoteDescription(net->pc, sdp, "answer");
    if (rc < 0) {
        PZ_LOG_ERROR(
            PZ_LOG_CAT_NET, "rtcSetRemoteDescription(answer) failed (%d)", rc);
        return false;
    }
    return true;
}

char *
pz_net_webrtc_create_answer(pz_net_webrtc *net, uint32_t timeout_ms)
{
    if (!net)
        return NULL;

    atomic_store(&net->gathering_complete, false);
    if (!pz_net_webrtc_wait_for_remote_offer(net, timeout_ms)) {
        PZ_LOG_ERROR(PZ_LOG_CAT_NET, "Timed out waiting for remote offer");
        return NULL;
    }

    int rc = rtcSetLocalDescription(net->pc, "answer");
    if (rc < 0) {
        PZ_LOG_ERROR(
            PZ_LOG_CAT_NET, "rtcSetLocalDescription(answer) failed (%d)", rc);
        return NULL;
    }
    if (!pz_net_webrtc_wait_for_gathering(net, timeout_ms)) {
        PZ_LOG_WARN(PZ_LOG_CAT_NET, "ICE gathering timed out for answer");
        return NULL;
    }
    return pz_net_webrtc_get_local_description(net);
}

bool
pz_net_webrtc_set_message_callback(pz_net_webrtc *net,
    pz_net_webrtc_message_callback callback, void *user_data)
{
    if (!net)
        return false;
    net->message_callback = callback;
    net->callback_user_data = user_data;
    return true;
}

bool
pz_net_webrtc_set_channel_callback(pz_net_webrtc *net,
    pz_net_webrtc_channel_callback callback, void *user_data)
{
    if (!net)
        return false;
    net->channel_callback = callback;
    net->callback_user_data = user_data;
    return true;
}

static bool
pz_net_webrtc_send_channel(int dc, atomic_bool *open, const uint8_t *data,
    size_t len, int max_buffered)
{
    if (dc < 0 || !data || len == 0 || len > INT_MAX)
        return false;
    if (!atomic_load(open) && !rtcIsOpen(dc))
        return false;
    atomic_store(open, true);

    int buffered = rtcGetBufferedAmount(dc);
    if (buffered < 0 || buffered > max_buffered)
        return false;

    int rc = rtcSendMessage(dc, (const char *)data, (int)len);
    if (rc < 0) {
        PZ_LOG_WARN(PZ_LOG_CAT_NET, "rtcSendMessage failed (%d)", rc);
        return false;
    }
    return true;
}

bool
pz_net_webrtc_send_game(pz_net_webrtc *net, const uint8_t *data, size_t len)
{
    return net
        && pz_net_webrtc_send_channel(
            net->game_dc, &net->game_open, data, len, PZ_NET_MAX_GAME_BUFFERED);
}

bool
pz_net_webrtc_send_reliable(pz_net_webrtc *net, const uint8_t *data, size_t len)
{
    return net
        && pz_net_webrtc_send_channel(net->reliable_dc, &net->reliable_open,
            data, len, PZ_NET_MAX_RELIABLE_BUFFERED);
}

#else

typedef struct pz_net_webrtc {
    int unused;
} pz_net_webrtc;

pz_net_webrtc *
pz_net_webrtc_create(const pz_net_webrtc_config *config)
{
    (void)config;
    PZ_LOG_WARN(PZ_LOG_CAT_NET,
        "WebRTC support is disabled (build with PZ_ENABLE_WEBRTC).");
    return NULL;
}

void
pz_net_webrtc_destroy(pz_net_webrtc *net)
{
    (void)net;
}

char *
pz_net_webrtc_create_offer(pz_net_webrtc *net, uint32_t timeout_ms)
{
    (void)net;
    (void)timeout_ms;
    return NULL;
}

bool
pz_net_webrtc_set_remote_offer(pz_net_webrtc *net, const char *sdp)
{
    (void)net;
    (void)sdp;
    return false;
}

bool
pz_net_webrtc_set_remote_answer(pz_net_webrtc *net, const char *sdp)
{
    (void)net;
    (void)sdp;
    return false;
}

char *
pz_net_webrtc_create_answer(pz_net_webrtc *net, uint32_t timeout_ms)
{
    (void)net;
    (void)timeout_ms;
    return NULL;
}

bool
pz_net_webrtc_set_message_callback(pz_net_webrtc *net,
    pz_net_webrtc_message_callback callback, void *user_data)
{
    (void)net;
    (void)callback;
    (void)user_data;
    return false;
}

bool
pz_net_webrtc_set_channel_callback(pz_net_webrtc *net,
    pz_net_webrtc_channel_callback callback, void *user_data)
{
    (void)net;
    (void)callback;
    (void)user_data;
    return false;
}

bool
pz_net_webrtc_send_game(pz_net_webrtc *net, const uint8_t *data, size_t len)
{
    (void)net;
    (void)data;
    (void)len;
    return false;
}

bool
pz_net_webrtc_send_reliable(pz_net_webrtc *net, const uint8_t *data, size_t len)
{
    (void)net;
    (void)data;
    (void)len;
    return false;
}

#endif
