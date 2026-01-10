/*
 * Tank Game - WebRTC Wrapper (libdatachannel)
 */

#include "pz_net_webrtc.h"

#include "../core/pz_log.h"
#include "../core/pz_mem.h"
#include "../core/pz_platform.h"
#include <string.h>

#ifdef PZ_ENABLE_WEBRTC
#    include <rtc/rtc.h>

typedef struct pz_net_webrtc {
    int pc;
    bool gathering_complete;
} pz_net_webrtc;

static bool g_pz_net_webrtc_logger_initialized = false;

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

static void RTC_API
pz_net_webrtc_on_gathering_state(int pc, rtcGatheringState state, void *ptr)
{
    (void)pc;
    pz_net_webrtc *net = ptr;
    if (!net)
        return;

    if (state == RTC_GATHERING_COMPLETE) {
        net->gathering_complete = true;
    }
}

static bool
pz_net_webrtc_wait_for_gathering(pz_net_webrtc *net, uint32_t timeout_ms)
{
    uint64_t start = pz_time_now_ms();

    while (!net->gathering_complete) {
        if (timeout_ms > 0 && (pz_time_now_ms() - start) > timeout_ms) {
            return false;
        }
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
    pz_net_webrtc *net = pz_alloc(sizeof(pz_net_webrtc));
    if (!net)
        return NULL;

    memset(net, 0, sizeof(*net));

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

    return net;
}

void
pz_net_webrtc_destroy(pz_net_webrtc *net)
{
    if (!net)
        return;

    rtcClosePeerConnection(net->pc);
    rtcDeletePeerConnection(net->pc);
    pz_free(net);
}

char *
pz_net_webrtc_create_offer(pz_net_webrtc *net, uint32_t timeout_ms)
{
    if (!net)
        return NULL;

    net->gathering_complete = false;

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

    int rc = rtcSetRemoteDescription(net->pc, sdp, "offer");
    if (rc < 0) {
        PZ_LOG_ERROR(
            PZ_LOG_CAT_NET, "rtcSetRemoteDescription(offer) failed (%d)", rc);
        return false;
    }

    return true;
}

char *
pz_net_webrtc_create_answer(pz_net_webrtc *net, uint32_t timeout_ms)
{
    if (!net)
        return NULL;

    net->gathering_complete = false;

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

#else

typedef struct pz_net_webrtc {
    int unused;
} pz_net_webrtc;

pz_net_webrtc *
pz_net_webrtc_create(const pz_net_webrtc_config *config)
{
    (void)config;
    PZ_LOG_WARN(PZ_LOG_CAT_NET,
        "WebRTC support is disabled (build with PZ_ENABLE_WEBRTC)"
        ".");
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

char *
pz_net_webrtc_create_answer(pz_net_webrtc *net, uint32_t timeout_ms)
{
    (void)net;
    (void)timeout_ms;
    return NULL;
}

#endif
