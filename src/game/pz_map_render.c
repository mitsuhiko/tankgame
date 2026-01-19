/*
 * Map Rendering System Implementation
 *
 * Renders terrain tiles and 3D wall geometry from map data.
 * Uses tile definitions from the tile registry for textures.
 */

#include "pz_map_render.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/pz_log.h"
#include "../core/pz_mem.h"
#include "../core/pz_platform.h"
#include "../core/pz_str.h"

// Wall height unit (in world units per height level)
#define WALL_HEIGHT_UNIT PZ_WALL_HEIGHT_UNIT

// Maximum number of tile textures we can cache
#define MAX_TILE_TEXTURES 32

// Ground mesh batch (one per unique texture)
typedef struct ground_batch {
    pz_texture_handle texture;
    pz_buffer_handle buffer;
    int vertex_count;
} ground_batch;

// Wall mesh batch (one per tile type with wall geometry)
typedef struct wall_batch {
    pz_texture_handle top_texture;
    pz_texture_handle side_texture;
    pz_buffer_handle buffer;
    int vertex_count;
} wall_batch;

// Blended ground vertex: position(3) + uv(2) + terrain_idx(1) + neighbors(4)
#define BLEND_GROUND_VERTEX_SIZE 10

struct pz_map_renderer {
    pz_renderer *renderer;
    pz_texture_manager *tex_manager;
    const pz_tile_registry *tile_registry;

    // Ground shader and pipeline (legacy, for walls on top)
    pz_shader_handle ground_shader;
    pz_pipeline_handle ground_pipeline;

    // Jump pad rendering (uses ground shader/pipeline for simplicity)
    pz_texture_handle jump_pad_texture;
    pz_buffer_handle jump_pad_buffer;
    int jump_pad_vertex_count;

    // Blended ground shader and pipeline
    pz_shader_handle ground_blend_shader;
    pz_pipeline_handle ground_blend_pipeline;
    pz_texture_handle ground_texture_array; // All ground textures packed
    pz_buffer_handle ground_blend_buffer; // Unified ground mesh
    int ground_blend_vertex_count;
    int ground_texture_array_size; // Size of textures in array (uniform)

    // Wall shader and pipeline
    pz_shader_handle wall_shader;
    pz_pipeline_handle wall_pipeline;

    // Water shader and pipeline
    pz_shader_handle water_shader;
    pz_pipeline_handle water_pipeline;
    pz_texture_handle water_caustic_texture;

    // Fog shader and pipeline
    pz_shader_handle fog_shader;
    pz_pipeline_handle fog_pipeline;

    // Ground batches (legacy - kept for wall tops)
    ground_batch ground_batches[MAX_TILE_TEXTURES];
    int ground_batch_count;

    // Wall batches (one per tile type with wall geometry)
    wall_batch wall_batches[MAX_TILE_TEXTURES];
    int wall_batch_count;

    // Water vertex buffer
    pz_buffer_handle water_buffer;
    int water_vertex_count;

    // Fog vertex buffer
    pz_buffer_handle fog_buffer;
    int fog_vertex_count;

    // Current map
    const pz_map *map;

    // Debug line rendering
    pz_shader_handle debug_line_shader;
    pz_pipeline_handle debug_line_pipeline;
    pz_buffer_handle debug_line_buffer;
    int debug_line_vertex_count;
    bool debug_texture_scale_enabled;
};

// ============================================================================
// Ground Mesh Generation
// ============================================================================

// Ground plane Y offset - slightly below walls
#define GROUND_Y_OFFSET -0.01f

// Water plane Y offset - water surface is at this Y level relative to ground
#define WATER_Y_OFFSET -0.5f

// Fog plane Y offset - fog is half a block above its level
#define FOG_Y_OFFSET 0.5f

static void
compute_tile_uv(int tile_x, int tile_y, int map_height, int scale, float *u0,
    float *v0, float *u1, float *v1)
{
    (void)map_height;
    float inv_scale = 1.0f / (float)scale;

    *u0 = (float)tile_x * inv_scale;
    *u1 = (float)(tile_x + 1) * inv_scale;
    *v0 = (float)tile_y * inv_scale;
    *v1 = (float)(tile_y + 1) * inv_scale;
}

// Create vertices for a rotated tile quad
// Rotation values as displayed on screen (with top-down camera, +Z toward
// bottom): rotation: 0=down(+Z), 1=left(-X), 2=up(-Z), 3=right(+X)
static float *
emit_plane_quad_rotated(
    float *v, float x0, float z0, float x1, float z1, float y, int rotation)
{
    // UV corners for each rotation (indexed by vertex position)
    // Vertex positions: 0=(x0,z0), 1=(x0,z1), 2=(x1,z1), 3=(x1,z0)
    // Default UVs:      0=(0,1),   1=(0,0),   2=(1,0),   3=(1,1)
    // Note: With our camera setup (looking down from +Y), texture "up" maps
    // to screen "down" due to Z axis orientation.
    static const float uv_table[4][4][2] = {
        // rotation=0: arrow points DOWN on screen (+Z direction)
        { { 0.0f, 1.0f }, { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 1.0f } },
        // rotation=1: arrow points LEFT on screen (-X direction)
        { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 1.0f } },
        // rotation=2: arrow points UP on screen (-Z direction)
        { { 1.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 1.0f }, { 0.0f, 0.0f } },
        // rotation=3: arrow points RIGHT on screen (+X direction)
        { { 1.0f, 1.0f }, { 0.0f, 1.0f }, { 0.0f, 0.0f }, { 1.0f, 0.0f } },
    };

    int r = rotation & 3; // Clamp to 0-3
    const float(*uv)[2] = uv_table[r];

    // Triangle 1 (CCW when viewed from above +Y): vertices 0, 1, 2
    *v++ = x0;
    *v++ = y;
    *v++ = z0;
    *v++ = uv[0][0];
    *v++ = uv[0][1];

    *v++ = x0;
    *v++ = y;
    *v++ = z1;
    *v++ = uv[1][0];
    *v++ = uv[1][1];

    *v++ = x1;
    *v++ = y;
    *v++ = z1;
    *v++ = uv[2][0];
    *v++ = uv[2][1];

    // Triangle 2: vertices 0, 2, 3
    *v++ = x0;
    *v++ = y;
    *v++ = z0;
    *v++ = uv[0][0];
    *v++ = uv[0][1];

    *v++ = x1;
    *v++ = y;
    *v++ = z1;
    *v++ = uv[2][0];
    *v++ = uv[2][1];

    *v++ = x1;
    *v++ = y;
    *v++ = z0;
    *v++ = uv[3][0];
    *v++ = uv[3][1];

    return v;
}

// Create vertices for a single tile quad on a flat plane (no rotation)
static float *
emit_plane_quad(float *v, float x0, float z0, float x1, float z1, float y)
{
    // Triangle 1 (CCW when viewed from above +Y)
    *v++ = x0;
    *v++ = y;
    *v++ = z0;
    *v++ = 0.0f;
    *v++ = 1.0f;

    *v++ = x0;
    *v++ = y;
    *v++ = z1;
    *v++ = 0.0f;
    *v++ = 0.0f;

    *v++ = x1;
    *v++ = y;
    *v++ = z1;
    *v++ = 1.0f;
    *v++ = 0.0f;

    // Triangle 2
    *v++ = x0;
    *v++ = y;
    *v++ = z0;
    *v++ = 0.0f;
    *v++ = 1.0f;

    *v++ = x1;
    *v++ = y;
    *v++ = z1;
    *v++ = 1.0f;
    *v++ = 0.0f;

    *v++ = x1;
    *v++ = y;
    *v++ = z0;
    *v++ = 1.0f;
    *v++ = 1.0f;

    return v;
}

// Create vertices for a single tile quad on ground plane (at custom Y height)
// tile_x, tile_y: grid position of the tile (used for world-space UV alignment)
// scale: texture scale (how many tiles the texture spans, e.g., 6 = 6x6)
static float *
emit_ground_quad_at_height(float *v, float x0, float z0, float x1, float z1,
    float y, int tile_x, int tile_y, int map_height, int scale)
{
    // World-space UVs: tile position divided by scale
    // This ensures adjacent tiles seamlessly continue the texture
    float u0, v0, u1, v1;
    compute_tile_uv(tile_x, tile_y, map_height, scale, &u0, &v0, &u1, &v1);

    // Triangle 1 (CCW when viewed from above +Y)
    *v++ = x0;
    *v++ = y;
    *v++ = z0;
    *v++ = u0;
    *v++ = v0;

    *v++ = x0;
    *v++ = y;
    *v++ = z1;
    *v++ = u0;
    *v++ = v1;

    *v++ = x1;
    *v++ = y;
    *v++ = z1;
    *v++ = u1;
    *v++ = v1;

    // Triangle 2
    *v++ = x0;
    *v++ = y;
    *v++ = z0;
    *v++ = u0;
    *v++ = v0;

    *v++ = x1;
    *v++ = y;
    *v++ = z1;
    *v++ = u1;
    *v++ = v1;

    *v++ = x1;
    *v++ = y;
    *v++ = z0;
    *v++ = u1;
    *v++ = v0;

    return v;
}

// Create vertices for a single tile quad on ground plane (default height)
__attribute__((unused)) static float *
emit_ground_quad(float *v, float x0, float z0, float x1, float z1, int tile_x,
    int tile_y, int map_height, int scale)
{
    return emit_ground_quad_at_height(
        v, x0, z0, x1, z1, GROUND_Y_OFFSET, tile_x, tile_y, map_height, scale);
}

// ============================================================================
// Wall Mesh Generation
// ============================================================================

#define WALL_VERTEX_SIZE 9

static bool
is_solid_block_level(const pz_map *map, int tile_x, int tile_y, int level)
{
    if (!pz_map_in_bounds(map, tile_x, tile_y)) {
        return true;
    }
    int8_t h = pz_map_get_height(map, tile_x, tile_y);

    if (h > 0) {
        // Raised wall: solid from ground (0) up to h-1
        return level >= 0 && level < h;
    }

    if (h < 0) {
        // Pit: empty from h to -1 (inside pit), solid below h (pit floor)
        return level < h;
    }

    // h == 0: Flat ground - solid below ground level, empty above
    return level < 0;
}

static float
compute_wall_corner_ao(
    const pz_map *map, int vx, int vy, int vz, int nx, int ny, int nz)
{
    int front_x = vx + (nx < 0 ? -1 : 0);
    int front_y = vy + (ny < 0 ? -1 : 0);
    int front_z = vz + (nz < 0 ? -1 : 0);

    int x_steps = (nx == 0) ? 2 : 1;
    int y_steps = (ny == 0) ? 2 : 1;
    int z_steps = (nz == 0) ? 2 : 1;

    int x_offsets[2] = { 0, 0 };
    int y_offsets[2] = { 0, 0 };
    int z_offsets[2] = { 0, 0 };

    if (nx == 0) {
        x_offsets[0] = -1;
        x_offsets[1] = 0;
    }
    if (ny == 0) {
        y_offsets[0] = -1;
        y_offsets[1] = 0;
    }
    if (nz == 0) {
        z_offsets[0] = -1;
        z_offsets[1] = 0;
    }

    float sum = 0.0f;
    int count = 0;
    for (int xi = 0; xi < x_steps; xi++) {
        for (int yi = 0; yi < y_steps; yi++) {
            for (int zi = 0; zi < z_steps; zi++) {
                int x = front_x + x_offsets[xi];
                int y = front_y + y_offsets[yi];
                int z = front_z + z_offsets[zi];
                bool solid = is_solid_block_level(map, x, z, y);
                sum += solid ? 0.0f : 1.0f;
                count++;
            }
        }
    }

    if (count == 0) {
        return 1.0f;
    }
    return sum / (float)count;
}

static float *
emit_wall_face(float *v, float x0, float y0, float z0, float x1, float y1,
    float z1, float x2, float y2, float z2, float x3, float y3, float z3,
    float nx, float ny, float nz, float u0, float v0_uv, float u1, float v1_uv,
    float ao0, float ao1, float ao2, float ao3)
{
    // Triangle 1: v0, v1, v2
    *v++ = x0;
    *v++ = y0;
    *v++ = z0;
    *v++ = nx;
    *v++ = ny;
    *v++ = nz;
    *v++ = u0;
    *v++ = v1_uv;
    *v++ = ao0;

    *v++ = x1;
    *v++ = y1;
    *v++ = z1;
    *v++ = nx;
    *v++ = ny;
    *v++ = nz;
    *v++ = u0;
    *v++ = v0_uv;
    *v++ = ao1;

    *v++ = x2;
    *v++ = y2;
    *v++ = z2;
    *v++ = nx;
    *v++ = ny;
    *v++ = nz;
    *v++ = u1;
    *v++ = v0_uv;
    *v++ = ao2;

    // Triangle 2: v0, v2, v3
    *v++ = x0;
    *v++ = y0;
    *v++ = z0;
    *v++ = nx;
    *v++ = ny;
    *v++ = nz;
    *v++ = u0;
    *v++ = v1_uv;
    *v++ = ao0;

    *v++ = x2;
    *v++ = y2;
    *v++ = z2;
    *v++ = nx;
    *v++ = ny;
    *v++ = nz;
    *v++ = u1;
    *v++ = v0_uv;
    *v++ = ao2;

    *v++ = x3;
    *v++ = y3;
    *v++ = z3;
    *v++ = nx;
    *v++ = ny;
    *v++ = nz;
    *v++ = u1;
    *v++ = v1_uv;
    *v++ = ao3;

    return v;
}

// Emit wall side faces for a tile. Works for both raised walls (h > 0) and pits
// (h < 0). y_bottom/y_top: world Y coordinates of wall extent h: height level
// of this tile (used for neighbor comparison) emit_top: whether to emit the top
// face (true for raised walls, false for pits)
static float *
emit_wall_sides(float *v, float x0, float z0, float x1, float z1,
    float y_bottom, float y_top, int tile_x, int tile_y, int8_t h,
    const pz_map *map, int scale, bool emit_top)
{
    // Check which faces are exposed (neighbor is lower than this tile)
    bool left_exposed = !pz_map_in_bounds(map, tile_x - 1, tile_y)
        || pz_map_get_height(map, tile_x - 1, tile_y) < h;
    bool right_exposed = !pz_map_in_bounds(map, tile_x + 1, tile_y)
        || pz_map_get_height(map, tile_x + 1, tile_y) < h;
    bool front_exposed = !pz_map_in_bounds(map, tile_x, tile_y + 1)
        || pz_map_get_height(map, tile_x, tile_y + 1) < h;
    bool back_exposed = !pz_map_in_bounds(map, tile_x, tile_y - 1)
        || pz_map_get_height(map, tile_x, tile_y - 1) < h;

    // World-space UVs for wall top (same as ground)
    float inv_scale = 1.0f / (float)scale;
    float u0, v0_uv, u1, v1_uv;
    compute_tile_uv(
        tile_x, tile_y, map->height, scale, &u0, &v0_uv, &u1, &v1_uv);

    int top_level = h > 0 ? h : 0; // For AO calculation on top face

    // Top face (only for raised walls, not pits)
    if (emit_top) {
        float top_ao0
            = compute_wall_corner_ao(map, tile_x, top_level, tile_y, 0, 1, 0);
        float top_ao1 = compute_wall_corner_ao(
            map, tile_x, top_level, tile_y + 1, 0, 1, 0);
        float top_ao2 = compute_wall_corner_ao(
            map, tile_x + 1, top_level, tile_y + 1, 0, 1, 0);
        float top_ao3 = compute_wall_corner_ao(
            map, tile_x + 1, top_level, tile_y, 0, 1, 0);
        v = emit_wall_face(v, x0, y_top, z0, x0, y_top, z1, x1, y_top, z1, x1,
            y_top, z0, 0.0f, 1.0f, 0.0f, u0, v0_uv, u1, v1_uv, top_ao0, top_ao1,
            top_ao2, top_ao3);
    }

    // Side faces use world-space UVs based on absolute Y position
    // This ensures textures align across walls at different heights
    // Convert world Y back to height units for UV calculation
    // Note: emit_wall_face expects (v0_uv=top, v1_uv=bottom) due to its
    // internal mapping
    float y_bottom_units = (y_bottom - GROUND_Y_OFFSET) / WALL_HEIGHT_UNIT;
    float y_top_units = (y_top - GROUND_Y_OFFSET) / WALL_HEIGHT_UNIT;
    float v_uv_for_top = y_top_units * inv_scale; // v0_uv in emit_wall_face
    float v_uv_for_bottom
        = y_bottom_units * inv_scale; // v1_uv in emit_wall_face

    // Side faces use uniform AO computed at ground level (0) to avoid
    // gradient artifacts when adjacent walls have different heights.
    // The AO represents occlusion from horizontal neighbors at the base.

    // Back face (-Z)
    if (back_exposed) {
        float ao_left
            = compute_wall_corner_ao(map, tile_x, 0, tile_y, 0, 0, -1);
        float ao_right
            = compute_wall_corner_ao(map, tile_x + 1, 0, tile_y, 0, 0, -1);
        v = emit_wall_face(v, x0, y_bottom, z0, x0, y_top, z0, x1, y_top, z0,
            x1, y_bottom, z0, 0.0f, 0.0f, -1.0f, u0, v_uv_for_top, u1,
            v_uv_for_bottom, ao_left, ao_left, ao_right, ao_right);
    }

    // Front face (+Z)
    if (front_exposed) {
        float ao_right
            = compute_wall_corner_ao(map, tile_x + 1, 0, tile_y + 1, 0, 0, 1);
        float ao_left
            = compute_wall_corner_ao(map, tile_x, 0, tile_y + 1, 0, 0, 1);
        v = emit_wall_face(v, x1, y_bottom, z1, x1, y_top, z1, x0, y_top, z1,
            x0, y_bottom, z1, 0.0f, 0.0f, 1.0f, u1, v_uv_for_top, u0,
            v_uv_for_bottom, ao_right, ao_right, ao_left, ao_left);
    }

    // Left face (-X)
    if (left_exposed) {
        float ao_front
            = compute_wall_corner_ao(map, tile_x, 0, tile_y + 1, -1, 0, 0);
        float ao_back
            = compute_wall_corner_ao(map, tile_x, 0, tile_y, -1, 0, 0);
        v = emit_wall_face(v, x0, y_bottom, z1, x0, y_top, z1, x0, y_top, z0,
            x0, y_bottom, z0, -1.0f, 0.0f, 0.0f, v1_uv, v_uv_for_top, v0_uv,
            v_uv_for_bottom, ao_front, ao_front, ao_back, ao_back);
    }

    // Right face (+X)
    if (right_exposed) {
        float ao_back
            = compute_wall_corner_ao(map, tile_x + 1, 0, tile_y, 1, 0, 0);
        float ao_front
            = compute_wall_corner_ao(map, tile_x + 1, 0, tile_y + 1, 1, 0, 0);
        v = emit_wall_face(v, x1, y_bottom, z0, x1, y_top, z0, x1, y_top, z1,
            x1, y_bottom, z1, 1.0f, 0.0f, 0.0f, v0_uv, v_uv_for_top, v1_uv,
            v_uv_for_bottom, ao_back, ao_back, ao_front, ao_front);
    }

    return v;
}

// Wrapper for raised walls (h > 0) - emits walls from y=0 to y=height with top
// face
__attribute__((unused)) static float *
emit_wall_box(float *v, float x0, float z0, float x1, float z1, float height,
    int tile_x, int tile_y, const pz_map *map, int scale)
{
    int8_t h = (int8_t)(height / WALL_HEIGHT_UNIT);
    return emit_wall_sides(
        v, x0, z0, x1, z1, 0.0f, height, tile_x, tile_y, h, map, scale, true);
}

// Count wall faces for a tile (works for both h > 0 and h < 0)
// For h > 0: includes top face
// For h < 0: no top face (pit floor is rendered as ground)
static int
count_wall_faces_for_tile(int tile_x, int tile_y, int8_t h, const pz_map *map)
{
    int count = (h > 0) ? 1 : 0; // Top face only for raised walls

    if (!pz_map_in_bounds(map, tile_x - 1, tile_y)
        || pz_map_get_height(map, tile_x - 1, tile_y) < h)
        count++;
    if (!pz_map_in_bounds(map, tile_x + 1, tile_y)
        || pz_map_get_height(map, tile_x + 1, tile_y) < h)
        count++;
    if (!pz_map_in_bounds(map, tile_x, tile_y + 1)
        || pz_map_get_height(map, tile_x, tile_y + 1) < h)
        count++;
    if (!pz_map_in_bounds(map, tile_x, tile_y - 1)
        || pz_map_get_height(map, tile_x, tile_y - 1) < h)
        count++;

    return count;
}

// Legacy wrapper
__attribute__((unused)) static int
count_wall_faces(int tile_x, int tile_y, float height, const pz_map *map)
{
    int8_t h = (int8_t)(height / WALL_HEIGHT_UNIT);
    return count_wall_faces_for_tile(tile_x, tile_y, h, map);
}
// ============================================================================
// Texture Loading
// ============================================================================

// Get tile config from registry, with fallback
static const pz_tile_config *
get_tile_config(const pz_map_renderer *mr, const char *tile_name)
{
    if (!mr->tile_registry || !tile_name || !tile_name[0]) {
        return NULL;
    }
    return pz_tile_registry_get(mr->tile_registry, tile_name);
}

// ============================================================================
// Map Renderer Implementation
// ============================================================================

pz_map_renderer *
pz_map_renderer_create(pz_renderer *renderer, pz_texture_manager *tex_manager,
    const pz_tile_registry *tile_registry)
{
    pz_map_renderer *mr = pz_calloc(1, sizeof(pz_map_renderer));
    if (!mr) {
        return NULL;
    }

    mr->renderer = renderer;
    mr->tex_manager = tex_manager;
    mr->tile_registry = tile_registry;

    // Ground shader
    mr->ground_shader = pz_renderer_load_shader(
        renderer, "shaders/ground.vert", "shaders/ground.frag", "ground");
    if (mr->ground_shader == PZ_INVALID_HANDLE) {
        pz_log(PZ_LOG_ERROR, PZ_LOG_CAT_RENDER, "Failed to load ground shader");
        pz_map_renderer_destroy(mr);
        return NULL;
    }

    // Ground pipeline
    pz_vertex_attr ground_attrs[] = {
        { .name = "a_position", .type = PZ_ATTR_FLOAT3, .offset = 0 },
        { .name = "a_texcoord",
            .type = PZ_ATTR_FLOAT2,
            .offset = 3 * sizeof(float) },
    };

    pz_pipeline_desc ground_desc = {
        .shader = mr->ground_shader,
        .vertex_layout = {
            .attrs = ground_attrs,
            .attr_count = 2,
            .stride = 5 * sizeof(float),
        },
        .blend = PZ_BLEND_NONE,
        .depth = PZ_DEPTH_READ_WRITE,
        .cull = PZ_CULL_BACK,
        .primitive = PZ_PRIMITIVE_TRIANGLES,
    };
    mr->ground_pipeline = pz_renderer_create_pipeline(renderer, &ground_desc);

    // Blended ground shader
    mr->ground_blend_shader
        = pz_renderer_load_shader(renderer, "shaders/ground_blend.vert",
            "shaders/ground_blend.frag", "ground_blend");
    if (mr->ground_blend_shader == PZ_INVALID_HANDLE) {
        pz_log(PZ_LOG_ERROR, PZ_LOG_CAT_RENDER,
            "Failed to load ground_blend shader");
        pz_map_renderer_destroy(mr);
        return NULL;
    }

    // Blended ground pipeline
    pz_vertex_attr ground_blend_attrs[] = {
        { .name = "a_position", .type = PZ_ATTR_FLOAT3, .offset = 0 },
        { .name = "a_texcoord",
            .type = PZ_ATTR_FLOAT2,
            .offset = 3 * sizeof(float) },
        { .name = "a_terrain_idx",
            .type = PZ_ATTR_FLOAT,
            .offset = 5 * sizeof(float) },
        { .name = "a_neighbor_idx",
            .type = PZ_ATTR_FLOAT4,
            .offset = 6 * sizeof(float) },
    };

    pz_pipeline_desc ground_blend_desc = {
        .shader = mr->ground_blend_shader,
        .vertex_layout = {
            .attrs = ground_blend_attrs,
            .attr_count = 4,
            .stride = BLEND_GROUND_VERTEX_SIZE * sizeof(float),
        },
        .blend = PZ_BLEND_NONE,
        .depth = PZ_DEPTH_READ_WRITE,
        .cull = PZ_CULL_BACK,
        .primitive = PZ_PRIMITIVE_TRIANGLES,
    };
    mr->ground_blend_pipeline
        = pz_renderer_create_pipeline(renderer, &ground_blend_desc);

    // Wall shader
    mr->wall_shader = pz_renderer_load_shader(
        renderer, "shaders/wall.vert", "shaders/wall.frag", "wall");
    if (mr->wall_shader == PZ_INVALID_HANDLE) {
        pz_log(PZ_LOG_ERROR, PZ_LOG_CAT_RENDER, "Failed to load wall shader");
        pz_map_renderer_destroy(mr);
        return NULL;
    }

    // Wall pipeline
    pz_vertex_attr wall_attrs[] = {
        { .name = "a_position", .type = PZ_ATTR_FLOAT3, .offset = 0 },
        { .name = "a_normal",
            .type = PZ_ATTR_FLOAT3,
            .offset = 3 * sizeof(float) },
        { .name = "a_texcoord",
            .type = PZ_ATTR_FLOAT2,
            .offset = 6 * sizeof(float) },
        { .name = "a_ao", .type = PZ_ATTR_FLOAT, .offset = 8 * sizeof(float) },
    };

    pz_pipeline_desc wall_desc = {
        .shader = mr->wall_shader,
        .vertex_layout = {
            .attrs = wall_attrs,
            .attr_count = 4,
            .stride = WALL_VERTEX_SIZE * sizeof(float),
        },
        .blend = PZ_BLEND_NONE,
        .depth = PZ_DEPTH_READ_WRITE,
        .cull = PZ_CULL_NONE, // Double-sided for editor camera / all view angles
        .primitive = PZ_PRIMITIVE_TRIANGLES,
    };
    mr->wall_pipeline = pz_renderer_create_pipeline(renderer, &wall_desc);

    // Water shader
    mr->water_shader = pz_renderer_load_shader(
        renderer, "shaders/water.vert", "shaders/water.frag", "water");
    if (mr->water_shader == PZ_INVALID_HANDLE) {
        pz_log(PZ_LOG_ERROR, PZ_LOG_CAT_RENDER, "Failed to load water shader");
        pz_map_renderer_destroy(mr);
        return NULL;
    }

    // Water pipeline (same vertex layout as ground, with alpha blending)
    pz_pipeline_desc water_desc = {
        .shader = mr->water_shader,
        .vertex_layout = {
            .attrs = ground_attrs,
            .attr_count = 2,
            .stride = 5 * sizeof(float),
        },
        .blend = PZ_BLEND_ALPHA,
        .depth = PZ_DEPTH_READ_WRITE,
        .cull = PZ_CULL_BACK,
        .primitive = PZ_PRIMITIVE_TRIANGLES,
    };
    mr->water_pipeline = pz_renderer_create_pipeline(renderer, &water_desc);

    // Load water caustic texture
    mr->water_caustic_texture
        = pz_texture_load(tex_manager, "assets/textures/water_caustic.png");
    if (mr->water_caustic_texture == PZ_INVALID_HANDLE) {
        pz_log(PZ_LOG_WARN, PZ_LOG_CAT_RENDER,
            "Failed to load water caustic texture, water effect degraded");
    }

    // Load jump pad texture
    mr->jump_pad_texture
        = pz_texture_load(tex_manager, "assets/textures/jump_pad.png");
    if (mr->jump_pad_texture == PZ_INVALID_HANDLE) {
        pz_log(
            PZ_LOG_WARN, PZ_LOG_CAT_RENDER, "Failed to load jump_pad texture");
    }

    mr->jump_pad_buffer = PZ_INVALID_HANDLE;
    mr->jump_pad_vertex_count = 0;

    // Fog shader
    mr->fog_shader = pz_renderer_load_shader(
        renderer, "shaders/fog.vert", "shaders/fog.frag", "fog");
    if (mr->fog_shader == PZ_INVALID_HANDLE) {
        pz_log(PZ_LOG_ERROR, PZ_LOG_CAT_RENDER, "Failed to load fog shader");
        pz_map_renderer_destroy(mr);
        return NULL;
    }

    // Fog pipeline (same vertex layout as ground)
    pz_pipeline_desc fog_desc = {
        .shader = mr->fog_shader,
        .vertex_layout = {
            .attrs = ground_attrs,
            .attr_count = 2,
            .stride = 5 * sizeof(float),
        },
        .blend = PZ_BLEND_ALPHA,
        .depth = PZ_DEPTH_READ,
        .cull = PZ_CULL_BACK,
        .primitive = PZ_PRIMITIVE_TRIANGLES,
    };
    mr->fog_pipeline = pz_renderer_create_pipeline(renderer, &fog_desc);

    // Initialize batch handles
    for (int i = 0; i < MAX_TILE_TEXTURES; i++) {
        mr->ground_batches[i].buffer = PZ_INVALID_HANDLE;
        mr->ground_batches[i].texture = PZ_INVALID_HANDLE;
        mr->ground_batches[i].vertex_count = 0;

        mr->wall_batches[i].buffer = PZ_INVALID_HANDLE;
        mr->wall_batches[i].top_texture = PZ_INVALID_HANDLE;
        mr->wall_batches[i].side_texture = PZ_INVALID_HANDLE;
        mr->wall_batches[i].vertex_count = 0;
    }
    mr->ground_batch_count = 0;
    mr->wall_batch_count = 0;

    mr->ground_blend_buffer = PZ_INVALID_HANDLE;
    mr->ground_blend_vertex_count = 0;
    mr->ground_texture_array = PZ_INVALID_HANDLE;
    mr->ground_texture_array_size = 0;

    mr->water_buffer = PZ_INVALID_HANDLE;
    mr->water_vertex_count = 0;
    mr->fog_buffer = PZ_INVALID_HANDLE;
    mr->fog_vertex_count = 0;

    // Debug line shader and pipeline
    mr->debug_line_shader
        = pz_renderer_load_shader(renderer, "shaders/debug_line_3d.vert",
            "shaders/debug_line_3d.frag", "debug_line_3d");
    if (mr->debug_line_shader == PZ_INVALID_HANDLE) {
        pz_log(PZ_LOG_WARN, PZ_LOG_CAT_RENDER,
            "Failed to load debug_line_3d shader (debug lines disabled)");
    } else {
        pz_vertex_attr debug_attrs[] = {
            { .name = "a_position", .type = PZ_ATTR_FLOAT3, .offset = 0 },
            { .name = "a_color",
                .type = PZ_ATTR_FLOAT4,
                .offset = 3 * sizeof(float) },
        };

        pz_pipeline_desc debug_desc = {
            .shader = mr->debug_line_shader,
            .vertex_layout = {
                .attrs = debug_attrs,
                .attr_count = 2,
                .stride = 7 * sizeof(float),
            },
            .blend = PZ_BLEND_ALPHA,
            .depth = PZ_DEPTH_READ,
            .cull = PZ_CULL_NONE,
            .primitive = PZ_PRIMITIVE_LINES,
        };
        mr->debug_line_pipeline
            = pz_renderer_create_pipeline(renderer, &debug_desc);
    }

    mr->debug_line_buffer = PZ_INVALID_HANDLE;
    mr->debug_line_vertex_count = 0;
    mr->debug_texture_scale_enabled = false;

    pz_log(PZ_LOG_INFO, PZ_LOG_CAT_RENDER, "Map renderer created");
    return mr;
}

void
pz_map_renderer_destroy(pz_map_renderer *mr)
{
    if (!mr) {
        return;
    }

    // Destroy ground batches
    for (int i = 0; i < MAX_TILE_TEXTURES; i++) {
        if (mr->ground_batches[i].buffer != PZ_INVALID_HANDLE) {
            pz_renderer_destroy_buffer(
                mr->renderer, mr->ground_batches[i].buffer);
        }
    }

    // Destroy wall batches
    for (int i = 0; i < MAX_TILE_TEXTURES; i++) {
        if (mr->wall_batches[i].buffer != PZ_INVALID_HANDLE) {
            pz_renderer_destroy_buffer(
                mr->renderer, mr->wall_batches[i].buffer);
        }
    }

    // Destroy water buffer
    if (mr->water_buffer != PZ_INVALID_HANDLE) {
        pz_renderer_destroy_buffer(mr->renderer, mr->water_buffer);
    }
    if (mr->fog_buffer != PZ_INVALID_HANDLE) {
        pz_renderer_destroy_buffer(mr->renderer, mr->fog_buffer);
    }

    // Destroy blended ground resources
    if (mr->ground_blend_buffer != PZ_INVALID_HANDLE) {
        pz_renderer_destroy_buffer(mr->renderer, mr->ground_blend_buffer);
    }
    if (mr->ground_texture_array != PZ_INVALID_HANDLE) {
        pz_renderer_destroy_texture(mr->renderer, mr->ground_texture_array);
    }

    // Pipelines
    if (mr->ground_pipeline != PZ_INVALID_HANDLE) {
        pz_renderer_destroy_pipeline(mr->renderer, mr->ground_pipeline);
    }
    if (mr->ground_blend_pipeline != PZ_INVALID_HANDLE) {
        pz_renderer_destroy_pipeline(mr->renderer, mr->ground_blend_pipeline);
    }
    if (mr->wall_pipeline != PZ_INVALID_HANDLE) {
        pz_renderer_destroy_pipeline(mr->renderer, mr->wall_pipeline);
    }
    if (mr->water_pipeline != PZ_INVALID_HANDLE) {
        pz_renderer_destroy_pipeline(mr->renderer, mr->water_pipeline);
    }
    if (mr->fog_pipeline != PZ_INVALID_HANDLE) {
        pz_renderer_destroy_pipeline(mr->renderer, mr->fog_pipeline);
    }

    // Shaders
    if (mr->ground_shader != PZ_INVALID_HANDLE) {
        pz_renderer_destroy_shader(mr->renderer, mr->ground_shader);
    }
    if (mr->ground_blend_shader != PZ_INVALID_HANDLE) {
        pz_renderer_destroy_shader(mr->renderer, mr->ground_blend_shader);
    }
    if (mr->wall_shader != PZ_INVALID_HANDLE) {
        pz_renderer_destroy_shader(mr->renderer, mr->wall_shader);
    }
    if (mr->water_shader != PZ_INVALID_HANDLE) {
        pz_renderer_destroy_shader(mr->renderer, mr->water_shader);
    }
    if (mr->fog_shader != PZ_INVALID_HANDLE) {
        pz_renderer_destroy_shader(mr->renderer, mr->fog_shader);
    }

    // Debug line resources
    if (mr->debug_line_buffer != PZ_INVALID_HANDLE) {
        pz_renderer_destroy_buffer(mr->renderer, mr->debug_line_buffer);
    }
    if (mr->debug_line_pipeline != PZ_INVALID_HANDLE) {
        pz_renderer_destroy_pipeline(mr->renderer, mr->debug_line_pipeline);
    }
    if (mr->debug_line_shader != PZ_INVALID_HANDLE) {
        pz_renderer_destroy_shader(mr->renderer, mr->debug_line_shader);
    }

    pz_free(mr);
}

void
pz_map_renderer_set_map(pz_map_renderer *mr, const pz_map *map)
{
    if (!mr || !map) {
        return;
    }

    mr->map = map;

    // Destroy old debug line buffer (will be regenerated on next draw if
    // enabled)
    if (mr->debug_line_buffer != PZ_INVALID_HANDLE) {
        pz_renderer_destroy_buffer(mr->renderer, mr->debug_line_buffer);
        mr->debug_line_buffer = PZ_INVALID_HANDLE;
        mr->debug_line_vertex_count = 0;
    }

    // Destroy old blended ground resources
    if (mr->ground_blend_buffer != PZ_INVALID_HANDLE) {
        pz_renderer_destroy_buffer(mr->renderer, mr->ground_blend_buffer);
        mr->ground_blend_buffer = PZ_INVALID_HANDLE;
        mr->ground_blend_vertex_count = 0;
    }
    if (mr->ground_texture_array != PZ_INVALID_HANDLE) {
        pz_renderer_destroy_texture(mr->renderer, mr->ground_texture_array);
        mr->ground_texture_array = PZ_INVALID_HANDLE;
        mr->ground_texture_array_size = 0;
    }

    // Destroy old ground batches
    for (int i = 0; i < MAX_TILE_TEXTURES; i++) {
        if (mr->ground_batches[i].buffer != PZ_INVALID_HANDLE) {
            pz_renderer_destroy_buffer(
                mr->renderer, mr->ground_batches[i].buffer);
            mr->ground_batches[i].buffer = PZ_INVALID_HANDLE;
        }
        mr->ground_batches[i].vertex_count = 0;
        mr->ground_batches[i].texture = PZ_INVALID_HANDLE;
    }
    mr->ground_batch_count = 0;

    // Destroy old wall batches
    for (int i = 0; i < MAX_TILE_TEXTURES; i++) {
        if (mr->wall_batches[i].buffer != PZ_INVALID_HANDLE) {
            pz_renderer_destroy_buffer(
                mr->renderer, mr->wall_batches[i].buffer);
            mr->wall_batches[i].buffer = PZ_INVALID_HANDLE;
        }
        mr->wall_batches[i].vertex_count = 0;
        mr->wall_batches[i].top_texture = PZ_INVALID_HANDLE;
        mr->wall_batches[i].side_texture = PZ_INVALID_HANDLE;
    }
    mr->wall_batch_count = 0;

    // Count tiles and wall faces per tile_def index
    int ground_counts[MAX_TILE_TEXTURES] = { 0 };
    int wall_face_counts[MAX_TILE_TEXTURES] = { 0 };

    for (int y = 0; y < map->height; y++) {
        for (int x = 0; x < map->width; x++) {
            uint8_t idx = pz_map_get_tile_index(map, x, y);
            if (idx >= MAX_TILE_TEXTURES)
                continue;

            ground_counts[idx]++;

            int8_t h = pz_map_get_height(map, x, y);
            // Count wall faces - any tile that is higher than a neighbor needs
            // walls
            wall_face_counts[idx] += count_wall_faces_for_tile(x, y, h, map);
        }
    }

    // Allocate vertex arrays for each tile type (ground and wall)
    float *ground_verts[MAX_TILE_TEXTURES] = { NULL };
    float *ground_ptrs[MAX_TILE_TEXTURES] = { NULL };
    float *wall_verts[MAX_TILE_TEXTURES] = { NULL };
    float *wall_ptrs[MAX_TILE_TEXTURES] = { NULL };

    for (int i = 0; i < map->tile_def_count && i < MAX_TILE_TEXTURES; i++) {
        if (ground_counts[i] > 0) {
            ground_verts[i]
                = pz_alloc(ground_counts[i] * 6 * 5 * sizeof(float));
            ground_ptrs[i] = ground_verts[i];
        }
        if (wall_face_counts[i] > 0) {
            wall_verts[i] = pz_alloc(
                wall_face_counts[i] * 6 * WALL_VERTEX_SIZE * sizeof(float));
            wall_ptrs[i] = wall_verts[i];
        }
    }

    float half_w = map->world_width / 2.0f;
    float half_h = map->world_height / 2.0f;

    // Pre-compute texture scales and blend flags for each tile type
    int ground_scales[MAX_TILE_TEXTURES];
    int wall_scales[MAX_TILE_TEXTURES];
    bool tile_blends[MAX_TILE_TEXTURES];
    for (int i = 0; i < MAX_TILE_TEXTURES; i++) {
        ground_scales[i] = 1;
        wall_scales[i] = 1;
        tile_blends[i] = false;
    }
    for (int i = 0; i < map->tile_def_count && i < MAX_TILE_TEXTURES; i++) {
        const pz_tile_def *def = &map->tile_defs[i];
        const pz_tile_config *config = get_tile_config(mr, def->name);
        if (config) {
            ground_scales[i] = config->ground_texture_scale;
            wall_scales[i] = config->wall_texture_scale;
            tile_blends[i] = config->blend;
            pz_log(PZ_LOG_DEBUG, PZ_LOG_CAT_RENDER,
                "Tile %d (%s): ground_scale=%d, wall_scale=%d, blend=%s", i,
                def->name, ground_scales[i], wall_scales[i],
                tile_blends[i] ? "yes" : "no");
        }
    }

    // Generate ground and wall vertices per tile type
    for (int y = 0; y < map->height; y++) {
        for (int x = 0; x < map->width; x++) {
            uint8_t idx = pz_map_get_tile_index(map, x, y);
            if (idx >= MAX_TILE_TEXTURES)
                continue;

            float x0 = x * map->tile_size - half_w;
            float x1 = (x + 1) * map->tile_size - half_w;
            float z0 = y * map->tile_size - half_h;
            float z1 = (y + 1) * map->tile_size - half_h;

            int8_t h = pz_map_get_height(map, x, y);

            // Ground
            if (ground_ptrs[idx]) {
                float ground_y = GROUND_Y_OFFSET;
                if (h < 0) {
                    ground_y = GROUND_Y_OFFSET + h * WALL_HEIGHT_UNIT;
                }
                ground_ptrs[idx]
                    = emit_ground_quad_at_height(ground_ptrs[idx], x0, z0, x1,
                        z1, ground_y, x, y, map->height, ground_scales[idx]);
            }

            // Walls - emit for any tile that has neighbors at different heights
            // A tile emits walls on sides where it's HIGHER than the neighbor
            if (wall_ptrs[idx]) {
                // Check if this tile has any exposed walls (is higher than any
                // neighbor). Out-of-bounds neighbors are treated as very low
                // (-128) so map edges always generate walls down to the void.
                bool has_walls = false;
                bool has_edge = false;
                int8_t left_h, right_h, front_h, back_h;
                if (pz_map_in_bounds(map, x - 1, y)) {
                    left_h = pz_map_get_height(map, x - 1, y);
                } else {
                    left_h = INT8_MIN;
                    has_edge = true;
                }
                if (pz_map_in_bounds(map, x + 1, y)) {
                    right_h = pz_map_get_height(map, x + 1, y);
                } else {
                    right_h = INT8_MIN;
                    has_edge = true;
                }
                if (pz_map_in_bounds(map, x, y + 1)) {
                    front_h = pz_map_get_height(map, x, y + 1);
                } else {
                    front_h = INT8_MIN;
                    has_edge = true;
                }
                if (pz_map_in_bounds(map, x, y - 1)) {
                    back_h = pz_map_get_height(map, x, y - 1);
                } else {
                    back_h = INT8_MIN;
                    has_edge = true;
                }

                if (left_h < h || right_h < h || front_h < h || back_h < h) {
                    has_walls = true;
                }

                if (has_walls) {
                    // Find the lowest IN-BOUNDS neighbor to determine wall
                    // extent. Edge neighbors (INT8_MIN) are excluded - they
                    // get clamped separately so they don't affect interior
                    // walls.
                    int8_t min_neighbor = h; // Start with own height
                    if (left_h != INT8_MIN && left_h < min_neighbor)
                        min_neighbor = left_h;
                    if (right_h != INT8_MIN && right_h < min_neighbor)
                        min_neighbor = right_h;
                    if (front_h != INT8_MIN && front_h < min_neighbor)
                        min_neighbor = front_h;
                    if (back_h != INT8_MIN && back_h < min_neighbor)
                        min_neighbor = back_h;

                    // For edge tiles, ensure walls extend at least 1 unit below
                    // ground level to cover the void
                    if (has_edge && min_neighbor > -1)
                        min_neighbor = -1;

                    // Wall goes from min_neighbor level to this tile's level
                    float y_bottom
                        = GROUND_Y_OFFSET + min_neighbor * WALL_HEIGHT_UNIT;
                    float y_top = GROUND_Y_OFFSET + h * WALL_HEIGHT_UNIT;

                    // Only emit top face if this tile is above ground (h > 0)
                    bool emit_top = (h > 0);

                    wall_ptrs[idx] = emit_wall_sides(wall_ptrs[idx], x0, z0, x1,
                        z1, y_bottom, y_top, x, y, h, map, wall_scales[idx],
                        emit_top);
                }
            }
        }
    }

    // DEBUG: Print first few vertices of first ground batch
    for (int i = 0; i < map->tile_def_count && i < MAX_TILE_TEXTURES; i++) {
        if (ground_verts[i] && ground_counts[i] >= 2) {
            float *v = ground_verts[i];
            // Print first tile (6 verts = 30 floats) and second tile
            pz_log(PZ_LOG_DEBUG, PZ_LOG_CAT_RENDER,
                "Ground batch %d tile0 v0: pos=(%.2f,%.2f,%.2f) uv=(%.4f,%.4f)",
                i, v[0], v[1], v[2], v[3], v[4]);
            pz_log(PZ_LOG_DEBUG, PZ_LOG_CAT_RENDER,
                "Ground batch %d tile0 v5: pos=(%.2f,%.2f,%.2f) uv=(%.4f,%.4f)",
                i, v[25], v[26], v[27], v[28], v[29]);
            // Second tile starts at index 30
            pz_log(PZ_LOG_DEBUG, PZ_LOG_CAT_RENDER,
                "Ground batch %d tile1 v0: pos=(%.2f,%.2f,%.2f) uv=(%.4f,%.4f)",
                i, v[30], v[31], v[32], v[33], v[34]);
            break; // Only first batch
        }
    }

    // Create GPU buffers for ground and wall batches
    for (int i = 0; i < map->tile_def_count && i < MAX_TILE_TEXTURES; i++) {
        const pz_tile_def *def = &map->tile_defs[i];
        const pz_tile_config *config = get_tile_config(mr, def->name);

        // Ground batch
        if (ground_counts[i] > 0 && ground_verts[i]) {
            int num_verts = ground_counts[i] * 6;

            ground_batch *batch = &mr->ground_batches[mr->ground_batch_count++];
            batch->vertex_count = num_verts;
            batch->texture = config
                ? config->ground_texture
                : pz_tile_registry_get_fallback(mr->tile_registry)
                      ->ground_texture;

            pz_buffer_desc desc = {
                .type = PZ_BUFFER_VERTEX,
                .usage = PZ_BUFFER_STATIC,
                .data = ground_verts[i],
                .size = num_verts * 5 * sizeof(float),
            };
            batch->buffer = pz_renderer_create_buffer(mr->renderer, &desc);

            pz_free(ground_verts[i]);
        }

        // Wall batch
        if (wall_face_counts[i] > 0 && wall_verts[i]) {
            int actual_floats = (int)(wall_ptrs[i] - wall_verts[i]);
            int actual_verts = actual_floats / WALL_VERTEX_SIZE;

            if (actual_verts > 0) {
                wall_batch *batch = &mr->wall_batches[mr->wall_batch_count++];
                batch->vertex_count = actual_verts;

                if (config) {
                    batch->top_texture = config->wall_texture;
                    batch->side_texture = config->wall_side_texture;
                } else {
                    const pz_tile_config *fallback
                        = pz_tile_registry_get_fallback(mr->tile_registry);
                    batch->top_texture = fallback->wall_texture;
                    batch->side_texture = fallback->wall_side_texture;
                }

                pz_buffer_desc desc = {
                    .type = PZ_BUFFER_VERTEX,
                    .usage = PZ_BUFFER_STATIC,
                    .data = wall_verts[i],
                    .size = actual_verts * WALL_VERTEX_SIZE * sizeof(float),
                };
                batch->buffer = pz_renderer_create_buffer(mr->renderer, &desc);
            }

            pz_free(wall_verts[i]);
        }
    }

    // ========================================================================
    // Generate blended ground mesh with texture array
    // ========================================================================

    // Step 1: Build texture array from all ground textures used by this map
    // We need consistent texture sizes, so find the first valid texture size
    int tex_array_width = 0;
    int tex_array_height = 0;

    // First pass: find texture dimensions from first valid texture
    for (int i = 0; i < map->tile_def_count && i < MAX_TILE_TEXTURES; i++) {
        const pz_tile_def *def = &map->tile_defs[i];
        const pz_tile_config *config = get_tile_config(mr, def->name);
        if (config && config->ground_texture != PZ_INVALID_HANDLE) {
            int w, h;
            if (pz_texture_get_size(
                    mr->tex_manager, config->ground_texture, &w, &h)) {
                tex_array_width = w;
                tex_array_height = h;
                break;
            }
        }
    }

    // If we found valid textures, create the texture array
    if (tex_array_width > 0 && tex_array_height > 0
        && map->tile_def_count > 0) {
        int num_layers = map->tile_def_count;
        const void **layer_data = pz_alloc(num_layers * sizeof(void *));
        unsigned char **loaded_images = pz_alloc(num_layers * sizeof(void *));

        for (int i = 0; i < num_layers; i++) {
            layer_data[i] = NULL;
            loaded_images[i] = NULL;

            const pz_tile_def *def = &map->tile_defs[i];
            const pz_tile_config *config = get_tile_config(mr, def->name);
            if (config && config->ground_texture_path[0] != '\0') {
                // Load the texture directly into RGBA data
                int w, h, ch;
                unsigned char *img_data
                    = pz_image_load(config->ground_texture_path, &w, &h, &ch);
                if (img_data) {
                    // Check if size matches
                    if (w == tex_array_width && h == tex_array_height) {
                        layer_data[i] = img_data;
                        loaded_images[i] = img_data;
                    } else {
                        pz_log(PZ_LOG_WARN, PZ_LOG_CAT_RENDER,
                            "Texture %s size mismatch (%dx%d vs %dx%d)",
                            config->ground_texture_path, w, h, tex_array_width,
                            tex_array_height);
                        pz_image_free(img_data);
                    }
                }
            }
        }

        // Create the texture array
        mr->ground_texture_array = pz_renderer_create_texture_array(
            mr->renderer, tex_array_width, tex_array_height, num_layers,
            layer_data, PZ_FILTER_LINEAR, PZ_WRAP_REPEAT);
        mr->ground_texture_array_size = tex_array_width;

        // Free loaded images
        for (int i = 0; i < num_layers; i++) {
            if (loaded_images[i]) {
                pz_image_free(loaded_images[i]);
            }
        }
        pz_free(loaded_images);
        pz_free(layer_data);

        if (mr->ground_texture_array != PZ_INVALID_HANDLE) {
            pz_log(PZ_LOG_INFO, PZ_LOG_CAT_RENDER,
                "Created ground texture array: %dx%d, %d layers",
                tex_array_width, tex_array_height, num_layers);
        }
    }

    // Step 2: Create unified blended ground mesh
    // Count total ground tiles
    int total_ground_tiles = 0;
    for (int y = 0; y < map->height; y++) {
        for (int x = 0; x < map->width; x++) {
            total_ground_tiles++;
        }
    }

    if (total_ground_tiles > 0
        && mr->ground_texture_array != PZ_INVALID_HANDLE) {
        int num_verts = total_ground_tiles * 6;
        float *blend_verts
            = pz_alloc(num_verts * BLEND_GROUND_VERTEX_SIZE * sizeof(float));
        float *bv = blend_verts;

        for (int y = 0; y < map->height; y++) {
            for (int x = 0; x < map->width; x++) {
                uint8_t idx = pz_map_get_tile_index(map, x, y);
                int8_t h = pz_map_get_height(map, x, y);

                float x0 = x * map->tile_size - half_w;
                float x1 = (x + 1) * map->tile_size - half_w;
                float z0 = y * map->tile_size - half_h;
                float z1 = (y + 1) * map->tile_size - half_h;

                float ground_y = GROUND_Y_OFFSET;
                if (h < 0) {
                    ground_y = GROUND_Y_OFFSET + h * WALL_HEIGHT_UNIT;
                } else if (h > 0) {
                    // Elevated tile - render at top of wall
                    ground_y = GROUND_Y_OFFSET + h * WALL_HEIGHT_UNIT;
                }

                // Get texture scale for UVs
                // Add 0.5 tile offset to shift texture wrap away from tile
                // boundaries This makes any texture seams fall in the middle
                // of tiles rather than at edges
                int scale = ground_scales[idx];
                float inv_scale = 1.0f / (float)scale;
                float uv_offset = 0.5f * inv_scale;
                float u0 = (float)x * inv_scale + uv_offset;
                float u1 = (float)(x + 1) * inv_scale + uv_offset;
                float v0 = (float)y * inv_scale + uv_offset;
                float v1 = (float)(y + 1) * inv_scale + uv_offset;

                // Determine neighbor indices for blending
                // -1 means "no blend" (same terrain or height differs)
                // Blending occurs when at least one of the two adjacent tiles
                // has blend=true.
                float n_north = -1.0f;
                float n_east = -1.0f;
                float n_south = -1.0f;
                float n_west = -1.0f;

                bool this_blends = tile_blends[idx];

                // North neighbor (y - 1)
                if (pz_map_in_bounds(map, x, y - 1)) {
                    int8_t n_h = pz_map_get_height(map, x, y - 1);
                    uint8_t n_idx = pz_map_get_tile_index(map, x, y - 1);
                    bool neighbor_blends
                        = (n_idx < MAX_TILE_TEXTURES) && tile_blends[n_idx];
                    if (n_h == h && n_idx != idx
                        && (this_blends || neighbor_blends)) {
                        n_north = (float)n_idx;
                    }
                }

                // East neighbor (x + 1)
                if (pz_map_in_bounds(map, x + 1, y)) {
                    int8_t n_h = pz_map_get_height(map, x + 1, y);
                    uint8_t n_idx = pz_map_get_tile_index(map, x + 1, y);
                    bool neighbor_blends
                        = (n_idx < MAX_TILE_TEXTURES) && tile_blends[n_idx];
                    if (n_h == h && n_idx != idx
                        && (this_blends || neighbor_blends)) {
                        n_east = (float)n_idx;
                    }
                }

                // South neighbor (y + 1)
                if (pz_map_in_bounds(map, x, y + 1)) {
                    int8_t n_h = pz_map_get_height(map, x, y + 1);
                    uint8_t n_idx = pz_map_get_tile_index(map, x, y + 1);
                    bool neighbor_blends
                        = (n_idx < MAX_TILE_TEXTURES) && tile_blends[n_idx];
                    if (n_h == h && n_idx != idx
                        && (this_blends || neighbor_blends)) {
                        n_south = (float)n_idx;
                    }
                }

                // West neighbor (x - 1)
                if (pz_map_in_bounds(map, x - 1, y)) {
                    int8_t n_h = pz_map_get_height(map, x - 1, y);
                    uint8_t n_idx = pz_map_get_tile_index(map, x - 1, y);
                    bool neighbor_blends
                        = (n_idx < MAX_TILE_TEXTURES) && tile_blends[n_idx];
                    if (n_h == h && n_idx != idx
                        && (this_blends || neighbor_blends)) {
                        n_west = (float)n_idx;
                    }
                }

                // Emit 6 vertices (2 triangles) for this tile
                // Vertex format: pos(3) + uv(2) + terrain_idx(1) + neighbors(4)
#define EMIT_BLEND_VERTEX(px, py, pz, pu, pv)                                  \
    *bv++ = (px);                                                              \
    *bv++ = (py);                                                              \
    *bv++ = (pz);                                                              \
    *bv++ = (pu);                                                              \
    *bv++ = (pv);                                                              \
    *bv++ = (float)idx;                                                        \
    *bv++ = n_north;                                                           \
    *bv++ = n_east;                                                            \
    *bv++ = n_south;                                                           \
    *bv++ = n_west;

                // Triangle 1 (CCW: v0-v1-v2)
                EMIT_BLEND_VERTEX(x0, ground_y, z0, u0, v0); // top-left
                EMIT_BLEND_VERTEX(x0, ground_y, z1, u0, v1); // bottom-left
                EMIT_BLEND_VERTEX(x1, ground_y, z1, u1, v1); // bottom-right

                // Triangle 2 (CCW: v0-v2-v3)
                EMIT_BLEND_VERTEX(x0, ground_y, z0, u0, v0); // top-left
                EMIT_BLEND_VERTEX(x1, ground_y, z1, u1, v1); // bottom-right
                EMIT_BLEND_VERTEX(x1, ground_y, z0, u1, v0); // top-right

#undef EMIT_BLEND_VERTEX
            }
        }

        mr->ground_blend_vertex_count = num_verts;

        pz_buffer_desc desc = {
            .type = PZ_BUFFER_VERTEX,
            .usage = PZ_BUFFER_STATIC,
            .data = blend_verts,
            .size = num_verts * BLEND_GROUND_VERTEX_SIZE * sizeof(float),
        };
        mr->ground_blend_buffer
            = pz_renderer_create_buffer(mr->renderer, &desc);

        pz_free(blend_verts);

        pz_log(PZ_LOG_INFO, PZ_LOG_CAT_RENDER,
            "Created blended ground mesh: %d vertices", num_verts);
    }

    // Generate water mesh (for tiles at or below water_level)
    if (mr->water_buffer != PZ_INVALID_HANDLE) {
        pz_renderer_destroy_buffer(mr->renderer, mr->water_buffer);
        mr->water_buffer = PZ_INVALID_HANDLE;
    }
    mr->water_vertex_count = 0;

    if (map->has_water) {
        int water_tile_count = 0;
        for (int y = 0; y < map->height; y++) {
            for (int x = 0; x < map->width; x++) {
                int8_t h = pz_map_get_height(map, x, y);
                if (h < map->water_level) {
                    water_tile_count++;
                }
            }
        }

        if (water_tile_count > 0) {
            int num_verts = water_tile_count * 6;
            float *water_verts_data = pz_alloc(num_verts * 5 * sizeof(float));
            float *water_ptr = water_verts_data;

            float water_y = GROUND_Y_OFFSET
                + map->water_level * WALL_HEIGHT_UNIT + WATER_Y_OFFSET;

            for (int y = 0; y < map->height; y++) {
                for (int x = 0; x < map->width; x++) {
                    int8_t h = pz_map_get_height(map, x, y);
                    if (h < map->water_level) {
                        float x0 = x * map->tile_size - half_w;
                        float x1 = (x + 1) * map->tile_size - half_w;
                        float z0 = y * map->tile_size - half_h;
                        float z1 = (y + 1) * map->tile_size - half_h;

                        water_ptr = emit_plane_quad(
                            water_ptr, x0, z0, x1, z1, water_y);
                    }
                }
            }

            mr->water_vertex_count = num_verts;

            pz_buffer_desc desc = {
                .type = PZ_BUFFER_VERTEX,
                .usage = PZ_BUFFER_STATIC,
                .data = water_verts_data,
                .size = num_verts * 5 * sizeof(float),
            };
            mr->water_buffer = pz_renderer_create_buffer(mr->renderer, &desc);

            pz_free(water_verts_data);
        }
    }

    // Generate fog mesh (for tiles at or below fog_level)
    if (mr->fog_buffer != PZ_INVALID_HANDLE) {
        pz_renderer_destroy_buffer(mr->renderer, mr->fog_buffer);
        mr->fog_buffer = PZ_INVALID_HANDLE;
    }
    mr->fog_vertex_count = 0;

    if (map->has_fog) {
        int fog_tile_count = 0;
        for (int y = 0; y < map->height; y++) {
            for (int x = 0; x < map->width; x++) {
                int8_t h = pz_map_get_height(map, x, y);
                if (h <= map->fog_level) {
                    fog_tile_count++;
                }
            }
        }

        if (fog_tile_count > 0) {
            int num_verts = fog_tile_count * 6;
            float *fog_verts_data = pz_alloc(num_verts * 5 * sizeof(float));
            float *fog_ptr = fog_verts_data;

            float fog_y = GROUND_Y_OFFSET + map->fog_level * WALL_HEIGHT_UNIT
                + FOG_Y_OFFSET;

            for (int y = 0; y < map->height; y++) {
                for (int x = 0; x < map->width; x++) {
                    int8_t h = pz_map_get_height(map, x, y);
                    if (h <= map->fog_level) {
                        float x0 = x * map->tile_size - half_w;
                        float x1 = (x + 1) * map->tile_size - half_w;
                        float z0 = y * map->tile_size - half_h;
                        float z1 = (y + 1) * map->tile_size - half_h;

                        fog_ptr
                            = emit_plane_quad(fog_ptr, x0, z0, x1, z1, fog_y);
                    }
                }
            }

            mr->fog_vertex_count = num_verts;

            pz_buffer_desc desc = {
                .type = PZ_BUFFER_VERTEX,
                .usage = PZ_BUFFER_STATIC,
                .data = fog_verts_data,
                .size = num_verts * 5 * sizeof(float),
            };
            mr->fog_buffer = pz_renderer_create_buffer(mr->renderer, &desc);

            pz_free(fog_verts_data);
        }
    }

    // Count total wall verts for logging
    int total_wall_verts = 0;
    for (int i = 0; i < mr->wall_batch_count; i++) {
        total_wall_verts += mr->wall_batches[i].vertex_count;
    }

    // Generate jump pad mesh
    if (mr->jump_pad_buffer != PZ_INVALID_HANDLE) {
        pz_renderer_destroy_buffer(mr->renderer, mr->jump_pad_buffer);
        mr->jump_pad_buffer = PZ_INVALID_HANDLE;
    }
    mr->jump_pad_vertex_count = 0;

    if (map->jump_pad_count > 0) {
        // Each jump pad link has 2 tiles (start and landing), each 6 verts
        int num_tiles = map->jump_pad_count * 2;
        int num_verts = num_tiles * 6;
        float *jump_pad_verts = pz_alloc(num_verts * 5 * sizeof(float));
        float *jp_ptr = jump_pad_verts;

        for (int i = 0; i < map->jump_pad_count; i++) {
            const struct pz_jump_pad_link *link = &map->jump_pads[i];

            // Calculate direction from start to landing for rotation
            int dx = link->landing_x - link->start_x;
            int dy = link->landing_y - link->start_y;

            // Rotation mapping (as displayed on screen with top-down camera):
            // 0=down(+Z), 1=left(-X), 2=up(-Z), 3=right(+X)
            // Each pad's arrow should point TOWARD the other pad
            int landing_rotation;
            if (abs(dx) >= abs(dy)) {
                // Horizontal: landing is right of start when dx>0
                // Landing arrow should point back left (rotation 1) if dx>0
                landing_rotation = (dx > 0) ? 1 : 3; // Point back left/right
            } else {
                // Vertical: landing is below start when dy>0 (larger Z)
                // Landing arrow should point back up (rotation 2) if below
                landing_rotation = (dy > 0) ? 2 : 0; // Point back up/down
            }
            // Start pad points toward landing (opposite of landing's direction)
            int start_rotation = (landing_rotation + 2) & 3;

            // Start pad - render as ground decal at tile height
            {
                int tx = link->start_x;
                int ty = link->start_y;
                int8_t h = pz_map_get_height(map, tx, ty);
                // Match ground height formula: only negative heights lower the
                // ground
                float ground_y = GROUND_Y_OFFSET;
                if (h != 0) {
                    ground_y = GROUND_Y_OFFSET + h * WALL_HEIGHT_UNIT;
                }
                float pad_y = ground_y + 0.01f; // Small decal offset

                float x0 = tx * map->tile_size - half_w;
                float x1 = (tx + 1) * map->tile_size - half_w;
                float z0 = ty * map->tile_size - half_h;
                float z1 = (ty + 1) * map->tile_size - half_h;

                jp_ptr = emit_plane_quad_rotated(
                    jp_ptr, x0, z0, x1, z1, pad_y, start_rotation);
            }

            // Landing pad - render as ground decal at tile height
            {
                int tx = link->landing_x;
                int ty = link->landing_y;
                int8_t h = pz_map_get_height(map, tx, ty);
                // Match ground height formula
                float ground_y = GROUND_Y_OFFSET;
                if (h != 0) {
                    ground_y = GROUND_Y_OFFSET + h * WALL_HEIGHT_UNIT;
                }
                float pad_y = ground_y + 0.01f; // Small decal offset

                float x0 = tx * map->tile_size - half_w;
                float x1 = (tx + 1) * map->tile_size - half_w;
                float z0 = ty * map->tile_size - half_h;
                float z1 = (ty + 1) * map->tile_size - half_h;

                jp_ptr = emit_plane_quad_rotated(
                    jp_ptr, x0, z0, x1, z1, pad_y, landing_rotation);
            }
        }

        mr->jump_pad_vertex_count = num_verts;

        pz_buffer_desc desc = {
            .type = PZ_BUFFER_VERTEX,
            .usage = PZ_BUFFER_STATIC,
            .data = jump_pad_verts,
            .size = num_verts * 5 * sizeof(float),
        };
        mr->jump_pad_buffer = pz_renderer_create_buffer(mr->renderer, &desc);

        pz_free(jump_pad_verts);

        pz_log(PZ_LOG_INFO, PZ_LOG_CAT_RENDER,
            "Jump pad mesh generated: %d pads, %d verts", map->jump_pad_count,
            num_verts);
    }

    pz_log(PZ_LOG_INFO, PZ_LOG_CAT_RENDER,
        "Map mesh generated: %d ground batches, %d wall batches (%d verts), %d "
        "water verts, %d fog verts",
        mr->ground_batch_count, mr->wall_batch_count, total_wall_verts,
        mr->water_vertex_count, mr->fog_vertex_count);
}

void
pz_map_renderer_draw_ground(pz_map_renderer *mr, const pz_mat4 *view_projection,
    const pz_map_render_params *params)
{
    if (!mr || !mr->map) {
        return;
    }

    // Use blended ground if available
    if (mr->ground_blend_buffer != PZ_INVALID_HANDLE
        && mr->ground_texture_array != PZ_INVALID_HANDLE
        && mr->ground_blend_vertex_count > 0) {

        pz_renderer_set_uniform_mat4(
            mr->renderer, mr->ground_blend_shader, "u_mvp", view_projection);

        // Bind texture array to slot 0
        pz_renderer_bind_texture(mr->renderer, 0, mr->ground_texture_array);

        // Track texture
        if (params && params->track_texture != PZ_INVALID_HANDLE
            && params->track_texture != 0) {
            pz_renderer_bind_texture(mr->renderer, 1, params->track_texture);
            pz_renderer_set_uniform_int(
                mr->renderer, mr->ground_blend_shader, "u_use_tracks", 1);
            pz_renderer_set_uniform_vec2(mr->renderer, mr->ground_blend_shader,
                "u_track_scale",
                (pz_vec2) { params->track_scale_x, params->track_scale_z });
            pz_renderer_set_uniform_vec2(mr->renderer, mr->ground_blend_shader,
                "u_track_offset",
                (pz_vec2) { params->track_offset_x, params->track_offset_z });
        } else {
            pz_renderer_set_uniform_int(
                mr->renderer, mr->ground_blend_shader, "u_use_tracks", 0);
        }

        // Light map
        if (params && params->light_texture != PZ_INVALID_HANDLE
            && params->light_texture != 0) {
            pz_renderer_bind_texture(mr->renderer, 2, params->light_texture);
            pz_renderer_set_uniform_int(
                mr->renderer, mr->ground_blend_shader, "u_use_lighting", 1);
            pz_renderer_set_uniform_vec2(mr->renderer, mr->ground_blend_shader,
                "u_light_scale",
                (pz_vec2) { params->light_scale_x, params->light_scale_z });
            pz_renderer_set_uniform_vec2(mr->renderer, mr->ground_blend_shader,
                "u_light_offset",
                (pz_vec2) { params->light_offset_x, params->light_offset_z });
        } else {
            pz_renderer_set_uniform_int(
                mr->renderer, mr->ground_blend_shader, "u_use_lighting", 0);
        }

        // Sun lighting
        if (params && params->has_sun) {
            pz_renderer_set_uniform_int(
                mr->renderer, mr->ground_blend_shader, "u_has_sun", 1);
            pz_renderer_set_uniform_vec3(mr->renderer, mr->ground_blend_shader,
                "u_sun_direction", params->sun_direction);
            pz_renderer_set_uniform_vec3(mr->renderer, mr->ground_blend_shader,
                "u_sun_color", params->sun_color);
        } else {
            pz_renderer_set_uniform_int(
                mr->renderer, mr->ground_blend_shader, "u_has_sun", 0);
        }

        // Blend parameters
        pz_renderer_set_uniform_float(
            mr->renderer, mr->ground_blend_shader, "u_blend_sharpness", 0.3f);
        pz_renderer_set_uniform_float(
            mr->renderer, mr->ground_blend_shader, "u_noise_scale", 0.8f);
        pz_renderer_set_uniform_float(mr->renderer, mr->ground_blend_shader,
            "u_tile_size", mr->map->tile_size);
        // Map offset: half of world size to align grid
        pz_renderer_set_uniform_vec2(mr->renderer, mr->ground_blend_shader,
            "u_map_offset",
            (pz_vec2) {
                mr->map->world_width / 2.0f, mr->map->world_height / 2.0f });

        // Draw unified blended ground mesh
        pz_draw_cmd cmd = {
            .pipeline = mr->ground_blend_pipeline,
            .vertex_buffer = mr->ground_blend_buffer,
            .vertex_count = mr->ground_blend_vertex_count,
        };
        pz_renderer_draw(mr->renderer, &cmd);
        return;
    }

    // Fallback: use old batched rendering
    pz_renderer_set_uniform_mat4(
        mr->renderer, mr->ground_shader, "u_mvp", view_projection);
    pz_renderer_set_uniform_int(
        mr->renderer, mr->ground_shader, "u_texture", 0);

    // Track texture
    if (params && params->track_texture != PZ_INVALID_HANDLE
        && params->track_texture != 0) {
        pz_renderer_bind_texture(mr->renderer, 1, params->track_texture);
        pz_renderer_set_uniform_int(
            mr->renderer, mr->ground_shader, "u_track_texture", 1);
        pz_renderer_set_uniform_int(
            mr->renderer, mr->ground_shader, "u_use_tracks", 1);
        pz_renderer_set_uniform_vec2(mr->renderer, mr->ground_shader,
            "u_track_scale",
            (pz_vec2) { params->track_scale_x, params->track_scale_z });
        pz_renderer_set_uniform_vec2(mr->renderer, mr->ground_shader,
            "u_track_offset",
            (pz_vec2) { params->track_offset_x, params->track_offset_z });
    } else {
        pz_renderer_set_uniform_int(
            mr->renderer, mr->ground_shader, "u_use_tracks", 0);
    }

    // Light map
    if (params && params->light_texture != PZ_INVALID_HANDLE
        && params->light_texture != 0) {
        pz_renderer_bind_texture(mr->renderer, 2, params->light_texture);
        pz_renderer_set_uniform_int(
            mr->renderer, mr->ground_shader, "u_light_texture", 2);
        pz_renderer_set_uniform_int(
            mr->renderer, mr->ground_shader, "u_use_lighting", 1);
        pz_renderer_set_uniform_vec2(mr->renderer, mr->ground_shader,
            "u_light_scale",
            (pz_vec2) { params->light_scale_x, params->light_scale_z });
        pz_renderer_set_uniform_vec2(mr->renderer, mr->ground_shader,
            "u_light_offset",
            (pz_vec2) { params->light_offset_x, params->light_offset_z });
    } else {
        pz_renderer_set_uniform_int(
            mr->renderer, mr->ground_shader, "u_use_lighting", 0);
    }

    // Sun lighting
    if (params && params->has_sun) {
        pz_renderer_set_uniform_int(
            mr->renderer, mr->ground_shader, "u_has_sun", 1);
        pz_renderer_set_uniform_vec3(mr->renderer, mr->ground_shader,
            "u_sun_direction", params->sun_direction);
        pz_renderer_set_uniform_vec3(
            mr->renderer, mr->ground_shader, "u_sun_color", params->sun_color);
    } else {
        pz_renderer_set_uniform_int(
            mr->renderer, mr->ground_shader, "u_has_sun", 0);
    }

    // Draw all ground batches
    for (int i = 0; i < mr->ground_batch_count; i++) {
        ground_batch *batch = &mr->ground_batches[i];

        if (batch->vertex_count > 0 && batch->buffer != PZ_INVALID_HANDLE
            && batch->texture != PZ_INVALID_HANDLE) {

            pz_renderer_bind_texture(mr->renderer, 0, batch->texture);

            pz_draw_cmd cmd = {
                .pipeline = mr->ground_pipeline,
                .vertex_buffer = batch->buffer,
                .vertex_count = batch->vertex_count,
            };
            pz_renderer_draw(mr->renderer, &cmd);
        }
    }
}

void
pz_map_renderer_draw_walls(pz_map_renderer *mr, const pz_mat4 *view_projection,
    const pz_map_render_params *params)
{
    if (!mr || !mr->map || mr->wall_batch_count == 0) {
        return;
    }

    pz_mat4 model = pz_mat4_identity();

    pz_renderer_set_uniform_mat4(
        mr->renderer, mr->wall_shader, "u_mvp", view_projection);
    pz_renderer_set_uniform_mat4(
        mr->renderer, mr->wall_shader, "u_model", &model);

    pz_vec3 light_dir = { 0.4f, 0.8f, 0.3f };
    pz_vec3 light_color = { 0.6f, 0.6f, 0.55f };
    pz_vec3 ambient = { 0.15f, 0.15f, 0.18f };

    pz_renderer_set_uniform_vec3(
        mr->renderer, mr->wall_shader, "u_light_dir", light_dir);
    pz_renderer_set_uniform_vec3(
        mr->renderer, mr->wall_shader, "u_light_color", light_color);
    pz_renderer_set_uniform_vec3(
        mr->renderer, mr->wall_shader, "u_ambient", ambient);

    // Light map
    if (params && params->light_texture != PZ_INVALID_HANDLE
        && params->light_texture != 0) {
        pz_renderer_bind_texture(mr->renderer, 2, params->light_texture);
        pz_renderer_set_uniform_int(
            mr->renderer, mr->wall_shader, "u_light_texture", 2);
        pz_renderer_set_uniform_int(
            mr->renderer, mr->wall_shader, "u_use_lighting", 1);
        pz_renderer_set_uniform_vec2(mr->renderer, mr->wall_shader,
            "u_light_scale",
            (pz_vec2) { params->light_scale_x, params->light_scale_z });
        pz_renderer_set_uniform_vec2(mr->renderer, mr->wall_shader,
            "u_light_offset",
            (pz_vec2) { params->light_offset_x, params->light_offset_z });
    } else {
        pz_renderer_set_uniform_int(
            mr->renderer, mr->wall_shader, "u_use_lighting", 0);
    }

    // Sun lighting
    if (params && params->has_sun) {
        pz_renderer_set_uniform_int(
            mr->renderer, mr->wall_shader, "u_has_sun", 1);
        pz_renderer_set_uniform_vec3(mr->renderer, mr->wall_shader,
            "u_sun_direction", params->sun_direction);
        pz_renderer_set_uniform_vec3(
            mr->renderer, mr->wall_shader, "u_sun_color", params->sun_color);
    } else {
        pz_renderer_set_uniform_int(
            mr->renderer, mr->wall_shader, "u_has_sun", 0);
    }

    // Tint (walls use no tint = white)
    pz_renderer_set_uniform_vec4(mr->renderer, mr->wall_shader, "u_tint",
        (pz_vec4) { 1.0f, 1.0f, 1.0f, 1.0f });

    pz_renderer_set_uniform_int(
        mr->renderer, mr->wall_shader, "u_texture_top", 0);
    pz_renderer_set_uniform_int(
        mr->renderer, mr->wall_shader, "u_texture_side", 1);

    // Draw each wall batch with its own textures
    for (int i = 0; i < mr->wall_batch_count; i++) {
        wall_batch *batch = &mr->wall_batches[i];
        if (batch->vertex_count == 0)
            continue;

        pz_renderer_bind_texture(mr->renderer, 0, batch->top_texture);
        pz_renderer_bind_texture(mr->renderer, 1, batch->side_texture);

        pz_draw_cmd cmd = {
            .pipeline = mr->wall_pipeline,
            .vertex_buffer = batch->buffer,
            .vertex_count = batch->vertex_count,
        };
        pz_renderer_draw(mr->renderer, &cmd);
    }
}

static void
pz_map_renderer_draw_water(pz_map_renderer *mr, const pz_mat4 *view_projection,
    const pz_map_render_params *params)
{
    if (!mr || !mr->map || mr->water_vertex_count == 0) {
        return;
    }

    pz_renderer_set_uniform_mat4(
        mr->renderer, mr->water_shader, "u_mvp", view_projection);

    // Time for animation
    float time = params ? params->time : 0.0f;
    pz_renderer_set_uniform_float(
        mr->renderer, mr->water_shader, "u_time", time);
    pz_renderer_set_uniform_float(
        mr->renderer, mr->water_shader, "u_wave_time", time);
    pz_renderer_set_uniform_float(mr->renderer, mr->water_shader,
        "u_wave_strength", mr->map->wave_strength);

    // Water colors - calculate dark and highlight from base color
    pz_vec3 base_color = mr->map->water_color;
    pz_vec3 dark_color = pz_color_darken(base_color, 0.6f);
    pz_vec3 highlight_color = pz_color_lighten(base_color, 0.5f);

    pz_renderer_set_uniform_vec3(
        mr->renderer, mr->water_shader, "u_water_color", base_color);
    pz_renderer_set_uniform_vec3(
        mr->renderer, mr->water_shader, "u_water_dark", dark_color);
    pz_renderer_set_uniform_vec3(
        mr->renderer, mr->water_shader, "u_water_highlight", highlight_color);

    // Wind direction and strength for caustic movement
    float wind_dir_x = cosf(mr->map->wind_direction);
    float wind_dir_z = sinf(mr->map->wind_direction);
    pz_renderer_set_uniform_vec2(mr->renderer, mr->water_shader, "u_wind_dir",
        (pz_vec2) { wind_dir_x, wind_dir_z });
    pz_renderer_set_uniform_float(mr->renderer, mr->water_shader,
        "u_wind_strength", mr->map->wind_strength);

    // Alpha (default 1.0 for normal gameplay, 0.5 for editor)
    float water_alpha
        = (params && params->water_alpha > 0.0f) ? params->water_alpha : 1.0f;
    pz_renderer_set_uniform_float(
        mr->renderer, mr->water_shader, "u_alpha", water_alpha);

    // Caustic texture
    if (mr->water_caustic_texture != PZ_INVALID_HANDLE) {
        pz_renderer_bind_texture(mr->renderer, 1, mr->water_caustic_texture);
    }

    // Light map
    if (params && params->light_texture != PZ_INVALID_HANDLE
        && params->light_texture != 0) {
        pz_renderer_bind_texture(mr->renderer, 0, params->light_texture);
        pz_renderer_set_uniform_int(
            mr->renderer, mr->water_shader, "u_water_light_texture", 0);
        pz_renderer_set_uniform_int(
            mr->renderer, mr->water_shader, "u_use_lighting", 1);
        pz_renderer_set_uniform_vec2(mr->renderer, mr->water_shader,
            "u_light_scale",
            (pz_vec2) { params->light_scale_x, params->light_scale_z });
        pz_renderer_set_uniform_vec2(mr->renderer, mr->water_shader,
            "u_light_offset",
            (pz_vec2) { params->light_offset_x, params->light_offset_z });
    } else {
        pz_renderer_set_uniform_int(
            mr->renderer, mr->water_shader, "u_use_lighting", 0);
    }

    pz_draw_cmd cmd = {
        .pipeline = mr->water_pipeline,
        .vertex_buffer = mr->water_buffer,
        .vertex_count = mr->water_vertex_count,
    };
    pz_renderer_draw(mr->renderer, &cmd);
}

static void
pz_map_renderer_draw_fog(pz_map_renderer *mr, const pz_mat4 *view_projection,
    const pz_map_render_params *params)
{
    if (!mr || !mr->map || mr->fog_vertex_count == 0) {
        return;
    }

    pz_renderer_set_uniform_mat4(
        mr->renderer, mr->fog_shader, "u_mvp", view_projection);

    float time = params ? params->time : 0.0f;
    pz_renderer_set_uniform_float(mr->renderer, mr->fog_shader, "u_time", time);

    pz_renderer_set_uniform_vec3(
        mr->renderer, mr->fog_shader, "u_fog_color", mr->map->fog_color);
    pz_renderer_set_uniform_float(
        mr->renderer, mr->fog_shader, "u_fog_alpha", 0.5f);

    int disturb_count = 0;
    float disturb_strength = 0.0f;
    pz_vec4 disturb_slots[PZ_FOG_DISTURB_MAX] = { 0 };

    if (params && params->fog_disturb_count > 0) {
        disturb_count = params->fog_disturb_count;
        if (disturb_count > PZ_FOG_DISTURB_MAX) {
            disturb_count = PZ_FOG_DISTURB_MAX;
        }
        disturb_strength = params->fog_disturb_strength;
        for (int i = 0; i < disturb_count; i++) {
            disturb_slots[i] = (pz_vec4) { params->fog_disturb_pos[i].x,
                params->fog_disturb_strengths[i], params->fog_disturb_pos[i].z,
                params->fog_disturb_radius[i] };
        }
    }

    pz_renderer_set_uniform_int(
        mr->renderer, mr->fog_shader, "u_fog_disturb_count", disturb_count);
    pz_renderer_set_uniform_float(mr->renderer, mr->fog_shader,
        "u_fog_disturb_strength", disturb_strength);
    pz_renderer_set_uniform_vec4(
        mr->renderer, mr->fog_shader, "u_fog_disturb0", disturb_slots[0]);
    pz_renderer_set_uniform_vec4(
        mr->renderer, mr->fog_shader, "u_fog_disturb1", disturb_slots[1]);
    pz_renderer_set_uniform_vec4(
        mr->renderer, mr->fog_shader, "u_fog_disturb2", disturb_slots[2]);
    pz_renderer_set_uniform_vec4(
        mr->renderer, mr->fog_shader, "u_fog_disturb3", disturb_slots[3]);
    pz_renderer_set_uniform_vec4(
        mr->renderer, mr->fog_shader, "u_fog_disturb4", disturb_slots[4]);
    pz_renderer_set_uniform_vec4(
        mr->renderer, mr->fog_shader, "u_fog_disturb5", disturb_slots[5]);
    pz_renderer_set_uniform_vec4(
        mr->renderer, mr->fog_shader, "u_fog_disturb6", disturb_slots[6]);
    pz_renderer_set_uniform_vec4(
        mr->renderer, mr->fog_shader, "u_fog_disturb7", disturb_slots[7]);

    if (params && params->track_texture != PZ_INVALID_HANDLE
        && params->track_texture != 0
        && (mr->map->fog_level == 0 || mr->map->fog_level == 1)) {
        pz_renderer_bind_texture(mr->renderer, 0, params->track_texture);
        pz_renderer_set_uniform_int(
            mr->renderer, mr->fog_shader, "u_fog_track_texture", 0);
        pz_renderer_set_uniform_int(
            mr->renderer, mr->fog_shader, "u_use_tracks", 1);
        pz_renderer_set_uniform_vec2(mr->renderer, mr->fog_shader,
            "u_track_scale",
            (pz_vec2) { params->track_scale_x, params->track_scale_z });
        pz_renderer_set_uniform_vec2(mr->renderer, mr->fog_shader,
            "u_track_offset",
            (pz_vec2) { params->track_offset_x, params->track_offset_z });
    } else {
        // Bind a dummy 2D texture to slot 0 to satisfy shader requirements
        // (ground uses a texture array at slot 0, fog expects a 2D texture)
        const pz_tile_config *fallback
            = pz_tile_registry_get_fallback(mr->tile_registry);
        if (fallback && fallback->ground_texture != PZ_INVALID_HANDLE) {
            pz_renderer_bind_texture(mr->renderer, 0, fallback->ground_texture);
        }
        pz_renderer_set_uniform_int(
            mr->renderer, mr->fog_shader, "u_use_tracks", 0);
    }

    pz_draw_cmd cmd = {
        .pipeline = mr->fog_pipeline,
        .vertex_buffer = mr->fog_buffer,
        .vertex_count = mr->fog_vertex_count,
    };
    pz_renderer_draw(mr->renderer, &cmd);
}

void
pz_map_renderer_draw_jump_pads(
    pz_map_renderer *mr, const pz_mat4 *view_projection, float blink_phase)
{
    if (!mr || !mr->map) {
        return;
    }
    if (mr->jump_pad_buffer == PZ_INVALID_HANDLE) {
        return;
    }
    if (mr->jump_pad_vertex_count == 0) {
        return;
    }
    if (mr->jump_pad_texture == PZ_INVALID_HANDLE) {
        return;
    }

    (void)blink_phase; // TODO: implement blink effect for countdown

    // Use ground shader for jump pads (simple textured rendering)
    pz_renderer_set_uniform_mat4(
        mr->renderer, mr->ground_shader, "u_mvp", view_projection);
    pz_renderer_set_uniform_int(
        mr->renderer, mr->ground_shader, "u_use_tracks", 0);
    pz_renderer_set_uniform_int(
        mr->renderer, mr->ground_shader, "u_use_lighting", 0);
    pz_renderer_set_uniform_int(
        mr->renderer, mr->ground_shader, "u_has_sun", 0);

    pz_renderer_bind_texture(mr->renderer, 0, mr->jump_pad_texture);

    pz_draw_cmd cmd = {
        .pipeline = mr->ground_pipeline,
        .vertex_buffer = mr->jump_pad_buffer,
        .vertex_count = mr->jump_pad_vertex_count,
    };

    pz_renderer_draw(mr->renderer, &cmd);
}

void
pz_map_renderer_draw(pz_map_renderer *mr, const pz_mat4 *view_projection,
    const pz_map_render_params *params)
{
    pz_map_renderer_draw_walls(mr, view_projection, params);
    pz_map_renderer_draw_ground(mr, view_projection, params);
    pz_map_renderer_draw_jump_pads(mr, view_projection, 0.0f);
    pz_map_renderer_draw_water(mr, view_projection, params);
    pz_map_renderer_draw_fog(mr, view_projection, params);
}

void
pz_map_renderer_check_hot_reload(pz_map_renderer *mr)
{
    (void)mr;
}

// ============================================================================
// Map Hot-Reload
// ============================================================================

struct pz_map_hot_reload {
    char *path;
    pz_map **map_ptr;
    pz_map_renderer *renderer;
    int64_t last_mtime;
};

pz_map_hot_reload *
pz_map_hot_reload_create(
    const char *path, pz_map **map_ptr, pz_map_renderer *renderer)
{
    if (!path || !map_ptr || !renderer) {
        return NULL;
    }

    pz_map_hot_reload *hr = pz_calloc(1, sizeof(pz_map_hot_reload));
    if (!hr) {
        return NULL;
    }

    hr->path = pz_str_dup(path);
    hr->map_ptr = map_ptr;
    hr->renderer = renderer;
    hr->last_mtime = pz_map_file_mtime(path);

    pz_log(PZ_LOG_INFO, PZ_LOG_CAT_GAME, "Map hot-reload watching: %s", path);

    return hr;
}

void
pz_map_hot_reload_destroy(pz_map_hot_reload *hr)
{
    if (!hr) {
        return;
    }

    pz_free(hr->path);
    pz_free(hr);
}

bool
pz_map_hot_reload_check(pz_map_hot_reload *hr)
{
    if (!hr || !hr->path) {
        return false;
    }

    int64_t mtime = pz_map_file_mtime(hr->path);
    if (mtime == 0 || mtime == hr->last_mtime) {
        return false;
    }

    pz_log(PZ_LOG_INFO, PZ_LOG_CAT_GAME, "Reloading map: %s", hr->path);

    pz_map *new_map = pz_map_load(hr->path);
    if (!new_map) {
        pz_log(PZ_LOG_ERROR, PZ_LOG_CAT_GAME, "Failed to reload map: %s",
            hr->path);
        hr->last_mtime = mtime;
        return false;
    }

    if (*hr->map_ptr) {
        pz_map_destroy(*hr->map_ptr);
    }

    *hr->map_ptr = new_map;
    pz_map_renderer_set_map(hr->renderer, new_map);
    hr->last_mtime = mtime;

    pz_log(PZ_LOG_INFO, PZ_LOG_CAT_GAME, "Map reloaded successfully");

    return true;
}

const char *
pz_map_hot_reload_get_path(const pz_map_hot_reload *hr)
{
    return hr ? hr->path : NULL;
}

// ============================================================================
// Debug Drawing
// ============================================================================

// Generate debug lines for texture scale boundaries
static void
generate_debug_lines(pz_map_renderer *mr)
{
    if (!mr || !mr->map || mr->debug_line_shader == PZ_INVALID_HANDLE) {
        return;
    }

    // Destroy old buffer
    if (mr->debug_line_buffer != PZ_INVALID_HANDLE) {
        pz_renderer_destroy_buffer(mr->renderer, mr->debug_line_buffer);
        mr->debug_line_buffer = PZ_INVALID_HANDLE;
    }
    mr->debug_line_vertex_count = 0;

    const pz_map *map = mr->map;

    // Get the maximum texture scale used in any tile
    int max_scale = 1;
    for (int i = 0; i < map->tile_def_count && i < MAX_TILE_TEXTURES; i++) {
        const pz_tile_def *def = &map->tile_defs[i];
        const pz_tile_config *config = get_tile_config(mr, def->name);
        if (config) {
            if (config->ground_texture_scale > max_scale) {
                max_scale = config->ground_texture_scale;
            }
        }
    }

    if (max_scale <= 1) {
        // No scaling, no debug lines needed
        return;
    }

    // Calculate how many grid lines we need
    float half_w = map->world_width / 2.0f;
    float half_h = map->world_height / 2.0f;

    // Lines every 'max_scale' tiles
    int x_lines = (map->width / max_scale) + 1;
    int y_lines = (map->height / max_scale) + 1;
    int total_lines = x_lines + y_lines;
    int total_vertices = total_lines * 2; // 2 verts per line

    // Each vertex: x, y, z, r, g, b, a (7 floats)
    float *verts = pz_alloc(total_vertices * 7 * sizeof(float));
    float *v = verts;

    // Debug line color - bright cyan
    float r = 0.0f, g = 1.0f, b = 1.0f, a = 0.6f;

    // Y position for lines (slightly above ground)
    float line_y = GROUND_Y_OFFSET + 0.05f;

    // Vertical lines (along Z axis)
    for (int i = 0; i <= map->width / max_scale; i++) {
        float x = (i * max_scale) * map->tile_size - half_w;
        float z0 = -half_h;
        float z1 = half_h;

        // Start vertex
        *v++ = x;
        *v++ = line_y;
        *v++ = z0;
        *v++ = r;
        *v++ = g;
        *v++ = b;
        *v++ = a;

        // End vertex
        *v++ = x;
        *v++ = line_y;
        *v++ = z1;
        *v++ = r;
        *v++ = g;
        *v++ = b;
        *v++ = a;
    }

    // Horizontal lines (along X axis)
    for (int i = 0; i <= map->height / max_scale; i++) {
        float z = (i * max_scale) * map->tile_size - half_h;
        float x0 = -half_w;
        float x1 = half_w;

        // Start vertex
        *v++ = x0;
        *v++ = line_y;
        *v++ = z;
        *v++ = r;
        *v++ = g;
        *v++ = b;
        *v++ = a;

        // End vertex
        *v++ = x1;
        *v++ = line_y;
        *v++ = z;
        *v++ = r;
        *v++ = g;
        *v++ = b;
        *v++ = a;
    }

    mr->debug_line_vertex_count = total_vertices;

    pz_buffer_desc desc = {
        .type = PZ_BUFFER_VERTEX,
        .usage = PZ_BUFFER_STATIC,
        .data = verts,
        .size = total_vertices * 7 * sizeof(float),
    };
    mr->debug_line_buffer = pz_renderer_create_buffer(mr->renderer, &desc);

    pz_free(verts);

    pz_log(PZ_LOG_DEBUG, PZ_LOG_CAT_RENDER,
        "Debug lines: %d vertices for scale=%d grid", total_vertices,
        max_scale);
}

void
pz_map_renderer_set_debug_texture_scale(pz_map_renderer *mr, bool enabled)
{
    if (!mr) {
        return;
    }
    mr->debug_texture_scale_enabled = enabled;

    if (enabled && mr->map && mr->debug_line_buffer == PZ_INVALID_HANDLE) {
        generate_debug_lines(mr);
    }
}

bool
pz_map_renderer_get_debug_texture_scale(pz_map_renderer *mr)
{
    return mr ? mr->debug_texture_scale_enabled : false;
}

void
pz_map_renderer_draw_debug(pz_map_renderer *mr, const pz_mat4 *view_projection)
{
    if (!mr || !mr->debug_texture_scale_enabled) {
        return;
    }

    if (mr->debug_line_shader == PZ_INVALID_HANDLE
        || mr->debug_line_pipeline == PZ_INVALID_HANDLE) {
        return;
    }

    // Generate lines if needed
    if (mr->debug_line_buffer == PZ_INVALID_HANDLE && mr->map) {
        generate_debug_lines(mr);
    }

    if (mr->debug_line_buffer == PZ_INVALID_HANDLE
        || mr->debug_line_vertex_count == 0) {
        return;
    }

    pz_renderer_set_uniform_mat4(
        mr->renderer, mr->debug_line_shader, "u_mvp", view_projection);

    pz_draw_cmd cmd = {
        .pipeline = mr->debug_line_pipeline,
        .vertex_buffer = mr->debug_line_buffer,
        .vertex_count = mr->debug_line_vertex_count,
    };
    pz_renderer_draw(mr->renderer, &cmd);
}
