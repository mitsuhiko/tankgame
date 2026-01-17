/*
 * Tank Game - Dynamic Lighting System Implementation
 *
 * Uses 2D shadow casting with visibility polygons to create dramatic lighting.
 * Each light renders to the light map with shadows from occluders.
 */

#include "pz_lighting.h"
#include "pz_map.h"

#include <math.h>
#include <string.h>

#include "../core/pz_log.h"
#include "../core/pz_mem.h"
#include "../core/pz_sort.h"

// Number of rays per light for shadow polygon (more = smoother shadows)
#define SHADOW_RAY_COUNT 256

// Small epsilon for ray casting
#define RAY_EPSILON 0.0001f

// Minimum ray distance (avoid issues with light inside/near occluders)
// Set high enough to skip internal edges of adjacent wall tiles
#define MIN_RAY_DISTANCE 0.5f

// Light geometry vertex: position (2) + color (3) + intensity (1) = 6 floats
#define LIGHT_VERTEX_FLOATS 6
#define LIGHT_VERTEX_SIZE (LIGHT_VERTEX_FLOATS * sizeof(float))

// Maximum vertices for light geometry (fan from center)
#define MAX_LIGHT_VERTICES PZ_MAX_SHADOW_VERTICES

// Edge structure for shadow casting
typedef struct edge {
    pz_vec2 a, b; // Edge endpoints
    int8_t floor_level; // Floor level of the occluder this edge belongs to
} edge;

struct pz_lighting {
    pz_renderer *renderer;

    // World dimensions
    float world_width;
    float world_height;
    int texture_size;

    // Ambient light
    pz_vec3 ambient;

    // Render target for light map
    pz_render_target_handle render_target;
    pz_texture_handle light_texture;

    // Shader and pipeline for rendering lights
    pz_shader_handle light_shader;
    pz_pipeline_handle light_pipeline;

    // Dynamic vertex buffer for light geometry
    pz_buffer_handle vertex_buffer;

    // Occluders (rebuilt each frame)
    pz_occluder occluders[PZ_MAX_OCCLUDERS];
    int occluder_count;
    const pz_map *map;
    int base_occluder_count;

    // Edges extracted from occluders
    edge edges[PZ_MAX_OCCLUDERS * PZ_MAX_EDGES_PER_OCCLUDER];
    int edge_count;
    int base_edge_count;

    // Lights
    pz_light lights[PZ_MAX_LIGHTS];
    int light_count;
};

// ============================================================================
// Helper Functions
// ============================================================================

// Get the four corners of a rotated rectangle
static void
get_rect_corners(
    pz_vec2 center, pz_vec2 half_size, float angle, pz_vec2 out_corners[4])
{
    float c = cosf(angle);
    float s = sinf(angle);

    // Local corners (before rotation)
    pz_vec2 local[4] = {
        { -half_size.x, -half_size.y },
        { half_size.x, -half_size.y },
        { half_size.x, half_size.y },
        { -half_size.x, half_size.y },
    };

    // Rotate and translate
    for (int i = 0; i < 4; i++) {
        float rx = local[i].x * c - local[i].y * s;
        float ry = local[i].x * s + local[i].y * c;
        out_corners[i].x = center.x + rx;
        out_corners[i].y = center.y + ry;
    }
}

// Add edges from a rectangle occluder
static void
add_occluder_edges(pz_lighting *lighting, const pz_occluder *occ)
{
    if (lighting->edge_count + 4
        > PZ_MAX_OCCLUDERS * PZ_MAX_EDGES_PER_OCCLUDER) {
        return;
    }

    pz_vec2 corners[4];
    get_rect_corners(occ->position, occ->half_size, occ->angle, corners);

    // Add four edges (CCW winding)
    for (int i = 0; i < 4; i++) {
        edge *e = &lighting->edges[lighting->edge_count++];
        e->a = corners[i];
        e->b = corners[(i + 1) % 4];
        e->floor_level = occ->floor_level;
    }
}

// Ray-segment intersection
// Returns parametric t along ray (0 to 1 means intersection within max_dist)
static bool
ray_segment_intersect(pz_vec2 ray_origin, pz_vec2 ray_dir, float max_dist,
    pz_vec2 seg_a, pz_vec2 seg_b, float *out_t)
{
    pz_vec2 v1 = { ray_origin.x - seg_a.x, ray_origin.y - seg_a.y };
    pz_vec2 v2 = { seg_b.x - seg_a.x, seg_b.y - seg_a.y };
    pz_vec2 v3 = { -ray_dir.y, ray_dir.x };

    float dot = v2.x * v3.x + v2.y * v3.y;
    if (fabsf(dot) < RAY_EPSILON) {
        return false; // Parallel
    }

    float t1 = (v2.x * v1.y - v2.y * v1.x) / dot;
    float t2 = (v1.x * v3.x + v1.y * v3.y) / dot;

    // Check intersection with minimum distance to avoid self-intersection
    // Use small epsilon for t2 to handle floating point issues at segment
    // endpoints
    const float t2_epsilon = 0.001f;
    if (t1 >= MIN_RAY_DISTANCE && t1 <= max_dist && t2 >= -t2_epsilon
        && t2 <= 1.0f + t2_epsilon) {
        *out_t = t1;
        return true;
    }

    return false;
}

// Cast a ray and find the nearest intersection with all edges
static float
cast_ray(const edge *edges, int edge_count, pz_vec2 origin, pz_vec2 direction,
    float max_dist)
{
    float nearest_t = max_dist;

    // NOTE: We skip pz_map_raycast and only use edge-based intersections.
    // The edges already cover all wall tiles (added via
    // pz_lighting_add_map_occluders). Using both caused issues with
    // inconsistent minimum distance handling.

    for (int i = 0; i < edge_count; i++) {
        float t;
        if (ray_segment_intersect(
                origin, direction, max_dist, edges[i].a, edges[i].b, &t)) {
            if (t < nearest_t) {
                nearest_t = t;
            }
        }
    }

    return nearest_t;
}

static float
point_segment_distance_sq(pz_vec2 p, pz_vec2 a, pz_vec2 b)
{
    pz_vec2 ab = { b.x - a.x, b.y - a.y };
    float denom = ab.x * ab.x + ab.y * ab.y;
    if (denom <= 0.0f) {
        float dx = p.x - a.x;
        float dy = p.y - a.y;
        return dx * dx + dy * dy;
    }
    float t = ((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / denom;
    if (t < 0.0f) {
        t = 0.0f;
    } else if (t > 1.0f) {
        t = 1.0f;
    }
    float cx = a.x + ab.x * t;
    float cy = a.y + ab.y * t;
    float dx = p.x - cx;
    float dy = p.y - cy;
    return dx * dx + dy * dy;
}

static int
compare_floats(const void *a, const void *b)
{
    float fa = *(const float *)a;
    float fb = *(const float *)b;
    return (fa > fb) - (fa < fb);
}

// Calculate spotlight intensity at a given angle
__attribute__((unused)) static float
spotlight_intensity(float angle_to_point, float light_dir, float cone_angle,
    float cone_softness)
{
    // Angle difference
    float diff = angle_to_point - light_dir;

    // Normalize to [-PI, PI]
    while (diff > PZ_PI)
        diff -= 2.0f * PZ_PI;
    while (diff < -PZ_PI)
        diff += 2.0f * PZ_PI;

    float abs_diff = fabsf(diff);

    if (abs_diff <= cone_angle) {
        // Inside cone
        if (cone_softness > 0.0f) {
            // Soft falloff at edges
            float edge_start = cone_angle * (1.0f - cone_softness);
            if (abs_diff > edge_start) {
                float t = (abs_diff - edge_start) / (cone_angle - edge_start);
                return 1.0f - t * t; // Quadratic falloff
            }
        }
        return 1.0f;
    }

    return 0.0f;
}

__attribute__((unused)) static float
normalize_angle_near(float angle, float center)
{
    float diff = angle - center;
    while (diff > PZ_PI) {
        angle -= 2.0f * PZ_PI;
        diff -= 2.0f * PZ_PI;
    }
    while (diff < -PZ_PI) {
        angle += 2.0f * PZ_PI;
        diff += 2.0f * PZ_PI;
    }
    return angle;
}

static float
normalize_angle_diff(float diff)
{
    while (diff > PZ_PI) {
        diff -= 2.0f * PZ_PI;
    }
    while (diff < -PZ_PI) {
        diff += 2.0f * PZ_PI;
    }
    return diff;
}

// ============================================================================
// Creation / Destruction
// ============================================================================

pz_lighting *
pz_lighting_create(pz_renderer *renderer, const pz_lighting_config *config)
{
    pz_lighting *lighting = pz_calloc(1, sizeof(pz_lighting));
    if (!lighting) {
        return NULL;
    }

    lighting->renderer = renderer;
    lighting->world_width = config->world_width;
    lighting->world_height = config->world_height;
    lighting->texture_size = config->texture_size;
    lighting->ambient = config->ambient;

    // Create render target
    pz_render_target_desc rt_desc = {
        .width = config->texture_size,
        .height = config->texture_size,
        .color_format = PZ_TEXTURE_RGBA8,
        .has_depth = true,
    };
    lighting->render_target
        = pz_renderer_create_render_target(renderer, &rt_desc);
    if (lighting->render_target == PZ_INVALID_HANDLE) {
        pz_log(PZ_LOG_ERROR, PZ_LOG_CAT_GAME,
            "Failed to create lighting render target");
        pz_free(lighting);
        return NULL;
    }

    lighting->light_texture = pz_renderer_get_render_target_texture(
        renderer, lighting->render_target);

    // Load light shader
    lighting->light_shader = pz_renderer_load_shader(
        renderer, "shaders/lightmap.vert", "shaders/lightmap.frag", "lightmap");
    if (lighting->light_shader == PZ_INVALID_HANDLE) {
        pz_log(PZ_LOG_ERROR, PZ_LOG_CAT_GAME, "Failed to load lightmap shader");
        pz_renderer_destroy_render_target(renderer, lighting->render_target);
        pz_free(lighting);
        return NULL;
    }

    // Create pipeline for light rendering (additive blending)
    pz_vertex_attr attrs[] = {
        { .name = "a_position", .type = PZ_ATTR_FLOAT2, .offset = 0 },
        { .name = "a_color",
            .type = PZ_ATTR_FLOAT3,
            .offset = 2 * sizeof(float) },
        { .name = "a_intensity",
            .type = PZ_ATTR_FLOAT,
            .offset = 5 * sizeof(float) },
    };
    pz_vertex_layout layout = {
        .attrs = attrs,
        .attr_count = 3,
        .stride = LIGHT_VERTEX_SIZE,
    };
    pz_pipeline_desc pipe_desc = {
        .shader = lighting->light_shader,
        .vertex_layout = layout,
        .blend = PZ_BLEND_ADDITIVE, // Additive blending for light accumulation
        .depth = PZ_DEPTH_NONE,
        .cull = PZ_CULL_NONE,
        .primitive = PZ_PRIMITIVE_TRIANGLES,
        .sample_count = 1,
    };
    lighting->light_pipeline
        = pz_renderer_create_pipeline(renderer, &pipe_desc);

    // Create vertex buffer (enough for all lights)
    size_t buffer_size
        = PZ_MAX_LIGHTS * MAX_LIGHT_VERTICES * 3 * LIGHT_VERTEX_SIZE;
    pz_buffer_desc buf_desc = {
        .type = PZ_BUFFER_VERTEX,
        .usage = PZ_BUFFER_DYNAMIC,
        .data = NULL,
        .size = buffer_size,
    };
    lighting->vertex_buffer = pz_renderer_create_buffer(renderer, &buf_desc);

    pz_log(PZ_LOG_INFO, PZ_LOG_CAT_GAME,
        "Lighting system created: %dx%d texture, world %.1fx%.1f",
        config->texture_size, config->texture_size, config->world_width,
        config->world_height);

    return lighting;
}

void
pz_lighting_destroy(pz_lighting *lighting)
{
    if (!lighting) {
        return;
    }

    if (lighting->vertex_buffer != PZ_INVALID_HANDLE) {
        pz_renderer_destroy_buffer(lighting->renderer, lighting->vertex_buffer);
    }
    if (lighting->light_pipeline != PZ_INVALID_HANDLE) {
        pz_renderer_destroy_pipeline(
            lighting->renderer, lighting->light_pipeline);
    }
    if (lighting->light_shader != PZ_INVALID_HANDLE) {
        pz_renderer_destroy_shader(lighting->renderer, lighting->light_shader);
    }
    if (lighting->render_target != PZ_INVALID_HANDLE) {
        pz_renderer_destroy_render_target(
            lighting->renderer, lighting->render_target);
    }

    pz_free(lighting);
}

// ============================================================================
// Occluder Management
// ============================================================================

void
pz_lighting_clear_occluders(pz_lighting *lighting)
{
    if (!lighting)
        return;
    lighting->occluder_count = 0;
    lighting->edge_count = 0;
    lighting->map = NULL;
    lighting->base_occluder_count = 0;
    lighting->base_edge_count = 0;
}

void
pz_lighting_set_map_occluders(pz_lighting *lighting, const pz_map *map)
{
    if (!lighting) {
        return;
    }

    pz_lighting_clear_occluders(lighting);
    if (!map) {
        return;
    }

    pz_lighting_add_map_occluders(lighting, map);
    lighting->base_occluder_count = lighting->occluder_count;
    lighting->base_edge_count = lighting->edge_count;
}

void
pz_lighting_clear_dynamic_occluders(pz_lighting *lighting)
{
    if (!lighting) {
        return;
    }

    lighting->occluder_count = lighting->base_occluder_count;
    lighting->edge_count = lighting->base_edge_count;
}

void
pz_lighting_add_occluder(pz_lighting *lighting, pz_vec2 position,
    pz_vec2 half_size, float angle, int8_t floor_level)
{
    if (!lighting || lighting->occluder_count >= PZ_MAX_OCCLUDERS) {
        return;
    }

    pz_occluder *occ = &lighting->occluders[lighting->occluder_count++];
    occ->position = position;
    occ->half_size = half_size;
    occ->angle = angle;
    occ->floor_level = floor_level;

    // Add edges for shadow casting
    add_occluder_edges(lighting, occ);
}

void
pz_lighting_add_map_occluders(pz_lighting *lighting, const pz_map *map)
{
    if (!lighting || !map) {
        return;
    }

    lighting->map = map;

    float half_w = map->world_width / 2.0f;
    float half_h = map->world_height / 2.0f;
    float tile_half = map->tile_size / 2.0f;

    for (int y = 0; y < map->height; y++) {
        for (int x = 0; x < map->width; x++) {
            int8_t h = pz_map_get_height(map, x, y);
            if (h > 0) {
                // Wall tile - add as occluder (pits h < 0 don't block light)
                // Use wall height as floor_level - walls block lights at same
                // or lower floor level
                float cx = x * map->tile_size + tile_half - half_w;
                float cz = y * map->tile_size + tile_half - half_h;

                pz_lighting_add_occluder(lighting, (pz_vec2) { cx, cz },
                    (pz_vec2) { tile_half, tile_half }, 0.0f, h);
            }
        }
    }
}

// ============================================================================
// Light Management
// ============================================================================

void
pz_lighting_clear_lights(pz_lighting *lighting)
{
    if (!lighting)
        return;
    lighting->light_count = 0;
    for (int i = 0; i < PZ_MAX_LIGHTS; i++) {
        lighting->lights[i].active = false;
    }
}

int
pz_lighting_add_point_light(pz_lighting *lighting, pz_vec2 position,
    pz_vec3 color, float intensity, float radius, int8_t floor_level)
{
    if (!lighting || lighting->light_count >= PZ_MAX_LIGHTS) {
        return -1;
    }

    int idx = lighting->light_count++;
    pz_light *light = &lighting->lights[idx];

    light->active = true;
    light->type = PZ_LIGHT_POINT;
    light->position = position;
    light->direction = 0.0f;
    light->color = color;
    light->intensity = intensity;
    light->radius = radius;
    light->cone_angle = PZ_PI; // Full circle
    light->cone_softness = 0.0f;
    light->floor_level = floor_level;

    return idx;
}

int
pz_lighting_add_spotlight(pz_lighting *lighting, pz_vec2 position,
    float direction, pz_vec3 color, float intensity, float radius,
    float cone_angle, float cone_softness, int8_t floor_level)
{
    if (!lighting || lighting->light_count >= PZ_MAX_LIGHTS) {
        return -1;
    }

    int idx = lighting->light_count++;
    pz_light *light = &lighting->lights[idx];

    light->active = true;
    light->type = PZ_LIGHT_SPOTLIGHT;
    light->position = position;
    light->direction = direction;
    light->color = color;
    light->intensity = intensity;
    light->radius = radius;
    light->cone_angle = cone_angle;
    light->cone_softness = cone_softness;
    light->floor_level = floor_level;

    return idx;
}

pz_light *
pz_lighting_get_light(pz_lighting *lighting, int index)
{
    if (!lighting || index < 0 || index >= PZ_MAX_LIGHTS) {
        return NULL;
    }
    return &lighting->lights[index];
}

// ============================================================================
// Light Geometry Generation
// ============================================================================

// Generate visibility polygon for a light using ray casting
// Returns number of vertices written
static int
generate_light_geometry(pz_lighting *lighting, const pz_light *light,
    float *vertices, int max_vertices)
{
    if (!light->active || max_vertices < 3) {
        return 0;
    }

    // For a spotlight, we only need to cast rays within the cone
    float start_angle, end_angle;
    int ray_count;

    if (light->type == PZ_LIGHT_SPOTLIGHT) {
        start_angle = light->direction - light->cone_angle;
        end_angle = light->direction + light->cone_angle;
        // Proportionally fewer rays for smaller cones
        ray_count = (int)(SHADOW_RAY_COUNT * (light->cone_angle / PZ_PI));
        if (ray_count < 16)
            ray_count = 16;
    } else {
        start_angle = 0.0f;
        end_angle = 2.0f * PZ_PI;
        ray_count = SHADOW_RAY_COUNT;
    }

    // Also cast rays to all edge endpoints for better shadow accuracy
    // (This creates crisp shadow edges)

    // First pass: collect all interesting angles
    // Only consider edges from occluders at same or higher floor level
    // (light at floor N is blocked by walls at floor >= N)
    edge filtered_edges[PZ_MAX_OCCLUDERS * PZ_MAX_EDGES_PER_OCCLUDER];
    int filtered_edge_count = 0;
    float max_dist = light->radius + MIN_RAY_DISTANCE;
    float max_dist_sq = max_dist * max_dist;

    for (int i = 0; i < lighting->edge_count; i++) {
        // Skip edges from lower floors - they don't block this light
        if (lighting->edges[i].floor_level < light->floor_level) {
            continue;
        }
        float dist_sq = point_segment_distance_sq(
            light->position, lighting->edges[i].a, lighting->edges[i].b);
        if (dist_sq <= max_dist_sq) {
            filtered_edges[filtered_edge_count++] = lighting->edges[i];
        }
    }

    int max_angles = ray_count + filtered_edge_count * 6 + 16;
    float *angles = pz_alloc(sizeof(float) * (size_t)max_angles);
    if (!angles) {
        return 0;
    }
    int angle_count = 0;

    if (light->type == PZ_LIGHT_SPOTLIGHT) {
        float angle_step = (2.0f * light->cone_angle) / (float)ray_count;
        for (int i = 0; i <= ray_count; i++) {
            angles[angle_count++] = -light->cone_angle + i * angle_step;
        }
    } else {
        // Add regular interval angles
        float angle_step = (end_angle - start_angle) / (float)ray_count;
        for (int i = 0; i <= ray_count; i++) {
            float angle = start_angle + i * angle_step;
            angles[angle_count++] = angle;
        }
    }

    // Add angles to all edge endpoints (with small offsets for shadow edges)
    // Only process edges that FACE the light (backface culling)
    for (int i = 0; i < filtered_edge_count && angle_count + 6 < max_angles;
         i++) {
        const edge *edge = &filtered_edges[i];
        // Check if this edge faces the light
        // Edge normal points perpendicular to edge direction (CCW winding)
        pz_vec2 edge_dir = { edge->b.x - edge->a.x, edge->b.y - edge->a.y };
        // Normal is perpendicular (rotate 90 degrees CCW): (-dy, dx)
        pz_vec2 edge_normal = { -edge_dir.y, edge_dir.x };

        // Vector from edge midpoint to light
        pz_vec2 edge_mid = { (edge->a.x + edge->b.x) * 0.5f,
            (edge->a.y + edge->b.y) * 0.5f };
        pz_vec2 to_light = { light->position.x - edge_mid.x,
            light->position.y - edge_mid.y };

        // If normal dot to_light is negative, this edge faces away from light
        float facing = edge_normal.x * to_light.x + edge_normal.y * to_light.y;
        if (facing < 0) {
            continue; // Skip backfacing edges
        }

        pz_vec2 to_a
            = { edge->a.x - light->position.x, edge->a.y - light->position.y };
        pz_vec2 to_b
            = { edge->b.x - light->position.x, edge->b.y - light->position.y };

        float angle_a = atan2f(to_a.y, to_a.x);
        float angle_b = atan2f(to_b.y, to_b.x);

        // Add with small offsets to catch both sides of corners
        float epsilon = 0.0001f;
        if (light->type == PZ_LIGHT_SPOTLIGHT) {
            float diff_a = normalize_angle_diff(angle_a - light->direction);
            float diff_b = normalize_angle_diff(angle_b - light->direction);
            if (fabsf(diff_a) <= light->cone_angle + 0.01f) {
                angles[angle_count++] = diff_a - epsilon;
                angles[angle_count++] = diff_a;
                angles[angle_count++] = diff_a + epsilon;
            }
            if (fabsf(diff_b) <= light->cone_angle + 0.01f) {
                angles[angle_count++] = diff_b - epsilon;
                angles[angle_count++] = diff_b;
                angles[angle_count++] = diff_b + epsilon;
            }
        } else {
            // Normalize angles to [0, 2π] to match the regular sweep angles
            if (angle_a < 0)
                angle_a += 2.0f * PZ_PI;
            if (angle_b < 0)
                angle_b += 2.0f * PZ_PI;
            angles[angle_count++] = angle_a - epsilon;
            angles[angle_count++] = angle_a;
            angles[angle_count++] = angle_a + epsilon;
            angles[angle_count++] = angle_b - epsilon;
            angles[angle_count++] = angle_b;
            angles[angle_count++] = angle_b + epsilon;
        }
    }

    // Sort angles
    pz_sort_floats_cmp(angles, (size_t)angle_count, compare_floats);

    // Remove duplicates and out-of-range angles (for spotlights)
    float *unique_angles = pz_alloc(sizeof(float) * (size_t)angle_count);
    if (!unique_angles) {
        pz_free(angles);
        return 0;
    }
    int unique_count = 0;
    float last_angle = -1000.0f;

    for (int i = 0; i < angle_count; i++) {
        float a = angles[i];
        if (a - last_angle > 0.0001f) {
            unique_angles[unique_count++] = a;
            last_angle = a;
        }
    }

    if (unique_count < 2) {
        pz_free(unique_angles);
        pz_free(angles);
        return 0;
    }

    // Cast rays and build triangle fan
    // Center vertex of fan
    float center_x = light->position.x;
    float center_z = light->position.y;

    // Convert to UV space for rendering
    float uv_center_x = center_x / lighting->world_width + 0.5f;
    float uv_center_z = center_z / lighting->world_height + 0.5f;

    // Generate triangles (fan from center)
    float *v = vertices;
    int vertex_count = 0;

    float prev_uv_x = 0, prev_uv_z = 0;
    bool has_prev = false;

    for (int i = 0; i < unique_count && vertex_count + 3 <= max_vertices; i++) {
        float angle = unique_angles[i];
        if (light->type == PZ_LIGHT_SPOTLIGHT) {
            angle = light->direction + angle;
        }
        pz_vec2 dir = { cosf(angle), sinf(angle) };

        // Cast ray
        float t = cast_ray(filtered_edges, filtered_edge_count, light->position,
            dir, light->radius);

        // Hit point in world space
        float hit_x = center_x + dir.x * t;
        float hit_z = center_z + dir.y * t;

        // Convert to UV space
        float uv_x = hit_x / lighting->world_width + 0.5f;
        float uv_z = hit_z / lighting->world_height + 0.5f;

        // Note: intensity falloff is now calculated in the fragment shader
        // based on actual fragment distance from light center.
        // We just pass the base intensity here.

        if (has_prev) {
            // Emit triangle: center, prev, current
            // All vertices use the same base intensity - shader does falloff

            // Center vertex
            *v++ = uv_center_x;
            *v++ = uv_center_z;
            *v++ = light->color.x;
            *v++ = light->color.y;
            *v++ = light->color.z;
            *v++ = light->intensity;

            // Previous vertex
            *v++ = prev_uv_x;
            *v++ = prev_uv_z;
            *v++ = light->color.x;
            *v++ = light->color.y;
            *v++ = light->color.z;
            *v++ = light->intensity;

            // Current vertex
            *v++ = uv_x;
            *v++ = uv_z;
            *v++ = light->color.x;
            *v++ = light->color.y;
            *v++ = light->color.z;
            *v++ = light->intensity;

            vertex_count += 3;
        }

        prev_uv_x = uv_x;
        prev_uv_z = uv_z;
        has_prev = true;
    }

    // For point lights, close the fan by connecting last vertex to first
    // (Spotlights don't need this as they have defined start/end angles)
    if (light->type == PZ_LIGHT_POINT && unique_count >= 2
        && vertex_count + 3 <= max_vertices) {
        // Cast ray at angle 0 (first angle direction) to get first vertex
        float first_angle = unique_angles[0];
        pz_vec2 first_dir = { cosf(first_angle), sinf(first_angle) };
        float first_t = cast_ray(filtered_edges, filtered_edge_count,
            light->position, first_dir, light->radius);
        float first_uv_x
            = (center_x + first_dir.x * first_t) / lighting->world_width + 0.5f;
        float first_uv_z
            = (center_z + first_dir.y * first_t) / lighting->world_height
            + 0.5f;

        // Emit closing triangle: center, last, first
        *v++ = uv_center_x;
        *v++ = uv_center_z;
        *v++ = light->color.x;
        *v++ = light->color.y;
        *v++ = light->color.z;
        *v++ = light->intensity;

        *v++ = prev_uv_x;
        *v++ = prev_uv_z;
        *v++ = light->color.x;
        *v++ = light->color.y;
        *v++ = light->color.z;
        *v++ = light->intensity;

        *v++ = first_uv_x;
        *v++ = first_uv_z;
        *v++ = light->color.x;
        *v++ = light->color.y;
        *v++ = light->color.z;
        *v++ = light->intensity;

        vertex_count += 3;
    }

    pz_free(unique_angles);
    pz_free(angles);
    return vertex_count;
}

// ============================================================================
// Rendering
// ============================================================================

void
pz_lighting_render(pz_lighting *lighting)
{
    if (!lighting) {
        return;
    }

    // Bind render target
    pz_renderer_set_render_target(lighting->renderer, lighting->render_target);

    // Clear to ambient color
    pz_renderer_clear_color(lighting->renderer, lighting->ambient.x,
        lighting->ambient.y, lighting->ambient.z, 1.0f);

    // Generate and render each light
    size_t total_buffer_size
        = PZ_MAX_LIGHTS * MAX_LIGHT_VERTICES * 3 * LIGHT_VERTEX_SIZE;
    float *light_vertices = pz_alloc(total_buffer_size);
    if (!light_vertices) {
        pz_renderer_set_render_target(lighting->renderer, 0);
        return;
    }

    typedef struct light_draw {
        pz_light *light;
        int vertex_offset;
        int vertex_count;
    } light_draw;

    light_draw draws[PZ_MAX_LIGHTS];
    int draw_count = 0;
    int max_verts = (int)(total_buffer_size / LIGHT_VERTEX_SIZE);
    int write_offset = 0;
    float *write_ptr = light_vertices;

    for (int i = 0; i < lighting->light_count; i++) {
        if (!lighting->lights[i].active) {
            continue;
        }

        pz_light *light = &lighting->lights[i];
        int remaining = max_verts - write_offset;
        if (remaining <= 0) {
            break;
        }

        int verts
            = generate_light_geometry(lighting, light, write_ptr, remaining);

        if (verts > 0) {
            draws[draw_count++] = (light_draw) {
                .light = light,
                .vertex_offset = write_offset,
                .vertex_count = verts,
            };
            write_ptr += verts * LIGHT_VERTEX_FLOATS;
            write_offset += verts;
        }
    }

    if (draw_count > 0) {
        pz_renderer_update_buffer(lighting->renderer, lighting->vertex_buffer,
            0, light_vertices, (size_t)write_offset * LIGHT_VERTEX_SIZE);

        for (int i = 0; i < draw_count; i++) {
            pz_light *light = draws[i].light;
            float light_center_uv_x
                = light->position.x / lighting->world_width + 0.5f;
            float light_center_uv_y
                = light->position.y / lighting->world_height + 0.5f;

            pz_renderer_set_uniform_vec2(lighting->renderer,
                lighting->light_shader, "u_light_center_uv",
                (pz_vec2) { light_center_uv_x, light_center_uv_y });
            pz_renderer_set_uniform_float(lighting->renderer,
                lighting->light_shader, "u_light_radius", light->radius);
            pz_renderer_set_uniform_vec2(lighting->renderer,
                lighting->light_shader, "u_world_size",
                (pz_vec2) { lighting->world_width, lighting->world_height });

            pz_draw_cmd cmd = {
                .pipeline = lighting->light_pipeline,
                .vertex_buffer = lighting->vertex_buffer,
                .vertex_count = (size_t)draws[i].vertex_count,
                .vertex_offset = (size_t)draws[i].vertex_offset,
            };
            pz_renderer_draw(lighting->renderer, &cmd);
        }
    }

    pz_free(light_vertices);

    // Reset to default framebuffer
    pz_renderer_set_render_target(lighting->renderer, 0);
}

// ============================================================================
// Accessors
// ============================================================================

pz_texture_handle
pz_lighting_get_texture(pz_lighting *lighting)
{
    if (!lighting) {
        return PZ_INVALID_HANDLE;
    }
    return lighting->light_texture;
}

void
pz_lighting_get_uv_transform(pz_lighting *lighting, float *out_scale_x,
    float *out_scale_z, float *out_offset_x, float *out_offset_z)
{
    if (!lighting) {
        *out_scale_x = 1.0f;
        *out_scale_z = 1.0f;
        *out_offset_x = 0.0f;
        *out_offset_z = 0.0f;
        return;
    }

    *out_scale_x = 1.0f / lighting->world_width;
    *out_scale_z = 1.0f / lighting->world_height;
    *out_offset_x = 0.5f;
    *out_offset_z = 0.5f;
}

pz_vec3
pz_lighting_get_ambient(pz_lighting *lighting)
{
    if (!lighting) {
        return (pz_vec3) { 0.1f, 0.1f, 0.1f };
    }
    return lighting->ambient;
}

void
pz_lighting_set_ambient(pz_lighting *lighting, pz_vec3 ambient)
{
    if (!lighting) {
        return;
    }
    lighting->ambient = ambient;
}

bool
pz_lighting_save_debug(pz_lighting *lighting, const char *path)
{
    if (!lighting) {
        return false;
    }
    return pz_renderer_save_render_target(
        lighting->renderer, lighting->render_target, path);
}

int
pz_lighting_get_light_count(const pz_lighting *lighting)
{
    return lighting ? lighting->light_count : 0;
}

int
pz_lighting_get_occluder_count(const pz_lighting *lighting)
{
    return lighting ? lighting->occluder_count : 0;
}

int
pz_lighting_get_edge_count(const pz_lighting *lighting)
{
    return lighting ? lighting->edge_count : 0;
}
