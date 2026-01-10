/*
 * Tank Game - Networking Offer URL Tests
 */

#include "test_framework.h"

#include "../src/core/pz_mem.h"
#include "../src/net/pz_net.h"

TEST(net_offer_round_trip)
{
    pz_mem_init();

    const char *sdp = "v=0\n"
                      "o=- 123 1 IN IP4 0.0.0.0\n"
                      "s=-\n"
                      "t=0 0\n"
                      "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\n";

    pz_net_offer *offer = pz_net_offer_create(1, "Host", "arena", sdp);
    ASSERT_NOT_NULL(offer);

    char *url = pz_net_offer_encode_url(offer);
    ASSERT_NOT_NULL(url);

    pz_net_offer *decoded = pz_net_offer_decode_url(url);
    ASSERT_NOT_NULL(decoded);

    ASSERT_EQ(1, decoded->version);
    ASSERT_STR_EQ("Host", decoded->host_name);
    ASSERT_STR_EQ("arena", decoded->map_name);
    ASSERT_STR_EQ(sdp, decoded->sdp);

    pz_net_offer_free(decoded);
    pz_net_offer_free(offer);
    pz_free(url);

    ASSERT(!pz_mem_has_leaks());
    pz_mem_shutdown();
}

TEST(net_offer_invalid_url)
{
    pz_mem_init();

    ASSERT_NULL(pz_net_offer_decode_url(NULL));
    ASSERT_NULL(pz_net_offer_decode_url(""));
    ASSERT_NULL(pz_net_offer_decode_url("not-a-valid-token"));

    ASSERT(!pz_mem_has_leaks());
    pz_mem_shutdown();
}

TEST_MAIN()
