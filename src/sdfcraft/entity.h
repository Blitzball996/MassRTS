#pragma once
// =============================================================================
// SDFCraft - Entity / Mob system (Phase D)
// -----------------------------------------------------------------------------
// A compact entity model isolated from the RTS ECS. Covers LivingEntity state
// (health, position, velocity), mob type defs (passive/hostile), and physics
// against the block world. AI goals + pathfinding live in ai.h; spawning in
// spawner.h. This mirrors MinecraftConsoles Entity/LivingEntity/Mob without the
// per-class file explosion: behaviour is driven by EntityType data + a goal set.
// =============================================================================
#include "world.h"
#include "items.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <vector>
#include <cmath>

namespace sdfcraft {

enum class MobKind : uint8_t {
    // passive
    Pig, Cow, Sheep, Chicken,
    // neutral / tamable
    Wolf,
    // hostile
    Zombie, Skeleton, Creeper, Spider, Enderman, Slime,
    // boss (Phase I) - listed so type table is complete
    EnderDragon, Wither,
    COUNT
};

enum class Hostility : uint8_t { Passive, Neutral, Hostile, Boss };

struct MobDef {
    const char* name;
    Hostility   hostility;
    float       max_health;
    float       move_speed;   // blocks/sec
    float       attack_dmg;   // melee (0 = doesn't melee)
    float       width;        // AABB half-width
    float       height;       // AABB full height
    bool        burns_in_sun; // zombies/skeletons
    glm::vec3   color;        // placeholder render tint until models (A3)
    ItemId      drop;         // primary drop item (ITEM_NONE = none)
    uint8_t     drop_max;     // max drop count
};

inline const MobDef& mob_def(MobKind k) {
    static const MobDef defs[(int)MobKind::COUNT] = {
        /* Pig      */ {"pig",      Hostility::Passive, 10, 2.5f, 0.0f, 0.45f, 0.9f, false, {0.92f,0.55f,0.60f}, ITEM_PORKCHOP, 3},
        /* Cow      */ {"cow",      Hostility::Passive, 10, 2.2f, 0.0f, 0.45f, 1.4f, false, {0.35f,0.26f,0.18f}, ITEM_NONE, 0},
        /* Sheep    */ {"sheep",    Hostility::Passive, 8,  2.3f, 0.0f, 0.45f, 1.3f, false, {0.92f,0.92f,0.90f}, ITEM_NONE, 0},
        /* Chicken  */ {"chicken",  Hostility::Passive, 4,  2.6f, 0.0f, 0.30f, 0.7f, false, {0.95f,0.95f,0.95f}, ITEM_NONE, 0},
        /* Wolf     */ {"wolf",     Hostility::Neutral, 8,  3.0f, 4.0f, 0.40f, 0.85f,false, {0.78f,0.78f,0.80f}, ITEM_NONE, 0},
        /* Zombie   */ {"zombie",   Hostility::Hostile, 20, 2.3f, 3.0f, 0.45f, 1.95f,true,  {0.30f,0.55f,0.40f}, ITEM_NONE, 0},
        /* Skeleton */ {"skeleton", Hostility::Hostile, 20, 2.5f, 2.0f, 0.45f, 1.99f,true,  {0.82f,0.82f,0.82f}, ITEM_NONE, 0},
        /* Creeper  */ {"creeper",  Hostility::Hostile, 20, 2.3f, 0.0f, 0.45f, 1.7f, false, {0.35f,0.75f,0.35f}, ITEM_NONE, 0},
        /* Spider   */ {"spider",   Hostility::Hostile, 16, 3.0f, 2.0f, 0.70f, 0.9f, false, {0.25f,0.20f,0.22f}, ITEM_NONE, 0},
        /* Enderman */ {"enderman", Hostility::Neutral, 40, 3.2f, 7.0f, 0.40f, 2.9f, false, {0.08f,0.06f,0.12f}, ITEM_NONE, 0},
        /* Slime    */ {"slime",    Hostility::Hostile, 16, 1.6f, 2.0f, 0.55f, 1.1f, false, {0.45f,0.80f,0.45f}, ITEM_NONE, 0},
        /* Dragon   */ {"ender_dragon",Hostility::Boss, 200,4.0f,10.0f, 2.0f, 2.0f, false, {0.10f,0.05f,0.14f}, ITEM_NONE, 0},
        /* Wither   */ {"wither",   Hostility::Boss,   300, 3.0f, 8.0f, 0.9f, 3.5f, false, {0.10f,0.10f,0.10f}, ITEM_NONE, 0},
    };
    return defs[(int)k];
}

struct Entity {
    uint32_t id = 0;
    MobKind  kind = MobKind::Pig;
    glm::vec3 pos{0,0,0};      // feet
    glm::vec3 vel{0,0,0};
    float yaw = 0.0f;
    float health = 10.0f;
    bool  on_ground = false;
    bool  alive = true;
    bool  tamed = false;       // wolves
    uint32_t target_id = 0;    // current attack target (0 = none)
    float hurt_cooldown = 0.0f;
    float attack_cooldown = 0.0f;
    float ai_timer = 0.0f;     // generic goal timer (stroll/wait)
    glm::vec3 wander_dir{0,0,0};
    // Render-only (NOT serialized): set true when the entity is actually moving
    // this frame, so the model renderer animates legs/arms only when walking
    // (no more idle mobs paddling in place). Server sets it from |vel.xz|;
    // clients set it from the authoritative `moving` bit in MobSnapshot.
    bool  render_moving = false;
    // Significance LOD (server-side scheduling, NOT serialized): far mobs tick
    // less often. `lod_accum` banks the dt skipped between ticks so when the mob
    // does run, it advances by the full elapsed time (physics/AI stay correct,
    // just coarser). `lod_phase` staggers far mobs across frames so they don't
    // all update on the same tick (avoids a periodic spike).
    float lod_accum = 0.0f;
    uint8_t lod_phase = 0;

    const MobDef& def() const { return mob_def(kind); }

    void hurt(float dmg) {
        if (hurt_cooldown > 0.0f) return;
        health -= dmg;
        hurt_cooldown = 0.4f;
        if (health <= 0.0f) { health = 0; alive = false; }
    }
};

// --- shared entity physics against the block world (gravity + AABB) ---
// Collision tests the smooth terrain surface + discrete object blocks. Uses the
// CHEAP terrain_solid_cheap (one FBM eval) instead of the full trilinear
// sample_sdf (~40 FBM evals) — a mob collider runs hundreds of times per tick,
// so the full path made the host/solo stutter with a populated world. The cheap
// test resolves to the SAME smooth surface (sign-identical) the player walks.
inline bool entity_point_solid(World& w, float x, float y, float z) {
    if (w.terrain_solid_cheap(x, y, z)) return true;           // smooth terrain (cheap, ~33x)
    BlockId b = w.get_block_ro((int)floorf(x), (int)floorf(y), (int)floorf(z));  // worker-safe (no chunk create)
    return block_is_solid(b) && !block_is_terrain(b);          // trees / placed
}

inline bool entity_collides(World& w, const MobDef& d, glm::vec3 p) {
    const float wd = d.width;
    // sample feet / mid / head over the AABB footprint (matches player collider)
    const float ys[3] = { 0.10f, d.height * 0.5f, d.height - 0.10f };
    for (float yy : ys)
        for (int sx = -1; sx <= 1; sx++)
        for (int sz = -1; sz <= 1; sz++)
            if (entity_point_solid(w, p.x + sx*wd, p.y + yy, p.z + sz*wd)) return true;
    return false;
}

inline void entity_move_axis(World& w, Entity& e, const MobDef& d, int axis, float amt,
                             bool step_grounded) {
    if (amt == 0.0f) return;
    glm::vec3 np = e.pos; np[axis] += amt;
    if (!entity_collides(w, d, np)) { e.pos = np; return; }

    // Horizontal block: auto step-up the smooth slope/ledge, EXACTLY like the
    // player (player.h move_axis). Gated on `step_grounded` = the ground state
    // BEFORE this frame's vertical move, captured once and used for both X and Z
    // (using the live e.on_ground breaks the Z axis because the Y move clears it).
    if (axis != 1 && step_grounded) {
        const float MAX_STEP = 1.1f;
        for (float lift = 0.2f; lift <= MAX_STEP; lift += 0.2f) {
            glm::vec3 stepped = np; stepped.y += lift;
            if (!entity_collides(w, d, stepped)) {
                // settle back down onto the surface (binary search the contact)
                float lo = 0.0f, hi = lift;
                for (int i = 0; i < 12; i++) {
                    float mid = 0.5f*(lo+hi);
                    glm::vec3 dn = stepped; dn.y -= mid;
                    if (entity_collides(w, d, dn)) hi = mid; else lo = mid;
                }
                glm::vec3 settled = stepped; settled.y -= lo;
                if (!entity_collides(w, d, settled)) { e.pos = settled; e.on_ground = true; return; }
            }
        }
    }

    if (axis == 1) { if (amt < 0) e.on_ground = true; e.vel.y = 0; }
    else            e.vel[axis] = 0;
    float lo = 0, hi = amt;
    for (int i=0;i<8;i++){ float m=0.5f*(lo+hi); glm::vec3 pr=e.pos; pr[axis]+=m;
        if (entity_collides(w,d,pr)) hi=m; else lo=m; }
    e.pos[axis] += lo;
}

// step physics; `want` is desired horizontal velocity. Mobs do NOT auto-jump to
// walk (same as the player — slopes are climbed via the step-up above); `jump`
// is only used for knockback/explicit hops if a caller ever sets it.
inline void entity_physics(World& w, Entity& e, glm::vec3 want, bool jump, float dt) {
    const MobDef& d = e.def();
    bool was_ground = e.on_ground;          // capture BEFORE moves, like the player
    e.vel.x = want.x; e.vel.z = want.z;
    e.vel.y -= 28.0f * dt;
    if (jump && e.on_ground) { e.vel.y = 7.5f; e.on_ground = false; was_ground = false; }
    entity_move_axis(w, e, d, 0, e.vel.x*dt, was_ground);
    e.on_ground = false;
    entity_move_axis(w, e, d, 1, e.vel.y*dt, was_ground);
    entity_move_axis(w, e, d, 2, e.vel.z*dt, was_ground);
}

} // namespace sdfcraft
