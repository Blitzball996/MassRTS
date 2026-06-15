#pragma once
#include "../ecs/world.h"
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstring>

// High-performance spatial grid using flat arrays to avoid per-cell heap allocation
class SpatialGrid {
public:
    static constexpr float CELL_SIZE = 15.0f;
    static constexpr int GRID_WIDTH = 200;
    static constexpr int GRID_HEIGHT = 200;
    static constexpr int TOTAL_CELLS = GRID_WIDTH * GRID_HEIGHT;
    static constexpr int MAX_PER_CELL = 32; // max entities tracked per cell

    struct Cell {
        Entity entities[MAX_PER_CELL];
        uint8_t count = 0;
    };

    Cell cells[TOTAL_CELLS];

    void clear() {
        // Only zero the counts — O(N_cells) but no heap ops
        for (int i = 0; i < TOTAL_CELLS; i++)
            cells[i].count = 0;
    }

    void insert(Entity e, glm::vec2 pos) {
        int cx = cell_x(pos.x), cy = cell_y(pos.y);
        if (cx >= 0 && cx < GRID_WIDTH && cy >= 0 && cy < GRID_HEIGHT) {
            Cell& c = cells[cy * GRID_WIDTH + cx];
            if (c.count < MAX_PER_CELL)
                c.entities[c.count++] = e;
        }
    }

    Entity find_nearest_enemy(glm::vec2 pos, float radius, Faction my_faction, const World& world) const {
        int min_cx = std::max(0, cell_x(pos.x - radius));
        int max_cx = std::min(GRID_WIDTH - 1, cell_x(pos.x + radius));
        int min_cy = std::max(0, cell_y(pos.y - radius));
        int max_cy = std::min(GRID_HEIGHT - 1, cell_y(pos.y + radius));

        // Limit search radius to avoid iterating too many cells
        if (max_cx - min_cx > 40) {
            int mid = cell_x(pos.x);
            min_cx = std::max(0, mid - 20);
            max_cx = std::min(GRID_WIDTH - 1, mid + 20);
        }
        if (max_cy - min_cy > 40) {
            int mid = cell_y(pos.y);
            min_cy = std::max(0, mid - 20);
            max_cy = std::min(GRID_HEIGHT - 1, mid + 20);
        }

        float best_dist = radius * radius;
        Entity best = INVALID_ENTITY;

        for (int cy = min_cy; cy <= max_cy; cy++) {
            for (int cx = min_cx; cx <= max_cx; cx++) {
                const Cell& cell = cells[cy * GRID_WIDTH + cx];
                for (int k = 0; k < cell.count; k++) {
                    Entity e = cell.entities[k];
                    if (world.units.faction[e] == my_faction) continue;
                    if (!world.is_alive(e)) continue;
                    float dx = world.transforms.position[e].x - pos.x;
                    float dy = world.transforms.position[e].y - pos.y;
                    float d2 = dx * dx + dy * dy;
                    if (d2 < best_dist) { best_dist = d2; best = e; }
                }
            }
        }
        return best;
    }

    void query_box(glm::vec2 min_pos, glm::vec2 max_pos, std::vector<Entity>& results) const {
        results.clear();
        int min_cx = std::max(0, cell_x(min_pos.x));
        int max_cx = std::min(GRID_WIDTH - 1, cell_x(max_pos.x));
        int min_cy = std::max(0, cell_y(min_pos.y));
        int max_cy = std::min(GRID_HEIGHT - 1, cell_y(max_pos.y));

        for (int cy = min_cy; cy <= max_cy; cy++) {
            for (int cx = min_cx; cx <= max_cx; cx++) {
                const Cell& cell = cells[cy * GRID_WIDTH + cx];
                for (int k = 0; k < cell.count; k++)
                    results.push_back(cell.entities[k]);
            }
        }
    }

    // Count allies vs enemies within `radius` of `pos`. Used by artillery to
    // avoid dropping shells on its own troops. Writes results to out params.
    void count_factions_near(glm::vec2 pos, float radius, Faction my_faction,
                             const World& world, int& allies, int& enemies) const {
        allies = 0; enemies = 0;
        int min_cx = std::max(0, cell_x(pos.x - radius));
        int max_cx = std::min(GRID_WIDTH - 1, cell_x(pos.x + radius));
        int min_cy = std::max(0, cell_y(pos.y - radius));
        int max_cy = std::min(GRID_HEIGHT - 1, cell_y(pos.y + radius));
        float r2 = radius * radius;
        for (int cy = min_cy; cy <= max_cy; cy++) {
            for (int cx = min_cx; cx <= max_cx; cx++) {
                const Cell& cell = cells[cy * GRID_WIDTH + cx];
                for (int k = 0; k < cell.count; k++) {
                    Entity e = cell.entities[k];
                    if (!world.is_alive(e)) continue;
                    float dx = world.transforms.position[e].x - pos.x;
                    float dy = world.transforms.position[e].y - pos.y;
                    if (dx*dx + dy*dy > r2) continue;
                    if (world.units.faction[e] == my_faction) allies++; else enemies++;
                }
            }
        }
    }

private:
    static int cell_x(float x) { return (int)(x / CELL_SIZE) + GRID_WIDTH / 2; }
    static int cell_y(float y) { return (int)(y / CELL_SIZE) + GRID_HEIGHT / 2; }
};
