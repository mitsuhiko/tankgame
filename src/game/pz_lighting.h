/*
 * Tank Game - Dynamic Lighting System
 *
 * 2D shadow-casting lights rendered to a light map texture,
 * then sampled by 3D geometry for dramatic lighting effects.
 *
 * Features:
 *   - Multiple colored lights with individual properties
 *   - Shadow casting from walls and tanks (rotated rectangles)
 *   - Spotlight (cone) and point light support
 *   - Additive light blending
 *
 * Usage:
 *   1. Create with pz_lighting_create()
 *   2. Add occluders (walls, tanks) with pz_lighting_add_occluder()
 *   3. Add lights with pz_lighting_add_light()
 *   4. Call pz_lighting_render() to build the light map
 *   5. Sample the light map texture in your shaders
 */

#ifndef PZ_LIGHTING_H
#define PZ_LIGHTING_H

#include "../core/pz_math.h"
#include "../engine/render/pz_renderer.h"

// Maximum number of lights
#define PZ_MAX_LIGHTS 32

// Maximum number of occluders (walls + tanks)
#define PZ_MAX_OCCLUDERS 512

// Maximum edges per occluder (4 for rectangles)
#define PZ_MAX_EDGES_PER_OCCLUDER 4

// Maximum shadow geometry vertices per light
#define PZ_MAX_SHADOW_VERTICES 4096

// Forward declarations
typedef struct pz_lighting pz_lighting;
typedef struct pz_map pz_map;

// Light type
typedef enum pz_light_type {
    PZ_LIGHT_POINT = 0, // Omnidirectional light
    PZ_LIGHT_SPOTLIGHT, // Directional cone light
} pz_light_type;

// Light definition
typedef struct pz_light {
    bool active;
    pz_light_type type;

    pz_vec2 position; // World position
    float direction; // Direction angle (radians, for spotlight)

    pz_vec3 color; // RGB color (0-1)
    float intensity; // Light intensity multiplier
    float radius; // Maximum range

    // Spotlight-specific
    float cone_angle; // Half-angle of the cone (radians)
    float cone_softness; // Edge softness (0 = hard, 1 = very soft)

    // Floor level - light only blocked by occluders at same or higher floor
    int8_t floor_level;
} pz_light;

// Occluder (shadow caster) - axis-aligned or rotated rectangle
typedef struct pz_occluder {
    pz_vec2 position; // Center position
    pz_vec2 half_size; // Half-width and half-height
    float angle; // Rotation angle (radians)
    int8_t floor_level; // Floor level - blocks lights at same or lower floor
} pz_occluder;

// Lighting system configuration
typedef struct pz_lighting_config {
    float world_width; // World dimensions for UV mapping
    float world_height;
    int texture_size; // Light map resolution (e.g., 512, 1024)
    pz_vec3 ambient; // Ambient light color
} pz_lighting_config;

// Create the lighting system
pz_lighting *pz_lighting_create(
    pz_renderer *renderer, const pz_lighting_config *config);

// Destroy the lighting system
void pz_lighting_destroy(pz_lighting *lighting);

// ============================================================================
// Occluder Management
// ============================================================================

// Clear all occluders (call before adding new ones each frame)
void pz_lighting_clear_occluders(pz_lighting *lighting);

// Set map occluders and cache them for reuse across frames
void pz_lighting_set_map_occluders(pz_lighting *lighting, const pz_map *map);

// Clear only dynamic occluders (keeps cached map occluders)
void pz_lighting_clear_dynamic_occluders(pz_lighting *lighting);

// Add a rectangular occluder (wall or tank)
// floor_level: occluder blocks lights at same or lower floor level
void pz_lighting_add_occluder(pz_lighting *lighting, pz_vec2 position,
    pz_vec2 half_size, float angle, int8_t floor_level);

// Add all wall occluders from a map
void pz_lighting_add_map_occluders(pz_lighting *lighting, const pz_map *map);

// ============================================================================
// Light Management
// ============================================================================

// Clear all lights
void pz_lighting_clear_lights(pz_lighting *lighting);

// Add a point light (returns light index, or -1 if full)
// floor_level: light is only blocked by occluders at same or higher floor
int pz_lighting_add_point_light(pz_lighting *lighting, pz_vec2 position,
    pz_vec3 color, float intensity, float radius, int8_t floor_level);

// Add a spotlight (returns light index, or -1 if full)
// floor_level: light is only blocked by occluders at same or higher floor
int pz_lighting_add_spotlight(pz_lighting *lighting, pz_vec2 position,
    float direction, pz_vec3 color, float intensity, float radius,
    float cone_angle, float cone_softness, int8_t floor_level);

// Get light by index for modification
pz_light *pz_lighting_get_light(pz_lighting *lighting, int index);

// ============================================================================
// Rendering
// ============================================================================

// Render the light map texture
// Call this once per frame before rendering geometry
void pz_lighting_render(pz_lighting *lighting);

// Get the light map texture for sampling in shaders
pz_texture_handle pz_lighting_get_texture(pz_lighting *lighting);

// Get world-to-UV transformation (same as tracks system)
void pz_lighting_get_uv_transform(pz_lighting *lighting, float *out_scale_x,
    float *out_scale_z, float *out_offset_x, float *out_offset_z);

// Get ambient light color
pz_vec3 pz_lighting_get_ambient(pz_lighting *lighting);

// Set ambient light color
void pz_lighting_set_ambient(pz_lighting *lighting, pz_vec3 ambient);

// Save the lightmap to a PNG file (for debugging)
bool pz_lighting_save_debug(pz_lighting *lighting, const char *path);

// ========================================================================
// Debug Stats
// ========================================================================

int pz_lighting_get_light_count(const pz_lighting *lighting);
int pz_lighting_get_occluder_count(const pz_lighting *lighting);
int pz_lighting_get_edge_count(const pz_lighting *lighting);

#endif // PZ_LIGHTING_H
