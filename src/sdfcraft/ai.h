#pragma once
// =============================================================================
// SDFCraft - Mob AI + entity manager (Phase D)
// -----------------------------------------------------------------------------
// Goal-based behaviour (mirrors MinecraftConsoles GoalSelector/Goal) collapsed
// into a per-tick state machine keyed off Hostility: passive mobs wander/flee,
// hostile mobs acquire the player and melee it. Movement is obstacle-aware
// (auto-jumps 1-block steps) instead of full A*; this keeps the Phase D loop
// light while leaving room for PathFinder later. The EntityManager spawns mobs
// around the player by day/night rules and culls distant/dead ones.
// =============================================================================
#include "entity.h"
#include "world.h"
#include <vector>
#include <cmath>
#include <cstdint>
#include <algorithm>

namespace sdfcraft {

// Deterministic-ish per-call RNG (seeded by caller for reproducibility).
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ULL) {}
    uint64_t next() { s ^= s<<13; s ^= s>>7; s ^= s<<17; return s; }
    float    f01()  { return (float)((next()>>11)&0xFFFFFF)/(float)0xFFFFFF; }
    float    range(float a, float b) { return a + (b-a)*f01(); }
    int      irange(int a, int b) { return a + (int)(f01()*(b-a+1)); }
};

class EntityManager {
public:
    std::vector<Entity> entities;
    uint32_t next_id = 1;
    bool is_night = false;       // driven by day/night cycle (Phase O)
    int  hostile_cap = 40;
    int  passive_cap = 30;

    explicit EntityManager(uint64_t seed = 12345) : rng_(seed) {}

    Entity& spawn(MobKind k, glm::vec3 pos) {
        Entity e;
        e.id = next_id++;
        e.kind = k;
        e.pos = pos;
        e.health = mob_def(k).max_health;
        e.yaw = rng_.range(0, 360);
        entities.push_back(e);
        return entities.back();
    }

    // Advance all mobs. `player_pos` lets hostiles target the player; returns
    // total melee damage dealt to the player this tick (applied by caller).
    float update(World& world, glm::vec3 player_pos, float dt) {
        float player_dmg = 0.0f;
        for (auto& e : entities) {
            if (!e.alive) continue;
            if (e.hurt_cooldown > 0)   e.hurt_cooldown   -= dt;
            if (e.attack_cooldown > 0) e.attack_cooldown -= dt;

            // sunlight burning for undead (very rough: daytime + open sky)
            if (e.def().burns_in_sun && !is_night && sky_exposed(world, e.pos))
                e.hurt(1.0f * dt * 2.0f);

            const MobDef& d = e.def();
            glm::vec3 want(0,0,0);
            bool jump = false;

            if (d.hostility == Hostility::Hostile || (d.hostility==Hostility::Boss)) {
                player_dmg += hostile_ai(world, e, player_pos, want, jump, dt);
            } else {
                passive_ai(world, e, want, jump, dt);
            }
            entity_physics(world, e, want, jump, dt);

            // void / fell-out-of-world cleanup
            if (e.pos.y < -8) e.alive = false;
        }
        // remove dead/despawned
        entities.erase(std::remove_if(entities.begin(), entities.end(),
            [&](const Entity& e){ return !e.alive; }), entities.end());
        return player_dmg;
    }

    // Spawn/cull around the player to keep a populated but bounded world.
    void manage_population(World& world, glm::vec3 player_pos) {
        int hostile = 0, passive = 0;
        for (auto& e : entities) {
            // cull far entities (outside ~10 chunks)
            if (glm::length(e.pos - player_pos) > 160.0f) { e.alive = false; continue; }
            Hostility h = e.def().hostility;
            if (h == Hostility::Hostile) hostile++;
            else if (h == Hostility::Passive) passive++;
        }
        // try a few spawns per call
        for (int i = 0; i < 4; i++) {
            if (is_night && hostile < hostile_cap) {
                if (try_spawn(world, player_pos, true))  hostile++;
            } else if (!is_night && passive < passive_cap) {
                if (try_spawn(world, player_pos, false)) passive++;
            }
        }
    }

private:
    Rng rng_;

    bool sky_exposed(World& w, glm::vec3 p) {
        int x=(int)floorf(p.x), z=(int)floorf(p.z), y=(int)floorf(p.y+1.5f);
        for (int yy=y+1; yy<CHUNK_SY; yy++)
            if (block_is_opaque(w.get_block(x,yy,z))) return false;
        return true;
    }

    // Find a valid ground spot in a ring around the player and spawn there.
    bool try_spawn(World& world, glm::vec3 player_pos, bool hostile) {
        float ang = rng_.range(0, 6.2831853f);
        float dist = rng_.range(24.0f, 80.0f);
        int x = (int)floorf(player_pos.x + cosf(ang)*dist);
        int z = (int)floorf(player_pos.z + sinf(ang)*dist);
        int h = world.surface_height(x, z);
        if (h <= 0 || h >= CHUNK_SY-3) return false;
        // need 2 air blocks above a solid surface
        if (!block_is_solid(world.get_block(x,h,z))) return false;
        if (block_is_solid(world.get_block(x,h+1,z)) || block_is_solid(world.get_block(x,h+2,z)))
            return false;
        glm::vec3 sp((float)x+0.5f, (float)(h+1), (float)z+0.5f);
        MobKind k = hostile ? pick_hostile() : pick_passive();
        spawn(k, sp);
        return true;
    }

    MobKind pick_hostile() {
        static const MobKind opts[] = {MobKind::Zombie, MobKind::Skeleton, MobKind::Creeper,
                                       MobKind::Spider, MobKind::Slime};
        return opts[rng_.irange(0, 4)];
    }
    MobKind pick_passive() {
        static const MobKind opts[] = {MobKind::Pig, MobKind::Cow, MobKind::Sheep,
                                       MobKind::Chicken, MobKind::Wolf};
        return opts[rng_.irange(0, 4)];
    }

    // --- behaviours ---
    void passive_ai(World& world, Entity& e, glm::vec3& want, bool& jump, float dt) {
        e.ai_timer -= dt;
        if (e.ai_timer <= 0.0f) {
            // pick a new wander direction or pause
            if (rng_.f01() < 0.35f) { e.wander_dir = glm::vec3(0); e.ai_timer = rng_.range(1,3); }
            else {
                float a = rng_.range(0, 6.2831853f);
                e.wander_dir = glm::vec3(cosf(a), 0, sinf(a));
                e.yaw = glm::degrees(a);
                e.ai_timer = rng_.range(2, 5);
            }
        }
        apply_move(world, e, e.wander_dir, e.def().move_speed * 0.5f, want, jump);
    }

    float hostile_ai(World& world, Entity& e, glm::vec3 player_pos, glm::vec3& want, bool& jump, float dt) {
        glm::vec3 to = player_pos - e.pos; to.y = 0;
        float d2 = glm::length(to);
        const MobDef& md = e.def();
        float dealt = 0.0f;
        // detection range
        if (d2 < 32.0f && d2 > 0.01f) {
            glm::vec3 dir = to / d2;
            e.yaw = glm::degrees(atan2f(dir.z, dir.x));
            if (d2 > 1.4f) {
                apply_move(world, e, dir, md.move_speed, want, jump);
            } else if (md.attack_dmg > 0.0f && e.attack_cooldown <= 0.0f) {
                dealt = md.attack_dmg;
                e.attack_cooldown = 1.0f;
            }
        } else {
            // idle wander when no target
            passive_ai(world, e, want, jump, dt);
        }
        return dealt;
    }

    // Move toward dir; auto-jump if a 1-block step is in the way and ground ahead.
    void apply_move(World& world, Entity& e, glm::vec3 dir, float speed,
                    glm::vec3& want, bool& jump) {
        if (glm::length(dir) < 1e-4f) { want = glm::vec3(0); return; }
        dir = glm::normalize(dir);
        want = dir * speed;
        // look one block ahead at foot height; if solid and head clear -> jump
        int fx = (int)floorf(e.pos.x + dir.x * 0.6f);
        int fz = (int)floorf(e.pos.z + dir.z * 0.6f);
        int fy = (int)floorf(e.pos.y);
        if (block_is_solid(world.get_block(fx, fy, fz)) &&
            !block_is_solid(world.get_block(fx, fy+1, fz)) &&
            !block_is_solid(world.get_block(fx, fy+2, fz))) {
            jump = true;
        }
    }
};

} // namespace sdfcraft
