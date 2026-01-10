/*
 * Tank Game - WebRTC Wrapper (datachannel-wasm)
 */

#include "pz_net_webrtc.h"

#include "../core/pz_log.h"
#include "../core/pz_mem.h"
#include "../core/pz_platform.h"

#include <rtc/rtc.hpp>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

struct pz_net_webrtc {
    std::unique_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::DataChannel> offer_channel;
    bool gathering_complete = false;
    bool local_description_ready = false;
    std::string local_description;
    std::string local_type;
};

static bool g_pz_net_webrtc_logger_initialized = false;

static void
pz_net_webrtc_log_callback(rtc::LogLevel level, rtc::string message)
{
    switch (level) {
    case rtc::LogLevel::Fatal:
    case rtc::LogLevel::Error:
        PZ_LOG_ERROR(PZ_LOG_CAT_NET, "%s", message.c_str());
        break;
    case rtc::LogLevel::Warning:
        PZ_LOG_WARN(PZ_LOG_CAT_NET, "%s", message.c_str());
        break;
    case rtc::LogLevel::Info:
        PZ_LOG_INFO(PZ_LOG_CAT_NET, "%s", message.c_str());
        break;
    case rtc::LogLevel::Debug:
    case rtc::LogLevel::Verbose:
        PZ_LOG_DEBUG(PZ_LOG_CAT_NET, "%s", message.c_str());
        break;
    case rtc::LogLevel::None:
    default:
        break;
    }
}

static bool
pz_net_webrtc_wait_for_description(pz_net_webrtc *net, uint32_t timeout_ms)
{
    uint64_t start = pz_time_now_ms();

    while (!(net->local_description_ready && net->gathering_complete)) {
        if (timeout_ms > 0 && (pz_time_now_ms() - start) > timeout_ms) {
            return false;
        }
        pz_time_sleep_ms(10);
    }

    return true;
}

static char *
pz_net_webrtc_copy_string(const std::string &value)
{
    char *buffer = static_cast<char *>(pz_alloc(value.size() + 1));
    if (!buffer)
        return nullptr;

    memcpy(buffer, value.c_str(), value.size() + 1);
    return buffer;
}

pz_net_webrtc *
pz_net_webrtc_create(const pz_net_webrtc_config *config)
{
    if (config && config->enable_logging && !g_pz_net_webrtc_logger_initialized) {
        rtc::InitLogger(rtc::LogLevel::Info, pz_net_webrtc_log_callback);
        g_pz_net_webrtc_logger_initialized = true;
    }

    auto net = std::make_unique<pz_net_webrtc>();

    rtc::Configuration rtc_config;
    if (config && config->ice_servers && config->ice_server_count > 0) {
        rtc_config.iceServers.reserve(static_cast<size_t>(config->ice_server_count));
        for (int i = 0; i < config->ice_server_count; i++) {
            if (config->ice_servers[i] == nullptr)
                continue;
            rtc_config.iceServers.emplace_back(config->ice_servers[i]);
        }
    }

    try {
        net->pc = std::make_unique<rtc::PeerConnection>(rtc_config);
    } catch (const std::exception &ex) {
        PZ_LOG_ERROR(PZ_LOG_CAT_NET, "Failed to create PeerConnection: %s", ex.what());
        return nullptr;
    }

    net->pc->onGatheringStateChange([net_ptr = net.get()](rtc::PeerConnection::GatheringState state) {
        if (state == rtc::PeerConnection::GatheringState::Complete) {
            net_ptr->gathering_complete = true;
        }
    });

    net->pc->onLocalDescription([net_ptr = net.get()](const rtc::Description &description) {
        net_ptr->local_description = std::string(description);
        net_ptr->local_type = description.typeString();
        net_ptr->local_description_ready = true;
    });

    return net.release();
}

void
pz_net_webrtc_destroy(pz_net_webrtc *net)
{
    if (!net)
        return;

    net->offer_channel.reset();
    net->pc.reset();
    pz_free(net);
}

char *
pz_net_webrtc_create_offer(pz_net_webrtc *net, uint32_t timeout_ms)
{
    if (!net || !net->pc)
        return nullptr;

    net->local_description_ready = false;
    net->gathering_complete = false;
    net->local_description.clear();
    net->local_type.clear();

    if (!net->offer_channel) {
        net->offer_channel = net->pc->createDataChannel("game");
    }

    if (!pz_net_webrtc_wait_for_description(net, timeout_ms)) {
        PZ_LOG_WARN(PZ_LOG_CAT_NET, "ICE gathering timed out for offer");
        return nullptr;
    }

    return pz_net_webrtc_copy_string(net->local_description);
}

bool
pz_net_webrtc_set_remote_offer(pz_net_webrtc *net, const char *sdp)
{
    if (!net || !net->pc || !sdp)
        return false;

    try {
        rtc::Description description(sdp, "offer");
        net->pc->setRemoteDescription(description);
    } catch (const std::exception &ex) {
        PZ_LOG_ERROR(PZ_LOG_CAT_NET, "Failed to set remote offer: %s", ex.what());
        return false;
    }

    return true;
}

char *
pz_net_webrtc_create_answer(pz_net_webrtc *net, uint32_t timeout_ms)
{
    if (!net || !net->pc)
        return nullptr;

    net->local_description_ready = false;
    net->gathering_complete = false;
    net->local_description.clear();
    net->local_type.clear();

    if (!pz_net_webrtc_wait_for_description(net, timeout_ms)) {
        PZ_LOG_WARN(PZ_LOG_CAT_NET, "ICE gathering timed out for answer");
        return nullptr;
    }

    return pz_net_webrtc_copy_string(net->local_description);
}
