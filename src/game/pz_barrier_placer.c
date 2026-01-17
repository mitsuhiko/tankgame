/*
 * Tank Game - Barrier Placer System Implementation
 */

#include "pz_barrier_placer.h"
#include "pz_barrier.h"
#include "pz_map.h"
#include "pz_tank.h"
#include "pz_tile_registry.h"

#include <math.h>
#include <string.h>

#include "../core/pz_log.h"
#include "../core/pz_mem.h"

// Maximum distance from tank to place ghost (in tiles)
#define GHOST_MAX_DISTANCE_TILES 3.0f

// Ghost barrier height
#define GHOST_HEIGHT 1.5f

// Vertex size: position (3) + normal (3) + texcoord (2)
#define GHOST_VERTEX_SIZE 8

// Vertices per ghost (6 faces * 6 verts = 36)
#define GHOST_VERTEX_COUNT 36

/* ============================================================================
 * Ghost Position Calculation
 * ============================================================================
 */

pz_vec2
pz_barrier_placer_calc_ghost_pos(
    const pz_tank *tank, pz_vec2 cursor_world, float tile_size)
{
    if (!tank || tile_size <= 0.0f) {
        return (pz_vec2) { 0.0f, 0.0f };
    }

    // Snap cursor to grid first
    int cursor_tile_x = (int)floorf(cursor_world.x / tile_size);
    int cursor_tile_z = (int)floorf(cursor_world.y / tile_size);
    pz_vec2 snapped_cursor = {
        (cursor_tile_x + 0.5f) * tile_size,
        (cursor_tile_z + 0.5f) * tile_size,
    };

    // Calculate distance from tank to snapped cursor
    pz_vec2 to_cursor = pz_vec2_sub(snapped_cursor, tank->pos);
    float dist = pz_vec2_len(to_cursor);
    float max_dist = tile_size * GHOST_MAX_DISTANCE_TILES;

    // If within range, use cursor position
    if (dist <= max_dist && dist > 0.01f) {
        return snapped_cursor;
    }

    // Clamp to max distance
    if (dist > 0.01f) {
        pz_vec2 dir = pz_vec2_scale(to_cursor, 1.0f / dist);
        pz_vec2 clamped_pos
            = pz_vec2_add(tank->pos, pz_vec2_scale(dir, max_dist));

        // Snap clamped position to grid
        int tile_x = (int)floorf(clamped_pos.x / tile_size);
        int tile_z = (int)floorf(clamped_pos.y / tile_size);

        return (pz_vec2) {
            (tile_x + 0.5f) * tile_size,
            (tile_z + 0.5f) * tile_size,
        };
    }

    // Cursor too close - place one tile in front based on turret angle
    float cos_a = cosf(tank->turret_angle);
    float sin_a = sinf(tank->turret_angle);
    pz_vec2 fallback_pos = {
        tank->pos.x + cos_a * tile_size,
        tank->pos.y + sin_a * tile_size,
    };

    int tile_x = (int)floorf(fallback_pos.x / tile_size);
    int tile_z = (int)floorf(fallback_pos.y / tile_size);

    return (pz_vec2) {
        (tile_x + 0.5f) * tile_size,
        (tile_z + 0.5f) * tile_size,
    };
}

void
pz_barrier_placer_update_ghost(pz_barrier_ghost *ghost, const pz_tank *tank,
    const pz_map *map, const pz_barrier_manager *barrier_mgr, float tile_size,
    pz_vec2 cursor_world)
{
    if (!ghost || !tank) {
        if (ghost)
            ghost->visible = false;
        return;
    }

    // Check if tank is holding barrier_placer
    const pz_tank_barrier_placer *placer = pz_tank_get_barrier_placer(tank);
    if (!placer) {
        ghost->visible = false;
        return;
    }

    ghost->visible = true;
    ghost->pos
        = pz_barrier_placer_calc_ghost_pos(tank, cursor_world, tile_size);

    // Check validity
    float tank_radius = 0.7f; // Standard tank radius
    ghost->valid = pz_barrier_is_valid_placement(
        barrier_mgr, map, ghost->pos, tank_radius, tank->pos);

    // Also check if we can place more barriers
    if (!pz_tank_can_place_barrier(tank)) {
        ghost->valid = false;
    }
}

/* ============================================================================
 * Mesh Generation
 * ============================================================================
 */

// Emit a single face (2 triangles, 6 vertices)
static float *
emit_ghost_face(float *v, float x0, float y0, float z0, float x1, float y1,
    float z1, float x2, float y2, float z2, float x3, float y3, float z3,
    float nx, float ny, float nz, float u0, float v0_uv, float u1, float v1_uv)
{
    // Triangle 1
    *v++ = x0;
    *v++ = y0;
    *v++ = z0;
    *v++ = nx;
    *v++ = ny;
    *v++ = nz;
    *v++ = u0;
    *v++ = v1_uv;

    *v++ = x1;
    *v++ = y1;
    *v++ = z1;
    *v++ = nx;
    *v++ = ny;
    *v++ = nz;
    *v++ = u0;
    *v++ = v0_uv;

    *v++ = x2;
    *v++ = y2;
    *v++ = z2;
    *v++ = nx;
    *v++ = ny;
    *v++ = nz;
    *v++ = u1;
    *v++ = v0_uv;

    // Triangle 2
    *v++ = x0;
    *v++ = y0;
    *v++ = z0;
    *v++ = nx;
    *v++ = ny;
    *v++ = nz;
    *v++ = u0;
    *v++ = v1_uv;

    *v++ = x2;
    *v++ = y2;
    *v++ = z2;
    *v++ = nx;
    *v++ = ny;
    *v++ = nz;
    *v++ = u1;
    *v++ = v0_uv;

    *v++ = x3;
    *v++ = y3;
    *v++ = z3;
    *v++ = nx;
    *v++ = ny;
    *v++ = nz;
    *v++ = u1;
    *v++ = v1_uv;

    return v;
}

// Generate unit box mesh centered at origin
static int
generate_ghost_mesh(float *verts, float half_size, float height)
{
    float x0 = -half_size;
    float x1 = half_size;
    float z0 = -half_size;
    float z1 = half_size;
    float y0 = 0.0f;
    float y1 = height;

    float *v = verts;

    // Top face
    v = emit_ghost_face(v, x0, y1, z0, x0, y1, z1, x1, y1, z1, x1, y1, z0, 0.0f,
        1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f);

    // Bottom face
    v = emit_ghost_face(v, x0, y0, z1, x0, y0, z0, x1, y0, z0, x1, y0, z1, 0.0f,
        -1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f);

    // Front face (+Z)
    v = emit_ghost_face(v, x1, y0, z1, x1, y1, z1, x0, y1, z1, x0, y0, z1, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f);

    // Back face (-Z)
    v = emit_ghost_face(v, x0, y0, z0, x0, y1, z0, x1, y1, z0, x1, y0, z0, 0.0f,
        0.0f, -1.0f, 0.0f, 0.0f, 1.0f, 1.0f);

    // Left face (-X)
    v = emit_ghost_face(v, x0, y0, z1, x0, y1, z1, x0, y1, z0, x0, y0, z0,
        -1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f);

    // Right face (+X)
    v = emit_ghost_face(v, x1, y0, z0, x1, y1, z0, x1, y1, z1, x1, y0, z1, 1.0f,
        0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f);

    return (int)(v - verts);
}

/* ============================================================================
 * Renderer
 * ============================================================================
 */

pz_barrier_placer_renderer *
pz_barrier_placer_renderer_create(pz_renderer *renderer, float tile_size)
{
    pz_barrier_placer_renderer *bpr
        = pz_calloc(1, sizeof(pz_barrier_placer_renderer));
    bpr->tile_size = tile_size;

    // Load entity shader (reuse for ghost rendering)
    bpr->shader = pz_renderer_load_shader(
        renderer, "shaders/entity.vert", "shaders/entity.frag", "entity");
    if (bpr->shader == PZ_INVALID_HANDLE) {
        pz_log(PZ_LOG_ERROR, PZ_LOG_CAT_RENDER,
            "Failed to load ghost barrier shader");
        pz_free(bpr);
        return NULL;
    }

    // Create pipeline with alpha blending
    pz_vertex_attr attrs[] = {
        { .name = "a_position", .type = PZ_ATTR_FLOAT3, .offset = 0 },
        { .name = "a_normal",
            .type = PZ_ATTR_FLOAT3,
            .offset = 3 * sizeof(float) },
        { .name = "a_texcoord",
            .type = PZ_ATTR_FLOAT2,
            .offset = 6 * sizeof(float) },
    };

    pz_pipeline_desc desc = {
        .shader = bpr->shader,
        .vertex_layout = {
            .attrs = attrs,
            .attr_count = 3,
            .stride = GHOST_VERTEX_SIZE * sizeof(float),
        },
        .blend = PZ_BLEND_ALPHA,
        .depth = PZ_DEPTH_READ, // Read depth but don't write
        .cull = PZ_CULL_BACK,
        .primitive = PZ_PRIMITIVE_TRIANGLES,
    };
    bpr->pipeline = pz_renderer_create_pipeline(renderer, &desc);

    // Generate mesh
    float half = tile_size / 2.0f;
    int total_floats = GHOST_VERTEX_COUNT * GHOST_VERTEX_SIZE;
    float *verts = pz_calloc(total_floats, sizeof(float));
    int floats_written = generate_ghost_mesh(verts, half, GHOST_HEIGHT);
    bpr->mesh_vertex_count = floats_written / GHOST_VERTEX_SIZE;

    pz_buffer_desc buf_desc = {
        .type = PZ_BUFFER_VERTEX,
        .data = verts,
        .size = floats_written * sizeof(float),
    };
    bpr->mesh_buffer = pz_renderer_create_buffer(renderer, &buf_desc);
    pz_free(verts);

    bpr->render_ready = (bpr->pipeline != PZ_INVALID_HANDLE
        && bpr->mesh_buffer != PZ_INVALID_HANDLE);

    pz_log(PZ_LOG_INFO, PZ_LOG_CAT_GAME, "Barrier placer renderer created");
    return bpr;
}

void
pz_barrier_placer_renderer_destroy(
    pz_barrier_placer_renderer *bpr, pz_renderer *renderer)
{
    if (!bpr)
        return;

    if (bpr->mesh_buffer != PZ_INVALID_HANDLE) {
        pz_renderer_destroy_buffer(renderer, bpr->mesh_buffer);
    }
    if (bpr->pipeline != PZ_INVALID_HANDLE) {
        pz_renderer_destroy_pipeline(renderer, bpr->pipeline);
    }
    if (bpr->shader != PZ_INVALID_HANDLE) {
        pz_renderer_destroy_shader(renderer, bpr->shader);
    }

    pz_free(bpr);
    pz_log(PZ_LOG_INFO, PZ_LOG_CAT_GAME, "Barrier placer renderer destroyed");
}

void
pz_barrier_placer_render_ghost(pz_barrier_placer_renderer *bpr,
    pz_renderer *renderer, const pz_mat4 *view_projection,
    const pz_barrier_ghost *ghost, pz_vec4 tank_color,
    const pz_tile_registry *tile_registry, const char *tile_name)
{
    (void)tile_registry; // Future: could use tile texture
    (void)tile_name;

    if (!bpr || !renderer || !view_projection || !ghost)
        return;

    if (!ghost->visible || !bpr->render_ready)
        return;

    // Build model matrix
    // Scale ghost slightly larger to prevent z-fighting with existing barriers.
    // The ghost fully envelops any barrier beneath it.
    static const float GHOST_SCALE = 1.02f; // 2% larger
    pz_mat4 model = pz_mat4_identity();
    model = pz_mat4_mul(model,
        pz_mat4_translate((pz_vec3) { ghost->pos.x, 0.0f, ghost->pos.y }));
    model = pz_mat4_mul(model,
        pz_mat4_scale((pz_vec3) { GHOST_SCALE, GHOST_SCALE, GHOST_SCALE }));

    pz_mat4 mvp = pz_mat4_mul(*view_projection, model);

    // Color: tinted by tank color, semi-transparent
    // Valid = greenish tint, Invalid = reddish tint
    pz_vec4 color;
    if (ghost->valid) {
        // Blend tank color with green tint
        color = (pz_vec4) {
            tank_color.x * 0.7f + 0.3f * 0.3f,
            tank_color.y * 0.7f + 0.3f * 0.9f,
            tank_color.z * 0.7f + 0.3f * 0.3f,
            0.5f, // 50% transparent
        };
    } else {
        // Red tint for invalid
        color = (pz_vec4) {
            0.9f, 0.3f, 0.3f,
            0.4f, // More transparent when invalid
        };
    }

    // Light params (simple ambient)
    pz_vec3 light_dir = { 0.5f, 1.0f, 0.3f };
    pz_vec3 light_color = { 0.6f, 0.6f, 0.6f };
    pz_vec3 ambient = { 0.5f, 0.5f, 0.5f };

    pz_renderer_set_uniform_mat4(renderer, bpr->shader, "u_mvp", &mvp);
    pz_renderer_set_uniform_mat4(renderer, bpr->shader, "u_model", &model);
    pz_renderer_set_uniform_vec4(renderer, bpr->shader, "u_color", color);
    pz_renderer_set_uniform_vec3(
        renderer, bpr->shader, "u_light_dir", light_dir);
    pz_renderer_set_uniform_vec3(
        renderer, bpr->shader, "u_light_color", light_color);
    pz_renderer_set_uniform_vec3(renderer, bpr->shader, "u_ambient", ambient);
    pz_renderer_set_uniform_vec2(
        renderer, bpr->shader, "u_shadow_params", (pz_vec2) { 0.0f, 0.0f });

    // Draw
    pz_draw_cmd cmd = {
        .pipeline = bpr->pipeline,
        .vertex_buffer = bpr->mesh_buffer,
        .index_buffer = PZ_INVALID_HANDLE,
        .vertex_count = bpr->mesh_vertex_count,
        .index_count = 0,
        .vertex_offset = 0,
        .index_offset = 0,
    };
    pz_renderer_draw(renderer, &cmd);
}

/* ============================================================================
 * Placement
 * ============================================================================
 */

int
pz_barrier_placer_place(pz_tank *tank, pz_barrier_manager *barrier_mgr,
    const pz_map *map, const pz_barrier_ghost *ghost, float tile_size)
{
    (void)tile_size; // Used indirectly via barrier_mgr

    if (!tank || !barrier_mgr || !map || !ghost)
        return -1;

    if (!ghost->visible || !ghost->valid)
        return -1;

    const pz_tank_barrier_placer *placer = pz_tank_get_barrier_placer(tank);
    if (!placer)
        return -1;

    // Double-check validity
    float tank_radius = 0.7f;
    if (!pz_barrier_is_valid_placement(
            barrier_mgr, map, ghost->pos, tank_radius, tank->pos)) {
        return -1;
    }

    if (!pz_tank_can_place_barrier(tank)) {
        return -1;
    }

    // Create tint color from tank's body color (strong blend toward player
    // color)
    pz_vec4 tint = {
        0.2f + 0.8f * tank->body_color.x,
        0.2f + 0.8f * tank->body_color.y,
        0.2f + 0.8f * tank->body_color.z,
        1.0f,
    };

    // Add barrier with lifetime from placer config
    // Use tank's floor_level for multi-floor support
    int barrier_id = pz_barrier_add_owned(barrier_mgr, ghost->pos,
        placer->barrier_tile, placer->barrier_health, tank->floor_level,
        tank->id, tint, placer->barrier_lifetime);

    if (barrier_id >= 0) {
        // Track in tank
        pz_tank_add_placed_barrier(tank, barrier_id);
        pz_log(PZ_LOG_INFO, PZ_LOG_CAT_GAME,
            "Tank %d placed barrier at (%.1f, %.1f)", tank->id, ghost->pos.x,
            ghost->pos.y);
    }

    return barrier_id;
}
