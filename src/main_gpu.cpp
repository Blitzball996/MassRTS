#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include <random>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>
#include <thread>
#include <atomic>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

#include "ecs/world.h"
#include "game/game_config.h"
#include "ecs/components.h"
#include "ai/combat_system.h"
#include "core/asset_manifest.h"
#include "ai/movement_system.h"
#include "render/renderer.h"
#include "input/camera.h"
#include "ui/hud.h"
#include "game/game_state.h"
#include "ui/menu.h"
#define MINIAUDIO_IMPLEMENTATION
#include "audio/audio_system.h"

Camera g_camera;
bool g_selecting = false;
glm::vec2 g_select_start = {0, 0};
int g_screen_w = 1600, g_screen_h = 900;
World* g_world = nullptr;
Renderer* g_renderer = nullptr;

// Nuke targeting mode
bool g_nuke_targeting = false;
// Game state machine
GameState g_game_state;
MenuRenderer g_menu;
bool g_mouse_clicked_this_frame = false;

// Shop state
int g_shop_selection = -1; // -1 = no selection
int g_buy_count = 1000; // buy in batches of 1000

// --- Terrain sculpt state ---
bool g_sculpt_mode = false;        // toggle with B
bool g_victory_enabled = false;    // OFF: endless battle, no win/lose screen
int  g_sculpt_brush = 1;           // 0=Raise 1=Dig 2=Smooth 3=Flatten
float g_sculpt_radius = 60.0f;     // world units
bool g_mouse_held = false;         // left button currently down
const int SCULPT_COST_PER_VERT = 1; // money charged per modified vertex-tick

void fatal_error(const char* msg) {
    std::cerr << msg << std::endl;
#ifdef _WIN32
    MessageBoxA(NULL, msg, "MassRTS Error", MB_OK | MB_ICONERROR);
#endif
}

void scroll_callback(GLFWwindow* w, double x, double y) { g_camera.on_scroll(y); }

static float s_terrain_height(float x, float z) {
    return g_renderer ? g_renderer->terrain.get_height_at(x, z) : 0.0f;
}

void mouse_button_callback(GLFWwindow* window, int button, int action, int mods) {
    // Skip game input during menu phases
    if (g_game_state.phase != GamePhase::Playing) {
        if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS)
            g_mouse_clicked_this_frame = true;
        return;
    }
    double mx, my;
    glfwGetCursorPos(window, &mx, &my);
    World& world = *g_world;

    if (button == GLFW_MOUSE_BUTTON_LEFT) {
        if (action == GLFW_PRESS) {
            // --- UI button hit-testing (consume click so it never leaks to world) ---
            auto inRect = [&](double px, double py, float rx, float ry, float rw, float rh) {
                return px >= rx && px <= rx + rw && py >= ry && py <= ry + rh;
            };
            // Sculpt toggle button (always visible): (screen_w-110, 50, 100, 26)
            if (inRect(mx, my, (float)g_screen_w - 110, 50.0f, 100.0f, 26.0f)) {
                g_sculpt_mode = !g_sculpt_mode;
                return;
            }
            // Buy-count [-]/[+] buttons (always visible)
            {
                float shop_x = (float)g_screen_w * 0.5f - 100.0f;
                float shop_y = 90.0f;
                if (inRect(mx, my, shop_x + 130, shop_y + 4, 20, 24)) { // [-]
                    g_buy_count = std::max(100, g_buy_count - 500);
                    return;
                }
                if (inRect(mx, my, shop_x + 196, shop_y + 4, 20, 24)) { // [+]
                    g_buy_count = std::min(5000, g_buy_count + 500);
                    return;
                }
            }
            // Sculpt panel buttons (only when sculpt mode active)
            if (g_sculpt_mode) {
                float bx = (float)g_screen_w * 0.5f - 150.0f;
                float by = (float)g_screen_h - 60.0f;
                for (int i = 0; i < 4; i++) { // brush slots
                    if (inRect(mx, my, bx + 44 + i * 30, by + 14, 24, 18)) {
                        g_sculpt_brush = i;
                        return;
                    }
                }
                if (inRect(mx, my, bx + 170, by + 12, 22, 22)) { // radius [-]
                    g_sculpt_radius = std::max(15.0f, g_sculpt_radius - 15.0f);
                    return;
                }
                if (inRect(mx, my, bx + 258, by + 12, 22, 22)) { // radius [+]
                    g_sculpt_radius = std::min(250.0f, g_sculpt_radius + 15.0f);
                    return;
                }
            }
            // Nuke targeting: left click to confirm target
            if (g_nuke_targeting) {
                Ray ray = g_camera.screen_to_ray((float)mx, (float)my, g_screen_w, g_screen_h);
                glm::vec2 target = g_camera.ray_to_ground(ray);
                glm::vec3 to(target.x, 0, target.y);
                g_renderer->projectiles.spawn_nuke(to + glm::vec3(0,400,0), to, Faction::Red);
                world.add_rad_zone(target, 100.0f, 10.0f, Faction::Red);
                world.score[0] -= world.nuke_cost;
                world.nuke_ready[0] = (world.score[0] >= world.nuke_cost);
                g_nuke_targeting = false;
                return;
            }

            g_mouse_held = true;
            if (g_sculpt_mode) return; // sculpt painting handled per-frame in main loop
            g_selecting = true;
            g_select_start = {(float)mx, (float)my};
        } else if (action == GLFW_RELEASE) {
            g_mouse_held = false;
            if (!g_selecting) return;
            g_selecting = false;
            glm::vec2 end_screen = {(float)mx, (float)my};
            Ray r1 = g_camera.screen_to_ray(g_select_start.x, g_select_start.y, g_screen_w, g_screen_h);
            Ray r2 = g_camera.screen_to_ray(end_screen.x, end_screen.y, g_screen_w, g_screen_h);
            glm::vec2 w1 = g_camera.ray_to_ground(r1);
            glm::vec2 w2 = g_camera.ray_to_ground(r2);
            glm::vec2 box_min = glm::min(w1, w2);
            glm::vec2 box_max = glm::max(w1, w2);

            for (uint32_t i = 0; i < world.entity_count; i++)
                world.selection.selected[i] = false;

            float area = (box_max.x-box_min.x)*(box_max.y-box_min.y);
            if (area < 100.0f) {
                glm::vec2 click = (w1+w2)*0.5f;
                float best = 400.0f; Entity be = INVALID_ENTITY;
                for (uint32_t i = 0; i < world.entity_count; i++) {
                    if (!world.is_alive(i) || !world.selection.player_owned[i]) continue;
                    float d = glm::length(world.transforms.position[i] - click);
                    if (d < best) { best = d; be = i; }
                }
                if (be != INVALID_ENTITY) world.selection.selected[be] = true;
            } else {
                for (uint32_t i = 0; i < world.entity_count; i++) {
                    if (!world.is_alive(i) || !world.selection.player_owned[i]) continue;
                    glm::vec2 p = world.transforms.position[i];
                    if (p.x >= box_min.x && p.x <= box_max.x && p.y >= box_min.y && p.y <= box_max.y)
                        world.selection.selected[i] = true;
                }
            }
        }
    }

    if (button == GLFW_MOUSE_BUTTON_RIGHT && action == GLFW_PRESS) {
        if (g_nuke_targeting) { g_nuke_targeting = false; return; } // cancel nuke

        Ray ray = g_camera.screen_to_ray((float)mx, (float)my, g_screen_w, g_screen_h);
        glm::vec2 target = g_camera.ray_to_ground(ray);

        std::vector<Entity> selected;
        for (uint32_t i = 0; i < world.entity_count; i++)
            if (world.selection.selected[i] && world.is_alive(i))
                selected.push_back(i);

        if (!selected.empty()) {
            int cols = std::max(1, (int)sqrt((float)selected.size()));
            float spacing = 6.0f;
            for (size_t idx = 0; idx < selected.size(); idx++) {
                Entity e = selected[idx];
                int row = (int)idx/cols, col = (int)idx%cols;
                float ox = ((float)col - cols*0.5f)*spacing;
                float oy = ((float)row - (float)(selected.size()/cols)*0.5f)*spacing;
                world.commands.move_target[e] = target + glm::vec2(ox, oy);
                world.commands.has_move_command[e] = true;
                world.units.target[e] = INVALID_ENTITY;
                world.units.state[e] = UnitState::Moving;
            }
            // Waypoint visual effect - green ring particles
            float ty = g_renderer->terrain.get_height_at(target.x, target.y) + 1.0f;
            for (int p = 0; p < 16; p++) {
                float a = (float)p / 16.0f * 6.283f;
                Particle wp;
                wp.position = glm::vec3(target.x + cos(a)*8.0f, ty, target.y + sin(a)*8.0f);
                wp.velocity = glm::vec3(cos(a)*5.0f, 10.0f, sin(a)*5.0f);
                wp.color = glm::vec3(0.2f, 1.0f, 0.3f);
                wp.life = 0.8f;
                wp.size = 2.0f;
                if (g_renderer->particles.particles.size() < ParticleSystem::MAX_PARTICLES)
                    g_renderer->particles.particles.push_back(wp);
            }
        }
    }
    // Middle mouse: set rally point
    if (button == GLFW_MOUSE_BUTTON_MIDDLE && action == GLFW_PRESS) {
        Ray ray = g_camera.screen_to_ray((float)mx, (float)my, g_screen_w, g_screen_h);
        glm::vec2 target = g_camera.ray_to_ground(ray);
        g_game_state.set_rally_point(target);
        // Visual feedback
        float ty = g_renderer->terrain.get_height_at(target.x, target.y) + 1.0f;
        for (int p = 0; p < 24; p++) {
            float a = (float)p / 24.0f * 6.283f;
            Particle rp;
            rp.position = glm::vec3(target.x + cos(a)*12.0f, ty, target.y + sin(a)*12.0f);
            rp.velocity = glm::vec3(0, 15.0f, 0);
            rp.color = glm::vec3(0.1f, 1.0f, 0.4f);
            rp.life = 1.2f;
            rp.size = 3.0f;
            if (g_renderer->particles.particles.size() < ParticleSystem::MAX_PARTICLES)
                g_renderer->particles.particles.push_back(rp);
        }
    }
}

void key_callback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    if (action != GLFW_PRESS) return;
    World& world = *g_world;

    // N: enter nuke targeting mode
    if (key == GLFW_KEY_N && world.nuke_ready[0]) {
        g_nuke_targeting = !g_nuke_targeting;
    }

    // B: toggle terrain sculpt mode
    if (key == GLFW_KEY_B) { g_sculpt_mode = !g_sculpt_mode; }
    // Sculpt brush + radius controls (only meaningful in sculpt mode)
    if (g_sculpt_mode) {
        if (key == GLFW_KEY_1) { g_sculpt_brush = 0; return; } // Raise
        if (key == GLFW_KEY_2) { g_sculpt_brush = 1; return; } // Dig
        if (key == GLFW_KEY_3) { g_sculpt_brush = 2; return; } // Smooth
        if (key == GLFW_KEY_4) { g_sculpt_brush = 3; return; } // Flatten
        if (key == GLFW_KEY_LEFT_BRACKET)  g_sculpt_radius = std::max(15.0f, g_sculpt_radius - 15.0f);
        if (key == GLFW_KEY_RIGHT_BRACKET) g_sculpt_radius = std::min(250.0f, g_sculpt_radius + 15.0f);
    }

    // Number keys 1-9: buy unit from shop (disabled while sculpting)
    if (!g_sculpt_mode && key >= GLFW_KEY_1 && key <= GLFW_KEY_9) {
        int idx = key - GLFW_KEY_1;
        if (idx < SHOP_COUNT) {
            // Buy at the player base (units spawn from the home keep)
            glm::vec2 spawn = g_renderer->bases.bases[0].position;
            int bought = world.buy_batch(idx, g_buy_count, spawn, Faction::Red);
            if (bought > 0) {
                std::cout << "Bought " << bought << "x " << UNIT_SHOP[idx].name << "\n";
            }
        }
    }

    // +/- change batch size
    if (key == GLFW_KEY_EQUAL) g_buy_count = std::min(5000, g_buy_count + 500);
    if (key == GLFW_KEY_MINUS) g_buy_count = std::max(100, g_buy_count - 500);

    // Numpad: switch map
    if (key >= GLFW_KEY_KP_0 && key <= GLFW_KEY_KP_5) {
        g_config.current_map = key - GLFW_KEY_KP_0;
        g_game_state.needs_map_reload = true;
        std::cout << "[Map] Switching to map " << g_config.current_map << "\n";
    }
}

void spawn_army(World& world, Faction faction, glm::vec2 center, int count,
                glm::vec3 base_color, bool player_owned) {
    std::mt19937 rng(42 + (uint32_t)faction * 7777);
    bool is_enemy = (faction == Faction::Blue);

    struct SpawnDef { UnitType type; float pct; float hp,dmg,range,spd,scl; };
    std::vector<SpawnDef> comp;

    if (is_enemy) {
        comp = {
            {UnitType::Infantry, 0.25f, 100,10,8,6,2.0f},
            {UnitType::Cavalry, 0.12f, 150,18,8,12,2.5f},
            {UnitType::Archer, 0.18f, 60,12,100,4,1.8f},
            {UnitType::Bomber, 0.05f, 80,80,12,5,2.2f},
            {UnitType::Artillery, 0.03f, 70,40,200,2,2.8f},
            {UnitType::Shield, 0.05f, 200,8,6,4,2.2f},
            {UnitType::Samurai, 0.17f, 120,22,8,10,2.0f},
            {UnitType::Militia, 0.10f, 60,6,6,5,1.8f},
        };
    } else {
        comp = {
            {UnitType::Infantry, 0.30f, 100,10,8,6,2.0f},
            {UnitType::Cavalry, 0.12f, 150,18,8,12,2.5f},
            {UnitType::Archer, 0.20f, 60,12,100,4,1.8f},
            {UnitType::Bomber, 0.06f, 80,80,12,5,2.2f},
            {UnitType::Artillery, 0.04f, 70,40,200,2,2.8f},
            {UnitType::Shield, 0.12f, 200,8,6,4,2.2f},
            {UnitType::Militia, 0.10f, 60,6,6,5,1.8f},
        };
    }

    float sign = (faction == Faction::Red) ? 1.0f : -1.0f;
    float y_offset = 0;

    for (auto& def : comp) {
        int n = (int)(count * def.pct);
        int cols = std::max(1, (int)sqrt((float)n * 2.0f));
        float spacing = 6.5f;
        std::uniform_real_distribution<float> jit(-1.5f, 1.5f);

        for (int i = 0; i < n; i++) {
            Entity e = world.create_entity();
            if (e == INVALID_ENTITY) return;
            int row = i/cols, col = i%cols;
            glm::vec2 pos = center + glm::vec2(
                (col - cols*0.5f)*spacing + jit(rng),
                y_offset + (row - n/cols*0.5f)*spacing + jit(rng));

            world.transforms.position[e] = pos;
            world.transforms.velocity[e] = {0,0};
            world.transforms.rotation[e] = (faction==Faction::Red)?0.0f:3.14159f;
            world.transforms.y_offset[e] = 0;
            world.transforms.y_velocity[e] = 0;
            world.units.faction[e] = faction;
            world.units.type[e] = def.type;
            world.units.state[e] = UnitState::Idle;
            world.units.health[e] = def.hp;
            world.units.max_health[e] = def.hp;
            world.units.attack_damage[e] = def.dmg;
            world.units.attack_range[e] = def.range;
            world.units.speed[e] = def.spd;
            world.units.attack_cooldown[e] = 0;
            world.units.target[e] = INVALID_ENTITY;
            world.units.hit_timer[e] = 0;
            world.units.ragdoll_timer[e] = 0;
            world.units.is_structure[e] = false;
            world.renders.color[e] = base_color;
            world.renders.scale[e] = def.scl;
            world.selection.player_owned[e] = player_owned;
            world.selection.selected[e] = false;
            world.commands.has_move_command[e] = false;
        }
        y_offset += 80.0f * sign;
    }
}

int main(int argc, char* argv[]) {
    std::string exe_path = argv[0];
    if (!glfwInit()) { fatal_error("GLFW failed"); return -1; }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_CONTEXT_ROBUSTNESS, GLFW_LOSE_CONTEXT_ON_RESET);
    glfwWindowHint(GLFW_SAMPLES, 4);

    GLFWwindow* window = glfwCreateWindow(1600, 900, "MassRTS 3D", nullptr, nullptr);
    if (!window) { fatal_error("Window failed"); glfwTerminate(); return -1; }

    glfwMakeContextCurrent(window);
    glfwSetScrollCallback(window, scroll_callback);
    glfwSetMouseButtonCallback(window, mouse_button_callback);
    glfwSetKeyCallback(window, key_callback);
    glfwSwapInterval(1);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) { fatal_error("GLAD failed"); return -1; }
    glEnable(GL_MULTISAMPLE);
    std::cout << "OpenGL: " << glGetString(GL_VERSION) << "\n";
    std::cout << "GPU: " << glGetString(GL_RENDERER) << "\n";

    World* world_ptr = new World();
    g_world = world_ptr;
    World& world = *world_ptr;

    Renderer renderer;
    g_renderer = &renderer;
    renderer.set_base_path(exe_path);
    g_manifest.load(); // data-driven art overrides (optional assets/manifest.json)
    if (!renderer.init(1600, 900)) { fatal_error("Renderer failed"); return -1; }
    g_menu.init(1600, 900);

    CombatSystem* combat = new CombatSystem();
    combat->proj_sys = &renderer.projectiles;
    combat->height_fn = &s_terrain_height;
    MovementSystem movement;
    movement.terrain_ptr = &renderer.terrain;
    UI hud;
    hud.init(1600, 900);

    AudioSystem audio;
    audio.init();

    std::cout << "Deploying armies...\n";
    spawn_army(world, Faction::Red, {-550, 0}, 30000, {0.25f, 0.3f, 0.2f}, true);
    spawn_army(world, Faction::Blue, {550, 0}, 30000, {0.50f, 0.35f, 0.15f}, false);
    g_game_state.init_capture_points(g_game_state.selected_map);
    g_game_state.phase = GamePhase::Playing;
    std::cout << "Deployed " << world.entity_count << " units\n";
    std::cout << "Controls: 1-9=Buy units, +/-=batch size, N=Nuke(click target), RMB=Move\n";

    g_camera.target = {0, 20, 0};
    g_camera.distance = 200.0f;  // Close up, immersive
    g_camera.pitch = 35.0f;      // Lower angle for drama

    double last_time = glfwGetTime();
    int frame_count = 0;
    double fps_timer = 0;
    double tm_grid=0, tm_upload=0, tm_dispatch=0, tm_readback=0, tm_cpu=0; // profiling
    uint32_t ai_batch = 0;

    // Enemy AI economy: auto-buy timer
    float enemy_buy_timer = 0;
    // Deterministic counter for any gameplay RNG (NEVER seed RNG with wall-clock
    // time in multiplayer/lockstep -- it differs per machine and causes desync).
    uint32_t g_sim_step = 0;

    // Shared "start / restart battle with the selected map" routine. Used by both
    // the main-menu Start button AND the Map Select confirm button, so changing
    // the map actually regenerates terrain + re-uploads it to the GPU + respawns.
    auto start_battle = [&]() {
        renderer.terrain.apply_preset(g_game_state.selected_map);
        renderer.terrain.generate_with_seed(MAP_PRESETS[g_game_state.selected_map].seed);
        renderer.gpu_compute.upload_heightmap(renderer.terrain);
        renderer.decor.generate(3000.0f, [&](float x, float z){ return renderer.terrain.get_height_at(x, z); });
        for (uint32_t i = 0; i < world.entity_count; i++) world.kill_entity(i);
        world.entity_count = 0;
        world.free_list.clear();
        world.live_count = 0;
        renderer.bases.reset({-550, 0}, {550, 0});
        spawn_army(world, Faction::Red, {-550, 0}, 30000, {0.25f, 0.3f, 0.2f}, true);
        spawn_army(world, Faction::Blue, {550, 0}, 30000, {0.50f, 0.35f, 0.15f}, false);
        g_game_state.init_capture_points(g_game_state.selected_map);
        g_game_state.match_time = 0;
        g_game_state.victory_timer = 0;
        g_game_state.winning_faction = -1;
        combat->rebuild_grid(world);
        renderer.gpu_compute.upload_units(world);
        g_game_state.phase = GamePhase::Playing;
    };

    while (!glfwWindowShouldClose(window)) {
        double now = glfwGetTime();
        g_sim_step++;
        float dt = std::min((float)(now - last_time), 0.05f);
        last_time = now;

        frame_count++;
        fps_timer += dt;
        if (fps_timer >= 1.0) {
            hud.fps = frame_count;
            char title[128];
            snprintf(title, 128, "MassRTS [GPU] | FPS:%d | Units:%u | $%d | Score:%d",
                     frame_count, world.entity_count, world.money[0], world.score[0]);
            {
                FILE* dlog = fopen("diag.log", "a");
                if (dlog) {
                    fprintf(dlog, "t=%.0f fps=%d ent=%u live=%u proj=%zu | grid=%.1f upload=%.1f disp=%.1f read=%.1f cpu=%.1f (ms/s)\n",
                            now, frame_count, world.entity_count, world.live_count,
                            renderer.projectiles.projectiles.size(),
                            tm_grid*1000, tm_upload*1000, tm_dispatch*1000, tm_readback*1000, tm_cpu*1000);
                    tm_grid=tm_upload=tm_dispatch=tm_readback=tm_cpu=0;
                    fclose(dlog);
                }
                GLenum rs = glGetGraphicsResetStatus();
                if (rs != GL_NO_ERROR) {
                    FILE* d2 = fopen("diag.log", "a");
                    if (d2) { fprintf(d2, "!!! GPU RESET 0x%X t=%.1f ent=%u live=%u\n", rs, now, world.entity_count, world.live_count); fclose(d2); }
                    std::cerr << "GPU device reset (TDR). See diag.log\n";
                    glfwSetWindowShouldClose(window, true);
                }
            }
            glfwSetWindowTitle(window, title);
            frame_count = 0; fps_timer = 0;
        }

        glfwPollEvents();
        g_mouse_clicked_this_frame = false;
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
            glfwSetWindowShouldClose(window, true);
        glfwGetFramebufferSize(window, &g_screen_w, &g_screen_h);
        g_camera.update(window, dt);

        // --- Terrain sculpting (paint while LMB held in sculpt mode) ---
        // Only paint when there's money; charge per modified vertex this frame.
        if (g_sculpt_mode && g_mouse_held && world.money[0] > 0 &&
            g_game_state.phase == GamePhase::Playing) {
            double cmx, cmy; glfwGetCursorPos(window, &cmx, &cmy);
            Ray sray = g_camera.screen_to_ray((float)cmx, (float)cmy, g_screen_w, g_screen_h);
            glm::vec2 gp = g_camera.ray_to_ground(sray);
            float strength = 1.4f; // height units per frame at brush center
            Terrain::Brush b = (Terrain::Brush)g_sculpt_brush;
            int changed = renderer.terrain.sculpt(gp.x, gp.y, g_sculpt_radius, strength, b);
            int cost = (b == Terrain::Brush::Smooth || b == Terrain::Brush::Flatten)
                       ? changed / 4 : changed;
            world.money[0] = std::max(0, world.money[0] - cost);
        }

        // === Economy ===
        world.tick_economy(dt);

        // Enemy auto-buy (every 8 seconds, buys mixed troops)
        enemy_buy_timer += dt;
        if (enemy_buy_timer > 8.0f && combat->faction_alive[1] < 20000) {
            enemy_buy_timer = 0;
            int budget = world.money[1];
            // Buy mix: samurai, infantry, archers
            std::mt19937 erng(0xBEEF1234u ^ (g_sim_step * 2654435761u));
            std::uniform_int_distribution<int> pick(0, 6);
            glm::vec2 espawn = g_renderer->bases.bases[1].position;
            while (budget > 100) {
                int idx = pick(erng);
                if (idx >= SHOP_COUNT) idx = 1;
                const auto& entry = UNIT_SHOP[idx];
                if (entry.cost > budget) { idx = 0; } // fallback to militia
                int affordable = std::min(500, budget / UNIT_SHOP[idx].cost);
                if (affordable <= 0) break;
                int bought = 0;
                int cols = std::max(1, (int)sqrt((float)affordable));
                for (int i = 0; i < affordable; i++) {
                    int row=i/cols, col=i%cols;
                    glm::vec2 pos = espawn + glm::vec2((col-cols*0.5f)*6.5f,(row-affordable/cols*0.5f)*6.5f);
                    Entity e = world.buy_unit(idx, pos, Faction::Blue);
                    if (e == INVALID_ENTITY) break;
                    world.selection.player_owned[e] = false;
                    world.renders.color[e] = glm::vec3(0.50f, 0.35f, 0.15f);
                    bought++;
                }
                budget = world.money[1];
                if (bought == 0) break;
            }
        }


        // === Combat AI ===
        bool sim_active = (g_game_state.phase == GamePhase::Playing);
        if (sim_active) {

        // GPU Combat + Movement Pipeline
        if (renderer.gpu_compute.combat_gpu_ready) {
            static uint32_t gpu_frame = 0;
            gpu_frame++;

            // Rebuild CPU spatial grid (needed for AOE explosion queries)
            { double _t=glfwGetTime(); combat->rebuild_grid(world); tm_grid += glfwGetTime()-_t; }

            // Upload combat data every frame (CPU modifies health/state via attacks)
            { double _t=glfwGetTime(); renderer.gpu_compute.upload_combat_data(world); tm_upload += glfwGetTime()-_t; }

            // Clear move commands only when arrived or unit is dead
            for (uint32_t i = 0; i < world.entity_count; i++) {
                if (!world.commands.has_move_command[i]) continue;
                // Clear if arrived
                glm::vec2 diff = world.commands.move_target[i] - world.transforms.position[i];
                if (glm::length(diff) < 5.0f) {
                    world.commands.has_move_command[i] = false;
                    continue;
                }
                // Only clear for dead units - player commands override combat AI
                if (world.units.state[i] == UnitState::Dead) {
                    world.commands.has_move_command[i] = false;
                }
            }
            renderer.gpu_compute.upload_move_commands(world);

            // Compute faction centers for GPU AI
            glm::vec2 fc[2] = {{0,0},{0,0}};
            uint32_t fa[2] = {0, 0};
            for (uint32_t i = 0; i < world.entity_count; i++) {
                if (!world.is_alive(i)) continue;
                int f = (int)world.units.faction[i];
                if (f < 2) { fc[f] += world.transforms.position[i]; fa[f]++; }
            }
            if (fa[0] > 0) fc[0] /= (float)fa[0];
            if (fa[1] > 0) fc[1] /= (float)fa[1];

            // GPU dispatch: spatial hash -> combat AI -> movement
            { double _t=glfwGetTime(); renderer.gpu_compute.dispatch_combat_movement(world.entity_count, dt, fc, fa, gpu_frame); tm_dispatch += glfwGetTime()-_t; }

            // Readback positions + AI decisions
            { double _t=glfwGetTime(); renderer.gpu_compute.readback_combat(world); tm_readback += glfwGetTime()-_t; }

            // === CHANGE 1: spawn VISUAL projectiles for ranged fire (BOTH factions) ===
            // (Projectiles are now spawned inside CombatSystem::perform_attack at the
            //  correct terrain height; the old rising-edge detector was removed.)

            double _tcpu=glfwGetTime();
            // CPU executes attacks (damage, projectiles, effects, death)
            for (uint32_t i = 0; i < world.entity_count; i++) {
                if (!world.is_alive(i)) continue;
                if (world.units.state[i] != UnitState::Attacking) continue;
                if (world.units.attack_cooldown[i] > 0.0f) continue;

                Entity t = world.units.target[i];
                if (t == INVALID_ENTITY || t >= world.entity_count || !world.is_alive(t)) {
                    world.units.target[i] = INVALID_ENTITY;
                    world.units.state[i] = UnitState::Idle;
                    continue;
                }

                float dist = glm::length(world.transforms.position[t] - world.transforms.position[i]);
                if (dist > world.units.attack_range[i] * 2.5f) {
                    world.units.target[i] = INVALID_ENTITY;
                    world.units.state[i] = UnitState::Idle;
                    continue;
                }

                combat->perform_attack(world, i, t, dist);
            }

            // Update hit timers + death cleanup (renderer handles visual effects)
            for (uint32_t i = 0; i < world.entity_count; i++) {
                if (!world.alive[i]) continue;
                if (world.units.hit_timer[i] > 0) {
                    world.units.hit_timer[i] -= dt;
                    if (world.units.hit_timer[i] <= 0) {
                        world.units.hit_timer[i] = 0;
                        if (world.units.state[i] == UnitState::Dead) {
                            world.alive[i] = false;
                        }
                    }
                }
            }

            // Count alive for HUD + siege damage to bases
            combat->faction_alive[0] = 0;
            combat->faction_alive[1] = 0;
            float base_dmg[2] = {0,0};
            for (uint32_t i = 0; i < world.entity_count; i++) {
                if (!world.is_alive(i)) continue;
                int f = (int)world.units.faction[i];
                combat->faction_alive[f]++;
                // Enemy units inside a base ring chip its health
                int enemy_base = 1 - f;
                if (g_renderer->bases.bases[enemy_base].alive &&
                    g_renderer->bases.point_in_base(enemy_base, world.transforms.position[i])) {
                    base_dmg[enemy_base] += world.units.attack_damage[i] * dt;
                }
            }
            tm_cpu += glfwGetTime()-_tcpu;
            if (base_dmg[0] > 0) g_renderer->bases.damage(0, base_dmg[0]);
            if (base_dmg[1] > 0) g_renderer->bases.damage(1, base_dmg[1]);
            // Base destroyed = instant win for the other side
            if (g_victory_enabled && !g_renderer->bases.bases[0].alive) g_game_state.phase = GamePhase::Defeat;
            else if (g_victory_enabled && !g_renderer->bases.bases[1].alive) g_game_state.phase = GamePhase::Victory;
        } else {
            // CPU fallback
            combat->update_batched(world, dt, ai_batch, 25000);
            ai_batch += 25000;
            if (ai_batch >= world.entity_count) ai_batch = 0;
            movement.update(world, dt, combat->grid);
        }

        // === Territory Control ===
        if (g_game_state.phase == GamePhase::Playing) {
            static std::vector<uint8_t> territory_alive;
            territory_alive.resize(world.entity_count);
            for (uint32_t i=0; i<world.entity_count; i++) territory_alive[i] = world.is_alive(i) ? 1 : 0;
            g_game_state.update_territory(world.transforms.position, (const uint8_t*)world.units.faction, territory_alive.data(), world.entity_count, dt);
            g_game_state.update(dt);
            if (g_victory_enabled && g_game_state.check_victory()) g_game_state.phase = g_game_state.get_winner();
        }

        // === Physics ===
        world.tick_corpses(dt);

        // === Projectiles ===
        renderer.projectiles.update(dt, s_terrain_height);

        // Explosion particles + process hits.
        // kind: 0=arrow (NO explosion, just dust puff), 1=cannon (explosion),
        //       2=nuke (huge explosion). Classification is set in projectiles.h update().
        for (auto& hit : renderer.projectiles.pending_hits) {
            if (hit.kind == 0) {
                // Arrow: small dust/impact puff, NO explosion, no knockback.
                renderer.particles.spawn_arrow_impact(hit.position);
                audio.trigger_arrow();
                world.apply_explosion(hit.position, hit.radius, 0.0f,
                                      hit.damage, hit.source_faction);
            } else {
                bool is_nuke = (hit.kind == 2);
                if (is_nuke) {
                    renderer.particles.spawn_nuke_blast(hit.position);
                    audio.trigger_nuke();
                } else {
                    // Cannon: clear EXPLOSION (fireball+smoke+shockwave).
                    renderer.particles.spawn_cannon_blast(hit.position);
                    audio.trigger_cannon();
                }
                // Apply AOE knockback + ragdoll so units get launched into the air.
                // The GPU shaders never do AOE damage, so the CPU must apply it here;
                // otherwise nukes/cannonballs show no ragdoll/blast effect.
                float force = hit.knockback_force > 0.0f ? hit.knockback_force
                                                         : (is_nuke ? 260.0f : 90.0f);
                world.apply_explosion(hit.position, hit.radius, force,
                                      hit.damage, hit.source_faction);
                // Carve a crater into the terrain at the impact point so blasts
                // leave a lasting mark. Cannon = small dent, nuke = deep crater.
                {
                    float crater_r = hit.radius * 0.6f;
                    float crater_depth = is_nuke ? 16.0f : 3.0f;
                    renderer.terrain.sculpt(hit.position.x, hit.position.z,
                                            crater_r, crater_depth, Terrain::Brush::Dig);
                }
            }
        }

        // Combat hit particles
        for (uint32_t i = ai_batch > 15000 ? ai_batch-15000 : 0;
             i < ai_batch && i < world.entity_count; i += 50) {
            if (world.is_alive(i) && world.units.state[i] == UnitState::Attacking &&
                world.units.attack_cooldown[i] > 0.7f)
                renderer.spawn_hit_particles(world.transforms.position[i], world.renders.color[i]);
        }

        renderer.update(dt);

        // Audio: intensity based on combat
        float audio_intensity = 0;
        if (combat->faction_alive[0] > 0 && combat->faction_alive[1] > 0) {
            // Higher intensity when armies are close
            float center_dist = glm::length(combat->faction_center[0] - combat->faction_center[1]);
            audio_intensity = glm::clamp(1.0f - center_dist / 800.0f, 0.0f, 1.0f);
        }
        audio.update(dt, audio_intensity);

        // === HUD stats ===
        int sel=0, sel_inf=0, sel_cav=0, sel_arc=0;
        for (uint32_t i = 0; i < world.entity_count; i++) {
            if (world.selection.selected[i] && world.is_alive(i)) {
                sel++;
                switch(world.units.type[i]) {
                    case UnitType::Infantry: sel_inf++; break;
                    case UnitType::Cavalry: sel_cav++; break;
                    case UnitType::Archer: sel_arc++; break;
                    default: break;
                }
            }
        }
        hud.selected_count = sel;
        hud.selected_infantry = sel_inf;
        hud.selected_cavalry = sel_cav;
        hud.selected_archer = sel_arc;
        hud.red_alive = combat->faction_alive[0];
        hud.blue_alive = combat->faction_alive[1];
        hud.total_units = (int)world.entity_count;
        hud.screen_w = g_screen_w;
        hud.screen_h = g_screen_h;
        hud.score = world.score[0];
        hud.nuke_cost = world.nuke_cost;
        hud.nuke_ready = world.nuke_ready[0];
        hud.money = world.money[0];
        hud.buy_count = g_buy_count;
        hud.sculpt_mode = g_sculpt_mode;
        hud.sculpt_brush = g_sculpt_brush;
        hud.sculpt_radius = (int)g_sculpt_radius;

        } // end sim_active
        // === Render ===
        glViewport(0, 0, g_screen_w, g_screen_h);
        float aspect = (float)g_screen_w / (float)g_screen_h;
        glm::mat4 view = g_camera.get_view();
        glm::mat4 proj = g_camera.get_projection(aspect);
        // Render the battlefield whenever we're in-game OR on the end screen.
        // The Victory/Defeat overlay draws ON TOP of the frozen battlefield so
        // the player sees the final battle state instead of a black screen.
        bool draw_world = (g_game_state.phase == GamePhase::Playing ||
                           g_game_state.phase == GamePhase::Victory ||
                           g_game_state.phase == GamePhase::Defeat);
        if (draw_world) { renderer.render(world, view, proj, g_camera.get_position()); }
        else { glClearColor(0.05f, 0.05f, 0.1f, 1.0f); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT); }

        // Selection box
        if (g_selecting) {
            double smx, smy;
            glfwGetCursorPos(window, &smx, &smy);
            glm::vec2 s0 = {g_select_start.x/g_screen_w*2-1, 1-g_select_start.y/g_screen_h*2};
            glm::vec2 s1 = {(float)smx/g_screen_w*2-1, 1-(float)smy/g_screen_h*2};
            glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            renderer.render_selection_box(glm::min(s0,s1), glm::max(s0,s1));
            glDisable(GL_BLEND);
            glDisable(GL_BLEND);
        }

        // HUD
        hud.render();

        // Territory bar + Shop Panel
        if (g_game_state.phase == GamePhase::Playing) {
            g_menu.screen_w = g_screen_w; g_menu.screen_h = g_screen_h;
            g_menu.render_territory_bar(g_game_state);

            // Clickable shop panel
            double smx, smy; glfwGetCursorPos(window, &smx, &smy);
            static const char* shop_names[] = {"Militia","Infantry","Archer","Shield","Cavalry","Bomber","Artillery","Wall","Turret"};
            static int shop_costs[] = {50,100,200,150,300,500,250,350,100};
            bool shop_click = (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS);
            static bool prev_shop_click = false;
            bool shop_clicked_now = shop_click && !prev_shop_click;
            prev_shop_click = shop_click;
            int shop_result = g_menu.render_shop_panel((float)smx, (float)smy, shop_clicked_now,
                                                       shop_names, shop_costs, 9,
                                                       world.money[0], g_buy_count);
            if (shop_result >= 0) {
                glm::vec2 spawn = g_renderer->bases.bases[0].position;
                int bought = world.buy_batch(shop_result, g_buy_count, spawn, Faction::Red);
                if (bought > 0) { }
            }
        }


        // Minimap
        static std::vector<char> alive_flags;
        alive_flags.resize(world.entity_count);
        for (uint32_t i=0; i<world.entity_count; i++) alive_flags[i] = world.is_visible(i)?1:0;
        hud.render_minimap_dots(world.transforms.position, world.renders.color,
                                alive_flags.data(), world.entity_count, 1200.0f);


        // === Menu / Map Select ===
        if (g_game_state.phase == GamePhase::Menu) {
            double mmx, mmy; glfwGetCursorPos(window, &mmx, &mmy);
            g_menu.screen_w = g_screen_w; g_menu.screen_h = g_screen_h;
            g_menu.render_main_menu(g_game_state, (float)mmx, (float)mmy);
            static bool menu_was_pressed = false;
            if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS) {
                if (!menu_was_pressed) {
                    if (g_game_state.menu_hover == 0) {
                        start_battle(); // start with currently-selected map
                    } else if (g_game_state.menu_hover == 1) {
                        g_game_state.phase = GamePhase::MapSelect;
                    }
                }
                menu_was_pressed = true;
            } else { menu_was_pressed = false; }
        } else if (g_game_state.phase == GamePhase::MapSelect) {
            double mmx, mmy; glfwGetCursorPos(window, &mmx, &mmy);
            g_menu.screen_w = g_screen_w; g_menu.screen_h = g_screen_h;
            g_menu.render_map_select(g_game_state, (float)mmx, (float)mmy);
            static bool map_was_pressed = false;
            if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS) {
                if (!map_was_pressed) {
                    if (g_game_state.menu_hover >= 0 && g_game_state.menu_hover < MAP_COUNT)
                        g_game_state.selected_map = g_game_state.menu_hover;
                    else if (g_game_state.menu_hover == 100)
                        start_battle(); // regenerate terrain for the picked map + respawn
                }
                map_was_pressed = true;
            } else { map_was_pressed = false; }
        }

        // === End screen / Menu overlay ===
        if (g_game_state.phase == GamePhase::Victory || g_game_state.phase == GamePhase::Defeat) {
            double emx, emy; glfwGetCursorPos(window, &emx, &emy);
            g_menu.screen_w = g_screen_w; g_menu.screen_h = g_screen_h;
            // 2D overlay must ignore depth so it always draws over the frozen
            // battlefield, and must blend so its alpha works.
            glDisable(GL_DEPTH_TEST);
            glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            g_menu.render_end_screen(g_game_state, g_game_state.phase == GamePhase::Victory, (float)emx, (float)emy);
            if (g_mouse_clicked_this_frame && g_game_state.menu_hover == 0) {
                g_game_state.phase = GamePhase::Playing;
            }
        }

        glDisable(GL_BLEND);
        glEnable(GL_DEPTH_TEST);
        glfwSwapBuffers(window);
    }

    audio.cleanup();
    hud.cleanup();
    renderer.cleanup();
    delete combat;
    delete world_ptr;
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
