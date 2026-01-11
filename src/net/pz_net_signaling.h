/*
 * Tank Game - WebRTC Signaling (ntfy.sh)
 */

#ifndef PZ_NET_SIGNALING_H
#define PZ_NET_SIGNALING_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Generate a random room code (6 hex chars)
// Returns pointer to static buffer
const char *pz_signaling_generate_room(void);

// Callbacks
typedef void (*pz_signaling_publish_cb)(bool success, void *user_data);
typedef void (*pz_signaling_fetch_cb)(const char *message, void *user_data);

// Publish message to room (async)
// suffix: "o" for offer, "a" for answer
void pz_signaling_publish(const char *room, const char *suffix,
    const char *message, pz_signaling_publish_cb callback, void *user_data);

// Fetch message from room (async, single poll)
void pz_signaling_fetch(const char *room, const char *suffix,
    pz_signaling_fetch_cb callback, void *user_data);

// Call each frame to process async operations
void pz_signaling_update(void);

// Cleanup
void pz_signaling_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif // PZ_NET_SIGNALING_H
