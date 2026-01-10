/*
 * Tank Game - WebRTC Wrapper (libdatachannel)
 */

#ifndef PZ_NET_WEBRTC_H
#define PZ_NET_WEBRTC_H

#include <stdbool.h>
#include <stdint.h>

typedef struct pz_net_webrtc pz_net_webrtc;

typedef struct pz_net_webrtc_config {
    const char **ice_servers;
    int ice_server_count;
    bool enable_logging;
} pz_net_webrtc_config;

pz_net_webrtc *pz_net_webrtc_create(const pz_net_webrtc_config *config);
void pz_net_webrtc_destroy(pz_net_webrtc *net);

char *pz_net_webrtc_create_offer(pz_net_webrtc *net, uint32_t timeout_ms);
bool pz_net_webrtc_set_remote_offer(pz_net_webrtc *net, const char *sdp);
char *pz_net_webrtc_create_answer(pz_net_webrtc *net, uint32_t timeout_ms);

#endif // PZ_NET_WEBRTC_H
