#pragma once
#include "../ecs/world.h"
#include "../render/terrain.h"
#include "spatial_grid.h"
#include <thread>

class MovementSystem {
public:
    static constexpr int NUM_THREADS = 4;
    Terrain* terrain_ptr = nullptr; // set externally

    void update(World& world, float dt, const SpatialGrid& grid) {
        uint32_t chunk = world.entity_count / NUM_THREADS;
        std::thread threads[NUM_THREADS - 1];

        auto integrate_range = [&](uint32_t start, uint32_t end) {
            for (uint32_t i = start; i < end; i++) {
                if (!world.is_alive(i)) continue;
                if (world.units.state[i] == UnitState::Dead) continue;
                if (world.units.state[i] == UnitState::Ragdoll) continue;
                if (world.units.is_structure[i]) continue; // walls/turrets don't move

                // Player move commands
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

                // Apply terrain speed modifier (slope + biome)
                glm::vec2 vel = world.transforms.velocity[i];
                float vlen = glm::length(vel);
                if (vlen > 0.1f && terrain_ptr) {
                    glm::vec2 dir = vel / vlen;
                    float mult = terrain_ptr->get_speed_mult(
                        world.transforms.position[i].x,
                        world.transforms.position[i].y,
                        dir);
                    vel *= mult;

                    // Swimming detection
                    if (terrain_ptr->is_water(world.transforms.position[i].x, world.transforms.position[i].y)) {
                        if (world.units.state[i] == UnitState::Moving)
                            world.units.state[i] = UnitState::Swimming;
                    } else {
                        if (world.units.state[i] == UnitState::Swimming)
                            world.units.state[i] = UnitState::Moving;
                    }
                }

                world.transforms.position[i] += vel * dt;
                float bound = 1450.0f;
                world.transforms.position[i].x = glm::clamp(world.transforms.position[i].x, -bound, bound);
                world.transforms.position[i].y = glm::clamp(world.transforms.position[i].y, -bound, bound);
            }
        };

        for (int t = 0; t < NUM_THREADS - 1; t++) {
            uint32_t s = t * chunk, e = s + chunk;
            threads[t] = std::thread(integrate_range, s, e);
        }
        integrate_range((NUM_THREADS-1)*chunk, world.entity_count);
        for (int t = 0; t < NUM_THREADS - 1; t++) threads[t].join();

        // Separation (staggered)
        static int sep_frame = 0;
        sep_frame++;
        if (sep_frame % 5 != 0) return;
        uint32_t step = 8;
        for (uint32_t i = (sep_frame/5) % step; i < world.entity_count; i += step) {
            if (!world.is_alive(i)) continue;
            if (world.units.state[i] == UnitState::Attacking) continue;
            if (world.units.is_structure[i]) continue;

            glm::vec2 my_pos = world.transforms.position[i];
            glm::vec2 push = {0, 0};
            int neighbors = 0;
            int cx = (int)(my_pos.x/15.0f)+100, cy = (int)(my_pos.y/15.0f)+100;
            for (int dy=-1; dy<=1; dy++) for (int dx=-1; dx<=1; dx++) {
                int gx=cx+dx, gy=cy+dy;
                if (gx<0||gx>=200||gy<0||gy>=200) continue;
                const auto& cell = grid.cells[gy*200+gx];
                for (int k=0; k<cell.count; k++) {
                    Entity e = cell.entities[k];
                    if (e==i) continue;
                    glm::vec2 diff = my_pos - world.transforms.position[e];
                    float d = glm::length(diff);
                    if (d < 3.5f && d > 0.01f) { push += (diff/d)*(3.5f-d); neighbors++; }
                }
            }
            if (neighbors > 0) {
                push /= (float)neighbors;
                world.transforms.position[i] += push * dt * 6.0f;
            }
        }
    }
};
