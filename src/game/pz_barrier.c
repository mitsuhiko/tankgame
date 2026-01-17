/*
 * Tank Game - Destructible Barrier System Implementation
 */

#include "pz_barrier.h"
#include "pz_collision.h"
#include "pz_lighting.h"
#include "pz_map.h"
#include "pz_particle.h"

#include <math.h>
#include <string.h>

#include "../core/pz_log.h"
#include "../core/pz_mem.h"

// Default barrier height (same as height=1 walls)
#define BARRIER_HEIGHT 1.5f

// Vertex size: position (3) + normal (3) + texcoord (2) + ao (1)
#define BARRIER_VERTEX_SIZE 9

// Maximum vertices per barrier (6 faces * 6 verts each = 36)
#define BARRIER_VERTS_PER_UNIT 36

/* ============================================================================
 * Mesh Generation
 * ============================================================================
 */

// Emit a single face (2 triangles, 6 vertices)
static float *
emit_face(float *v, float x0, float y0, float z0, float x1, float y1, float z1,
    float x2, float y2, float z2, float x3, float y3, float z3, float nx,
    float ny, float nz, float u0, float v0_uv, float u1, float v1_uv, float ao)
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
    *v++ = ao;

    *v++ = x1;
    *v++ = y1;
    *v++ = z1;
    *v++ = nx;
    *v++ = ny;
    *v++ = nz;
    *v++ = u0;
    *v++ = v0_uv;
    *v++ = ao;

    *v++ = x2;
    *v++ = y2;
    *v++ = z2;
    *v++ = nx;
    *v++ = ny;
    *v++ = nz;
    *v++ = u1;
    *v++ = v0_uv;
    *v++ = ao;

    // Triangle 2: v0, v2, v3
    *v++ = x0;
    *v++ = y0;
    *v++ = z0;
    *v++ = nx;
    *v++ = ny;
    *v++ = nz;
    *v++ = u0;
    *v++ = v1_uv;
    *v++ = ao;

    *v++ = x2;
    *v++ = y2;
    *v++ = z2;
    *v++ = nx;
    *v++ = ny;
    *v++ = nz;
    *v++ = u1;
    *v++ = v0_uv;
    *v++ = ao;

    *v++ = x3;
    *v++ = y3;
    *v++ = z3;
    *v++ = nx;
    *v++ = ny;
    *v++ = nz;
    *v++ = u1;
    *v++ = v1_uv;
    *v++ = ao;

    return v;
}

// Generate a complete box mesh for a barrier at given position
// Returns number of floats written
static int
generate_barrier_mesh(
    float *verts, float cx, float cz, float tile_size, int texture_scale)
{
    float half = tile_size / 2.0f;
    float x0 = cx - half;
    float x1 = cx + half;
    float z0 = cz - half;
    float z1 = cz + half;
    float y0 = 0.0f;
    float y1 = BARRIER_HEIGHT;

    // UV calculation - match wall texture scaling
    // For simplicity, use normalized UVs that tile across the barrier
    float inv_scale = 1.0f / (float)texture_scale;

    // Approximate tile position for UV continuity (won't be perfect since
    // barriers can be placed anywhere, but close enough)
    int tile_x = (int)floorf(cx / tile_size);
    int tile_y = (int)floorf(cz / tile_size);

    float u0 = (float)tile_x * inv_scale;
    float u1 = (float)(tile_x + 1) * inv_scale;
    float v0_uv = (float)tile_y * inv_scale;
    float v1_uv = (float)(tile_y + 1) * inv_scale;

    float v_bottom = 0.0f;
    float v_top = inv_scale;

    float *v = verts;

    // Top face
    v = emit_face(v, x0, y1, z0, x0, y1, z1, x1, y1, z1, x1, y1, z0, 0.0f, 1.0f,
        0.0f, u0, v0_uv, u1, v1_uv, 1.0f);

    // Bottom face (not usually visible, but include for completeness)
    v = emit_face(v, x0, y0, z1, x0, y0, z0, x1, y0, z0, x1, y0, z1, 0.0f,
        -1.0f, 0.0f, u0, v0_uv, u1, v1_uv, 1.0f);

    // Front face (+Z)
    v = emit_face(v, x1, y0, z1, x1, y1, z1, x0, y1, z1, x0, y0, z1, 0.0f, 0.0f,
        1.0f, u1, v_bottom, u0, v_top, 1.0f);

    // Back face (-Z)
    v = emit_face(v, x0, y0, z0, x0, y1, z0, x1, y1, z0, x1, y0, z0, 0.0f, 0.0f,
        -1.0f, u0, v_bottom, u1, v_top, 1.0f);

    // Left face (-X)
    v = emit_face(v, x0, y0, z1, x0, y1, z1, x0, y1, z0, x0, y0, z0, -1.0f,
        0.0f, 0.0f, v1_uv, v_bottom, v0_uv, v_top, 1.0f);

    // Right face (+X)
    v = emit_face(v, x1, y0, z0, x1, y1, z0, x1, y1, z1, x1, y0, z1, 1.0f, 0.0f,
        0.0f, v0_uv, v_bottom, v1_uv, v_top, 1.0f);

    return (int)(v - verts);
}

/* ============================================================================
 * Manager Lifecycle
 * ============================================================================
 */

pz_barrier_manager *
pz_barrier_manager_create(pz_renderer *renderer,
    const pz_tile_registry *tile_registry, float tile_size)
{
    pz_barrier_manager *mgr = pz_calloc(1, sizeof(pz_barrier_manager));
    mgr->tile_registry = tile_registry;
    mgr->tile_size = tile_size;

    // Load wall shader (same as map renderer uses)
    mgr->shader = pz_renderer_load_shader(
        renderer, "shaders/wall.vert", "shaders/wall.frag", "wall");
    if (mgr->shader == PZ_INVALID_HANDLE) {
        pz_log(PZ_LOG_ERROR, PZ_LOG_CAT_RENDER,
            "Failed to load barrier shader (wall shader)");
        pz_free(mgr);
        return NULL;
    }

    // Create pipeline
    pz_vertex_attr attrs[] = {
        { .name = "a_position", .type = PZ_ATTR_FLOAT3, .offset = 0 },
        { .name = "a_normal",
            .type = PZ_ATTR_FLOAT3,
            .offset = 3 * sizeof(float) },
        { .name = "a_texcoord",
            .type = PZ_ATTR_FLOAT2,
            .offset = 6 * sizeof(float) },
        { .name = "a_ao", .type = PZ_ATTR_FLOAT, .offset = 8 * sizeof(float) },
    };

    pz_pipeline_desc desc = {
        .shader = mgr->shader,
        .vertex_layout = {
            .attrs = attrs,
            .attr_count = 4,
            .stride = BARRIER_VERTEX_SIZE * sizeof(float),
        },
        .blend = PZ_BLEND_ALPHA,
        .depth = PZ_DEPTH_READ_WRITE,
        .cull = PZ_CULL_BACK,
        .primitive = PZ_PRIMITIVE_TRIANGLES,
    };
    mgr->pipeline = pz_renderer_create_pipeline(renderer, &desc);

    // No mesh buffer yet - will be created when barriers are added
    mgr->mesh_buffer = PZ_INVALID_HANDLE;
    mgr->mesh_vertex_count = 0;
    mgr->render_ready = true;

    pz_log(PZ_LOG_INFO, PZ_LOG_CAT_GAME, "Barrier manager created");
    return mgr;
}

void
pz_barrier_manager_destroy(pz_barrier_manager *mgr, pz_renderer *renderer)
{
    if (!mgr)
        return;

    if (mgr->mesh_buffer != PZ_INVALID_HANDLE) {
        pz_renderer_destroy_buffer(renderer, mgr->mesh_buffer);
    }
    if (mgr->pipeline != PZ_INVALID_HANDLE) {
        pz_renderer_destroy_pipeline(renderer, mgr->pipeline);
    }
    if (mgr->shader != PZ_INVALID_HANDLE) {
        pz_renderer_destroy_shader(renderer, mgr->shader);
    }

    pz_free(mgr);
    pz_log(PZ_LOG_INFO, PZ_LOG_CAT_GAME, "Barrier manager destroyed");
}

/* ============================================================================
 * Mesh Rebuilding
 * ============================================================================
 */

__attribute__((unused)) static void
rebuild_mesh(pz_barrier_manager *mgr, pz_renderer *renderer)
{
    // Count active barriers
    int active_count = 0;
    for (int i = 0; i < PZ_MAX_BARRIERS; i++) {
        if (mgr->barriers[i].active && !mgr->barriers[i].destroyed) {
            active_count++;
        }
    }

    // Destroy old buffer
    if (mgr->mesh_buffer != PZ_INVALID_HANDLE) {
        pz_renderer_destroy_buffer(renderer, mgr->mesh_buffer);
        mgr->mesh_buffer = PZ_INVALID_HANDLE;
    }

    if (active_count == 0) {
        mgr->mesh_vertex_count = 0;
        return;
    }

    // Allocate vertex data
    int floats_per_barrier = BARRIER_VERTS_PER_UNIT * BARRIER_VERTEX_SIZE;
    int total_floats = active_count * floats_per_barrier;
    float *verts = pz_calloc(total_floats, sizeof(float));

    float *v = verts;
    for (int i = 0; i < PZ_MAX_BARRIERS; i++) {
        pz_barrier *barrier = &mgr->barriers[i];
        if (!barrier->active || barrier->destroyed)
            continue;

        // Get texture scale from tile config
        int texture_scale = 4; // Default
        const pz_tile_config *tile
            = pz_tile_registry_get(mgr->tile_registry, barrier->tile_name);
        if (tile) {
            texture_scale = tile->wall_texture_scale;
            if (texture_scale < 1)
                texture_scale = 1;
        }

        int floats_written = generate_barrier_mesh(
            v, barrier->pos.x, barrier->pos.y, mgr->tile_size, texture_scale);
        v += floats_written;
    }

    int actual_floats = (int)(v - verts);
    mgr->mesh_vertex_count = actual_floats / BARRIER_VERTEX_SIZE;

    // Create buffer
    pz_buffer_desc buf_desc = {
        .type = PZ_BUFFER_VERTEX,
        .data = verts,
        .size = actual_floats * sizeof(float),
    };
    mgr->mesh_buffer = pz_renderer_create_buffer(renderer, &buf_desc);

    pz_free(verts);
}

/* ============================================================================
 * Barrier Management
 * ============================================================================
 */

int
pz_barrier_add(pz_barrier_manager *mgr, pz_vec2 pos, const char *tile_name,
    float health, int8_t floor_level)
{
    // Map-placed barriers have no owner, no tint, and no lifetime
    return pz_barrier_add_owned(mgr, pos, tile_name, health, floor_level, -1,
        (pz_vec4) { 1.0f, 1.0f, 1.0f, 1.0f }, 0.0f);
}

int
pz_barrier_add_owned(pz_barrier_manager *mgr, pz_vec2 pos,
    const char *tile_name, float health, int8_t floor_level, int owner_tank_id,
    pz_vec4 tint_color, float lifetime)
{
    if (!mgr || !tile_name)
        return -1;

    // Find free slot
    int slot = -1;
    for (int i = 0; i < PZ_MAX_BARRIERS; i++) {
        if (!mgr->barriers[i].active) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        pz_log(PZ_LOG_WARN, PZ_LOG_CAT_GAME,
            "Barrier limit reached (%d), cannot add more", PZ_MAX_BARRIERS);
        return -1;
    }

    pz_barrier *barrier = &mgr->barriers[slot];
    barrier->active = true;
    barrier->destroyed = false;
    barrier->pos = pos;
    barrier->health = health;
    barrier->max_health = health;
    barrier->floor_level = floor_level;
    barrier->destroy_timer = 0.0f;
    barrier->owner_tank_id = owner_tank_id;
    barrier->tint_color = tint_color;
    barrier->lifetime = lifetime;
    barrier->max_lifetime = lifetime;
    strncpy(barrier->tile_name, tile_name, sizeof(barrier->tile_name) - 1);
    barrier->tile_name[sizeof(barrier->tile_name) - 1] = '\0';

    mgr->active_count++;

    if (owner_tank_id >= 0) {
        pz_log(PZ_LOG_INFO, PZ_LOG_CAT_GAME,
            "Player barrier added at (%.1f, %.1f), tile=%s, health=%.0f, "
            "owner=%d, lifetime=%.1fs, tint=(%.2f, %.2f, %.2f)",
            pos.x, pos.y, tile_name, health, owner_tank_id, lifetime,
            tint_color.x, tint_color.y, tint_color.z);
    } else {
        pz_log(PZ_LOG_INFO, PZ_LOG_CAT_GAME,
            "Barrier added at (%.1f, %.1f), tile=%s, health=%.0f", pos.x, pos.y,
            tile_name, health);
    }

    return slot;
}

void
pz_barrier_update(pz_barrier_manager *mgr, float dt)
{
    if (!mgr)
        return;

    // Clear expired list from previous frame
    mgr->expired_count = 0;

    for (int i = 0; i < PZ_MAX_BARRIERS; i++) {
        pz_barrier *barrier = &mgr->barriers[i];
        if (!barrier->active)
            continue;

        if (barrier->destroy_timer > 0.0f) {
            barrier->destroy_timer -= dt;
        }

        // Handle lifetime expiration for player-placed barriers
        if (!barrier->destroyed && barrier->lifetime > 0.0f) {
            barrier->lifetime -= dt;
            if (barrier->lifetime <= 0.0f) {
                barrier->lifetime = 0.0f;
                barrier->destroyed = true;
                barrier->destroy_timer = 1.0f; // Same destruction effect
                mgr->active_count--;

                // Track expired barrier so it can be credited back to player
                if (mgr->expired_count < PZ_MAX_EXPIRED_BARRIERS) {
                    pz_expired_barrier *exp
                        = &mgr->expired[mgr->expired_count++];
                    exp->barrier_index = i;
                    exp->owner_tank_id = barrier->owner_tank_id;
                    exp->pos = barrier->pos;
                }

                pz_log(PZ_LOG_INFO, PZ_LOG_CAT_GAME,
                    "Barrier at (%.1f, %.1f) expired (lifetime ended)",
                    barrier->pos.x, barrier->pos.y);
            }
        }
    }
}

const pz_expired_barrier *
pz_barrier_get_expired(const pz_barrier_manager *mgr, int *count)
{
    if (!mgr) {
        if (count)
            *count = 0;
        return NULL;
    }

    if (count)
        *count = mgr->expired_count;
    return mgr->expired;
}

bool
pz_barrier_apply_damage(pz_barrier_manager *mgr, pz_vec2 pos, float damage,
    int8_t floor_level, bool *destroyed)
{
    if (!mgr)
        return false;

    if (destroyed)
        *destroyed = false;

    float half = mgr->tile_size / 2.0f;

    for (int i = 0; i < PZ_MAX_BARRIERS; i++) {
        pz_barrier *barrier = &mgr->barriers[i];
        if (!barrier->active || barrier->destroyed)
            continue;

        // Only damage barriers on the same floor
        if (barrier->floor_level != floor_level)
            continue;

        // Check if position is within barrier bounds
        float dx = fabsf(pos.x - barrier->pos.x);
        float dz = fabsf(pos.y - barrier->pos.y);

        if (dx <= half && dz <= half) {
            barrier->health -= damage;
            pz_log(PZ_LOG_DEBUG, PZ_LOG_CAT_GAME,
                "Barrier at (%.1f, %.1f) hit for %.0f damage, health=%.0f",
                barrier->pos.x, barrier->pos.y, damage, barrier->health);

            if (barrier->health <= 0.0f) {
                barrier->destroyed = true;
                barrier->destroy_timer = 1.0f; // Destruction effect duration
                mgr->active_count--;

                if (destroyed)
                    *destroyed = true;

                pz_log(PZ_LOG_INFO, PZ_LOG_CAT_GAME,
                    "Barrier at (%.1f, %.1f) destroyed", barrier->pos.x,
                    barrier->pos.y);
            }

            return true;
        }
    }

    return false;
}

pz_barrier *
pz_barrier_check_collision(
    pz_barrier_manager *mgr, pz_vec2 pos, float radius, int8_t floor_level)
{
    if (!mgr)
        return NULL;

    float half = mgr->tile_size / 2.0f;
    pz_circle circle = pz_circle_new(pos, radius);

    for (int i = 0; i < PZ_MAX_BARRIERS; i++) {
        pz_barrier *barrier = &mgr->barriers[i];
        if (!barrier->active || barrier->destroyed)
            continue;

        // Only check barriers on the same floor
        if (barrier->floor_level != floor_level)
            continue;

        // Box-circle collision
        pz_aabb box
            = pz_aabb_from_center(barrier->pos, (pz_vec2) { half, half });
        if (pz_collision_circle_aabb(circle, box, NULL)) {
            return barrier;
        }
    }

    return NULL;
}

bool
pz_barrier_resolve_collision(
    pz_barrier_manager *mgr, pz_vec2 *pos, float radius, int8_t floor_level)
{
    if (!mgr || !pos)
        return false;

    bool collided = false;
    float half = mgr->tile_size / 2.0f;

    for (int i = 0; i < PZ_MAX_BARRIERS; i++) {
        pz_barrier *barrier = &mgr->barriers[i];
        if (!barrier->active || barrier->destroyed)
            continue;

        // Only collide with barriers on the same floor
        if (barrier->floor_level != floor_level)
            continue;

        pz_aabb box
            = pz_aabb_from_center(barrier->pos, (pz_vec2) { half, half });
        pz_circle circle = pz_circle_new(*pos, radius);
        pz_vec2 push_out = { 0.0f, 0.0f };

        if (pz_collision_circle_aabb(circle, box, &push_out)) {
            pos->x += push_out.x;
            pos->y += push_out.y;
            collided = true;
        }
    }

    return collided;
}

bool
pz_barrier_raycast(pz_barrier_manager *mgr, pz_vec2 start, pz_vec2 end,
    int8_t floor_level, pz_vec2 *hit_pos, pz_vec2 *hit_normal,
    pz_barrier **barrier_out)
{
    if (!mgr)
        return false;

    float half = mgr->tile_size / 2.0f;
    float closest_t = 2.0f; // > 1.0 means no hit
    pz_barrier *closest_barrier = NULL;
    pz_vec2 closest_normal = { 0, 0 };

    pz_vec2 dir = pz_vec2_sub(end, start);
    float ray_len = pz_vec2_len(dir);
    if (ray_len < 0.0001f)
        return false;

    for (int i = 0; i < PZ_MAX_BARRIERS; i++) {
        pz_barrier *barrier = &mgr->barriers[i];
        if (!barrier->active || barrier->destroyed)
            continue;

        // Only raycast against barriers on the same floor
        if (barrier->floor_level != floor_level)
            continue;

        // AABB ray intersection
        float min_x = barrier->pos.x - half;
        float max_x = barrier->pos.x + half;
        float min_z = barrier->pos.y - half;
        float max_z = barrier->pos.y + half;

        float t_min = 0.0f;
        float t_max = 1.0f;
        pz_vec2 normal = { 0, 0 };

        // X axis
        if (fabsf(dir.x) > 0.0001f) {
            float t1 = (min_x - start.x) / dir.x;
            float t2 = (max_x - start.x) / dir.x;
            pz_vec2 n1 = { -1, 0 };
            pz_vec2 n2 = { 1, 0 };

            if (t1 > t2) {
                float tmp = t1;
                t1 = t2;
                t2 = tmp;
                // Swap normals (we only need n1 after this)
                n1 = n2;
            }

            if (t1 > t_min) {
                t_min = t1;
                normal = n1;
            }
            t_max = fminf(t_max, t2);
        } else {
            if (start.x < min_x || start.x > max_x)
                continue;
        }

        // Z axis
        if (fabsf(dir.y) > 0.0001f) {
            float t1 = (min_z - start.y) / dir.y;
            float t2 = (max_z - start.y) / dir.y;
            pz_vec2 n1 = { 0, -1 };
            pz_vec2 n2 = { 0, 1 };

            if (t1 > t2) {
                float tmp = t1;
                t1 = t2;
                t2 = tmp;
                // Swap normals (we only need n1 after this)
                n1 = n2;
            }

            if (t1 > t_min) {
                t_min = t1;
                normal = n1;
            }
            t_max = fminf(t_max, t2);
        } else {
            if (start.y < min_z || start.y > max_z)
                continue;
        }

        if (t_max >= t_min && t_min >= 0.0f && t_min < closest_t) {
            closest_t = t_min;
            closest_barrier = barrier;
            closest_normal = normal;
        }
    }

    if (closest_barrier) {
        if (hit_pos) {
            *hit_pos = pz_vec2_add(start, pz_vec2_scale(dir, closest_t));
        }
        if (hit_normal) {
            *hit_normal = closest_normal;
        }
        if (barrier_out) {
            *barrier_out = closest_barrier;
        }
        return true;
    }

    return false;
}

void
pz_barrier_add_occluders(pz_barrier_manager *mgr, pz_lighting *lighting)
{
    if (!mgr || !lighting)
        return;

    float half = mgr->tile_size / 2.0f;

    for (int i = 0; i < PZ_MAX_BARRIERS; i++) {
        pz_barrier *barrier = &mgr->barriers[i];
        if (!barrier->active || barrier->destroyed)
            continue;

        pz_lighting_add_occluder(lighting, barrier->pos,
            (pz_vec2) { half, half }, 0.0f, barrier->floor_level);
    }
}

void
pz_barrier_render(pz_barrier_manager *mgr, pz_renderer *renderer,
    const pz_mat4 *view_projection, const pz_barrier_render_params *params)
{
    if (!mgr || !renderer || !mgr->render_ready)
        return;

    // Count active, non-destroyed barriers
    int visible_count = 0;
    for (int i = 0; i < PZ_MAX_BARRIERS; i++) {
        if (mgr->barriers[i].active && !mgr->barriers[i].destroyed) {
            visible_count++;
        }
    }

    if (visible_count == 0)
        return;

    // Model matrix is identity (positions are baked into mesh)
    pz_mat4 model = pz_mat4_identity();

    pz_renderer_set_uniform_mat4(
        renderer, mgr->shader, "u_mvp", view_projection);
    pz_renderer_set_uniform_mat4(renderer, mgr->shader, "u_model", &model);

    // Lighting uniforms
    if (params) {
        pz_renderer_set_uniform_vec3(
            renderer, mgr->shader, "u_light_dir", params->sun_direction);
        pz_renderer_set_uniform_vec3(
            renderer, mgr->shader, "u_light_color", params->sun_color);
        pz_renderer_set_uniform_vec3(
            renderer, mgr->shader, "u_ambient", params->ambient);

        // Sun lighting
        if (params->has_sun) {
            pz_renderer_set_uniform_int(renderer, mgr->shader, "u_has_sun", 1);
            pz_renderer_set_uniform_vec3(renderer, mgr->shader,
                "u_sun_direction", params->sun_direction);
            pz_renderer_set_uniform_vec3(
                renderer, mgr->shader, "u_sun_color", params->sun_color);
        } else {
            pz_renderer_set_uniform_int(renderer, mgr->shader, "u_has_sun", 0);
        }

        // Dynamic lighting
        if (params->light_texture != PZ_INVALID_HANDLE) {
            pz_renderer_bind_texture(renderer, 2, params->light_texture);
            pz_renderer_set_uniform_int(
                renderer, mgr->shader, "u_light_texture", 2);
            pz_renderer_set_uniform_int(
                renderer, mgr->shader, "u_use_lighting", 1);

            pz_vec2 light_scale
                = { params->light_scale_x, params->light_scale_z };
            pz_vec2 light_offset
                = { params->light_offset_x, params->light_offset_z };
            pz_renderer_set_uniform_vec2(
                renderer, mgr->shader, "u_light_scale", light_scale);
            pz_renderer_set_uniform_vec2(
                renderer, mgr->shader, "u_light_offset", light_offset);
        } else {
            pz_renderer_set_uniform_int(
                renderer, mgr->shader, "u_use_lighting", 0);
        }
    } else {
        // No params - set defaults
        pz_renderer_set_uniform_int(renderer, mgr->shader, "u_use_lighting", 0);
        pz_renderer_set_uniform_int(renderer, mgr->shader, "u_has_sun", 0);
    }

    pz_renderer_set_uniform_int(renderer, mgr->shader, "u_texture_top", 0);
    pz_renderer_set_uniform_int(renderer, mgr->shader, "u_texture_side", 1);

    // Render each barrier individually with its own tint color
    int floats_per_barrier = BARRIER_VERTS_PER_UNIT * BARRIER_VERTEX_SIZE;
    float *verts = pz_calloc(floats_per_barrier, sizeof(float));

    for (int i = 0; i < PZ_MAX_BARRIERS; i++) {
        pz_barrier *barrier = &mgr->barriers[i];
        if (!barrier->active || barrier->destroyed)
            continue;

        // Get texture from tile registry for this barrier
        pz_texture_handle texture = PZ_INVALID_HANDLE;
        pz_texture_handle side_texture = PZ_INVALID_HANDLE;

        const pz_tile_config *tile
            = pz_tile_registry_get(mgr->tile_registry, barrier->tile_name);
        if (tile) {
            texture = tile->wall_texture;
            side_texture = tile->wall_side_texture;
            if (side_texture == PZ_INVALID_HANDLE) {
                side_texture = texture;
            }
        }

        if (texture == PZ_INVALID_HANDLE) {
            const pz_tile_config *fallback
                = pz_tile_registry_get_fallback(mgr->tile_registry);
            if (fallback) {
                texture = fallback->ground_texture;
                side_texture = texture;
            }
        }

        // Generate mesh for this barrier
        int texture_scale = 4;
        if (tile) {
            texture_scale = tile->wall_texture_scale;
            if (texture_scale < 1)
                texture_scale = 1;
        }

        int floats_written = generate_barrier_mesh(verts, barrier->pos.x,
            barrier->pos.y, mgr->tile_size, texture_scale);
        int vertex_count = floats_written / BARRIER_VERTEX_SIZE;

        // Create temporary buffer
        pz_buffer_desc buf_desc = {
            .type = PZ_BUFFER_VERTEX,
            .data = verts,
            .size = floats_written * sizeof(float),
        };
        pz_buffer_handle temp_buffer
            = pz_renderer_create_buffer(renderer, &buf_desc);

        // Bind textures
        if (texture != PZ_INVALID_HANDLE) {
            pz_renderer_bind_texture(renderer, 0, texture);
        }
        if (side_texture != PZ_INVALID_HANDLE) {
            pz_renderer_bind_texture(renderer, 1, side_texture);
        }

        // Calculate tint color with lifetime-based alpha
        pz_vec4 tint = barrier->tint_color;
        if (barrier->max_lifetime > 0.0f && barrier->lifetime > 0.0f) {
            // Fade from full alpha to 35% alpha as lifetime decreases
            float ratio = barrier->lifetime / barrier->max_lifetime;
            float alpha
                = 0.35f + 0.65f * ratio; // 100% -> 35% as ratio goes 1->0
            tint.w *= alpha;
        }

        // Set tint color for this barrier
        pz_renderer_set_uniform_vec4(renderer, mgr->shader, "u_tint", tint);

        // Draw
        pz_draw_cmd cmd = {
            .pipeline = mgr->pipeline,
            .vertex_buffer = temp_buffer,
            .index_buffer = PZ_INVALID_HANDLE,
            .vertex_count = vertex_count,
            .index_count = 0,
            .vertex_offset = 0,
            .index_offset = 0,
        };
        pz_renderer_draw(renderer, &cmd);

        // Destroy temporary buffer
        pz_renderer_destroy_buffer(renderer, temp_buffer);
    }

    pz_free(verts);
}

int
pz_barrier_count(const pz_barrier_manager *mgr)
{
    if (!mgr)
        return 0;

    int count = 0;
    for (int i = 0; i < PZ_MAX_BARRIERS; i++) {
        if (mgr->barriers[i].active && !mgr->barriers[i].destroyed) {
            count++;
        }
    }
    return count;
}

void
pz_barrier_clear(pz_barrier_manager *mgr)
{
    if (!mgr)
        return;

    memset(mgr->barriers, 0, sizeof(mgr->barriers));
    mgr->active_count = 0;
}

void
pz_barrier_clear_owned_by(pz_barrier_manager *mgr, int tank_id)
{
    if (!mgr || tank_id < 0)
        return;

    for (int i = 0; i < PZ_MAX_BARRIERS; i++) {
        pz_barrier *barrier = &mgr->barriers[i];
        if (barrier->active && barrier->owner_tank_id == tank_id) {
            barrier->active = false;
            barrier->destroyed = false;
            mgr->active_count--;
            pz_log(PZ_LOG_DEBUG, PZ_LOG_CAT_GAME,
                "Cleared barrier %d owned by tank %d", i, tank_id);
        }
    }
}

pz_barrier *
pz_barrier_get(pz_barrier_manager *mgr, int index)
{
    if (!mgr || index < 0 || index >= PZ_MAX_BARRIERS)
        return NULL;
    return &mgr->barriers[index];
}

bool
pz_barrier_is_valid_placement(const pz_barrier_manager *mgr, const pz_map *map,
    pz_vec2 pos, float tank_radius, pz_vec2 tank_pos)
{
    if (!mgr || !map)
        return false;

    float half = mgr->tile_size / 2.0f;

    // Check map bounds
    if (!pz_map_in_bounds_world(map, pos))
        return false;

    // Check for wall at position
    if (pz_map_is_solid(map, pos))
        return false;

    // Check for existing barrier
    for (int i = 0; i < PZ_MAX_BARRIERS; i++) {
        const pz_barrier *barrier = &mgr->barriers[i];
        if (!barrier->active || barrier->destroyed)
            continue;

        float dx = fabsf(pos.x - barrier->pos.x);
        float dz = fabsf(pos.y - barrier->pos.y);
        if (dx < mgr->tile_size * 0.9f && dz < mgr->tile_size * 0.9f) {
            return false; // Overlaps existing barrier
        }
    }

    // Check if tank would overlap the barrier
    pz_circle tank_circle = pz_circle_new(tank_pos, tank_radius);
    pz_aabb barrier_box = pz_aabb_from_center(pos, (pz_vec2) { half, half });
    if (pz_collision_circle_aabb(tank_circle, barrier_box, NULL)) {
        return false; // Would overlap tank
    }

    return true;
}
