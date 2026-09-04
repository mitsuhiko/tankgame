# Tank Game

<p align="center">
  <img src="screenshot.png" width="70%" alt="Tank Game Screenshot">
</p>

A multiplayer tank combat game built in C17 with Sokol (sokol_app + sokol_gfx) with the help of Pi and Claude Opus.

Terrible and work in progress.

You can [play it online](https://mitsuhiko.github.io/tankgame/)

## Building

### Prerequisites
- CMake 3.16+
- C17 compiler (clang or gcc)
- Git (for submodules)

### macOS
```bash
brew install cmake
git submodule update --init --recursive
```

### Build
```bash
make build    # Build the project
make run      # Build and run
make clean    # Clean build directory
make debug    # Build with debug config
make release  # Build with release config
make web      # Build WASM build with docker
```

## Multiplayer

The host runs the authoritative simulation. Inputs and snapshots use an
unordered, no-retransmit WebRTC channel; gameplay events use a separate reliable
channel. The client predicts its local tank and reconciles against host
snapshots.

In the web build, choose a map and click **Host match**, then share the generated
8-character room code or join link. A second player can paste the code into the
**Room code** field.

Desktop builds use the same signaling flow:

```bash
./build/tankgame host --map assets/maps/night_arena.map
./build/tankgame join <room-code>
```

WebRTC still requires a working route between peers. Public STUN is configured;
networks that require TURN relaying are not currently supported.

## Tools

### Map Tool
The `tools/map_tool.py` script provides CLI commands and a Python API for map manipulation:
```bash
./tools/map_tool.py --help           # Full documentation and examples
./tools/map_tool.py info <map>       # Show map info
./tools/map_tool.py validate <map>   # Validate and re-serialize
```

## License

- License: [Apache-2.0](https://github.com/mitsuhiko/tankgame/blob/main/LICENSE)

This code is entirely LLM generated. It is unclear if LLM generated code
can be copyrighted.
