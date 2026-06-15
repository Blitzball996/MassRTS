#pragma once
#include "../ecs/world.h"
#include "spatial_grid.h"
#include "../render/projectiles.h"
#include <cmath>

class CombatSystem {
public:
    SpatialGrid grid;
    glm::vec2 faction_center[2] = {};
    int faction_alive[2] = {0, 0};
    ProjectileSystem* proj_sys = nullptr;
    // Terrain height lookup so projectiles spawn/aim at correct elevation
    // (without this, y=0 made cannonballs spawn underground and never fly).
    float (*height_fn)(float,float) = nullptr;

    void rebuild_grid(World& world) {
        static int grid_frame = 0;
        grid_frame++;
        faction_alive[0] = faction_alive[1] = 0;
        faction_center[0] = faction_center[1] = {0,0};
        bool do_full = (grid_frame % 2 == 0);
        if (do_full) grid.clear();
        for (uint32_t i = 0; i < world.entity_count; i++) {
            if (!world.is_alive(i)) continue;
            if (do_full) grid.insert(i, world.transforms.position[i]);
            int f = (int)world.units.faction[i];
            if (f<2) { faction_center[f] += world.transforms.position[i]; faction_alive[f]++; }
        }
        for (int f=0; f<2; f++)
            if (faction_alive[f]>0) faction_center[f] /= (float)faction_alive[f];
    }

    void process_projectile_hits(World& world) {
        if (!proj_sys) return;
        for (auto& hit : proj_sys->pending_hits) {
            if (hit.radius > 5.0f) {
                world.apply_explosion(hit.position, hit.radius, hit.knockback_force, hit.damage, hit.source_faction);
            } else {
                glm::vec2 pos2d(hit.position.x, hit.position.z);
                Entity target = grid.find_nearest_enemy(pos2d, hit.radius+5.0f, hit.source_faction, world);
                if (target != INVALID_ENTITY && world.is_alive(target)) {
                    world.units.health[target] -= hit.damage;
                    world.units.hit_timer[target] = 0.3f;
                    if (world.units.health[target] <= 0) world.kill_entity(target);
                }
            }
        }
    }

    void update_batched(World& world, float dt, uint32_t start, uint32_t batch_size) {
        rebuild_grid(world);
        process_projectile_hits(world);

        uint32_t end = start + batch_size;
        if (end > world.entity_count) end = world.entity_count;

        for (uint32_t i = start; i < end; i++) {
            if (!world.is_alive(i)) continue;
            if (world.units.hit_timer[i] > 0) world.units.hit_timer[i] -= dt;
            if (world.units.attack_cooldown[i] > 0) world.units.attack_cooldown[i] -= dt;

            auto type = world.units.type[i];
            auto state = world.units.state[i];

            // *** PLAYER COMMANDS HAVE ABSOLUTE PRIORITY ***
            if (world.commands.has_move_command[i]) continue;

            // Structures: turrets auto-attack, walls do nothing
            if (world.units.is_structure[i]) {
                if (type == UnitType::Turret) turret_ai(world, i);
                continue;
            }

            // Target finding
            if (state != UnitState::Retreating &&
                state != UnitState::Swimming &&
                world.units.target[i] == INVALID_ENTITY) {
                find_and_engage(world, i);
            } else if (world.units.target[i] != INVALID_ENTITY &&
                       !world.is_alive(world.units.target[i])) {
                world.units.target[i] = INVALID_ENTITY;
                world.units.state[i] = UnitState::Idle;
            }

            // Retreat
            if (state == UnitState::Retreating) {
                do_retreat(world, i, dt);
                continue;
            }

            // Check retreat threshold
            float retreat_thresh = (type==UnitType::Shield) ? 0.1f : 0.2f;
            if (world.units.health[i] < world.units.max_health[i]*retreat_thresh && state != UnitState::Retreating) {
                world.units.state[i] = UnitState::Retreating;
                world.units.target[i] = INVALID_ENTITY;
                continue;
            }

            // Attacking
            if (state == UnitState::Attacking) {
                do_attack(world, i, dt);
            }

            // Idle AI (enemy AND uncontrolled player units)
            if (!world.commands.has_move_command[i] &&
                state == UnitState::Idle &&
                world.units.target[i] == INVALID_ENTITY) {
                ai_idle(world, i, type);
            }

            // Moving toward target
            if (state == UnitState::Moving && world.units.target[i] != INVALID_ENTITY) {
                Entity t = world.units.target[i];
                if (world.is_alive(t)) {
                    glm::vec2 diff = world.transforms.position[t] - world.transforms.position[i];
                    float dist = glm::length(diff);
                    if (dist <= world.units.attack_range[i]) {
                        world.units.state[i] = UnitState::Attacking;
                        world.transforms.velocity[i] = glm::vec2(0);
                    } else {
                        glm::vec2 dir = diff/(dist+0.001f);
                        world.transforms.velocity[i] = dir * world.units.speed[i];
                        world.transforms.rotation[i] = atan2(dir.x, dir.y);
                    }
                } else {
                    world.units.target[i] = INVALID_ENTITY;
                    world.units.state[i] = UnitState::Idle;
                }
            }
        }
    }

public:
    void turret_ai(World& world, uint32_t i) {
        if (world.units.attack_cooldown[i] > 0) return;
        Entity t = grid.find_nearest_enemy(world.transforms.position[i],
                                           world.units.attack_range[i],
                                           world.units.faction[i], world);
        if (t == INVALID_ENTITY) return;

        // Fire arrow at target
        if (proj_sys) {
            glm::vec2 from2d = world.transforms.position[i];
            glm::vec2 to2d = world.transforms.position[t];
            float fy = height_fn ? height_fn(from2d.x,from2d.y) : 0.0f;
            float ty = height_fn ? height_fn(to2d.x,to2d.y) : 0.0f;
            proj_sys->spawn_arrow(glm::vec3(from2d.x,fy+4.0f,from2d.y), glm::vec3(to2d.x,ty+2.0f,to2d.y), world.units.faction[i]);
        }
        world.units.attack_cooldown[i] = 1.5f;
    }

    void do_retreat(World& world, uint32_t i, float dt) {
        int my_f = (int)world.units.faction[i];
        glm::vec2 to_safety = faction_center[my_f] - world.transforms.position[i];
        float dist = glm::length(to_safety);
        if (dist > 10.0f) {
            glm::vec2 dir = to_safety / dist;
            world.transforms.velocity[i] = dir * world.units.speed[i] * 1.3f;
            world.transforms.rotation[i] = atan2(dir.x, dir.y);
        }
        world.units.health[i] += dt * 5.0f;
        if (world.units.health[i] >= world.units.max_health[i] * 0.5f) {
            world.units.health[i] = world.units.max_health[i] * 0.5f;
            world.units.state[i] = UnitState::Idle;
            world.units.target[i] = INVALID_ENTITY;
        }
    }

    void do_attack(World& world, uint32_t i, float dt) {
        Entity t = world.units.target[i];
        if (t == INVALID_ENTITY || !world.is_alive(t)) {
            world.units.target[i] = INVALID_ENTITY;
            world.units.state[i] = UnitState::Idle;
            return;
        }

        auto type = world.units.type[i];
        glm::vec2 diff = world.transforms.position[t] - world.transforms.position[i];
        float dist = glm::length(diff);
        world.transforms.rotation[i] = atan2(diff.x, diff.y);
        float range = world.units.attack_range[i];

        // Archer kiting - run if enemy too close
        if (type == UnitType::Archer && dist < range*0.3f) {
            glm::vec2 dir = -diff/(dist+0.001f);
            world.transforms.velocity[i] = dir * world.units.speed[i] * 0.9f;
            return;
        }
        // Cavalry charges THROUGH - never stops
        if (type == UnitType::Cavalry) {
            glm::vec2 dir = diff/(dist+0.001f);
            if (dist > range * 1.5f) {
                world.transforms.velocity[i] = dir * world.units.speed[i] * 1.3f;
            } else {
                world.transforms.velocity[i] = dir * world.units.speed[i] * 0.5f;
                if (world.units.attack_cooldown[i] <= 0)
                    perform_attack(world, i, t, dist);
            }
            return;
        }
        // Samurai charge fast then fight
        if (type == UnitType::Samurai && dist > range*1.2f && dist < range*5.0f) {
            glm::vec2 dir = diff/(dist+0.001f);
            world.transforms.velocity[i] = dir * world.units.speed[i] * 1.5f;
            return;
        }
        // Out of range - close in
        if (dist > range * 1.5f) {
            world.units.state[i] = UnitState::Moving;
            glm::vec2 dir = diff/(dist+0.001f);
            world.transforms.velocity[i] = dir * world.units.speed[i];
        } else {
            // Infantry/Shield/Bomber: STOP and fight
            world.transforms.velocity[i] = glm::vec2(0);
            if (world.units.attack_cooldown[i] <= 0)
                perform_attack(world, i, t, dist);
        }
    }

    void perform_attack(World& world, uint32_t i, Entity t, float dist) {
        auto type = world.units.type[i];
        float cooldown = 0.8f;

        if (type == UnitType::Archer) {
            cooldown = 1.2f;
            if (proj_sys) {
                glm::vec2 f2d=world.transforms.position[i], t2d=world.transforms.position[t];
                proj_sys->spawn_arrow(glm::vec3(f2d.x,0,f2d.y), glm::vec3(t2d.x,0,t2d.y), world.units.faction[i]);
            }
        } else if (type == UnitType::Artillery) {
            cooldown = 4.0f; // SLOW fire rate - makes it impactful
            if (proj_sys) {
                glm::vec2 f2d=world.transforms.position[i], t2d=world.transforms.position[t];
                float fy = height_fn ? height_fn(f2d.x,f2d.y) : 0.0f;
                float ty = height_fn ? height_fn(t2d.x,t2d.y) : 0.0f;
                proj_sys->spawn_cannonball(glm::vec3(f2d.x,fy+5.0f,f2d.y), glm::vec3(t2d.x,ty+1.0f,t2d.y), world.units.faction[i]);
            }
        } else if (type == UnitType::Samurai) {
            cooldown = 0.5f;
            world.units.health[t] -= world.units.attack_damage[i];
            world.units.hit_timer[t] = 0.3f;
            if (world.units.health[t] <= 0) { world.kill_entity(t); world.units.target[i]=INVALID_ENTITY; world.units.state[i]=UnitState::Idle; }
        } else if (type == UnitType::Bomber) {
            cooldown = 1.5f;
            // Self-destruct: massive damage in radius
            glm::vec2 pos = world.transforms.position[i];
            glm::vec3 center(pos.x, 0, pos.y);
            world.apply_explosion(center, 20.0f, 40.0f, 80.0f, world.units.faction[i]);
            world.units.health[i] = 0;
            world.kill_entity(i);
            return;
        } else {
            // Melee
            world.units.health[t] -= world.units.attack_damage[i];
            world.units.hit_timer[t] = 0.4f;
            if (world.units.health[t] <= 0) { world.kill_entity(t); world.units.target[i]=INVALID_ENTITY; world.units.state[i]=UnitState::Idle; }
        }
        world.units.attack_cooldown[i] = cooldown;
    }

    void ai_idle(World& world, uint32_t i, UnitType type) {
        int my_f = (int)world.units.faction[i];
        int enemy_f = 1 - my_f;
        if (faction_alive[enemy_f] <= 0) return;
        glm::vec2 my_pos = world.transforms.position[i];

        // Shield/Infantry: protect artillery
        if (type == UnitType::Shield || type == UnitType::Infantry) {
            Entity art = INVALID_ENTITY; float best_d = 300.0f;
            for (uint32_t j = (i>200?i-200:0); j < i+200 && j < world.entity_count; j++) {
                if (!world.is_alive(j) || world.units.faction[j] != world.units.faction[i]) continue;
                if (world.units.type[j] != UnitType::Artillery) continue;
                float d = glm::length(world.transforms.position[j] - my_pos);
                if (d < best_d) { best_d = d; art = j; }
            }
            if (art != INVALID_ENTITY && best_d > 30.0f) {
                glm::vec2 dir = glm::normalize(world.transforms.position[art] - my_pos);
                world.transforms.velocity[i] = dir * world.units.speed[i] * 0.6f;
                world.transforms.rotation[i] = atan2(dir.x, dir.y);
                world.units.state[i] = UnitState::Moving;
                return;
            }
        }

        // Cavalry/Samurai: flank
        if (type == UnitType::Cavalry || type == UnitType::Samurai) {
            glm::vec2 to_enemy = faction_center[enemy_f] - my_pos;
            float dist = glm::length(to_enemy);
            if (dist > 30.0f) {
                glm::vec2 fwd = to_enemy/dist;
                glm::vec2 side(-fwd.y, fwd.x);
                float flank_sign = (my_pos.x > faction_center[my_f].x) ? 1.0f : -1.0f;
                glm::vec2 dir = glm::normalize(fwd + side*flank_sign*0.4f);
                world.transforms.velocity[i] = dir * world.units.speed[i] * 0.7f;
                world.transforms.rotation[i] = atan2(dir.x, dir.y);
                world.units.state[i] = UnitState::Moving;
            }
            return;
        }

        // Default: march
        glm::vec2 to_enemy = faction_center[enemy_f] - my_pos;
        float dist = glm::length(to_enemy);
        if (dist > 50.0f) {
            glm::vec2 dir = to_enemy/dist;
            float spd = (type==UnitType::Artillery) ? 0.15f : 0.4f;
            world.transforms.velocity[i] = dir * world.units.speed[i] * spd;
            world.transforms.rotation[i] = atan2(dir.x, dir.y);
            world.units.state[i] = UnitState::Moving;
        }
    }

    void find_and_engage(World& world, uint32_t i) {
        float search = 300.0f;
        if (world.units.type[i] == UnitType::Artillery) search = 400.0f;
        Entity t = grid.find_nearest_enemy(world.transforms.position[i], search, world.units.faction[i], world);
        world.units.target[i] = t;
        if (t == INVALID_ENTITY) return;
        glm::vec2 diff = world.transforms.position[t] - world.transforms.position[i];
        float dist = glm::length(diff);
        if (dist <= world.units.attack_range[i]) {
            world.units.state[i] = UnitState::Attacking;
            world.transforms.velocity[i] = glm::vec2(0);
        } else {
            world.units.state[i] = UnitState::Moving;
            glm::vec2 dir = diff/(dist+0.001f);
            world.transforms.velocity[i] = dir * world.units.speed[i];
            world.transforms.rotation[i] = atan2(dir.x, dir.y);
        }
    }
};
