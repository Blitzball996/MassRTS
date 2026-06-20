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
    glm::vec3 point{0,0,0};      // precise world-space contact point
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

    // Render-time eye: same as eye() horizontally, but the vertical component is
    // smoothed. Step-ups and landing snap pos.y by up to ~1 block in a single
    // frame, which makes the camera visibly judder when walking uphill. We ease
    // the rendered height toward the true height so slopes/stairs feel smooth
    // while collision/physics still use the exact pos.
    glm::vec3 render_eye() const {
        return glm::vec3(pos.x, smooth_y_ + EYE, pos.z);
    }
    // Call once per frame (after update) to advance the smoothed camera height.
    void update_camera_smoothing(float dt) {
        float target = pos.y;
        float diff = target - smooth_y_;
        // Snap if far off (teleport/fall), otherwise critically-damped follow.
        if (std::fabs(diff) > 2.0f || flying) { smooth_y_ = target; return; }
        float rate = 16.0f;                 // higher = snappier
        float a = 1.0f - std::exp(-rate * dt);
        smooth_y_ += diff * a;
    }

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
    float smooth_y_ = 80.0f;    // eased vertical position for the camera

    // wish = horizontal input (x=strafe, z=forward) in [-1,1]; up = jump/fly up.
    // fly_boost multiplies fly speed (hold to cover planet-scale distances fast).
    void update(World& world, float dt, glm::vec3 wish, bool jump, bool down,
                float fly_boost = 1.0f) {
        glm::vec3 dir = right_flat() * wish.x + forward_flat() * wish.z;
        if (glm::length(dir) > 1e-4f) dir = glm::normalize(dir);

        bool in_liquid = block_is_liquid(world.get_block(
            (int)floorf(pos.x), (int)floorf(pos.y + 0.5f), (int)floorf(pos.z)));

        if (flying) {
            // free-fly: full 3D, no gravity/collision-stop on the vertical input.
            // Boosted speed lets you fly up to orbit / across the planet quickly.
            float fs = FLY_SPEED * fly_boost;
            // fly in the FULL look direction (incl. pitch) when moving forward,
            // so you can ascend toward space just by looking up and holding W.
            glm::vec3 look = forward();
            glm::vec3 flyDir = right_flat()*wish.x + look*wish.z;
            if (glm::length(flyDir) > 1e-4f) flyDir = glm::normalize(flyDir);
            vel = flyDir * fs;
            vel.y += (jump ? fs : 0.0f) - (down ? fs : 0.0f);
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

        bool prev_ground = on_ground;
        step_grounded_ = prev_ground;   // both horizontal axes may step-up
        move_axis(world, 0, vel.x * dt);
        on_ground = false;
        move_axis(world, 1, vel.y * dt);
        step_grounded_ = prev_ground;   // restore for z (axis 1 may have cleared)
        move_axis(world, 2, vel.z * dt);

        // landed this frame -> apply fall damage beyond a 3-block safe drop
        if (on_ground && !prev_ground && was_falling && !flying) {
            float dist = fall_start_y - pos.y;
            if (dist > 3.0f) hurt((dist - 3.0f) * 1.0f);
            was_falling = false;
        }
    }

    // SDF sphere-trace: march the *continuous* terrain field the renderer
    // actually draws, so "what you see is what you carve". The old version
    // walked the discrete block grid (DDA), but the visible surface is the
    // smooth SDF isosurface at a fractional height — the two disagreed, so the
    // carve sphere landed beside the surface and digging hit empty air. Marching
    // sample_sdf fixes that: we stop at the zero-crossing, place the carve there,
    // and derive the block coord/face from the surface normal for selection,
    // placement and the hardness lookup.
    RayHit raycast(World& world, float max_dist = 6.0f) {
        glm::vec3 o = eye(), d = forward();
        RayHit r;
        float t = 0.0f;
        // If we start inside solid (prev<0) the first sample already crossed.
        const float step = 0.05f;     // fine march keeps the contact crisp
        // Track the voxel we were last in so we can detect entering an object
        // block (trees / placed blocks) and recover the face we crossed.
        int pvx = (int)std::floor(o.x), pvy = (int)std::floor(o.y), pvz = (int)std::floor(o.z);
        for (int i = 0; i < (int)(max_dist / step) + 2 && t <= max_dist; i++) {
            float nt = t + step;
            glm::vec3 p = o + d * nt;

            // (1) Discrete object blocks (logs / leaves / planks / placed) are
            // NOT in the smooth SDF field, so the ray would pass straight
            // through them. March the voxel grid in parallel and stop on the
            // first solid non-terrain block we step into — this is what makes
            // trees selectable / breakable.
            int vx = (int)std::floor(p.x), vy = (int)std::floor(p.y), vz = (int)std::floor(p.z);
            if (vx != pvx || vy != pvy || vz != pvz) {
                BlockId b = world.get_block(vx, vy, vz);
                if (block_is_solid(b) && !block_is_terrain(b)) {
                    r.hit = true;
                    r.dist = nt;
                    r.point = p;
                    r.bx = vx; r.by = vy; r.bz = vz;
                    // Entry face: the air voxel we came from. Reduce to the
                    // single dominant axis the ray crossed (largest |d|).
                    int dx = pvx - vx, dy = pvy - vy, dz = pvz - vz;
                    float ax = dx ? fabsf(d.x) : -1.0f;
                    float ay = dy ? fabsf(d.y) : -1.0f;
                    float az = dz ? fabsf(d.z) : -1.0f;
                    if (ax >= ay && ax >= az)      { r.nx = dx; }
                    else if (ay >= az)             { r.ny = dy; }
                    else                           { r.nz = dz; }
                    return r;
                }
            }
            pvx = vx; pvy = vy; pvz = vz;

            // (2) Smooth Marching-Cubes terrain via the continuous SDF.
            float s = world.sample_sdf(p.x, p.y, p.z);
            if (s < 0.0f) {
                // crossed the surface between t and nt: refine by bisection
                float lo = t, hi = nt;
                for (int b = 0; b < 12; b++) {
                    float mid = 0.5f * (lo + hi);
                    glm::vec3 pm = o + d * mid;
                    if (world.sample_sdf(pm.x, pm.y, pm.z) < 0.0f) hi = mid; else lo = mid;
                }
                glm::vec3 hitp = o + d * hi;
                glm::vec3 nrm  = world.sdf_normal(hitp);   // points toward air
                r.hit = true;
                r.dist = hi;
                r.point = hitp;
                // Block coord = the solid voxel just inside the surface.
                glm::vec3 inside = hitp - nrm * 0.5f;
                r.bx = (int)std::floor(inside.x);
                r.by = (int)std::floor(inside.y);
                r.bz = (int)std::floor(inside.z);
                // Face normal: dominant axis of the surface normal (for placing).
                if (fabsf(nrm.x) >= fabsf(nrm.y) && fabsf(nrm.x) >= fabsf(nrm.z))
                    r.nx = nrm.x > 0 ? 1 : -1;
                else if (fabsf(nrm.y) >= fabsf(nrm.z))
                    r.ny = nrm.y > 0 ? 1 : -1;
                else
                    r.nz = nrm.z > 0 ? 1 : -1;
                return r;
            }
            t = nt;
        }
        return r;
    }

private:
    bool solid_at(World& world, int x, int y, int z) {
        return block_is_solid(world.get_block(x, y, z));
    }

    // True if the world is solid at world-point p. Terrain uses the CONTINUOUS
    // SDF (negative = inside ground) so the collision body matches the smooth
    // surface the player actually sees — the old per-block test collided with
    // invisible integer-height steps on slopes, which is what jammed the player
    // when walking uphill. Object blocks (logs, placed blocks) are not part of
    // the SDF field, so we still test those discretely.
    bool point_solid(World& world, float x, float y, float z) {
        if (world.sample_sdf(x, y, z) < 0.0f) return true;            // smooth terrain
        BlockId b = world.get_block((int)floorf(x),(int)floorf(y),(int)floorf(z));
        return block_is_solid(b) && !block_is_terrain(b);             // trees/placed
    }

    // Does the player capsule/AABB at position p overlap solid ground? Samples a
    // small set of points around the AABB (cheap and matches the smooth field).
    bool collides(World& world, glm::vec3 p) {
        const float w = HALF_W;
        // sample at feet, mid, head over a 3x3 footprint
        static const float ys[3] = {0.10f, HEIGHT*0.5f, HEIGHT-0.10f};
        for (float yy : ys)
        for (int sx = -1; sx <= 1; sx++)
        for (int sz = -1; sz <= 1; sz++) {
            if (point_solid(world, p.x + sx*w, p.y + yy, p.z + sz*w)) return true;
        }
        return false;
    }

    // Vertical clearance check used by step-up: is the column at (p) free over
    // the player's full height? (so we don't step into a low ceiling).
    bool head_clear(World& world, glm::vec3 p) {
        return !collides(world, p);
    }

    // Move along a single axis, stopping flush against the first collision.
    // Horizontal moves (x/z) that hit terrain attempt an automatic STEP-UP: if
    // raising the player by up to MAX_STEP clears the obstruction, we climb it.
    // This is what lets the player walk smoothly up steep slopes (up to ~75°+)
    // and over small ledges instead of jamming against the SDF surface.
    static constexpr float MAX_STEP = 1.1f;   // max auto-climb height (blocks)
    bool step_grounded_ = false;              // last-frame ground state for step-up

    void move_axis(World& world, int axis, float amount) {
        if (amount == 0.0f) return;
        glm::vec3 np = pos;
        np[axis] += amount;
        if (!collides(world, np)) { pos = np; return; }

        // --- horizontal collision: try to step up the slope/ledge ---
        if (axis != 1 && step_grounded_) {
            // Probe increasing lift heights; take the smallest that frees both
            // the new horizontal position AND leaves head clearance.
            for (float lift = 0.20f; lift <= MAX_STEP; lift += 0.20f) {
                glm::vec3 stepped = np;
                stepped.y += lift;
                if (!collides(world, stepped)) {
                    // Settle back down onto the surface so we hug the slope.
                    // Binary-search the exact contact height instead of stepping
                    // down in coarse 0.05 increments — the old loop snapped pos.y
                    // to a 5cm grid every frame, so walking across the smooth
                    // (never perfectly flat) FBM ground made the height oscillate
                    // and the camera bob. A continuous solve lands flush on the
                    // surface and keeps pos.y stable frame to frame.
                    float lo = 0.0f, hi = lift;     // drop amount: lo free, hi unknown
                    glm::vec3 deepest = stepped; deepest.y -= lift;
                    if (collides(world, deepest)) {
                        for (int i = 0; i < 12; i++) {
                            float mid = 0.5f * (lo + hi);
                            glm::vec3 d = stepped; d.y -= mid;
                            if (collides(world, d)) hi = mid; else lo = mid;
                        }
                    } else {
                        lo = lift;                  // free all the way down to np
                    }
                    glm::vec3 settled = stepped; settled.y -= lo;
                    if (!collides(world, settled)) { pos = settled; on_ground = true; return; }
                }
            }
        }

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
