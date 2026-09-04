# Networking Protocol

## Status and Scope

The current multiplayer implementation supports one host and one joining player
on desktop and WebAssembly builds. The host is authoritative: it runs gameplay,
accepts client input, and broadcasts state. The client predicts only its local
tank and renders all authoritative objects from host snapshots.

WebRTC encrypts peer traffic with DTLS. Public STUN servers are configured for
NAT traversal. TURN relaying, host migration, spectators, and matches with more
than two peers are not implemented.

## Connection Flow

### Room Signaling

Players exchange SDP through a transient ntfy.sh signaling mailbox:

1. The host creates both DataChannels and gathers an SDP offer.
2. The host generates a human-friendly 8-character room code from 40 bits of
   OS/browser cryptographic randomness.
3. The host publishes the offer to the room's `o` topic.
4. The joiner fetches the offer, validates its protocol version, creates an SDP
   answer, and publishes it to the room's `a` topic.
5. The host fetches and applies the answer.
6. WebRTC establishes encrypted peer-to-peer DataChannels.

A join URL contains only the room code:

```
https://mitsuhiko.github.io/tankgame/#join/<ROOM_CODE>
```

A DataChannel cannot carry its own SDP answer because it does not open until the
host applies that answer. The signaling mailbox is therefore required before
the peer-to-peer connection exists. SDP does not contain game-state traffic.

Native signaling work runs on a worker thread with bounded curl timeouts;
callbacks are delivered on the main thread. Browser signaling uses Emscripten's
asynchronous Fetch API. Polls are paced rather than issued every frame.

Full offer/answer payloads and debug-script file exchange remain available for
local deterministic testing.

### Offer Metadata

Offer and answer envelopes contain:

```json
{
  "v": 2,
  "name": "host",
  "map": "assets/maps/night_arena.map",
  "sdp": "v=0..."
}
```

Peers reject envelopes whose protocol version does not equal
`PZ_NET_PROTOCOL_VERSION`.

## DataChannels

A connection has two channels:

| Channel | Delivery | Traffic |
|---------|----------|---------|
| `game` | unordered, `maxRetransmits = 0` | client input and host snapshots |
| `reliable` | ordered, reliable | audiovisual/gameplay events |

Both channels must be open before the game reports the peer as connected.
Sends are rejected if a channel is closed or its buffered-byte limit is
exceeded, preventing unbounded backpressure.

## Wire Protocol v2

The current wire format is defined in `src/net/pz_net_protocol.h`. Messages use
fixed-width, packed fields. Desktop targets and WebAssembly are little-endian;
a future cross-endian port must add explicit byte-order conversion.

Every message begins with:

```c
struct pz_net_msg_header {
    uint8_t  type;
    uint8_t  version;
    uint16_t size;
    uint32_t tick;     // input sequence for INPUT; simulation tick otherwise
};
```

Parsers require the exact protocol version and message size. They reject excess
entity counts, malformed enums/booleans, non-finite floats, and truncated or
oversized packets before touching simulation state.

### Client Input (`PZ_NET_MSG_INPUT`)

Sent once per client simulation tick on `game`:

- Input sequence and most recently observed host tick
- Action sequence for acknowledged one-shot actions
- Normalized movement direction
- World-space cursor target
- Fire-held and fire-pressed state
- Mine/barrier placement requests
- Weapon switch direction

Input sequence acknowledgement drives movement prediction replay. A separate
action sequence lets the client repeat unacknowledged one-shot actions across
an unordered/unreliable channel without the host firing or placing an object
more than once.

The host accepts only newer sequences, clamps movement and action fields, and
uses the latest input for the remote player. The host never accepts client
position, health, ammo, or entity-spawn decisions.

### Host Snapshot (`PZ_NET_MSG_GAME_STATE`)

Sent at approximately 20 Hz on `game`:

- Host tick and acknowledged input/action sequences
- Local and remote tank IDs
- Tank transform, velocity, turret, health, flags, floor/jump state, cooldown,
  mines, loadout, ammo, and weapon index
- Active projectiles, including motion, damage, bounce, owner, and floor state
- Powerup active state and position
- Mine position, trigger state, timer, owner, and floor state
- Barrier position, dimensions, health, tile identity, and floor state

The fixed upper bounds in the protocol mirror entity-manager capacities.
Snapshots are self-contained, so losing an earlier snapshot does not prevent a
later one from restoring the current world.

### Reliable Event (`PZ_NET_MSG_EVENT`)

Sent on `reliable` for effects that should not vanish with a dropped snapshot:

- Tank death and respawn
- Projectile hit
- Mine explosion
- Powerup collection
- Gunfire
- Jump
- Barrier placement

Events include a sequence, host tick, world position, source entity, floor, and
small event-specific values. The client uses them for sounds, particles,
lights, and short-lived feedback; authoritative entity state still comes from
snapshots.

## Simulation and Presentation

### Host

- Runs the fixed 60 Hz simulation for both player tanks.
- Applies the latest validated remote input.
- Creates projectiles, mines, barriers, pickups, damage, deaths, and respawns.
- Sends periodic self-contained snapshots and reliable effect events.

### Client

- Sends local input every fixed tick.
- Immediately simulates local tank movement.
- On a snapshot, restores the acknowledged authoritative local state, discards
  confirmed input history, and replays only unacknowledged movement inputs.
- Interpolates remote tank transforms between snapshots.
- Extrapolates projectiles over the short interval between snapshots.
- Does not run authoritative collision, damage, pickup, or spawning systems.

Both peers create player tanks in the same order, preserving entity IDs. The
snapshot also carries explicit local/remote IDs rather than relying solely on
spawn assumptions.

## Threading and Queues

Native libdatachannel callbacks may run off the game thread. They only parse
bounded protocol data and push it into single-producer/single-consumer input,
snapshot, or event queues. The game thread drains those queues before updating
the simulation. Channel-open state is atomic, and debug-script notifications
are delivered from the main frame loop.

This prevents network callbacks from mutating tank, projectile, renderer, audio,
or script state concurrently.

## User Flows

### Web

1. Select a map and click **Host match**.
2. Share the displayed room code or join link.
3. The joining player pastes the code and clicks **Join**.
4. Both pages show signaling/connection status and enter the match when both
   DataChannels open.

### Desktop

```bash
./build/tankgame host --map assets/maps/night_arena.map
./build/tankgame join <room-code>
```

### Local Integration Test

Debug scripts can exchange offer and answer files, wait for connection, inject
movement/fire, capture screenshots, and dump state. This avoids depending on
the public mailbox for deterministic host/client regression tests.

## Known Limitations

- No TURN server: some symmetric or restrictive NAT combinations cannot connect.
- Two players only.
- No host migration or reconnect/resume.
- No matchmaking, identity, authentication, or room passwords.
- Room signaling uses a public transient service; room codes are unguessable in
  normal use but are capabilities and should be shared only with the intended
  peer.
