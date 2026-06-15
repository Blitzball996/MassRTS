#pragma once
#include "components.h"
#include <vector>
#include <cstring>
#include <cmath>

class World {
public:
    TransformComponents transforms;
    UnitComponents units;
    RenderComponents renders;
    SelectionComponents selection;
    CommandComponents commands;

    uint32_t entity_count = 0;
    std::vector<bool> alive;
    std::vector<uint32_t> free_list; // recycled dead slots (prevents unbounded entity_count growth -> GPU TDR/black screen)
    uint32_t live_count = 0;         // currently-alive units (incl. corpses still occupying a slot)
    // Hard ceiling on simultaneously-alive units. Per-frame GPU cost (compute
    // dispatch + readback) scales with this, so capping it keeps frame time
    // safely under the driver's ~2s TDR timeout no matter how long you play.
    static constexpr uint32_t MAX_LIVE_UNITS = 50000; // bounds per-frame GPU cost to stay well under driver TDR

    // Economy
    int money[2] = {500, 500};        // starting money
    int income_rate[2] = {15, 15};    // per second
    float income_timer[2] = {0, 0};
    int score[2] = {0, 0};
    int nuke_cost = 500;
    bool nuke_ready[2] = {false, false};

    // Bonus for player-controlled kills
    int player_kill_bonus = 5; // extra money for micro-managed kills

    // Radiation zones
    struct RadZone { glm::vec2 center; float radius; float life; Faction owner; };
    std::vector<RadZone> rad_zones;

    World() {
        alive.resize(MAX_ENTITIES, false);
        memset(&transforms, 0, sizeof(transforms));
        memset(&units, 0, sizeof(units));
        memset(&renders, 0, sizeof(renders));
        memset(&selection, 0, sizeof(selection));
        memset(&commands, 0, sizeof(commands));
    }

    Entity create_entity() {
        // Reuse a freed slot first so entity_count stays bounded by the number
        // of *simultaneously* live units, not the lifetime total. Without this
        // the GPU dispatch/readback loop grows every frame until it trips the
        // driver watchdog (TDR) -> permanent black screen after a long game.
        if (live_count >= MAX_LIVE_UNITS) return INVALID_ENTITY; // bounded GPU load
        if (!free_list.empty()) {
            Entity e = free_list.back();
            free_list.pop_back();
            alive[e] = true;
            live_count++;
            return e;
        }
        if (entity_count >= MAX_ENTITIES) return INVALID_ENTITY;
        Entity e = entity_count++;
        alive[e] = true;
        live_count++;
        return e;
    }

    void kill_entity(Entity e) {
        if (e >= entity_count || !alive[e]) return;
        if (units.state[e] == UnitState::Dead) return;

        int killer_f = 1 - (int)units.faction[e];
        if (killer_f >= 0 && killer_f < 2) {
            score[killer_f]++;
            // Money reward based on unit value
            int reward = get_kill_reward(units.type[e]);
            money[killer_f] += reward;
            if (score[killer_f] >= nuke_cost) nuke_ready[killer_f] = true;
        }
        units.state[e] = UnitState::Dead;
        units.hit_timer[e] = 6.0f; // corpse linger (kept short so slots free up fast)
        transforms.velocity[e] = glm::vec2(0);
    }

    int get_kill_reward(UnitType type) {
        switch (type) {
            case UnitType::Militia: return 5;
            case UnitType::Infantry: return 12;
            case UnitType::Archer: return 15;
            case UnitType::Shield: return 20;
            case UnitType::Cavalry: return 30;
            case UnitType::Bomber: return 25;
            case UnitType::Artillery: return 50;
            case UnitType::Samurai: return 35;
            case UnitType::Wall: return 5;
            case UnitType::Turret: return 40;
            default: return 10;
        }
    }

    // Cavalry dismount: becomes militia on ragdoll
    void dismount_cavalry(Entity e) {
        if (units.type[e] != UnitType::Cavalry) return;
        units.type[e] = UnitType::Militia;
        units.attack_damage[e] = 6.0f;
        units.attack_range[e] = 6.0f;
        units.speed[e] = 5.0f;
        units.max_health[e] = 60.0f;
        if (units.health[e] > 60.0f) units.health[e] = 60.0f;
        renders.scale[e] = 1.8f;
    }

    // Apply explosion: 3D ragdoll
    void apply_explosion(glm::vec3 center, float radius, float force, float damage, Faction source) {
        glm::vec2 c2d(center.x, center.z);
        for (uint32_t i = 0; i < entity_count; i++) {
            if (!alive[i]) continue;
            if (units.state[i] == UnitState::Dead) continue;
            if (units.is_structure[i]) {
                // Structures just take damage
                float dist = glm::length(transforms.position[i] - c2d);
                if (dist < radius) {
                    float falloff = 1.0f - (dist / radius);
                    units.health[i] -= damage * falloff;
                    if (units.health[i] <= 0) kill_entity(i);
                }
                continue;
            }

            glm::vec2 diff = transforms.position[i] - c2d;
            float dist = glm::length(diff);
            if (dist > radius) continue;

            float falloff = 1.0f - (dist / radius);
            falloff = falloff * falloff;

            units.health[i] -= damage * falloff;

            // 3D knockback
            float kb = force * falloff;
            glm::vec2 dir = dist > 0.5f ? diff / dist : glm::vec2(1, 0);
            transforms.velocity[i] = dir * kb;
            transforms.y_velocity[i] = kb * 1.0f + 40.0f * falloff; // high vertical launch!
            units.state[i] = UnitState::Ragdoll;
            units.ragdoll_timer[i] = 2.0f + falloff;
            units.target[i] = INVALID_ENTITY;

            // Cavalry dismount on explosion
            if (units.type[i] == UnitType::Cavalry && units.health[i] > 0) {
                dismount_cavalry(i);
            }

            if (units.health[i] <= 0) kill_entity(i);
        }
    }

    // Buy units for player
    Entity buy_unit(int shop_index, glm::vec2 pos, Faction faction) {
        if (shop_index < 0 || shop_index >= SHOP_COUNT) return INVALID_ENTITY;
        const auto& entry = UNIT_SHOP[shop_index];
        int f = (int)faction;
        if (money[f] < entry.cost) return INVALID_ENTITY;
        money[f] -= entry.cost;

        Entity e = create_entity();
        if (e == INVALID_ENTITY) { money[f] += entry.cost; return INVALID_ENTITY; }

        transforms.position[e] = pos;
        transforms.velocity[e] = {0, 0};
        transforms.rotation[e] = (faction == Faction::Red) ? 0.0f : 3.14159f;
        transforms.y_offset[e] = 0;
        transforms.y_velocity[e] = 0;
        units.faction[e] = faction;
        units.type[e] = entry.type;
        units.state[e] = UnitState::Idle;
        units.health[e] = entry.hp;
        units.max_health[e] = entry.hp;
        units.attack_damage[e] = entry.dmg;
        units.attack_range[e] = entry.range;
        units.speed[e] = entry.speed;
        units.attack_cooldown[e] = 0;
        units.target[e] = INVALID_ENTITY;
        units.hit_timer[e] = 0;
        units.ragdoll_timer[e] = 0;
        units.is_structure[e] = (entry.type == UnitType::Wall || entry.type == UnitType::Turret);
        renders.scale[e] = entry.scale;
        renders.color[e] = (faction == Faction::Red) ? glm::vec3(0.25f,0.3f,0.2f) : glm::vec3(0.5f,0.35f,0.15f);
        selection.player_owned[e] = (faction == Faction::Red);
        selection.selected[e] = false;
        commands.has_move_command[e] = false;
        return e;
    }

    // Buy batch of units
    int buy_batch(int shop_index, int count, glm::vec2 center, Faction faction) {
        int bought = 0;
        int cols = std::max(1, (int)sqrt((float)count));
        for (int i = 0; i < count; i++) {
            int row = i / cols, col = i % cols;
            glm::vec2 pos = center + glm::vec2(
                (col - cols*0.5f) * 6.5f,
                (row - count/cols*0.5f) * 6.5f);
            Entity e = buy_unit(shop_index, pos, faction);
            if (e == INVALID_ENTITY) break;
            bought++;
        }
        return bought;
    }

    void tick_economy(float dt) {
        for (int f = 0; f < 2; f++) {
            income_timer[f] += dt;
            if (income_timer[f] >= 1.0f) {
                income_timer[f] -= 1.0f;
                money[f] += income_rate[f];
            }
        }
    }

    void tick_corpses(float dt) {
        for (uint32_t i = 0; i < entity_count; i++) {
            if (!alive[i]) continue;

            // Ragdoll 3D physics
            if (units.state[i] == UnitState::Ragdoll) {
                transforms.y_velocity[i] -= 150.0f * dt; // strong gravity
                transforms.y_offset[i] += transforms.y_velocity[i] * dt;
                transforms.velocity[i] *= (1.0f - 2.5f * dt);
                transforms.position[i] += transforms.velocity[i] * dt;
                transforms.rotation[i] += dt * 8.0f; // spin

                if (transforms.y_offset[i] <= 0) {
                    transforms.y_offset[i] = 0;
                    // Bounce if still has velocity
                    if (transforms.y_velocity[i] < -20.0f) {
                        transforms.y_velocity[i] *= -0.3f; // bounce
                    } else {
                        transforms.y_velocity[i] = 0;
                        transforms.velocity[i] = glm::vec2(0);
                        units.ragdoll_timer[i] -= dt;
                        if (units.ragdoll_timer[i] <= 0) {
                            if (units.health[i] <= 0) {
                                units.state[i] = UnitState::Dead;
                                units.hit_timer[i] = 6.0f; // ragdoll corpse linger
                            } else {
                                units.state[i] = UnitState::Idle;
                            }
                        }
                    }
                }
                continue;
            }

            // Corpse fade
            if (units.state[i] == UnitState::Dead) {
                units.hit_timer[i] -= dt;
                if (units.hit_timer[i] <= 0) {
                    alive[i] = false;
                    free_list.push_back(i); // recycle this slot
                    if (live_count > 0) live_count--;
                }
            }
        }

        // Radiation zones
        for (int z = (int)rad_zones.size()-1; z >= 0; z--) {
            rad_zones[z].life -= dt;
            if (rad_zones[z].life <= 0) { rad_zones[z] = rad_zones.back(); rad_zones.pop_back(); continue; }
            for (uint32_t i = 0; i < entity_count; i++) {
                if (!is_alive(i)) continue;
                float dist = glm::length(transforms.position[i] - rad_zones[z].center);
                if (dist < rad_zones[z].radius) {
                    units.health[i] -= 20.0f * dt;
                    if (units.health[i] <= 0) kill_entity(i);
                }
            }
        }
        compact();
    }

    // Trim trailing dead/free slots so entity_count shrinks after battles.
    // Every per-frame GPU dispatch & readback loops over entity_count, so if it
    // stays at the lifetime peak (e.g. 70000) the GPU keeps paying for empty
    // slots forever -> rising frame time -> eventual driver TDR (black screen).
    void compact() {
        while (entity_count > 0 && !alive[entity_count - 1]) {
            entity_count--;
        }
        // Drop any free_list entries that are now beyond entity_count.
        if (!free_list.empty()) {
            size_t w = 0;
            for (size_t r = 0; r < free_list.size(); r++) {
                if (free_list[r] < entity_count) free_list[w++] = free_list[r];
            }
            free_list.resize(w);
        }
    }

    void add_rad_zone(glm::vec2 c, float r, float d, Faction o) { rad_zones.push_back({c,r,d,o}); }

    bool is_alive(Entity e) const {
        return e < entity_count && alive[e] &&
               units.state[e] != UnitState::Dead &&
               units.state[e] != UnitState::Ragdoll;
    }
    bool is_visible(Entity e) const { return e < entity_count && alive[e]; }
};
