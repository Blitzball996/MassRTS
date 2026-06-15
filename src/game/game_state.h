#pragma once
#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>

// Game State Machine: Menu -> MapSelect -> Playing -> Victory/Defeat
enum class GamePhase { Menu, MapSelect, Playing, Victory, Defeat };

// Map Presets
struct MapPreset {
    const char* name;
    const char* description;
    uint32_t seed;
    float height_scale;
    float flatten_radius;
    float mountain_edge;
    bool has_river;
    int num_trenches;
    float world_size;
};

static const MapPreset MAP_PRESETS[] = {
    {"Plains", "Open field, cavalry paradise",
     1001, 0.3f, 500.0f, 1200.0f, false, 0, 3000.0f},
    {"River Valley", "Central river divides armies",
     2002, 0.5f, 300.0f, 900.0f, true, 2, 3000.0f},
    {"Mountain Fort", "Mountain passes, defense advantage",
     3003, 1.2f, 200.0f, 500.0f, false, 4, 3000.0f},
    {"Desert Storm", "Wide open, nowhere to hide",
     4004, 0.15f, 800.0f, 1400.0f, false, 0, 4000.0f},
    {"Forest Ambush", "Dense forest, archer territory",
     5005, 0.6f, 250.0f, 800.0f, true, 1, 2500.0f},
    {"Trench War", "WWI trenches, bombers rule",
     6006, 0.4f, 150.0f, 1000.0f, false, 6, 3000.0f},
};
static constexpr int MAP_COUNT = sizeof(MAP_PRESETS) / sizeof(MAP_PRESETS[0]);

// Territory / Capture Points
struct CapturePoint {
    glm::vec2 position;
    float radius = 80.0f;
    int owner = -1;          // -1=neutral, 0=red, 1=blue
    float capture_progress[2] = {0, 0};
    float capture_threshold = 100.0f;
    const char* name;
};

// Game State
class GameState {
public:
    GamePhase phase = GamePhase::Menu;
    bool needs_map_reload = false;
    int selected_map = 0;
    int menu_hover = -1;

    // Territory control
    std::vector<CapturePoint> capture_points;
    int red_territory = 0;
    int blue_territory = 0;
    float victory_timer = 0;
    float victory_threshold = 30.0f;
    int winning_faction = -1;

    // Rally point (player spawn location)
    glm::vec2 rally_point = {-350, 0};
    bool rally_visible = true;
    float rally_pulse = 0;

    // Shop UI
    int shop_hover = -1;
    bool shop_panel_open = true;

    // Match stats
    float match_time = 0;
    int total_kills[2] = {0, 0};

    void init_capture_points(int map_idx) {
        capture_points.clear();
        float spread = (map_idx == 3) ? 600.0f : 400.0f;
        capture_points.push_back({{0, 0}, 100.0f, -1, {0,0}, 100.0f, "Central Hill"});
        capture_points.push_back({{-spread, 0}, 80.0f, 0, {100,0}, 100.0f, "West Outpost"});
        capture_points.push_back({{spread, 0}, 80.0f, 1, {0,100}, 100.0f, "East Outpost"});
        capture_points.push_back({{0, -spread*0.7f}, 80.0f, -1, {0,0}, 100.0f, "North Pass"});
        capture_points.push_back({{0, spread*0.7f}, 80.0f, -1, {0,0}, 100.0f, "South Ford"});
    }

    void update_territory(const glm::vec2* positions, const uint8_t* factions,
                          const uint8_t* alive_flags, uint32_t count, float dt) {
        red_territory = 0;
        blue_territory = 0;

        for (auto& cp : capture_points) {
            int red_near = 0, blue_near = 0;
            for (uint32_t i = 0; i < count; i++) {
                if (alive_flags[i] == 0) continue;
                float dist = glm::length(positions[i] - cp.position);
                if (dist < cp.radius) {
                    if (factions[i] == 0) red_near++;
                    else blue_near++;
                }
            }

            if (red_near > 0 && blue_near > 0) continue; // contested

            float cap_speed = dt * 10.0f;
            if (red_near > 0) {
                cp.capture_progress[0] += cap_speed * (float)std::min(red_near, 20);
                cp.capture_progress[1] = std::max(0.0f, cp.capture_progress[1] - cap_speed * 5.0f);
                if (cp.capture_progress[0] >= cp.capture_threshold) {
                    cp.owner = 0;
                    cp.capture_progress[0] = cp.capture_threshold;
                }
            } else if (blue_near > 0) {
                cp.capture_progress[1] += cap_speed * (float)std::min(blue_near, 20);
                cp.capture_progress[0] = std::max(0.0f, cp.capture_progress[0] - cap_speed * 5.0f);
                if (cp.capture_progress[1] >= cp.capture_threshold) {
                    cp.owner = 1;
                    cp.capture_progress[1] = cp.capture_threshold;
                }
            }

            if (cp.owner == 0) red_territory++;
            else if (cp.owner == 1) blue_territory++;
        }

        // Victory check: hold 3+ points for 30 seconds
        int majority = (int)capture_points.size() / 2 + 1;
        if (red_territory >= majority) {
            if (winning_faction != 0) { winning_faction = 0; victory_timer = 0; }
            victory_timer += dt;
        } else if (blue_territory >= majority) {
            if (winning_faction != 1) { winning_faction = 1; victory_timer = 0; }
            victory_timer += dt;
        } else {
            winning_faction = -1;
            victory_timer = 0;
        }
    }

    bool check_victory() const { return victory_timer >= victory_threshold; }
    GamePhase get_winner() const { return (winning_faction == 0) ? GamePhase::Victory : GamePhase::Defeat; }

    void set_rally_point(glm::vec2 pos) { rally_point = pos; rally_pulse = 0; }

    void update(float dt) {
        match_time += dt;
        rally_pulse += dt * 3.0f;
        if (rally_pulse > 6.283f) rally_pulse -= 6.283f;
    }
};
