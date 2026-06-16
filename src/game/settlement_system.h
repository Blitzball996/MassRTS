#pragma once
// ============================================================================
// SETTLEMENT SYSTEM  (Phase 1: autonomous encampment + production)
//
// Vision: an AI commander marches a vanguard forward, picks a spot, sets up
// camp (plants an HQ), develops it (builds a barracks), and the barracks then
// produces reinforcements that rally toward the enemy. Later phases add
// economy (workers/miners/traders), expansion, garrison pressure and an
// anti-snowball guerrilla layer.
//
// Architecture (hybrid "1C"): this module owns the STRATEGIC state. Each
// building is also a real ECS entity (is_structure) so it renders and takes
// damage through the existing unit pipeline -- HQ is mapped to a Turret-type
// entity (defends itself), a Barracks to a tanky Wall-type entity. No renderer
// changes are required.
//
// Player authority: this system never touches units that carry a player move
// command, so player orders always win.
// ============================================================================
#include "../ecs/world.h"
#include "../ecs/components.h"
#include <glm/glm.hpp>
#include <vector>
#include <cmath>
#include <iostream>

enum class BuildingKind : uint8_t { HQ = 0, Barracks = 1, Mine = 2, Farm = 3 };

struct Building {
    BuildingKind kind;
    Entity entity = INVALID_ENTITY; // backing ECS structure entity
    glm::vec2 pos{0, 0};
    float build_progress = 0.0f;    // 0..1, construction completeness
    float prod_timer = 0.0f;        // production accumulator (barracks)
    bool placed = false;            // ECS entity created yet?
};

struct Settlement {
    Faction faction = Faction::Red;
    glm::vec2 center{0, 0};
    float claim_radius = 140.0f;    // claimed territory around the HQ
    bool established = false;       // HQ planted?
    std::vector<Building> buildings;
};

// Commander strategic phases.
enum class CommanderState : uint8_t {
    March = 0,   // vanguard moving toward chosen campsite
    Settle = 1,  // plant the HQ
    Develop = 2, // construct economy/military buildings (Phase 1: a barracks)
    Produce = 3  // barracks pumps out reinforcements
};

// One strategic brain per faction.
struct AICommander {
    Faction faction = Faction::Blue;
    CommanderState state = CommanderState::March;
    glm::vec2 home{0, 0};        // where this faction starts
    glm::vec2 enemy_dir{1, 0};   // unit vector pointing at the enemy
    glm::vec2 campsite{0, 0};    // chosen settle location
    float advance = 0.0f;        // how far the vanguard has pushed (world units)
    float think_timer = 0.0f;
    bool active = false;
};

// ============================================================================
// SettlementSystem: drives both AI commanders and all settlements/buildings.
// ============================================================================
class SettlementSystem {
public:
    float (*height_fn)(float, float) = nullptr; // terrain sampler (optional)

    std::vector<Settlement> settlements;
    AICommander commanders[2];

    // Tunables (Phase 1)
    float settle_distance = 700.0f;   // how far from home the vanguard camps
    float hq_build_time = 6.0f;       // seconds to raise the HQ
    float barracks_build_time = 8.0f; // seconds to raise a barracks
    float produce_interval = 5.0f;    // barracks production cadence
    int   produce_batch = 40;         // units per production tick
    float march_speed = 120.0f;       // vanguard advance speed (world u/s)

    // Map a building to a backing ECS structure (reuses existing unit types so
    // it renders + fights with no renderer changes).
    static UnitType backing_type(BuildingKind k) {
        // HQ self-defends (Turret); everything else is a passive Wall-type box.
        return (k == BuildingKind::HQ) ? UnitType::Turret : UnitType::Wall;
    }
    static float building_hp(BuildingKind k) {
        switch (k) {
            case BuildingKind::HQ:       return 4000.0f;
            case BuildingKind::Barracks: return 2500.0f;
            case BuildingKind::Mine:     return 1500.0f;
            case BuildingKind::Farm:     return 1200.0f;
        }
        return 1500.0f;
    }
    static float building_scale(BuildingKind k) {
        switch (k) {
            case BuildingKind::HQ:       return 9.0f;
            case BuildingKind::Barracks: return 6.0f;
            case BuildingKind::Mine:     return 5.0f;
            case BuildingKind::Farm:     return 5.5f;
        }
        return 5.0f;
    }
    // Per-second resource yield (gold) once construction completes.
    static float building_income(BuildingKind k) {
        switch (k) {
            case BuildingKind::Mine: return 6.0f;  // gold/sec
            case BuildingKind::Farm: return 3.5f;  // food -> treated as gold in Phase 1
            default: return 0.0f;
        }
    }

    void init(glm::vec2 red_home, glm::vec2 blue_home) {
        settlements.clear();
        glm::vec2 homes[2] = {red_home, blue_home};
        for (int f = 0; f < 2; f++) {
            AICommander& c = commanders[f];
            c.faction = (Faction)f;
            c.state = CommanderState::March;
            c.home = homes[f];
            glm::vec2 d = homes[1 - f] - homes[f];
            float len = glm::length(d);
            c.enemy_dir = (len > 0.001f) ? d / len : glm::vec2(1, 0);
            c.campsite = c.home + c.enemy_dir * settle_distance;
            c.advance = 0.0f;
            c.think_timer = 0.0f;
            c.active = true;
        }
    }

    // Spawn the backing ECS structure for a building (renders + takes damage).
    void place_building(World& world, Settlement& s, Building& b) {
        Entity e = world.create_entity();
        if (e == INVALID_ENTITY) return;
        b.entity = e;
        b.placed = true;
        const char* kn = (b.kind==BuildingKind::HQ)?"HQ":
                         (b.kind==BuildingKind::Barracks)?"Barracks":
                         (b.kind==BuildingKind::Mine)?"Mine":"Farm";
        std::cout << "[Settlement] " << ((int)s.faction==0?"Red":"Blue")
                  << " placed " << kn << " at (" << b.pos.x << "," << b.pos.y << ")\n";
        world.transforms.position[e] = b.pos;
        world.transforms.velocity[e] = {0, 0};
        world.transforms.rotation[e] = (s.faction == Faction::Red) ? 0.0f : 3.14159f;
        world.transforms.y_offset[e] = 0;
        world.transforms.y_velocity[e] = 0;
        world.units.faction[e] = s.faction;
        world.units.type[e] = backing_type(b.kind);
        world.units.state[e] = UnitState::Idle;
        float hp = building_hp(b.kind);
        world.units.health[e] = hp;
        world.units.max_health[e] = hp;
        // HQ defends itself; barracks is passive (no attack).
        world.units.attack_damage[e] = (b.kind == BuildingKind::HQ) ? 30.0f : 0.0f;
        world.units.attack_range[e]  = (b.kind == BuildingKind::HQ) ? 130.0f : 0.0f;
        world.units.speed[e] = 0.0f;
        world.units.attack_cooldown[e] = 0;
        world.units.target[e] = INVALID_ENTITY;
        world.units.hit_timer[e] = 0;
        world.units.ragdoll_timer[e] = 0;
        world.units.is_structure[e] = true;
        world.renders.scale[e] = building_scale(b.kind);
        glm::vec3 fac_tint = (s.faction == Faction::Red)
            ? glm::vec3(0.30f, 0.32f, 0.22f) : glm::vec3(0.55f, 0.38f, 0.16f);
        switch (b.kind) {
            case BuildingKind::Mine: fac_tint = glm::vec3(0.32f, 0.34f, 0.40f); break; // ore grey
            case BuildingKind::Farm: fac_tint = glm::vec3(0.45f, 0.55f, 0.20f); break; // crop green
            default: break;
        }
        world.renders.color[e] = fac_tint;
        world.selection.player_owned[e] = false; // AI-owned in Phase 1
        world.selection.selected[e] = false;
        world.commands.has_move_command[e] = false;
    }

    bool building_alive(World& world, const Building& b) const {
        return b.placed && b.entity != INVALID_ENTITY && world.is_alive(b.entity);
    }

    // ------------------------------------------------------------------------
    // Per-frame strategic update. `rally` is where freshly produced units head
    // (Phase 1: toward the enemy home). Returns nothing; mutates world + state.
    // ------------------------------------------------------------------------
    void update(World& world, float dt) {
        for (int f = 0; f < 2; f++) {
            AICommander& c = commanders[f];
            if (!c.active) continue;
            switch (c.state) {
                case CommanderState::March:   tick_march(c, dt);            break;
                case CommanderState::Settle:  tick_settle(world, c);        break;
                case CommanderState::Develop: tick_develop(world, c, dt);   break;
                case CommanderState::Produce: tick_produce(world, c, dt);   break;
            }
        }
        // Advance construction + production for every settlement.
        for (auto& s : settlements) {
            int f = (int)s.faction;
            for (auto& b : s.buildings) {
                if (!building_alive(world, b)) continue;
                if (b.build_progress < 1.0f) {
                    float t = (b.kind == BuildingKind::HQ) ? hq_build_time : barracks_build_time;
                    b.build_progress = std::min(1.0f, b.build_progress + dt / t);
                } else {
                    // Finished economy buildings generate gold over time. The
                    // accumulator carries fractional income across frames so a
                    // 6 gold/sec mine pays out exactly, independent of framerate.
                    float inc = building_income(b.kind);
                    if (inc > 0.0f && f >= 0 && f < 2) {
                        b.prod_timer += inc * dt; // reuse prod_timer as gold accumulator
                        int whole = (int)b.prod_timer;
                        if (whole > 0) { world.money[f] += whole; b.prod_timer -= whole; }
                    }
                }
            }
        }
    }

private:
    Settlement* find_settlement(Faction fac) {
        for (auto& s : settlements)
            if (s.faction == fac && s.established) return &s;
        return nullptr;
    }

    void tick_march(AICommander& c, float dt) {
        c.advance += march_speed * dt;
        if (c.advance >= settle_distance) {
            c.campsite = c.home + c.enemy_dir * settle_distance;
            c.state = CommanderState::Settle;
        }
    }

    void tick_settle(World& world, AICommander& c) {
        Settlement s;
        s.faction = c.faction;
        s.center = c.campsite;
        s.established = true;
        // Plant the HQ at the campsite center.
        Building hq;
        hq.kind = BuildingKind::HQ;
        hq.pos = c.campsite;
        place_building(world, s, hq);
        s.buildings.push_back(hq);
        settlements.push_back(s);
        c.state = CommanderState::Develop;
        c.think_timer = 0.0f;
    }

    void tick_develop(World& world, AICommander& c, float dt) {
        Settlement* s = find_settlement(c.faction);
        if (!s) { c.state = CommanderState::March; return; }
        // Lay out the settlement: a barracks toward the enemy, plus economy
        // buildings (mine + farm) tucked behind the HQ. perpendicular spreads
        // them so they don't overlap.
        glm::vec2 perp(-c.enemy_dir.y, c.enemy_dir.x);
        auto ensure = [&](BuildingKind k, glm::vec2 off) {
            for (auto& b : s->buildings) if (b.kind == k) return;
            Building nb; nb.kind = k; nb.pos = s->center + off;
            place_building(world, *s, nb);
            s->buildings.push_back(nb);
        };
        ensure(BuildingKind::Barracks, c.enemy_dir *  45.0f);
        ensure(BuildingKind::Mine,     -c.enemy_dir * 40.0f + perp * 35.0f);
        ensure(BuildingKind::Farm,     -c.enemy_dir * 40.0f - perp * 35.0f);
        // Wait until the barracks finishes construction, then produce.
        for (auto& b : s->buildings) {
            if (b.kind == BuildingKind::Barracks && building_alive(world, b)
                && b.build_progress >= 1.0f) {
                c.state = CommanderState::Produce;
                break;
            }
        }
    }

    void tick_produce(World& world, AICommander& c, float dt) {
        Settlement* s = find_settlement(c.faction);
        if (!s) { c.state = CommanderState::March; return; }
        Building* barracks = nullptr;
        for (auto& b : s->buildings)
            if (b.kind == BuildingKind::Barracks && building_alive(world, b)) { barracks = &b; break; }
        if (!barracks) {
            // Barracks destroyed -> fall back to redevelop.
            c.state = CommanderState::Develop;
            return;
        }
        barracks->prod_timer += dt;
        if (barracks->prod_timer >= produce_interval) {
            barracks->prod_timer = 0.0f;
            spawn_reinforcements(world, c, *barracks);
        }
    }

    // Spawn a small mixed batch at the barracks and rally them at the enemy.
    void spawn_reinforcements(World& world, AICommander& c, Building& barracks) {
        glm::vec2 rally = c.home + c.enemy_dir * (settle_distance + 1200.0f);
        int cols = std::max(1, (int)std::sqrt((float)produce_batch));
        for (int i = 0; i < produce_batch; i++) {
            int row = i / cols, col = i % cols;
            glm::vec2 pos = barracks.pos + glm::vec2(
                (col - cols * 0.5f) * 5.0f,
                (row - produce_batch / cols * 0.5f) * 5.0f)
                + c.enemy_dir * 30.0f; // muster just in front of the barracks
            Entity e = world.create_entity();
            if (e == INVALID_ENTITY) break;
            // Cycle a simple infantry/archer mix.
            UnitType t = (i % 4 == 0) ? UnitType::Archer : UnitType::Infantry;
            float hp  = (t == UnitType::Archer) ? 60.0f : 100.0f;
            float dmg = (t == UnitType::Archer) ? 12.0f : 10.0f;
            float rng = (t == UnitType::Archer) ? 100.0f : 8.0f;
            float spd = (t == UnitType::Archer) ? 4.0f : 6.0f;
            world.transforms.position[e] = pos;
            world.transforms.velocity[e] = {0, 0};
            world.transforms.rotation[e] = (c.faction == Faction::Red) ? 0.0f : 3.14159f;
            world.transforms.y_offset[e] = 0;
            world.transforms.y_velocity[e] = 0;
            world.units.faction[e] = c.faction;
            world.units.type[e] = t;
            world.units.state[e] = UnitState::Idle;
            world.units.health[e] = hp;
            world.units.max_health[e] = hp;
            world.units.attack_damage[e] = dmg;
            world.units.attack_range[e] = rng;
            world.units.speed[e] = spd;
            world.units.attack_cooldown[e] = 0;
            world.units.target[e] = INVALID_ENTITY;
            world.units.hit_timer[e] = 0;
            world.units.ragdoll_timer[e] = 0;
            world.units.is_structure[e] = false;
            world.renders.scale[e] = (t == UnitType::Archer) ? 1.8f : 2.0f;
            world.renders.color[e] = (c.faction == Faction::Red)
                ? glm::vec3(0.25f, 0.3f, 0.2f) : glm::vec3(0.5f, 0.35f, 0.15f);
            world.selection.player_owned[e] = false;
            world.selection.selected[e] = false;
            // Rally order: a non-player move command so combat AI lets them
            // advance toward the front instead of idling at the barracks.
            world.commands.move_target[e] = rally;
            world.commands.has_move_command[e] = true;
        }
    }
};



