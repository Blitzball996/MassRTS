#pragma once
// =============================================================================
// SDFCraft - First-person player controller + block raycast
// -----------------------------------------------------------------------------
// AABB physics against the block world (swept per-axis resolution), gravity,
// jump, and a creative fly toggle. Also a voxel DDA raycast for selecting the
// block to break / the face to place against (ported concept from MC
// Level::clip / HitResult).
// =============================================================================
#include "world.h"
#include <glm/glm.hpp>
#include <cmath>

namespace sdfcraft {

struct RayHit {
    bool   hit = false;
    int    bx=0, by=0, bz=0;     // block that was hit
    int    nx=0, ny=0, nz=0;     // face normal (points to the empty side)
    float  dist = 0.0f;
};

class Player {
public:
    glm::vec3 pos{0, 80, 0};   // feet position (bottom-center of AABB)
    glm::vec3 vel{0, 0, 0};
    float yaw = 0.0f;          // degrees
    float pitch = 0.0f;        // degrees, clamped [-89,89]
    bool  on_ground = false;
    bool  flying = false;

    // AABB half-extents (Steve-ish): 0.6 wide, 1.8 tall, eyes at 1.62
    static constexpr float HALF_W = 0.30f;
    static constexpr float HEIGHT = 1.80f;
    static constexpr float EYE    = 1.62f;
    static constexpr float GRAVITY = 28.0f;
    static constexpr float JUMP_V  = 8.4f;
    static constexpr float WALK_SPEED = 4.5f;
    static constexpr float FLY_SPEED  = 12.0f;

    glm::vec3 eye() const { return pos + glm::vec3(0, EYE, 0); }

    glm::vec3 forward() const {
        float y = glm::radians(yaw), p = glm::radians(pitch);
        return glm::normalize(glm::vec3(
            cosf(p) * sinf(y), sinf(p), -cosf(p) * cosf(y)));
    }
    glm::vec3 forward_flat() const {
        float y = glm::radians(yaw);
        return glm::vec3(sinf(y), 0, -cosf(y));
    }
    glm::vec3 right_flat() const {
        float y = glm::radians(yaw);
        return glm::vec3(cosf(y), 0, sinf(y));
    }

    // wish = horizontal input (x=strafe, z=forward) in [-1,1]; up = jump/fly up
    void update(World& world, float dt, glm::vec3 wish, bool jump, bool down) {
        glm::vec3 dir = right_flat() * wish.x + forward_flat() * wish.z;
        if (glm::length(dir) > 1e-4f) dir = glm::normalize(dir);

        if (flying) {
            vel.x = dir.x * FLY_SPEED;
            vel.z = dir.z * FLY_SPEED;
            vel.y = (jump ? FLY_SPEED : 0.0f) - (down ? FLY_SPEED : 0.0f);
        } else {
            vel.x = dir.x * WALK_SPEED;
            vel.z = dir.z * WALK_SPEED;
            vel.y -= GRAVITY * dt;
            if (jump && on_ground) { vel.y = JUMP_V; on_ground = false; }
        }

        move_axis(world, 0, vel.x * dt);
        on_ground = false;
        move_axis(world, 1, vel.y * dt);
        move_axis(world, 2, vel.z * dt);
    }

    // Voxel DDA: walk the grid from the eye along `dir` up to max_dist.
    RayHit raycast(World& world, float max_dist = 6.0f) {
        glm::vec3 o = eye(), d = forward();
        RayHit r;
        int x = (int)floorf(o.x), y = (int)floorf(o.y), z = (int)floorf(o.z);
        int sx = d.x > 0 ? 1 : -1, sy = d.y > 0 ? 1 : -1, sz = d.z > 0 ? 1 : -1;
        float inv_x = d.x != 0 ? fabsf(1.0f/d.x) : 1e30f;
        float inv_y = d.y != 0 ? fabsf(1.0f/d.y) : 1e30f;
        float inv_z = d.z != 0 ? fabsf(1.0f/d.z) : 1e30f;
        float tx = ((sx>0 ? (x+1-o.x) : (o.x-x))) * inv_x;
        float ty = ((sy>0 ? (y+1-o.y) : (o.y-y))) * inv_y;
        float tz = ((sz>0 ? (z+1-o.z) : (o.z-z))) * inv_z;
        float t = 0.0f;
        int face = -1;
        while (t <= max_dist) {
            BlockId b = world.get_block(x, y, z);
            if (b != BLOCK_AIR && !block_is_liquid(b)) {
                r.hit = true; r.bx = x; r.by = y; r.bz = z; r.dist = t;
                switch (face) {
                    case 0: r.nx = -sx; break;
                    case 1: r.ny = -sy; break;
                    case 2: r.nz = -sz; break;
                    default: r.ny = 1; break; // started inside; arbitrary
                }
                return r;
            }
            if (tx < ty && tx < tz)      { x += sx; t = tx; tx += inv_x; face = 0; }
            else if (ty < tz)            { y += sy; t = ty; ty += inv_y; face = 1; }
            else                         { z += sz; t = tz; tz += inv_z; face = 2; }
        }
        return r;
    }

private:
    bool solid_at(World& world, int x, int y, int z) {
        return block_is_solid(world.get_block(x, y, z));
    }

    // Does the player AABB at position p overlap any solid block?
    bool collides(World& world, glm::vec3 p) {
        float minx = p.x - HALF_W, maxx = p.x + HALF_W;
        float miny = p.y,          maxy = p.y + HEIGHT;
        float minz = p.z - HALF_W, maxz = p.z + HALF_W;
        for (int bx = (int)floorf(minx); bx <= (int)floorf(maxx); bx++)
        for (int by = (int)floorf(miny); by <= (int)floorf(maxy); by++)
        for (int bz = (int)floorf(minz); bz <= (int)floorf(maxz); bz++)
            if (solid_at(world, bx, by, bz)) return true;
        return false;
    }

    // Move along a single axis, stopping flush against the first collision.
    void move_axis(World& world, int axis, float amount) {
        if (amount == 0.0f) return;
        glm::vec3 np = pos;
        np[axis] += amount;
        if (!collides(world, np)) { pos = np; return; }
        // collision on this axis: zero velocity, then binary-search the largest
        // safe sub-step so we end up flush against the contact face.
        if (axis == 1) { if (amount < 0) on_ground = true; vel.y = 0.0f; }
        else           { vel[axis] = 0.0f; }
        float lo = 0.0f, hi = amount;          // lo is known-safe, hi is blocked
        for (int i = 0; i < 8; i++) {
            float mid = 0.5f * (lo + hi);
            glm::vec3 probe = pos; probe[axis] += mid;
            if (collides(world, probe)) hi = mid; else lo = mid;
        }
        pos[axis] += lo;
    }
};

} // namespace sdfcraft
