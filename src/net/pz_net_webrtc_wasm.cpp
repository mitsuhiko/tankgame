/*
 * Tank Game - WebRTC Wrapper (datachannel-wasm)
 */

#include "pz_net_webrtc.h"

#include "../core/pz_log.h"
#include "../core/pz_mem.h"
#include "../core/pz_platform.h"

#include <rtc/rtc.hpp>

#include <atomic>
#include <cstring>
#include <memory>
#include <string>

struct pz_net_webrtc {
    std::unique_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::DataChannel> game_channel;
    std::shared_ptr<rtc::DataChannel> reliable_channel;
    std::atomic_bool gathering_complete { false };
    std::atomic_bool local_description_ready { false };
    std::atomic_bool game_open { false };
    std::atomic_bool reliable_open { false };
    std::atomic_bool reported_open { false };
    std::string local_description;
    pz_net_webrtc_message_callback message_callback = nullptr;
    pz_net_webrtc_channel_callback channel_callback = nullptr;
    void *callback_user_data = nullptr;
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
pz_net_webrtc_refresh_local_description(pz_net_webrtc *net)
{
    if (!net || !net->pc)
        return false;

    auto description = net->pc->localDescription();
    if (!description)
        return false;
    net->local_description = std::string(*description);
    net->local_description_ready.store(true);
    return true;
}

static bool
pz_net_webrtc_wait_for_description(pz_net_webrtc *net, uint32_t timeout_ms)
{
    uint64_t start = pz_time_now_ms();
    const uint32_t min_wait_ms = 2000;

    while (true) {
        uint64_t elapsed = pz_time_now_ms() - start;
        bool ready = net->local_description_ready.load();
        bool complete = net->gathering_complete.load();

        // localDescription() is the authoritative SDP. The callback usually
        // fires before candidates have been appended, so always refresh after
        // gathering completes instead of returning the callback's stale SDP.
        if (ready && complete && pz_net_webrtc_refresh_local_description(net))
            return true;

        if (elapsed >= min_wait_ms
            && pz_net_webrtc_refresh_local_description(net)
            && net->local_description.find("candidate:") != std::string::npos) {
            return true;
        }

        if (timeout_ms > 0 && elapsed > timeout_ms)
            return false;
        pz_time_sleep_ms(10);
    }
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

static void
pz_net_webrtc_report_channel_state(pz_net_webrtc *net)
{
    if (!net)
        return;

    bool open = net->game_open.load() && net->reliable_open.load();
    bool previous = net->reported_open.exchange(open);
    if (open != previous && net->channel_callback)
        net->channel_callback(open, net->callback_user_data);
}

static void
pz_net_webrtc_attach_data_channel(
    pz_net_webrtc *net, const std::shared_ptr<rtc::DataChannel> &channel)
{
    if (!net || !channel)
        return;

    bool game = channel->label() == "game";
    if (game)
        net->game_channel = channel;
    else if (channel->label() == "reliable")
        net->reliable_channel = channel;
    else {
        PZ_LOG_WARN(PZ_LOG_CAT_NET, "Ignoring unknown data channel '%s'",
            channel->label().c_str());
        channel->close();
        return;
    }

    channel->onOpen([net, game]() {
        if (game)
            net->game_open.store(true);
        else
            net->reliable_open.store(true);
        pz_net_webrtc_report_channel_state(net);
    });
    channel->onClosed([net, game]() {
        if (game)
            net->game_open.store(false);
        else
            net->reliable_open.store(false);
        pz_net_webrtc_report_channel_state(net);
    });
    channel->onMessage(
        [net](rtc::binary data) {
            if (net->message_callback) {
                net->message_callback(
                    reinterpret_cast<const uint8_t *>(data.data()), data.size(),
                    net->callback_user_data);
            }
        },
        [net](rtc::string data) {
            if (net->message_callback) {
                net->message_callback(
                    reinterpret_cast<const uint8_t *>(data.data()), data.size(),
                    net->callback_user_data);
            }
        });

    if (channel->isOpen()) {
        if (game)
            net->game_open.store(true);
        else
            net->reliable_open.store(true);
        pz_net_webrtc_report_channel_state(net);
    }
}

pz_net_webrtc *
pz_net_webrtc_create(const pz_net_webrtc_config *config)
{
    if (config && config->enable_logging
        && !g_pz_net_webrtc_logger_initialized) {
        rtc::InitLogger(rtc::LogLevel::Info, pz_net_webrtc_log_callback);
        g_pz_net_webrtc_logger_initialized = true;
    }

    auto net = std::make_unique<pz_net_webrtc>();
    rtc::Configuration rtc_config;
    if (config && config->ice_servers && config->ice_server_count > 0) {
        rtc_config.iceServers.reserve(
            static_cast<size_t>(config->ice_server_count));
        for (int i = 0; i < config->ice_server_count; i++) {
            if (config->ice_servers[i])
                rtc_config.iceServers.emplace_back(config->ice_servers[i]);
        }
    }

    try {
        net->pc = std::make_unique<rtc::PeerConnection>(rtc_config);
    } catch (const std::exception &ex) {
        PZ_LOG_ERROR(
            PZ_LOG_CAT_NET, "Failed to create PeerConnection: %s", ex.what());
        return nullptr;
    }

    net->pc->onGatheringStateChange(
        [net_ptr = net.get()](rtc::PeerConnection::GatheringState state) {
            if (state == rtc::PeerConnection::GatheringState::Complete)
                net_ptr->gathering_complete.store(true);
        });
    net->pc->onLocalDescription(
        [net_ptr = net.get()](const rtc::Description &description) {
            net_ptr->local_description = std::string(description);
            net_ptr->local_description_ready.store(true);
        });
    net->pc->onDataChannel(
        [net_ptr = net.get()](std::shared_ptr<rtc::DataChannel> channel) {
            pz_net_webrtc_attach_data_channel(net_ptr, channel);
        });

    return net.release();
}

void
pz_net_webrtc_destroy(pz_net_webrtc *net)
{
    if (!net)
        return;

    net->message_callback = nullptr;
    net->channel_callback = nullptr;
    if (net->game_channel)
        net->game_channel->close();
    if (net->reliable_channel)
        net->reliable_channel->close();
    net->game_channel.reset();
    net->reliable_channel.reset();
    if (net->pc)
        net->pc->close();
    net->pc.reset();
    delete net;
}

char *
pz_net_webrtc_create_offer(pz_net_webrtc *net, uint32_t timeout_ms)
{
    if (!net || !net->pc)
        return nullptr;

    net->local_description_ready.store(false);
    net->gathering_complete.store(false);
    net->local_description.clear();

    if (!net->game_channel) {
        rtc::DataChannelInit init;
        init.reliability.unordered = true;
        init.reliability.maxRetransmits = 0u;
        pz_net_webrtc_attach_data_channel(
            net, net->pc->createDataChannel("game", init));
    }
    if (!net->reliable_channel) {
        pz_net_webrtc_attach_data_channel(
            net, net->pc->createDataChannel("reliable"));
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

    // datachannel-wasm auto-generates the answer from setRemoteDescription(),
    // so reset readiness before starting that asynchronous operation.
    net->local_description_ready.store(false);
    net->gathering_complete.store(false);
    net->local_description.clear();
    try {
        net->pc->setRemoteDescription(rtc::Description(sdp, "offer"));
    } catch (const std::exception &ex) {
        PZ_LOG_ERROR(
            PZ_LOG_CAT_NET, "Failed to set remote offer: %s", ex.what());
        return false;
    }
    return true;
}

bool
pz_net_webrtc_set_remote_answer(pz_net_webrtc *net, const char *sdp)
{
    if (!net || !net->pc || !sdp)
        return false;
    try {
        net->pc->setRemoteDescription(rtc::Description(sdp, "answer"));
    } catch (const std::exception &ex) {
        PZ_LOG_ERROR(
            PZ_LOG_CAT_NET, "Failed to set remote answer: %s", ex.what());
        return false;
    }
    return true;
}

char *
pz_net_webrtc_create_answer(pz_net_webrtc *net, uint32_t timeout_ms)
{
    if (!net || !net->pc)
        return nullptr;

    if (!pz_net_webrtc_wait_for_description(net, timeout_ms)) {
        PZ_LOG_WARN(PZ_LOG_CAT_NET, "ICE gathering timed out for answer");
        return nullptr;
    }
    return pz_net_webrtc_copy_string(net->local_description);
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
pz_net_webrtc_send_channel(const std::shared_ptr<rtc::DataChannel> &channel,
    bool open, size_t max_buffered, const uint8_t *data, size_t len)
{
    if (!channel || !open || !data || len == 0
        || channel->bufferedAmount() > max_buffered) {
        return false;
    }
    return channel->send(reinterpret_cast<const rtc::byte *>(data), len);
}

bool
pz_net_webrtc_send_game(pz_net_webrtc *net, const uint8_t *data, size_t len)
{
    return net
        && pz_net_webrtc_send_channel(
            net->game_channel, net->game_open.load(), 256 * 1024, data, len);
}

bool
pz_net_webrtc_send_reliable(pz_net_webrtc *net, const uint8_t *data, size_t len)
{
    return net
        && pz_net_webrtc_send_channel(net->reliable_channel,
            net->reliable_open.load(), 1024 * 1024, data, len);
}
