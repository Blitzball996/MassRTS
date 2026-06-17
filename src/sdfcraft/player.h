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
#include <algorithm>

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

    // --- survival stats (Phase D/E) ---
    float health     = 20.0f;   // 10 hearts
    float max_health = 20.0f;
    float hunger     = 20.0f;   // 10 drumsticks
    float saturation = 5.0f;
    float air        = 10.0f;   // breath underwater
    float heal_timer = 0.0f;
    float starve_timer = 0.0f;
    float hurt_flash = 0.0f;    // HUD red flash timer
    bool  dead = false;

    void hurt(float dmg) {
        if (dead || dmg <= 0) return;
        health -= dmg;
        hurt_flash = 0.4f;
        if (health <= 0) { health = 0; dead = true; }
    }
    void heal(float amt) { health = std::min(max_health, health + amt); }
    void eat(int food) {
        hunger = std::min(20.0f, hunger + (float)food);
        saturation = std::min(hunger, saturation + food * 0.6f);
    }

    // Survival tick: natural regen when fed, starvation when empty, drowning
    // when head is in water. Fall damage is handled by the caller (needs the
    // pre-landing velocity). dt is real seconds.
    void survival_tick(World& world, float dt) {
        if (dead) return;
        // slow hunger drain
        if (saturation > 0) saturation = std::max(0.0f, saturation - dt * 0.10f);
        else if (hunger > 0) hunger = std::max(0.0f, hunger - dt * 0.30f);

        // regen when well-fed, starve when empty
        if (hunger >= 18.0f && health < max_health) {
            heal_timer += dt;
            if (heal_timer >= 3.0f) { heal(1.0f); heal_timer = 0.0f; }
        } else heal_timer = 0.0f;
        if (hunger <= 0.0f) {
            starve_timer += dt;
            if (starve_timer >= 4.0f) { hurt(1.0f); starve_timer = 0.0f; }
        } else starve_timer = 0.0f;

        // drowning: head block is water
        glm::vec3 e = eye();
        bool head_in_water = block_is_liquid(world.get_block((int)floorf(e.x),(int)floorf(e.y),(int)floorf(e.z)));
        if (head_in_water) {
            air = std::max(0.0f, air - dt * 2.0f);
            if (air <= 0.0f) hurt(dt * 2.0f);
        } else air = std::min(10.0f, air + dt * 4.0f);

        if (hurt_flash > 0) hurt_flash -= dt;
    }

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

    float fall_start_y = 0.0f;  // y where the current fall began
    bool  was_falling = false;

    // wish = horizontal input (x=strafe, z=forward) in [-1,1]; up = jump/fly up
    void update(World& world, float dt, glm::vec3 wish, bool jump, bool down) {
        glm::vec3 dir = right_flat() * wish.x + forward_flat() * wish.z;
        if (glm::length(dir) > 1e-4f) dir = glm::normalize(dir);

        bool in_liquid = block_is_liquid(world.get_block(
            (int)floorf(pos.x), (int)floorf(pos.y + 0.5f), (int)floorf(pos.z)));

        if (flying) {
            vel.x = dir.x * FLY_SPEED;
            vel.z = dir.z * FLY_SPEED;
            vel.y = (jump ? FLY_SPEED : 0.0f) - (down ? FLY_SPEED : 0.0f);
            was_falling = false;
        } else if (in_liquid) {
            // swimming: slow, buoyant, no fall damage
            vel.x = dir.x * WALK_SPEED * 0.6f;
            vel.z = dir.z * WALK_SPEED * 0.6f;
            vel.y -= GRAVITY * 0.30f * dt;
            if (jump) vel.y = 3.0f;
            if (vel.y < -4.0f) vel.y = -4.0f;
            was_falling = false;
        } else {
            vel.x = dir.x * WALK_SPEED;
            vel.z = dir.z * WALK_SPEED;
            vel.y -= GRAVITY * dt;
            if (jump && on_ground) { vel.y = JUMP_V; on_ground = false; }
            // track fall apex for fall damage
            if (!on_ground && vel.y < 0 && !was_falling) { was_falling = true; fall_start_y = pos.y; }
        }

        move_axis(world, 0, vel.x * dt);
        bool prev_ground = on_ground;
        on_ground = false;
        move_axis(world, 1, vel.y * dt);
        move_axis(world, 2, vel.z * dt);

        // landed this frame -> apply fall damage beyond a 3-block safe drop
        if (on_ground && !prev_ground && was_falling && !flying) {
            float dist = fall_start_y - pos.y;
            if (dist > 3.0f) hurt((dist - 3.0f) * 1.0f);
            was_falling = false;
        }
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
