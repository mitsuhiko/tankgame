# Network Protocol and Implementation

## Architecture Overview

**Peer-to-Peer with Host Authority:**
- One player hosts (becomes authoritative server)
- Other players connect via WebRTC DataChannels
- Host validates all inputs, broadcasts state
- Works desktop↔desktop, desktop↔browser, browser↔browser

## Transport Layer: WebRTC DataChannels

We use **WebRTC DataChannels** for all networking:

| Feature | Benefit for Game |
|---------|------------------|
| **P2P connections** | No dedicated server needed |
| **NAT traversal** | ICE handles most NAT types automatically |
| **Unreliable mode** | Low-latency game state (like UDP) |
| **Reliable mode** | Chat, events, connection control |
| **Browser native** | Works in all browsers including Safari |
| **Encrypted** | DTLS, no separate security layer |

### Implementation

| Platform | Library |
|----------|---------|
| Desktop (C) | libdatachannel |
| Browser (WASM) | libdatachannel via datachannel-wasm |

**libdatachannel:** https://github.com/paullouisageneau/libdatachannel
- Clean C API
- Small footprint (~50KB)
- MIT license
- DataChannel-focused (no overkill video/audio code)
- Can compile to WebAssembly via datachannel-wasm for code reuse
- https://github.com/paullouisageneau/datachannel-wasm

## Connection Handshake: QR Code + URL

### The Problem

WebRTC requires exchanging SDP (Session Description Protocol) offers/answers to establish connections. Normally this needs a signaling server, but we use **out-of-band signaling** via QR codes and URLs.

### Join URL Format

```
https://mitsuhiko.github.io/tankgame/#join/<COMPRESSED_OFFER>
```

The URL fragment (`#join/...`) contains:
- Compressed SDP offer
- ICE candidates
- Game metadata (host name, map, etc.)

When opened:
- **Browser:** Loads WASM game, auto-joins with embedded offer
- **Desktop:** Could register `tankgame://` URL scheme, or user pastes into game

### Offer Payload

```
{
  "v": 1,                          // Protocol version
  "name": "Player1",               // Host name
  "map": "battlefield",            // Map name  
  "sdp": "<minified SDP>",         // SDP offer
  "ice": ["candidate:..."]         // ICE candidates
}
```

Compressed with zlib/deflate → Base64 URL-safe encoding → ~400-800 chars

### Handshake Flow

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              HOST GAME                                      │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  1. Player clicks "Host Game"                                               │
│  2. Create RTCPeerConnection with STUN servers                              │
│  3. Create DataChannels:                                                    │
│     - "game" (unreliable, unordered) - inputs & state                       │
│     - "reliable" (reliable, ordered) - chat, events, control                │
│  4. Create SDP offer                                                        │
│  5. Wait for ICE gathering complete                                         │
│  6. Compress offer + candidates into URL                                    │
│  7. Display QR code + copyable URL                                          │
│                                                                             │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │                                                                     │   │
│  │   ┌───────────────┐    Share this link to invite players:          │   │
│  │   │ ▄▄▄▄▄ ▄▄▄▄▄ │                                                  │   │
│  │   │ █ ▄▄▄ █   █ │    https://mitsuhiko.github.io/tankgame/        │   │
│  │   │ █ ███ █ █ █ │    #join/eJxNj8EKwjAQRP9lzl...                  │   │
│  │   │ █▄▄▄█ █▄█▄█ │                                                  │   │
│  │   │ ▄▄▄▄▄▄▄▄▄▄▄ │    [Copy Link]  [Copy for Desktop]              │   │
│  │   └───────────────┘                                                 │   │
│  │                                                                     │   │
│  │   Waiting for players...                  [Start Game]              │   │
│  │                                                                     │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────────┐
│                              JOIN GAME                                      │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  Browser: Click link → Game loads → Auto-connect                            │
│                                                                             │
│  Desktop: Paste link in "Join Game" screen                                  │
│                                                                             │
│  1. Parse offer from URL fragment                                           │
│  2. Create RTCPeerConnection with same STUN servers                         │
│  3. Set remote description (the offer)                                      │
│  4. Create SDP answer                                                       │
│  5. Wait for ICE gathering complete                                         │
│  6. Send answer payload back to host                                        │
│  6. Send answer back to host via DataChannel (once connected)               │
│     OR display answer for manual exchange (fallback)                        │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### The Answer Problem

WebRTC needs a **bidirectional** exchange - host sends offer, joiner sends answer. Options:

#### Option A: Trickle via DataChannel (Preferred)

Once ICE finds *any* working candidate pair, the DataChannel opens. We send the full answer through it:

```
1. Host creates offer with all ICE candidates
2. Joiner gets offer via URL
3. Joiner sets remote description
4. ICE starts, finds candidate pair, DataChannel opens
5. Joiner sends answer via DataChannel
6. Host sets remote description
7. Connection fully established
```

This works because ICE can establish connectivity before the answer is formally set, as long as the offer is processed.

#### Option B: Two-Way QR/URL (Fallback)

If Option A fails (strict NAT), fall back to manual answer exchange:

```
Host shows:  QR/URL with offer
Joiner shows: QR/short-code with answer
Host enters:  Joiner's code

Joiner's answer can be shorter (~200 bytes) since it doesn't need ICE candidates 
if we use ice-lite or aggressive nomination.
```

#### Option C: Tiny Relay (Last Resort)

For symmetric NAT where P2P is impossible:
- Lightweight WebSocket relay (few KB/s per connection)
- Could be a free Cloudflare Worker or similar
- Only used when direct P2P fails

### ICE Servers

```c
// Public STUN servers (free, no account needed)
const char* ice_servers[] = {
    "stun:stun.l.google.com:19302",
    "stun:stun1.l.google.com:19302",
    "stun:stun.cloudflare.com:3478",
    NULL
};
```

TURN servers (relay) could be added for strict NAT, but most home networks work with STUN only.

## DataChannel Design

Two channels with different reliability modes:

### "game" Channel (Unreliable, Unordered)

For real-time game data where latest state matters more than every packet:

```c
rtc_data_channel_init config = {
    .reliability = {
        .unordered = true,
        .unreliable = true,
        .maxPacketLifeTime = 100,  // ms, drop after this
    }
};
```

Messages:
- Client inputs (60 Hz)
- Game state deltas (20-30 Hz)
- Entity positions

### "reliable" Channel (Reliable, Ordered)

For important events that must arrive:

```c
rtc_data_channel_init config = {
    .reliability = {
        .unordered = false,
        .unreliable = false,
    }
};
```

Messages:
- Connection handshake
- Player join/leave
- Chat messages
- Kill events, flag captures, round start/end
- Full game state (on join)

## Message Protocol

Binary protocol, little-endian. All messages have a 1-byte type header.

### Message Types

```c
typedef enum pz_msg_type {
    // Control messages (reliable channel)
    PZ_MSG_JOIN_REQUEST = 1,     // Joiner → Host
    PZ_MSG_JOIN_ACCEPT,          // Host → Joiner
    PZ_MSG_JOIN_DENY,            // Host → Joiner
    PZ_MSG_PLAYER_JOINED,        // Host → All (broadcast)
    PZ_MSG_PLAYER_LEFT,          // Host → All
    PZ_MSG_FULL_STATE,           // Host → Joiner (on join)
    PZ_MSG_WEBRTC_ANSWER,        // Joiner → Host (SDP answer)
    
    // Game messages (unreliable channel)
    PZ_MSG_CLIENT_INPUT = 32,    // Player → Host
    PZ_MSG_GAME_DELTA,           // Host → All
    
    // Chat (reliable channel)
    PZ_MSG_CHAT = 64,            // Any → Host → All
    
    // Events (reliable channel)
    PZ_MSG_EVENT_KILL = 96,
    PZ_MSG_EVENT_FLAG_TAKEN,
    PZ_MSG_EVENT_FLAG_CAPTURED,
    PZ_MSG_EVENT_FLAG_RETURNED,
    PZ_MSG_EVENT_ROUND_START,
    PZ_MSG_EVENT_ROUND_END,
} pz_msg_type;
```

### Key Messages

**Join Request (Joiner → Host, reliable):**
```c
typedef struct pz_msg_join_request {
    uint8_t  type;              // PZ_MSG_JOIN_REQUEST
    uint8_t  protocol_version;
    uint8_t  name_len;
    char     name[32];          // Player name
} pz_msg_join_request;
```

**Join Accept (Host → Joiner, reliable):**
```c
typedef struct pz_msg_join_accept {
    uint8_t  type;              // PZ_MSG_JOIN_ACCEPT
    uint8_t  player_id;         // Assigned player ID (1-255, 0 = host)
    uint8_t  team;
    uint32_t entity_id;         // Your tank's entity ID
    uint32_t host_tick;
    // Followed by full game state
} pz_msg_join_accept;
```

**Client Input (Player → Host, unreliable):**
```c
typedef struct pz_msg_client_input {
    uint8_t  type;              // PZ_MSG_CLIENT_INPUT
    uint32_t input_sequence;
    uint32_t last_host_tick;    // Last tick player processed
    uint8_t  buttons;           // Bitfield
    uint16_t turret_angle;      // 0-65535 = 0-360°
} pz_msg_client_input;

// Button bitfield
enum {
    PZ_INPUT_FORWARD  = 1 << 0,
    PZ_INPUT_BACKWARD = 1 << 1,
    PZ_INPUT_LEFT     = 1 << 2,
    PZ_INPUT_RIGHT    = 1 << 3,
    PZ_INPUT_FIRE     = 1 << 4,
    PZ_INPUT_MINE     = 1 << 5,
    PZ_INPUT_ITEM     = 1 << 6,
};
```

**Game Delta (Host → All, unreliable):**
```c
typedef struct pz_msg_game_delta {
    uint8_t  type;              // PZ_MSG_GAME_DELTA
    uint32_t host_tick;
    uint8_t  player_count;
    // Per-player: last_input_seq (for their reconciliation)
    // Followed by entity states
} pz_msg_game_delta;

typedef struct pz_tank_state {
    uint32_t entity_id;
    int16_t  x, y;              // Fixed-point (8.8)
    uint16_t angle;             // 0-65535 = 0-360°
    uint16_t turret_angle;
    uint8_t  hp;
    uint8_t  weapon;
    uint8_t  ammo;
    uint8_t  flags;
} pz_tank_state;
```

## Network Wrapper API

Unified C API that works with both libdatachannel (desktop) and browser WebRTC:

```c
// pz_net.h - Platform-agnostic networking API

typedef struct pz_net_config {
    const char** ice_servers;    // NULL-terminated array
    bool         is_host;
    const char*  player_name;
    
    // Callbacks
    void (*on_connected)(int player_id, const char* name, void* userdata);
    void (*on_disconnected)(int player_id, void* userdata);
    void (*on_game_message)(int player_id, const uint8_t* data, size_t len, void* userdata);
    void (*on_reliable_message)(int player_id, const uint8_t* data, size_t len, void* userdata);
    void* userdata;
} pz_net_config;

typedef struct pz_net pz_net;

// Lifecycle
pz_net*     pz_net_create(const pz_net_config* config);
void        pz_net_destroy(pz_net* net);
void        pz_net_update(pz_net* net);  // Call each frame

// Hosting
char*       pz_net_create_offer(pz_net* net);           // Returns join URL (caller frees)
void        pz_net_set_answer(pz_net* net, const char* answer);  // If manual exchange needed

// Joining
bool        pz_net_join(pz_net* net, const char* offer_url);

// Sending (host broadcasts, joiners send to host)
void        pz_net_send_game(pz_net* net, const uint8_t* data, size_t len);      // Unreliable
void        pz_net_send_reliable(pz_net* net, const uint8_t* data, size_t len);  // Reliable
void        pz_net_send_to(pz_net* net, int player_id, const uint8_t* data, size_t len, bool reliable);

// State
bool        pz_net_is_connected(pz_net* net);
int         pz_net_player_count(pz_net* net);
float       pz_net_get_rtt(pz_net* net, int player_id);  // Round-trip time in ms
```

### Platform Implementations

```c
// Desktop: src/net/pz_net_datachannel.c
// Uses libdatachannel C API

// Browser: src/net/pz_net_browser.c  
// Uses Emscripten JS interop with RTCPeerConnection
```

Browser implementation calls into JavaScript:

```javascript
// web/pz_net.js
class PZNet {
    constructor() {
        this.pc = null;
        this.gameChannel = null;
        this.reliableChannel = null;
    }
    
    async createOffer(playerName) {
        this.pc = new RTCPeerConnection({
            iceServers: [
                { urls: 'stun:stun.l.google.com:19302' },
                { urls: 'stun:stun.cloudflare.com:3478' }
            ]
        });
        
        // Create channels
        this.gameChannel = this.pc.createDataChannel('game', {
            ordered: false,
            maxRetransmits: 0
        });
        this.reliableChannel = this.pc.createDataChannel('reliable', {
            ordered: true
        });
        
        // Create offer and gather ICE
        const offer = await this.pc.createOffer();
        await this.pc.setLocalDescription(offer);
        
        // Wait for ICE gathering
        await this.waitForIceGathering();
        
        // Compress and encode
        const payload = {
            v: 1,
            name: playerName,
            sdp: this.minifySDP(this.pc.localDescription.sdp)
        };
        
        return 'https://mitsuhiko.github.io/tankgame/#join/' + 
               this.compressAndEncode(payload);
    }
    
    async joinWithOffer(offerUrl) {
        const payload = this.decodeAndDecompress(
            offerUrl.split('#join/')[1]
        );
        
        this.pc = new RTCPeerConnection({
            iceServers: [
                { urls: 'stun:stun.l.google.com:19302' },
                { urls: 'stun:stun.cloudflare.com:3478' }
            ]
        });
        
        this.pc.ondatachannel = (event) => {
            if (event.channel.label === 'game') {
                this.gameChannel = event.channel;
                this.setupGameChannel();
            } else if (event.channel.label === 'reliable') {
                this.reliableChannel = event.channel;
                this.setupReliableChannel();
            }
        };
        
        await this.pc.setRemoteDescription({
            type: 'offer',
            sdp: this.expandSDP(payload.sdp)
        });
        
        const answer = await this.pc.createAnswer();
        await this.pc.setLocalDescription(answer);
        
        // Send answer via reliable channel once connected
        this.reliableChannel.onopen = () => {
            this.sendAnswer(answer);
        };
    }
}
```

## Tick Rate and Timing

- **Host tick rate:** 60 Hz (16.67ms)
- **Input send rate:** 60 Hz
- **State broadcast rate:** 20-30 Hz (every 2-3 ticks)

### Time Synchronization

WebRTC gives us RTT via `pc.getStats()`. Use this to:
1. Estimate host time on clients
2. Set interpolation delay
3. Adjust input timing for prediction

## Client-Side Prediction

Same as before - predict local tank movement, reconcile when host state arrives:

```c
typedef struct pz_prediction {
    pz_msg_client_input input_history[64];
    pz_tank_state       state_history[64];
    int                 head;
    uint32_t            last_confirmed_input;
} pz_prediction;

void pz_prediction_record(pz_prediction* pred, 
    pz_msg_client_input* input, pz_tank_state* predicted);
    
void pz_prediction_reconcile(pz_prediction* pred,
    pz_tank_state* host_state, uint32_t host_input_seq);
```

## Entity Interpolation

For other players (render slightly in the past for smooth movement):

```c
typedef struct pz_interp_buffer {
    struct { uint32_t tick; pz_tank_state state; } snapshots[32];
    int count;
} pz_interp_buffer;

pz_tank_state pz_interp_get(pz_interp_buffer* buf, uint32_t render_tick);
```

## Host Migration (Future)

If the host disconnects, another player could take over:
1. Highest player_id becomes new host
2. New host broadcasts `PZ_MSG_HOST_MIGRATE`
3. All clients reconnect to new host (new WebRTC handshake)
4. Game state preserved via last known state

This is complex and can be deferred.

## Build Configuration

### Desktop (CMake)

```cmake
# Fetch libdatachannel
include(FetchContent)

FetchContent_Declare(
    libdatachannel
    GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
    GIT_TAG v0.20.0
)

set(NO_MEDIA ON CACHE BOOL "" FORCE)  # We only need DataChannels
set(NO_WEBSOCKET ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(libdatachannel)

target_link_libraries(tankgame PRIVATE datachannel)
```

### Browser (Emscripten)

No extra libraries needed - RTCPeerConnection is built into browsers.

```cmake
if(EMSCRIPTEN)
    target_link_options(tankgame PRIVATE
        --js-library ${CMAKE_SOURCE_DIR}/web/pz_net.js
    )
endif()
```

## Security Considerations

WebRTC provides:
- **DTLS encryption:** All DataChannel traffic encrypted
- **SRTP:** If we ever add voice (not planned)
- **Origin isolation:** Browser security model applies

Game-level:
- Host validates all inputs
- Host is authoritative for game state
- Rate limiting on inputs
- Sequence numbers prevent replay within session

## QR Code Generation

For desktop, use a simple QR library. Options:
- **qrcodegen** (single header, C): https://github.com/nayuki/QR-Code-generator
- Render as pixels on a texture, display in UI

```c
#include "qrcodegen.h"

// Generate QR code for join URL
uint8_t qr[qrcodegen_BUFFER_LEN_MAX];
uint8_t temp[qrcodegen_BUFFER_LEN_MAX];

bool ok = qrcodegen_encodeText(join_url, temp, qr,
    qrcodegen_Ecc_LOW, qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
    qrcodegen_Mask_AUTO, true);

int size = qrcodegen_getSize(qr);
for (int y = 0; y < size; y++) {
    for (int x = 0; x < size; x++) {
        bool black = qrcodegen_getModule(qr, x, y);
        // Draw pixel
    }
}
```

## Summary: Connection Methods

| Method | Steps | Use Case |
|--------|-------|----------|
| **QR Scan** | Host shows QR, joiner scans with phone camera | Couch co-op, LAN party |
| **URL Share** | Host copies link, sends via Discord/etc | Online friends |
| **URL Direct** | Click link, browser opens game | Streamlined join |
| **Paste in Desktop** | Copy URL, paste in desktop game | Desktop-to-desktop |
