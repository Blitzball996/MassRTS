#pragma once
#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>

// Game State Machine: Menu -> MapSelect -> Playing -> Victory/Defeat
// Paused: ESC during Playing freezes the battle and shows the pause overlay.
// (Appended last so the existing enumerator values are unchanged.)
enum class GamePhase { Menu, MapSelect, Settings, Loading, Playing, Victory, Defeat, Paused };

// Which core loop the current match runs. Skirmish = classic Red-vs-Blue
// territory battle. Survival = the endless wave / roguelite loop (ddd.txt).
enum class GameMode { Skirmish = 0, Survival = 1 };
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
    int settings_submenu = 0;  // 0=main settings page, 1=Video, 2=Graphics, 3=Audio, 4=Controls

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

    // Settings (Graphics, Audio, Controls)
    bool settings_bloom = true;
    float settings_exposure = 0.7f;  // lowered from 0.85 (still too bright)
    float settings_master_volume = 0.7f;
    float settings_sfx_volume = 0.8f;
    float settings_camera_speed = 600.0f;
    bool settings_edge_pan = true;
    
    // Video settings (PC-specific)
    int settings_resolution_w = 1920;
    int settings_resolution_h = 1080;
    bool settings_fullscreen = true;  // default fullscreen on startup
    bool settings_dirty = false;       // true when user changed settings (shows APPLY)
    bool settings_confirming = false;  // true during the 15s countdown
    float settings_confirm_timer = 0;  // countdown timer
    // Snapshot to restore if user declines/times out
    struct { int w, h; bool fs, vsync, bloom, epan; float exp, fov, rng, gam, mvol, svol, cam; } settings_snapshot;
    bool settings_vsync = true;
    
    // Graphics advanced (MinecraftConsoles-inspired)
    float settings_fov = 75.0f;  // field of view in degrees
    float settings_render_distance = 6000.0f;  // terrain draw distance
    float settings_gamma = 1.0f;  // brightness adjustment
    // Match stats
    float match_time = 0;
    int total_kills[2] = {0, 0};

    // === Mode selection / Survival run config ===
    GameMode mode = GameMode::Skirmish;
    int survival_tier = 1;       // difficulty tier chosen on the menu (1..5)
    uint32_t survival_seed = 1337;
    bool survival_setup_open = false; // tier/seed picker overlay shown over Menu
    // Last-run summary (filled when a survival run ends, for the results screen).
    int  last_run_wave = 0;
    int  last_run_tier = 1;
    int  last_run_kills = 0;
    int  last_run_points = 0;
    bool last_run_new_unlock = false;
    char last_run_unlock_text[48] = {0};
    // HUD mirror of the live WaveDirector state (filled each frame in Playing).
    int hud_wave = 0;
    int hud_phase = 0;           // 0=Prep 1=Combat 2=Draft
    float hud_prep_timer = 0;
    int hud_enemies_left = 0;
    int hud_nests_alive = 0;

    void init_capture_points(int map_idx) {
        capture_points.clear();
        float spread = (map_idx == 3) ? 600.0f : 400.0f;
        capture_points.push_back({{0, 0}, 100.0f, -1, {0,0}, 100.0f, "Central Hill"});
        capture_points.push_back({{-spread, 0}, 80.0f, 0, {100,0}, 100.0f, "West Outpost"});
        capture_points.push_back({{spread, 0}, 80.0f, 1, {0,100}, 100.0f, "East Outpost"});
        capture_points.push_back({{0, -spread*0.7f}, 80.0f, -1, {0,0}, 100.0f, "North Pass"});
        capture_points.push_back({{0, spread*0.7f}, 80.0f, -1, {0,0}, 100.0f, "South Ford"});
    }

    void snapshot_settings() {
        settings_snapshot = {settings_resolution_w, settings_resolution_h, settings_fullscreen,
                             settings_vsync, settings_bloom, settings_edge_pan, settings_exposure,
                             settings_fov, settings_render_distance, settings_gamma,
                             settings_master_volume, settings_sfx_volume, settings_camera_speed};
    }

    void restore_settings() {
        settings_resolution_w = settings_snapshot.w;
        settings_resolution_h = settings_snapshot.h;
        settings_fullscreen = settings_snapshot.fs;
        settings_vsync = settings_snapshot.vsync;
        settings_bloom = settings_snapshot.bloom;
        settings_edge_pan = settings_snapshot.epan;
        settings_exposure = settings_snapshot.exp;
        settings_fov = settings_snapshot.fov;
        settings_render_distance = settings_snapshot.rng;
        settings_gamma = settings_snapshot.gam;
        settings_master_volume = settings_snapshot.mvol;
        settings_sfx_volume = settings_snapshot.svol;
        settings_camera_speed = settings_snapshot.cam;
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
