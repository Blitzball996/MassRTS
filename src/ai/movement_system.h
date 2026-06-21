#pragma once
#include "../ecs/world.h"
#include "../render/terrain.h"
#include "spatial_grid.h"
#include <thread>
#include <vector>
#include <cmath>

// ============================================================================
// MovementSystem — integration + RVO (Reciprocal Velocity Obstacle) avoidance.
//
// The old system only did reactive positional separation: units pushed apart
// AFTER they overlapped, which causes jitter and clumping in dense armies. RVO
// is PROACTIVE: each unit predicts time-to-collision with neighbors from
// relative velocity and steers its velocity to avoid the collision before it
// happens, so masses of units flow around each other smoothly.
//
// To stay race-free at 100k units the update runs as distinct parallel passes:
//   A. preferred velocity from move command          (writes vel/rot/state[i])
//   B. RVO adjust (read-only on pos/vel) -> rvo_vel[i] (disjoint writes)
//   C. terrain modifier + integrate position          (disjoint writes)
// A residual separation pass still resolves hard overlaps RVO can't.
// ============================================================================
class MovementSystem {
public:
    static constexpr int NUM_THREADS = 4;
    Terrain* terrain_ptr = nullptr; // set externally

    // RVO tuning
    float unit_radius     = 1.8f;   // half of the 3.5 overlap distance
    float sense_radius    = 14.0f;  // neighbor query range for avoidance
    float time_horizon    = 2.0f;   // how far ahead to predict collisions (s)
    float max_avoid_accel = 30.0f;  // cap on steering acceleration

    std::vector<glm::vec2> rvo_vel; // per-entity adjusted velocity buffer

    // PLACEHOLDER_UPDATE

    void update(World& world, float dt, const SpatialGrid& grid) {
        if (rvo_vel.size() != world.entity_count) rvo_vel.resize(world.entity_count);

        uint32_t chunk = world.entity_count / NUM_THREADS;
        std::thread threads[NUM_THREADS - 1];

        // ====================================================================
        // PASS A: Preferred velocity (move command -> velocity/rotation/state)
        // ====================================================================
        auto compute_preferred = [&](uint32_t start, uint32_t end) {
            for (uint32_t i = start; i < end; i++) {
                if (!world.is_alive(i)) continue;
                if (world.units.state[i] == UnitState::Dead) continue;
                if (world.units.state[i] == UnitState::Ragdoll) continue;
                if (world.units.is_structure[i]) continue;

                if (world.commands.has_move_command[i]) {
                    glm::vec2 diff = world.commands.move_target[i] - world.transforms.position[i];
                    float dist = glm::length(diff);
                    if (dist < 5.0f) {
                        world.commands.has_move_command[i] = false;
                        world.transforms.velocity[i] = glm::vec2(0);
                        world.units.state[i] = UnitState::Idle;
                    } else {
                        glm::vec2 dir = diff / dist;
                        world.transforms.velocity[i] = dir * world.units.speed[i];
                        world.transforms.rotation[i] = atan2(dir.x, dir.y);
                        world.units.state[i] = UnitState::Moving;
                    }
                }
            }
        };

        for (int t = 0; t < NUM_THREADS - 1; t++) {
            uint32_t s = t * chunk, e = s + chunk;
            threads[t] = std::thread(compute_preferred, s, e);
        }
        compute_preferred((NUM_THREADS - 1) * chunk, world.entity_count);
        for (int t = 0; t < NUM_THREADS - 1; t++) threads[t].join();

        // ====================================================================
        // PASS B: RVO collision avoidance (read pos/vel, write rvo_vel)
        // ====================================================================
        auto compute_rvo = [&](uint32_t start, uint32_t end) {
            for (uint32_t i = start; i < end; i++) {
                if (!world.is_alive(i) || world.units.state[i] == UnitState::Dead ||
                    world.units.state[i] == UnitState::Ragdoll || world.units.is_structure[i]) {
                    rvo_vel[i] = glm::vec2(0);
                    continue;
                }

                glm::vec2 pref_vel = world.transforms.velocity[i];
                glm::vec2 my_pos = world.transforms.position[i];

                // Query neighbors within sense_radius
                std::vector<uint32_t> nbs = grid.query_radius(my_pos.x, my_pos.y, sense_radius);
                glm::vec2 avoidance_delta(0);

                for (uint32_t j : nbs) {
                    if (j == i || !world.is_alive(j)) continue;
                    if (world.units.state[j] == UnitState::Dead) continue;
                    
                    glm::vec2 rel_pos = world.transforms.position[j] - my_pos;
                    float dist = glm::length(rel_pos);
                    if (dist < 0.01f || dist > sense_radius) continue;

                    glm::vec2 rel_vel = world.transforms.velocity[j] - pref_vel;
                    float vlen = glm::length(rel_vel);

                    // Time-to-collision estimate: (dist - combined_radius) / speed_toward
                    float combined_r = unit_radius * 2.0f;
                    float approach_speed = glm::dot(rel_pos, rel_vel) / dist; // closing speed
                    if (approach_speed <= 0) continue; // moving apart
                    float tc = (dist - combined_r) / approach_speed;
                    if (tc > time_horizon || tc < 0) continue;

                    // Steer perpendicular to relative velocity to "slip past" the obstacle
                    glm::vec2 perp = (vlen > 0.01f) ? glm::vec2(-rel_vel.y, rel_vel.x) / vlen
                                                    : glm::vec2(-rel_pos.y, rel_pos.x) / dist;
                    // Weight by urgency (closer tc = bigger steer)
                    float urgency = 1.0f - (tc / time_horizon);
                    avoidance_delta += perp * urgency * 12.0f;
                }

                // Clamp the steering to max acceleration
                float mag = glm::length(avoidance_delta);
                if (mag > max_avoid_accel) avoidance_delta = avoidance_delta / mag * max_avoid_accel;

                rvo_vel[i] = pref_vel + avoidance_delta * dt;
                // Cap to reasonable speed (avoid runaway)
                float speed = glm::length(rvo_vel[i]);
                float max_speed = world.units.speed[i] * 1.5f;
                if (speed > max_speed) rvo_vel[i] = rvo_vel[i] / speed * max_speed;
            }
        };

        for (int t = 0; t < NUM_THREADS - 1; t++) {
            uint32_t s = t * chunk, e = s + chunk;
            threads[t] = std::thread(compute_rvo, s, e);
        }
        compute_rvo((NUM_THREADS - 1) * chunk, world.entity_count);
        for (int t = 0; t < NUM_THREADS - 1; t++) threads[t].join();

        // ====================================================================
        // PASS C: Integrate position with terrain modifier
        // ====================================================================
        auto integrate = [&](uint32_t start, uint32_t end) {
            for (uint32_t i = start; i < end; i++) {
                if (!world.is_alive(i) || world.units.state[i] == UnitState::Dead ||
                    world.units.state[i] == UnitState::Ragdoll || world.units.is_structure[i])
                    continue;

                glm::vec2 vel = rvo_vel[i];
                // Terrain slope modifier: slower uphill
                if (terrain_ptr) {
                    glm::vec2 p = world.transforms.position[i];
                    float h0 = terrain_ptr->get_height_at(p.x, p.y);
                    float h1 = terrain_ptr->get_height_at(p.x + vel.x * dt, p.y + vel.y * dt);
                    float slope = (h1 - h0) / (glm::length(vel) * dt + 1e-5f);
                    if (slope > 0.05f) vel *= glm::clamp(1.0f - slope * 2.0f, 0.3f, 1.0f);
                }
                world.transforms.position[i] += vel * dt;
                world.transforms.velocity[i] = vel;
            }
        };

        for (int t = 0; t < NUM_THREADS - 1; t++) {
            uint32_t s = t * chunk, e = s + chunk;
            threads[t] = std::thread(integrate, s, e);
        }
        integrate((NUM_THREADS - 1) * chunk, world.entity_count);
        for (int t = 0; t < NUM_THREADS - 1; t++) threads[t].join();

        // ====================================================================
        // Residual separation (handles hard overlaps RVO can't predict)
        // ====================================================================
        auto separate = [&](uint32_t start, uint32_t end) {
            for (uint32_t i = start; i < end; i++) {
                if (!world.is_alive(i) || world.units.state[i] == UnitState::Dead ||
                    world.units.state[i] == UnitState::Ragdoll || world.units.is_structure[i])
                    continue;

                glm::vec2 my_pos = world.transforms.position[i];
                std::vector<uint32_t> nbs = grid.query_radius(my_pos.x, my_pos.y, 5.0f);
                glm::vec2 push(0);

                for (uint32_t j : nbs) {
                    if (j == i || !world.is_alive(j)) continue;
                    if (world.units.state[j] == UnitState::Dead) continue;
                    glm::vec2 diff = my_pos - world.transforms.position[j];
                    float dist = glm::length(diff);
                    float overlap_dist = 3.5f; // existing separation threshold
                    if (dist < overlap_dist && dist > 0.01f) {
                        push += (diff / dist) * (overlap_dist - dist) * 0.5f;
                    }
                }
                world.transforms.position[i] += push;
            }
        };

        for (int t = 0; t < NUM_THREADS - 1; t++) {
            uint32_t s = t * chunk, e = s + chunk;
            threads[t] = std::thread(separate, s, e);
        }
        separate((NUM_THREADS - 1) * chunk, world.entity_count);
        for (int t = 0; t < NUM_THREADS - 1; t++) threads[t].join();
    }
};
