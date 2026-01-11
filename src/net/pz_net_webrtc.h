/*
 * Tank Game - WebRTC Wrapper (libdatachannel)
 */

#ifndef PZ_NET_WEBRTC_H
#define PZ_NET_WEBRTC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pz_net_webrtc pz_net_webrtc;

typedef struct pz_net_webrtc_config {
    const char **ice_servers;
    int ice_server_count;
    bool enable_logging;
} pz_net_webrtc_config;

typedef void (*pz_net_webrtc_message_callback)(
    const uint8_t *data, size_t len, void *user_data);
typedef void (*pz_net_webrtc_channel_callback)(bool open, void *user_data);

pz_net_webrtc *pz_net_webrtc_create(const pz_net_webrtc_config *config);
void pz_net_webrtc_destroy(pz_net_webrtc *net);

char *pz_net_webrtc_create_offer(pz_net_webrtc *net, uint32_t timeout_ms);
bool pz_net_webrtc_set_remote_offer(pz_net_webrtc *net, const char *sdp);
bool pz_net_webrtc_set_remote_answer(pz_net_webrtc *net, const char *sdp);
char *pz_net_webrtc_create_answer(pz_net_webrtc *net, uint32_t timeout_ms);

bool pz_net_webrtc_set_message_callback(pz_net_webrtc *net,
    pz_net_webrtc_message_callback callback, void *user_data);
bool pz_net_webrtc_set_channel_callback(pz_net_webrtc *net,
    pz_net_webrtc_channel_callback callback, void *user_data);
bool pz_net_webrtc_send(pz_net_webrtc *net, const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif // PZ_NET_WEBRTC_H
