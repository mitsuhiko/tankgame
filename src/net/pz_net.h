/*
 * Tank Game - Networking Helpers
 *
 * WebRTC offer encoding/decoding for QR/URL handshakes.
 */

#ifndef PZ_NET_H
#define PZ_NET_H

#include <stdint.h>

#define PZ_NET_JOIN_URL_PREFIX "https://mitsuhiko.github.io/tankgame/#join/"

typedef struct pz_net_offer {
    uint32_t version;
    char *host_name;
    char *map_name;
    char *sdp;
} pz_net_offer;

pz_net_offer *pz_net_offer_create(uint32_t version, const char *host_name,
    const char *map_name, const char *sdp);
void pz_net_offer_free(pz_net_offer *offer);

char *pz_net_offer_encode_json(const pz_net_offer *offer);
char *pz_net_offer_encode_url(const pz_net_offer *offer);

pz_net_offer *pz_net_offer_decode_json(const char *json);
pz_net_offer *pz_net_offer_decode_url(const char *url);

#endif // PZ_NET_H
