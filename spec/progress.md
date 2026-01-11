# Progress

## Phase 0: Project Setup
- [x] M0.1: repo structure, CMake, builds on macOS
- [x] M0.2: Sokol app/gfx integrated, window opens/closes
- [x] M0.3: OpenGL 3.3 context, cornflower blue clear
- [x] M0.4: test framework, smoke tests pass

## Phase 1: Core Utilities
- [x] M1.1: memory system with leak detection
- [x] M1.2: logging system with levels/categories
- [x] M1.3: math library (vec2, vec3, vec4, mat4)
- [x] M1.4: data structures (list, array, hashmap)
- [x] M1.5: string utilities
- [x] M1.6: platform layer (timer, file I/O, paths)

## Phase 2: Rendering Foundation
- [x] M2.1: renderer API, null backend, GL33 backend, shaders, test triangle renders
- [x] M2.2: VAO/VBO helpers, colored triangle, textured quad
- [x] M2.3: texture loading (stb_image), texture manager with caching, hot-reload
- [x] M2.4: camera system, perspective projection, screen-to-world, ground grid renders
- [x] M2.5: debug overlay (bitmap font, FPS counter, frame time graph, F2 toggle)

## Phase 3: Game World Foundation
- [x] M3.1: ground rendering using map (wood textures, terrain types visible)
- [x] M3.2: map data structure (pz_map with terrain, height, spawns, coordinate conversion)
- [x] M3.3: terrain mesh batched by type, different textures per terrain
- [x] M3.4: 3D wall geometry from height map with lighting
- [x] M3.5: map hot-reload

## Quality Improvements
- [x] Wider map (24x14) for better 16:9 fit
- [x] MSAA (4x multisampling) for anti-aliased edges
- [x] Mipmapping on terrain textures for cleaner distant tiles
- [x] Camera auto-fit to show entire map
- [x] Fixed map parser to correctly load terrain types

## Phase 4: First Entity - The Tank
- [x] M4.1: tank body mesh, turret mesh, entity shader, renders at origin
- [x] M4.2: entity system (specialized managers: pz_tank_manager, pz_projectile_manager, pz_ai_manager)
- [x] M4.3: tank entity structure (pz_tank with health, loadout, rendering params)
- [x] M4.4: input system (WASD + mouse aiming)
- [x] M4.5: simulation timestep + determinism harness (pz_sim with fixed timestep, RNG, state hashing)
- [x] M4.6: tank movement (WASD control, mouse turret aim)
- [x] M4.7: tank-wall collision (separate axis, sliding along walls)

## Phase 5: Physics and Collision
- [x] M5.1: collision shapes (circle, rect as reusable API)
- [x] M5.3: raycast (DDA algorithm with normals, used for projectiles and AI LOS)
- [x] M5.4: tank-tank collision (circle-circle with push-out resolution)

## Phase 6: Weapons and Projectiles
- [x] M6.1: projectile entity, mesh, movement, lifetime
- [x] M6.2: tank firing (left mouse button, cooldown, spawn at turret tip)
- [x] M6.3: projectile-wall collision with bouncing (1 bounce)
- [x] M6.4: projectile-tank collision (damage, flash)
- [x] M6.5: tank death and respawn (player respawns, enemies stay dead)

## Phase 7: Track Accumulation
- [x] M7.1: FBO accumulation texture, track rendering pipeline
- [x] M7.2: ground shader samples track texture using world coordinates
- [x] M7.3: track marks placed at distance intervals for each tread

## Phase 7B: Enemy AI and Campaign
- [x] M7B.1: enemy types and spawn data (Level 1/2/3 enemies)
- [x] M7B.2: Level 1 enemy AI (stationary, aim + fire at player)
- [x] M7B.2+: Level 2 enemy AI (uses cover, peeks to fire, retreats)
- [x] M7B.3: enemy death (no respawn)
- [x] M7B.4: campaign maps (13 maps in main.campaign)
- [x] M7B.5: win condition (all enemies defeated)
- [x] A* pathfinding system (pz_pathfinding.c)

## Phase 8: Audio Foundation
- [x] M8.1: sokol_audio + MIDI music layers
- [x] M8.2: sound effects system (WAV loading, mixing, looping)
- [x] M8.3: game SFX integration (engine sounds, gunfire, explosions)

## Phase 9: Mines and Pickups (partial)
- [x] M9.1: mine entity (pz_mine.h/c, mesh, rendering)
- [x] M9.2: mine triggering (proximity to tanks, arm delay)
- [x] M9.3: shooting mines (projectile-mine collision)
- [x] M9.4: pickup entity (pz_powerup with floating animation)
- [x] M9.5: pickup collection (tank-powerup collision, effect applied)
- [x] M9.6: weapon switching (machine gun, ricochet weapons with ammo)

## Phase 10: Destructibles
- [x] M10.1: destructible entity (pz_barrier system)
- [x] M10.2: destructible collision (blocks movement and projectiles)
- [x] M10.3: destroying destructibles (HP tracking, destruction)
- [x] M10.4: crate placing (pickup gives crate placer)

## Phase 11: Map Editor
- [x] M11.1: --edit-map flag (F1 toggle removed)
- [x] M11.2: tile painting (left click place/raise, right click lower)
- [x] M11.3: wall height editing (via tile painting)
- [ ] M11.4: entity placement
- [x] M11.5: map format + saving (text format exists, used by map_tool.py)
- [x] M11.6: auto-save on edit (Phase 1 of editor spec)
- [x] M11.7: grid overlay rendering
- [x] M11.8: shortcut bar UI (Phase 2)
- [x] M11.9: editor UI system (buttons, panels, slots)
- [x] M11.10: hover highlight / ghost preview (Phase 3)
- [x] M11.11: scroll wheel slot cycling (Phase 3)
- [x] M11.12: number keys 1-6 slot selection (Phase 3)
- [x] M11.13: Tab toggle slot 1/2 (Phase 3)
- [x] M11.14: mouse_screen debug script command
- [x] M11.15: tile picker with quick-assign (hover + 1-6 assigns to slot)
- [x] M11.16: tag picker dropdown (Phase 4)
- [x] M11.17: tags editor (Phase 7+)
- [x] M11.18: dynamic map expansion (click in padding area to expand)
- [x] M11.19: mouse_click debug script command
- [x] M11.20: click-to-rotate entities (click on tag to rotate, visual arrow indicator)

## Phase 12: Game Modes
- [ ] M12.1: mode framework
- [ ] M12.2: deathmatch mode
- [ ] M12.3: capture the flag mode
- [ ] M12.4: domination mode

## Phase 13: Networking Foundation
- [x] M13.1: libdatachannel + datachannel-wasm integration
- [x] M13.2: WebRTC wrapper
- [x] M13.3: offer URL encoding

## Phase 16: Polish and Game Feel (partial)
- [x] M16.2: particle system (smoke, fog, impact particles)
- [x] M16.3: hit feedback (damage flash, sound)
- [ ] M16.4: UI polish (main menu, etc.)

## Dynamic Lighting System
- [x] 2D light map with shadow casting from walls and tanks
- [x] Spotlight lights attached to tanks (follow turret direction)
- [x] Multiple colored lights with additive blending
- [x] Shadows cast by rectangular occluders (walls, tanks)
- [x] Light map applied to ground, wall sides, tanks, and projectiles
- [x] Wall tops remain unlit by dynamic lights (ambient only)

## UI/HUD System
- [x] SDF font rendering system (pz_font.h/c) using stb_truetype
- [x] Russo One and Caveat Brush fonts loaded
- [x] Player health display (bottom-right, color-coded by health %)
- [x] Level/lives/enemies HUD
- [x] State overlays (Level Complete, Game Over, Campaign Complete)

## Campaign System
- [x] Campaign file format (plain text, NAME/MAP/LIVES commands)
- [x] Campaign manager (load, start, advance, track lives)
- [x] Map session system (encapsulates all map-dependent state)
- [x] Level transitions (SPACE to advance, R to restart)
- [x] Lives system (lose life on death, game over when 0)
- [x] 13-level main campaign

---

## Remaining Work

### High Priority (gameplay gaps)
- [x] M9.1-9.3: mines (player can drop mines with right-click/G, max 2)

### Medium Priority (content/polish)
- [ ] M16.4: main menu, UI polish

### Lower Priority (extra features)
- [ ] Phase 11: map editor (map_tool.py exists for CLI editing)
- [ ] Phase 12: game modes (deathmatch, CTF, domination)

## Plan Changes
- Networking phases (13-15) resumed with WebRTC P2P plan
- Focus on single-player campaign (complete)
- Enemies defined in map files (type, position)
- 3 enemy types with different stats/behaviors

## Toxic Cloud
- [x] P1: core system and boundary tests
- [x] P2: map integration, map tool, docs, tests
- [x] P3: tank damage, slowdown, and session wiring
- [x] P4: AI toxic escape behavior
- [x] P5: toxic cloud rendering and tank tint
- [x] P6: toxic_dawn map and polish
