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
#include <winsock2.h>   // MUST precede windows.h to avoid sockaddr redefinition
#include <ws2tcpip.h>
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
#include "game/loading_manager.h"
#include "game/settlement_system.h"
#include "game/wave_director.h"
#include "game/meta_progression.h"
#include "ui/menu.h"
#include "net/session.h"
#define MINIAUDIO_IMPLEMENTATION
#include "audio/audio_system.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

// Capture the current framebuffer to a PNG (flips rows; GL origin is bottom-left).
static void save_screenshot(const char* path, int w, int h) {
    std::vector<unsigned char> px((size_t)w*h*3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px.data());
    std::vector<unsigned char> flip((size_t)w*h*3);
    for (int y = 0; y < h; ++y)
        std::memcpy(&flip[(size_t)(h-1-y)*w*3], &px[(size_t)y*w*3], (size_t)w*3);
    stbi_write_png(path, w, h, 3, flip.data(), w*3);
    std::cout << "[screenshot] " << path << "\n";
}

Camera g_camera;
bool g_selecting = false;
glm::vec2 g_select_start = {0, 0};
int g_screen_w = 1600, g_screen_h = 900;
World* g_world = nullptr;
Renderer* g_renderer = nullptr;

// Nuke targeting mode
bool g_nuke_targeting = false;
// Survival build-placement: when armed, the next ground click builds the chosen
// static defense (wall/turret) at the cursor instead of spawning at the base.
int  g_build_place_shop = -1;   // shop index being placed, or -1 = inactive
// Game state machine
GameState g_game_state;
// Survival / roguelite wave director (only active when mode == Survival).
WaveDirector g_wave_director;
// Cross-run meta progression (persistent save file).
MetaProgression g_meta;
bool g_meta_recorded = false;  // guard so a run's result is banked exactly once
SettlementSystem g_settlements; // AI commanders: march -> settle -> build -> produce
MenuRenderer g_menu;
LoadingManager g_loader;
bool g_mouse_clicked_this_frame = false;

// Shop state
int g_shop_selection = -1; // -1 = no selection
int g_buy_count = 1000; // buy in batches of 1000

// --- Networking (lockstep listen-server) ---
NetSession g_net;                  // inactive unless --host / --join
bool  g_net_active = false;        // mirrors g_net.active()
uint32_t g_sim_tick = 0;           // deterministic lockstep tick counter
double g_net_accum = 0.0;          // fixed-step accumulator for net ticks

// Reinterpret a float's bits as uint32 (and back) so carve params (y/radius)
// ride inside NetCommand's integer fields without precision loss.
static inline uint32_t f2u(float f){ uint32_t u; std::memcpy(&u,&f,4); return u; }
static inline float    u2f(uint32_t u){ float f; std::memcpy(&f,&u,4); return f; }

// --- Terrain sculpt state ---
bool g_sculpt_mode = false;        // toggle with B
bool g_victory_enabled = false;    // OFF: endless battle, no win/lose screen
// Dynamic shallow-water sim. Disabled for now (the FluidSystem stays dormant:
// it is never seeded/updated, so fluid.render()/update() early-out on !enabled).
// Flip to true to bring the dynamic water back.
bool g_dynamic_water = false;
int  g_sculpt_brush = 1;           // 0=Raise 1=Dig 2=Smooth 3=Flatten
float g_sculpt_radius = 60.0f;     // world units
float g_sculpt_strength = 0.15f;   // brush strength 0.05..1.0, gentle default; , / . to tune
glm::vec3 g_tunnel_last(0.0f);      // last cave-brush carve point (for tunneling)
bool g_tunnel_active = false;       // mid-drag tunnel in progress
bool g_mouse_held = false;         // left button currently down
const int SCULPT_COST_PER_VERT = 1; // money charged per modified vertex-tick

void fatal_error(const char* msg) {
    std::cerr << msg << std::endl;
#ifdef _WIN32
    MessageBoxA(NULL, msg, "MassRTS Error", MB_OK | MB_ICONERROR);
#endif
}

// Toggle between fullscreen (borderless on primary monitor) and windowed mode
// at the resolution chosen in settings. Called when the user flips the toggle.
void apply_fullscreen(GLFWwindow* window, GameState& state) {
    static int saved_x = 100, saved_y = 100;
    if (state.settings_fullscreen) {
        // BORDERLESS WINDOWED fullscreen (not exclusive). We keep the monitor
        // arg as nullptr and just size/position a decoration-less window to fill
        // the primary monitor. Exclusive fullscreen (glfwSetWindowMonitor with a
        // real monitor) grabs the display and stays always-on-top, which blocks
        // the Windows Start menu / Win key and Alt-Tab overlays — especially
        // annoying during the loading screen. Borderless lets the OS draw its
        // shell over us normally while still looking fullscreen.
        glfwGetWindowPos(window, &saved_x, &saved_y);
        GLFWmonitor* mon = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode = glfwGetVideoMode(mon);
        int mx = 0, my = 0;
        glfwGetMonitorPos(mon, &mx, &my);
        glfwSetWindowAttrib(window, GLFW_DECORATED, GLFW_FALSE);
        glfwSetWindowAttrib(window, GLFW_FLOATING, GLFW_FALSE);
        glfwSetWindowMonitor(window, nullptr, mx, my, mode->width, mode->height, 0);
    } else {
        glfwSetWindowAttrib(window, GLFW_DECORATED, GLFW_TRUE);
        glfwSetWindowMonitor(window, nullptr, saved_x, saved_y,
                             state.settings_resolution_w, state.settings_resolution_h, 0);
    }
    glfwSwapInterval(state.settings_vsync ? 1 : 0);
}

void scroll_callback(GLFWwindow* w, double x, double y) {
    // The wheel ALWAYS controls camera zoom (sculpt mode included). Brush
    // strength is tuned with [ and ] keys, so the player never loses camera
    // control while editing terrain.
    g_camera.on_scroll(y);
}

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

            // Survival build placement: left click builds the armed defense at
            // the cursor and consumes the click (no selection box).
            if (g_build_place_shop >= 0 && !g_sculpt_mode) {
                Ray ray = g_camera.screen_to_ray((float)mx, (float)my, g_screen_w, g_screen_h);
                glm::vec2 gp = g_camera.ray_to_ground(ray);
                int built = g_world->buy_batch(g_build_place_shop, 1, gp, Faction::Red);
                // Out of metal -> disarm; otherwise stay armed for rapid laying.
                if (built <= 0) g_build_place_shop = -1;
                return;
            }

            g_mouse_held = true;
            if (g_sculpt_mode) return; // sculpt painting handled per-frame in main loop
            g_selecting = true;
            g_select_start = {(float)mx, (float)my};
        } else if (action == GLFW_RELEASE) {
            g_mouse_held = false;
            g_tunnel_active = false; // end the current tunnel run
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
        // Brush strength on , / .  (wheel stays on camera zoom)
        if (key == GLFW_KEY_COMMA)  g_sculpt_strength = std::max(0.05f, g_sculpt_strength - 0.05f);
        if (key == GLFW_KEY_PERIOD) g_sculpt_strength = std::min(1.0f,  g_sculpt_strength + 0.05f);
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

    // K: toggle battlefield blood splats (default OFF). Corpses always remain.
    if (key == GLFW_KEY_K && g_renderer) {
        g_renderer->corpses.blood_enabled = !g_renderer->corpses.blood_enabled;
        std::cout << "[Blood] " << (g_renderer->corpses.blood_enabled ? "ON" : "OFF") << "\n";
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

// Auto-screenshot mode: when run with --shots, the game warms up, then orbits
// the camera through preset angles, saves a PNG per angle, and exits. Lets the
// terrain be inspected from multiple viewpoints without manual piloting.
static bool g_shot_mode = false;
static int  g_shot_warmup = 90; // frames to let terrain mesh/settle first
static bool g_survmenu_dbg = false; // DEBUG: drive menu->setup->START path

int main(int argc, char* argv[]) {
    // EARLY diagnostic: write to verify main() runs and file I/O works.
    FILE* early = fopen("main_entry.txt", "w");
    if (early) { fprintf(early, "main() entry argc=%d\n", argc); fclose(early); }
    
    std::string exe_path = argv[0];
    std::string net_mode;            // "host" or "join"
    std::string net_ip = "127.0.0.1";
    bool auto_survival = false;       // --survival: auto-start a survival run
    try {
        for (int ai = 1; ai < argc; ++ai) {
            std::string a = argv[ai];
            if (a == "--shots") g_shot_mode = true;
            else if (a == "--survival") auto_survival = true;
            else if (a == "--survmenu") g_survmenu_dbg = true;
            else if (a == "--host") net_mode = "host";
            else if (a == "--join") { net_mode = "join"; if (ai+1 < argc && argv[ai+1][0] != '-') net_ip = argv[++ai]; }
        }
    } catch (...) {
        FILE* e=fopen("arg_crash.txt","w"); if(e){fprintf(e,"exception in arg loop\n");fclose(e);}
    }
    // Diagnostic checkpoint 1: after arg parse, before GLFW init
    { FILE* d=fopen("checkpoint1.txt","w"); if(d){fprintf(d,"args parsed net_mode=[%s]\n",net_mode.c_str());fclose(d);} }
    if (!glfwInit()) { fatal_error("GLFW failed"); return -1; }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_CONTEXT_ROBUSTNESS, GLFW_LOSE_CONTEXT_ON_RESET);
    glfwWindowHint(GLFW_SAMPLES, 4);
    // Create the window HIDDEN. All the heavy GL init below (shader compiles,
    // World allocation, FBO/postfx setup) happens before the first frame is
    // drawn; if the window were visible it would show as a black rectangle for
    // that whole time. We reveal it (glfwShowWindow) only after the first menu
    // frame is rendered, so the player sees the menu immediately, never black.
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

    GLFWwindow* window = glfwCreateWindow(1600, 900, "MassRTS 3D", nullptr, nullptr);
    if (!window) { fatal_error("Window failed"); glfwTerminate(); return -1; }

    glfwMakeContextCurrent(window);
    glfwSetScrollCallback(window, scroll_callback);
    glfwSetMouseButtonCallback(window, mouse_button_callback);
    glfwSetKeyCallback(window, key_callback);
    glfwSwapInterval(1);

    // Apply user's fullscreen/resolution preference on startup (defaults to fullscreen)
    apply_fullscreen(window, g_game_state);

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

    // Feed the real terrain height into the camera and particle system so the
    // view can descend into bomb craters and effects settle on the crater floor
    // instead of the flat Z=0 plane.
    g_camera.ground_height = &s_terrain_height;
    renderer.particles.ground_height = &s_terrain_height;

    CombatSystem* combat = new CombatSystem();
    combat->proj_sys = &renderer.projectiles;
    combat->height_fn = &s_terrain_height;
    MovementSystem movement;
    movement.terrain_ptr = &renderer.terrain;
    UI hud;
    hud.init(1600, 900);

    AudioSystem audio;
    audio.init();
    // Sync the audio buses to the saved settings, then start the menu track.
    audio.set_volumes(g_game_state.settings_master_volume, 0.6f,
                      g_game_state.settings_sfx_volume);
    audio.play_music(0); // menu music (no-op if assets/audio/music_menu.* absent)

    // DON'T spawn armies here — that blocks the main menu from showing.
    // Armies are spawned inside start_battle() when the player clicks START.
    g_game_state.init_capture_points(g_game_state.selected_map);
    g_game_state.phase = GamePhase::Menu;
    std::cout << "Deployed " << world.entity_count << " units\n";
    std::cout << "Controls: 1-9=Buy units, +/-=batch size, N=Nuke(click target), RMB=Move\n";
    std::cout << "Camera: WASD=Pan, Q/E=Rotate, R/F=Rise/Sink, MMB-drag=Orbit(tilt up to look out of craters), Scroll=Zoom\n";

    g_camera.target = {0, 20, 0};
    g_camera.distance = 200.0f;  // Close up, immersive
    g_camera.pitch = 35.0f;      // Lower angle for drama

    double last_time = glfwGetTime();
    int frame_count = 0;

    // Where the Settings screen returns when you back out of it: the main menu
    // (when opened from the title screen) or the Pause overlay (when opened
    // mid-battle via ESC -> Pause -> Settings).
    GamePhase settings_return = GamePhase::Menu;
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
    // ASYNC LOADING: start_battle now kicks off CPU world-gen on a background
    // thread and switches to the Loading phase. The main loop shows a progress
    // bar; when the CPU work finishes, finalize_battle() (main thread) runs the
    // GL uploads and switches to Playing. This stops the multi-second freeze.
    auto start_battle = [&]() {
        g_game_state.phase = GamePhase::Loading;
        g_loader.start([&](LoadingManager& L) {
            L.set(0.05f, "GENERATING TERRAIN");
            renderer.terrain.apply_preset(g_game_state.selected_map);
            renderer.terrain.generate_with_seed(MAP_PRESETS[g_game_state.selected_map].seed);

            L.set(0.30f, "PLACING DECORATIONS");
            renderer.decor.generate_cpu(6000.0f, [&](float x, float z){ return renderer.terrain.get_height_at(x, z); });

            L.set(0.45f, "DEPLOYING ARMIES");
            for (uint32_t i = 0; i < world.entity_count; i++) world.kill_entity(i);
            world.entity_count = 0;
            world.free_list.clear();
            world.live_count = 0;
            if (g_game_state.mode == GameMode::Survival) {
                // Survival: only the player's starter defenders spawn here. The
                // enemy swarm pours out of nests per-wave (WaveDirector). Give a
                // modest garrison + starting metal so wave 1 is buildable.
                spawn_army(world, Faction::Red, {-550, 0}, 800, {0.25f, 0.3f, 0.2f}, true);
                world.money[0] = 800;
                world.money[1] = 0;
            } else {
                spawn_army(world, Faction::Red, {-550, 0}, 35000, {0.25f, 0.3f, 0.2f}, true);
                spawn_army(world, Faction::Blue, {550, 0}, 35000, {0.50f, 0.35f, 0.15f}, false);
            }

            L.set(0.55f, "PREPARING BATTLEFIELD");
            g_game_state.init_capture_points(g_game_state.selected_map);
            g_game_state.match_time = 0;
            g_game_state.victory_timer = 0;
            g_game_state.winning_faction = -1;
            g_settlements.height_fn = &s_terrain_height;
            g_settlements.init({-550, 0}, {550, 0});

            L.set(0.65f, "BUILDING TERRAIN MESH");
            // Heavy SDF voxel fill (CPU, ~900M samples) on the worker thread so
            // the main thread stays responsive (loading screen keeps drawing).
            renderer.sdf_terrain.init_cpu(&renderer.terrain, 6000.0f);
            L.set(1.0f, "FINALIZING");
        });
    };


    // Main-thread GL finalize after the background CPU work completes.
    static bool battle_first_init = true;
    auto finalize_battle = [&]() {
        // Upload the CPU-built terrain mesh + decor instances to the GPU (these
        // MUST be on the main thread — the worker only did the CPU assembly).
        renderer.terrain.upload_mesh_gl();
        renderer.decor.upload_gl();

        if (battle_first_init) {
            renderer.gpu_compute.init(renderer.shader_dir_cached, renderer.terrain);
            renderer.bases.init({-550, 0}, {550, 0}, [&](float x, float z){ return renderer.terrain.get_height_at(x, z); });
            renderer.sdf_terrain.init_gl(); // CPU fill already done on worker; just remesh+upload
            if (g_dynamic_water) {
                renderer.fluid.init(&renderer.terrain, renderer.water_shader);
                renderer.fluid.seed_sea_level(8.0f); // flood natural basins at start
            }
            battle_first_init = false;
        } else {
            renderer.gpu_compute.upload_heightmap(renderer.terrain);
            // Worker already refilled SDF chunks (init_cpu); just rebuild GL meshes.
            renderer.sdf_terrain.init_gl();
            renderer.bases.reset({-550, 0}, {550, 0});
            if (g_dynamic_water) {
                renderer.fluid.init(&renderer.terrain, renderer.water_shader);
                renderer.fluid.seed_sea_level(8.0f);
            }
        }
        combat->rebuild_grid(world);
        renderer.gpu_compute.upload_units(world);
        g_game_state.phase = GamePhase::Playing;

        // Reposition the camera to look at the player's base, sitting safely
        // ABOVE the local terrain so it never starts buried under a mountain
        // (the startup default target.y was a fixed 20 which clips into tall
        // height-scaled maps and ends up looking up from under the ground).
        {
            glm::vec2 pbase = renderer.bases.bases[0].position;
            float ground = renderer.terrain.get_height_at(pbase.x, pbase.y);
            g_camera.target = glm::vec3(pbase.x, ground + 30.0f, pbase.y);
            g_camera.distance = 420.0f;
            g_camera.pitch = 42.0f;   // look down at the field
            g_camera.yaw = 0.0f;
        }

        // Survival: arm the wave director and open the first build window.
        if (g_game_state.mode == GameMode::Survival) {
            glm::vec2 base = renderer.bases.bases[0].position;
            float wsize = MAP_PRESETS[g_game_state.selected_map].world_size;
            g_wave_director.start_run(base, wsize, g_game_state.survival_seed,
                                      g_game_state.survival_tier);
            // Apply persistent cross-run unlocks (局间成长) to this run.
            if (g_meta.unlock_richstart) world.money[0] += 400;
            if (g_meta.unlock_veterans) {
                // 50% bigger starter garrison.
                spawn_army(world, Faction::Red, {-560, 60}, 400, {0.25f, 0.3f, 0.2f}, true);
            }
            g_meta_recorded = false; // arm result-banking for this run
            g_wave_director.begin_prep(world);
        }
        audio.play_music(1); // switch menu -> battle track
    };

    // --- Start networking session if requested ---
    if (!net_mode.empty()) {
        bool ok = (net_mode == "host") ? g_net.host(NET_PORT, "Host")
                                       : g_net.join(net_ip.c_str(), NET_PORT, "Client");
        g_net_active = ok;
        // GUI apps have no console; log to a file so we can verify the session.
        FILE* nl = fopen(net_mode == "host" ? "net_host.log" : "net_client.log", "w");
        if (nl) {
            fprintf(nl, "[Net] mode=%s ok=%d port=%d ip=%s faction=%d player=%d\n",
                    net_mode.c_str(), ok?1:0, NET_PORT, net_ip.c_str(),
                    (int)g_net.local_faction, (int)g_net.local_player);
            fclose(nl);
        }
        std::cout << "[Net] " << net_mode << " ok=" << ok << "\n";
    }
    // Unconditional diagnostic: write args to a temp file to verify parsing.
    {
        FILE* diag = fopen("net_diag.txt", "w");
        if (diag) {
            fprintf(diag, "argc=%d net_mode=[%s] net_ip=[%s]\n", argc, net_mode.c_str(), net_ip.c_str());
            for (int i = 0; i < argc; i++) fprintf(diag, "argv[%d]=%s\n", i, argv[i]);
            fclose(diag);
        }
    }

    // --survival: jump straight into a survival run (skips the menu click).
    // Load cross-run survival progression (unlocks, best results) before any
    // run can start. Defaults are kept if the save file is absent.
    g_meta.load();
    // Load data-driven wave/balance tuning (BAR-style: balance lives in data).
    g_wave_director.load_config();
    g_game_state.survival_tier = std::min(g_game_state.survival_tier, g_meta.unlocked_tier);

    // --survival: jump straight into a survival run (skips the menu click).
    if (auto_survival) {
        g_game_state.mode = GameMode::Survival;
        start_battle();
    } else if (g_shot_mode) {
        // --shots without --survival: auto-start a skirmish so we can capture.
        g_game_state.mode = GameMode::Skirmish;
        start_battle();
    } else if (g_survmenu_dbg) {
        // DEBUG: exercise the exact menu->setup->START path (reproduces the
        // player's survival entry, which --survival bypasses).
        g_game_state.survival_setup_open = true;
    }

    // === Reveal the window with the menu already drawn ===
    // Everything above (GL init, world alloc, audio, optional net handshake) is
    // finished. Reveal the (already fullscreen-sized) window, sync the render
    // targets to its REAL framebuffer size, then draw one menu frame and swap.
    // The player's first sight of the window is the main menu at the correct
    // size, never the black screen that used to show during the init above.
    {
        glfwShowWindow(window); // realizes the borderless-fullscreen framebuffer
        // Query the ACTUAL framebuffer size (the window was created at 1600x900
        // but apply_fullscreen() resized it to the monitor). The post-processing
        // FBOs were built at 1600x900 in renderer.init(); on_resize() rebuilds
        // them at the real size, otherwise the scene only fills a 1600x900 corner
        // of the fullscreen backbuffer (the "window stuck in the bottom-left" bug).
        glfwGetFramebufferSize(window, &g_screen_w, &g_screen_h);
        if (g_screen_w <= 0) g_screen_w = 1600;
        if (g_screen_h <= 0) g_screen_h = 900;
        renderer.on_resize(g_screen_w, g_screen_h);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, g_screen_w, g_screen_h);
        glClearColor(0.02f, 0.03f, 0.08f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        g_menu.screen_w = g_screen_w; g_menu.screen_h = g_screen_h;
        g_menu.render_main_menu(g_game_state, -1.0f, -1.0f); // -1,-1: no hover yet
        glfwSwapBuffers(window);
    }

    while (!glfwWindowShouldClose(window)) {
        double now = glfwGetTime();
        g_sim_step++;
        float dt = std::min((float)(now - last_time), 0.05f);
        last_time = now;

        // --- Networking pump (receive/relay commands every frame) ---
        if (g_net_active) g_net.poll();

        // --- Lockstep tick: advance synced ticks at a fixed rate. Each player
        // confirms an (empty or command-bearing) tick; once ALL confirm, we
        // apply every player's commands for that tick in a deterministic order.
        if (g_net_active) {
            g_net_accum += dt;
            const double NET_STEP = 1.0 / (double)NET_TICK_RATE;
            int guard = 0;
            while (g_net_accum >= NET_STEP && guard++ < 8) {
                g_net_accum -= NET_STEP;
                // Confirm this tick (no-op if we already queued real commands).
                g_net.confirm_empty_tick(g_sim_tick);
                if (!g_net.tick_ready(g_sim_tick)) { g_net_accum = 0; break; }
                for (auto& c : g_net.commands_for(g_sim_tick)) {
                    if (c.kind == CmdKind::Carve) {
                        glm::vec3 ctr(c.target.x, u2f(c.param_a), c.target.y);
                        float rad = u2f(c.param_b);
                        bool dig = (c.unit_end == 0);
                        renderer.carve_terrain(ctr, rad, dig);
                    }
                }
                g_net.advance(g_sim_tick);
                g_sim_tick++;
            }
        }

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
                    fprintf(dlog, "t=%.0f fps=%d ent=%u live=%u proj=%zu | grid=%.1f upload=%.1f disp=%.1f read=%.1f cpu=%.1f gpuMS=%.2f (ms/s)\n",
                            now, frame_count, world.entity_count, world.live_count,
                            renderer.projectiles.projectiles.size(),
                            tm_grid*1000, tm_upload*1000, tm_dispatch*1000, tm_readback*1000, tm_cpu*1000, renderer.gpu_compute.last_gpu_ms);
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
        // ESC is context-sensitive (edge-triggered so it toggles, not repeats):
        //   Playing            -> Pause (freeze battle, show pause overlay)
        //   Paused             -> Resume
        //   Settings           -> back to wherever Settings was opened from
        //   MapSelect          -> back to main menu
        //   Victory/Defeat     -> back to main menu (restore menu music)
        //   Menu               -> quit the game
        //   Loading            -> ignored (mid world-gen)
        {
            static bool esc_prev = false;
            bool esc_now = (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS);
            if (esc_now && !esc_prev) {
                // ESC first cancels an armed build placement, then pauses.
                if (g_game_state.phase == GamePhase::Playing && g_build_place_shop >= 0) {
                    g_build_place_shop = -1;
                } else
                switch (g_game_state.phase) {
                    case GamePhase::Playing:
                        g_game_state.phase = GamePhase::Paused;
                        break;
                    case GamePhase::Paused:
                        g_game_state.phase = GamePhase::Playing;
                        break;
                    case GamePhase::Settings:
                        g_game_state.phase = settings_return;
                        g_game_state.settings_submenu = 0;
                        break;
                    case GamePhase::MapSelect:
                        g_game_state.phase = GamePhase::Menu;
                        break;
                    case GamePhase::Victory:
                    case GamePhase::Defeat:
                        g_game_state.phase = GamePhase::Menu;
                        audio.play_music(0);
                        break;
                    case GamePhase::Menu:
                        glfwSetWindowShouldClose(window, true);
                        break;
                    default: break; // Loading: ignore
                }
            }
            esc_prev = esc_now;
        }
        // Track framebuffer size; when it changes (e.g. fullscreen toggle), the
        // HDR/bloom render targets must be resized to match or the scene only
        // fills a corner of the screen.
        int prev_w = g_screen_w, prev_h = g_screen_h;
        glfwGetFramebufferSize(window, &g_screen_w, &g_screen_h);
        if (g_screen_w != prev_w || g_screen_h != prev_h) {
            renderer.on_resize(g_screen_w, g_screen_h);
        }
        g_camera.update(window, dt);

        // --- Terrain sculpting (paint while LMB held in sculpt mode) ---
        // Only paint when there's money; charge per modified vertex this frame.
        if (g_sculpt_mode && g_mouse_held && world.money[0] > 0 &&
            g_game_state.phase == GamePhase::Playing) {
            double cmx, cmy; glfwGetCursorPos(window, &cmx, &cmy);
            Ray sray = g_camera.screen_to_ray((float)cmx, (float)cmy, g_screen_w, g_screen_h);
            glm::vec2 gp = g_camera.ray_to_ground(sray);
            // Plan B: every brush drives the SDF (the heightmap is no longer
            // drawn). 0=Raise soil, 1=Dig bowl, 2=Smooth, 3=Cave (deep dig).
            float gy = renderer.terrain.get_height_at(gp.x, gp.y);
            float cr = g_sculpt_radius * 0.55f;
            // Strength scales how far each stamp pushes (depth/height of the
            // sphere offset), so a low strength paints gentle, shallow changes.
            float str = g_sculpt_strength;
            int changed = (int)(cr * cr * 0.05f * str);
            switch (g_sculpt_brush) {
                case 0: { // Raise soil: add material as a dome sitting on the surface
                    glm::vec3 ctr(gp.x, gy + cr * 0.45f * str, gp.y);
                    if (g_net_active) {
                        g_net.queue_local_command(g_sim_tick, CmdKind::Carve, {ctr.x, ctr.z},
                                                  0, 1, f2u(ctr.y), f2u(cr)); // unit_end=1: fill
                    } else {
                        renderer.carve_terrain(ctr, cr, false);
                    }
                    break;
                }
                case 1: { // Dig: shallow rounded bowl
                    glm::vec3 ctr(gp.x, gy - cr * 0.35f * str, gp.y);
                    if (g_net_active) {
                        g_net.queue_local_command(g_sim_tick, CmdKind::Carve, {ctr.x, ctr.z},
                                                  0, 0, f2u(ctr.y), f2u(cr)); // unit_end=0: dig
                    } else {
                        renderer.carve_terrain(ctr, cr, true);
                    }
                    break;
                }
                case 2: // Smooth: gentle fill that rounds off sharp features
                    renderer.smooth_terrain(glm::vec3(gp.x, gy, gp.y), cr);
                    changed /= 4;
                    break;
                case 3: { // Cave/Tunnel: a TBM that keeps boring along the look
                          // direction, going deeper every frame while LMB held.
                    float tr = cr * 0.85f;
                    if (!g_tunnel_active) {
                        // Start the bore at the surface point under the cursor.
                        g_tunnel_last = glm::vec3(gp.x, gy, gp.y);
                        g_tunnel_active = true;
                    }
                    // Advance the cutting head along the cursor ray; strength
                    // controls bore speed so a low strength digs slowly.
                    float step = cr * (0.5f + str * 1.6f);
                    glm::vec3 tip = g_tunnel_last + sray.direction * step;
                    renderer.carve_tunnel(g_tunnel_last, tip, tr);
                    g_tunnel_last = tip;
                    changed = (int)(tr * tr * 0.08f);
                    break;
                }
            }
            int cost = changed;
            world.money[0] = std::max(0, world.money[0] - cost);
        }

        // === Economy === (only while actually playing — during Loading the
        // worker thread owns the world arrays, so we must not touch them here)
        if (g_game_state.phase == GamePhase::Playing) {
        world.tick_economy(dt);
        // Survival run grants the roguelite income bonus on top of base income.
        if (g_game_state.mode == GameMode::Survival && g_wave_director.m_income_bonus > 0.0f) {
            static float surv_income_accum = 0.0f;
            surv_income_accum += g_wave_director.m_income_bonus * dt;
            if (surv_income_accum >= 1.0f) { int add=(int)surv_income_accum; world.money[0]+=add; surv_income_accum-=add; }
        }

        // Enemy auto-buy (every 8 seconds, buys mixed troops). Skirmish only —
        // in Survival the WaveDirector owns all enemy spawning.
        enemy_buy_timer += dt;
        if (g_game_state.mode == GameMode::Skirmish &&
            enemy_buy_timer > 8.0f && combat->faction_alive[1] < 20000) {
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
        } // end Economy/auto-buy (Playing-only guard)


        // === Combat AI ===
        bool sim_active = (g_game_state.phase == GamePhase::Playing);
        if (sim_active) {

        // === AI Commanders (settlement / production) ===
        // March -> Settle (plant HQ) -> Develop (build barracks) -> Produce.
        // Runs before the combat grid rebuild so new structures/units are
        // included this frame. Player move commands are never overridden.
        g_settlements.update(world, dt);

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

        // === Terrain destruction: drain explosion-queued craters ===
        // Cap per frame so a big barrage cannot stall the frame on remeshing.
        if (!world.carve_requests.empty()) {
            const int MAX_CARVES_PER_FRAME = 8;
            int n = (int)world.carve_requests.size();
            int start = (n > MAX_CARVES_PER_FRAME) ? n - MAX_CARVES_PER_FRAME : 0;
            for (int ci = start; ci < n; ci++) {
                const auto& cr = world.carve_requests[ci];
                renderer.carve_terrain(cr.center, cr.radius, cr.dig);
                // Terrain-destruction <-> water coupling: re-sample the bed under
                // the crater so water reacts next tick (craters fill, walls breach
                // and flood), and inject a splash from the blast.
                if (g_dynamic_water) {
                    renderer.fluid.refresh_bed_region(cr.center.x, cr.center.z, cr.radius);
                    if (cr.dig) renderer.fluid.add_splash(cr.center.x, cr.center.z, 2.5f);
                }
            }
            world.carve_requests.clear();
        }
        renderer.flush_terrain_gpu(); // one batched GPU heightmap re-upload

        // === Death decals: turn queued deaths into corpse + blood ground decals
        if (!world.death_events.empty()) {
            for (const auto& d : world.death_events) {
                float gy = renderer.terrain.get_height_at(d.pos.x, d.pos.y);
                renderer.corpses.spawn(d.pos, d.color, d.rotation, d.type, gy);
            }
            world.death_events.clear();
        }

        // === Territory Control ===
        if (g_game_state.phase == GamePhase::Playing && g_game_state.mode == GameMode::Skirmish) {
            static std::vector<uint8_t> territory_alive;
            territory_alive.resize(world.entity_count);
            for (uint32_t i=0; i<world.entity_count; i++) territory_alive[i] = world.is_alive(i) ? 1 : 0;
            g_game_state.update_territory(world.transforms.position, (const uint8_t*)world.units.faction, territory_alive.data(), world.entity_count, dt);
            g_game_state.update(dt);
            if (g_victory_enabled && g_game_state.check_victory()) g_game_state.phase = g_game_state.get_winner();
        }

        // === Survival / Roguelite wave director ===
        if (g_game_state.phase == GamePhase::Playing &&
            g_game_state.mode == GameMode::Survival && g_wave_director.run_active) {
            WaveDirector& wd = g_wave_director;
            // Push roguelite multipliers into the ECS so purchases pick them up.
            world.rl_player_hp_mult = wd.m_unit_hp;
            world.rl_player_dmg_mult = wd.m_unit_damage;
            world.rl_player_cost_mult = wd.m_unit_cost;
            world.rl_kill_reward_mult = wd.m_kill_bounty;
            // Base destroyed -> run over (survival always enforces this).
            if (!renderer.bases.bases[0].alive) {
                wd.run_active = false;
                g_game_state.phase = GamePhase::Defeat;
                // Bank the run result once (unlocks, best wave/tier, meta points).
                if (!g_meta_recorded) {
                    // Snapshot unlock state to detect what (if anything) this run opened.
                    int prev_tier = g_meta.unlocked_tier;
                    bool prev_rich = g_meta.unlock_richstart;
                    bool prev_vets = g_meta.unlock_veterans;
                    bool prev_extra = g_meta.unlock_extracard;
                    // wd.wave is the wave we died on (>=1 once a wave started).
                    int earned = g_meta.record_run(wd.wave, wd.difficulty_tier, world.score[0]);
                    g_meta_recorded = true;

                    // Fill the results-screen summary.
                    GameState& gs = g_game_state;
                    gs.last_run_wave = wd.wave;
                    gs.last_run_tier = wd.difficulty_tier;
                    gs.last_run_kills = world.score[0];
                    gs.last_run_points = earned;
                    gs.last_run_new_unlock = false;
                    gs.last_run_unlock_text[0] = '\0';
                    if (g_meta.unlocked_tier > prev_tier) {
                        gs.last_run_new_unlock = true;
                        snprintf(gs.last_run_unlock_text, sizeof(gs.last_run_unlock_text),
                                 "DIFFICULTY TIER %d", g_meta.unlocked_tier);
                    } else if (g_meta.unlock_richstart && !prev_rich) {
                        gs.last_run_new_unlock = true;
                        snprintf(gs.last_run_unlock_text, sizeof(gs.last_run_unlock_text),
                                 "RICH START (+400 METAL)");
                    } else if (g_meta.unlock_veterans && !prev_vets) {
                        gs.last_run_new_unlock = true;
                        snprintf(gs.last_run_unlock_text, sizeof(gs.last_run_unlock_text),
                                 "VETERAN GARRISON");
                    } else if (g_meta.unlock_extracard && !prev_extra) {
                        gs.last_run_new_unlock = true;
                        snprintf(gs.last_run_unlock_text, sizeof(gs.last_run_unlock_text),
                                 "EXTRA DRAFT CARD");
                    }
                }
            } else if (wd.phase == SurvivalPhase::Prep) {
                if (wd.tick_prep(dt)) wd.begin_wave(world);
            } else if (wd.phase == SurvivalPhase::Combat) {
                wd.sync_nests(world);
                wd.tick_combat(world, dt, combat->faction_alive[1]);
            }
            // (Draft phase is handled by the UI/input block below.)
            // Mirror state into GameState for the HUD.
            int nlive = 0; for (auto& n : wd.nests) if (n.alive) nlive++;
            g_game_state.hud_wave = wd.wave;
            g_game_state.hud_phase = (int)wd.phase;
            g_game_state.hud_prep_timer = wd.prep_timer;
            g_game_state.hud_enemies_left = wd.enemies_remaining;
            g_game_state.hud_nests_alive = nlive;
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
                // Crater carving is handled by apply_explosion -> carve_requests
                // (real 3D SDF crater), drained earlier this frame. No heightmap
                // sculpt here (heightmap is no longer rendered under Plan B).
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
        if (g_dynamic_water) renderer.fluid.update(dt); // deterministic fixed-step shallow-water solve

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
        hud.sculpt_strength = g_sculpt_strength;

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
                           g_game_state.phase == GamePhase::Paused ||
                           g_game_state.phase == GamePhase::Victory ||
                           g_game_state.phase == GamePhase::Defeat);
        if (draw_world) { renderer.render(world, view, proj, g_camera.get_position()); }
        else {
            // Menu / MapSelect / Settings / Loading: plain dark background.
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glViewport(0, 0, g_screen_w, g_screen_h);
            glClearColor(0.02f, 0.03f, 0.08f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        }

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

        // Sculpt brush cursor: a ground ring under the mouse showing footprint.
        if (g_sculpt_mode && draw_world && g_game_state.phase == GamePhase::Playing) {
            double bmx, bmy; glfwGetCursorPos(window, &bmx, &bmy);
            Ray bray = g_camera.screen_to_ray((float)bmx, (float)bmy, g_screen_w, g_screen_h);
            glm::vec2 bgp = g_camera.ray_to_ground(bray);
            glm::vec3 bcol =
                g_sculpt_brush==0 ? glm::vec3(0.3f,1.0f,0.3f) :
                g_sculpt_brush==1 ? glm::vec3(1.0f,0.3f,0.2f) :
                g_sculpt_brush==2 ? glm::vec3(0.3f,0.8f,1.0f) :
                                    glm::vec3(1.0f,0.7f,0.2f);
            glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            renderer.render_brush_ring(view, proj, bgp, g_sculpt_radius * 0.55f, bcol);
            glDisable(GL_BLEND);
        }

        // Survival build-placement preview: a green footprint ring at the cursor
        // showing where the armed wall/turret will be built.
        if (g_game_state.phase == GamePhase::Playing &&
            g_game_state.mode == GameMode::Survival && g_build_place_shop >= 0 &&
            !g_sculpt_mode) {
            double qmx, qmy; glfwGetCursorPos(window, &qmx, &qmy);
            Ray qray = g_camera.screen_to_ray((float)qmx, (float)qmy, g_screen_w, g_screen_h);
            glm::vec2 qgp = g_camera.ray_to_ground(qray);
            bool affordable = (world.money[0] >= world.unit_cost_for(g_build_place_shop));
            glm::vec3 col = affordable ? glm::vec3(0.3f, 1.0f, 0.4f) : glm::vec3(1.0f, 0.3f, 0.2f);
            glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            renderer.render_brush_ring(view, proj, qgp, 14.0f, col);
            glDisable(GL_BLEND);
        }

        // Survival PREP preview: pulse warning rings where the NEXT wave's nests
        // will erupt, so the player can place defenses / sculpt terrain there.
        if (g_game_state.phase == GamePhase::Playing &&
            g_game_state.mode == GameMode::Survival &&
            g_wave_director.run_active &&
            g_wave_director.phase == SurvivalPhase::Prep) {
            static std::vector<glm::vec2> preview_nests;
            g_wave_director.compute_nest_positions(g_wave_director.wave + 1, preview_nests);
            float pulse = 0.55f + 0.45f * (float)sin(glfwGetTime() * 3.0);
            glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glm::vec3 warn(1.0f, 0.2f * pulse, 0.5f * pulse);
            for (auto& np : preview_nests) {
                renderer.render_brush_ring(view, proj, np, 70.0f + 30.0f * pulse, warn);
                renderer.render_brush_ring(view, proj, np, 130.0f, glm::vec3(0.8f, 0.1f, 0.3f));
            }
            glDisable(GL_BLEND);
        }

        // HUD — only during gameplay (Playing/Victory/Defeat). In Menu/MapSelect/
        // Settings/Loading the HUD top-bar + score panels would overlap the menu
        // and look like a "second menu".
        if (g_game_state.phase == GamePhase::Playing ||
            g_game_state.phase == GamePhase::Victory ||
            g_game_state.phase == GamePhase::Defeat) {
            hud.survival_mode = (g_game_state.mode == GameMode::Survival);
            hud.render();
        }

        // Territory bar + Shop Panel
        if (g_game_state.phase == GamePhase::Playing) {
            g_menu.screen_w = g_screen_w; g_menu.screen_h = g_screen_h;
            if (g_game_state.mode == GameMode::Skirmish)
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
                // Survival: walls (7) and turrets (8) are placed at the cursor so
                // players can build a fortress around the incoming nest lanes.
                bool is_defense = (shop_result == 7 || shop_result == 8);
                if (g_game_state.mode == GameMode::Survival && is_defense) {
                    g_build_place_shop = shop_result; // arm placement; next click builds
                } else {
                    glm::vec2 spawn = g_renderer->bases.bases[0].position;
                    int bought = world.buy_batch(shop_result, g_buy_count, spawn, Faction::Red);
                    if (bought > 0) { }
                }
            }
        }

        // === Survival build placement: RMB cancels the armed defense. The
        // actual build happens in the mouse callback so it consumes the click
        // (the left-click never leaks into a selection box). ===
        if (g_game_state.phase == GamePhase::Playing &&
            g_game_state.mode == GameMode::Survival && g_build_place_shop >= 0) {
            if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS)
                g_build_place_shop = -1;
        }

        // === Survival HUD banner + roguelite draft overlay ===
        if (g_game_state.phase == GamePhase::Playing &&
            g_game_state.mode == GameMode::Survival && g_wave_director.run_active) {
            g_menu.screen_w = g_screen_w; g_menu.screen_h = g_screen_h;
            WaveDirector& wd = g_wave_director;
            g_menu.render_survival_banner(g_game_state.hud_wave, g_game_state.hud_phase,
                                          g_game_state.hud_prep_timer,
                                          g_game_state.hud_enemies_left,
                                          g_game_state.hud_nests_alive,
                                          wd.difficulty_tier);
            // Build-placement hint when a wall/turret is armed.
            if (g_build_place_shop >= 0) {
                g_menu.begin_2d();
                const char* what = (g_build_place_shop == 8) ? "TURRET" : "WALL";
                char hint[64];
                snprintf(hint, sizeof(hint), "PLACING %s - CLICK GROUND  (ESC/RMB CANCEL)", what);
                g_menu.draw_text_centered(hint, g_screen_w * 0.5f, g_screen_h - 70.0f, 1.7f,
                                          {0.4f, 1.0f, 0.5f, 1.0f});
                g_menu.end_2d();
            }
            // SPACE skips the prep window.
            if (wd.phase == SurvivalPhase::Prep) {
                static bool space_prev = false;
                bool space_now = (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS);
                if (space_now && !space_prev) wd.prep_skip = true;
                space_prev = space_now;
            }
            // Draft: 3-choose-1 upgrade cards.
            if (wd.phase == SurvivalPhase::Draft && wd.draft_ready) {
                double dmx, dmy; glfwGetCursorPos(window, &dmx, &dmy);
                const char* names[3]; const char* descs[3];
                for (int i = 0; i < 3; i++) {
                    names[i] = wd.draft_offer[i].name.c_str();
                    descs[i] = wd.draft_offer[i].desc.c_str();
                }
                bool dclick = g_mouse_clicked_this_frame;
                int pick = g_menu.render_draft((float)dmx, (float)dmy, dclick, names, descs);
                if (pick >= 0) {
                    audio.play_click();
                    // BaseRepair card heals the base (director can't see BaseSystem).
                    if (wd.draft_offer[pick].kind == ModKind::BaseRepair) {
                        auto& b = renderer.bases.bases[0];
                        b.health = std::min(b.max_health, b.health + wd.draft_offer[pick].value);
                        b.alive = true;
                    }
                    wd.choose_card(world, pick);
                }
            }
        }


        // Minimap — skip during Loading: the worker thread is mutating the
        // world arrays (spawn_army), so reading them here would race/crash.
        if (g_game_state.phase != GamePhase::Loading && g_game_state.phase != GamePhase::Menu) {
            static std::vector<char> alive_flags;
            alive_flags.resize(world.entity_count);
            for (uint32_t i=0; i<world.entity_count; i++) alive_flags[i] = world.is_visible(i)?1:0;
            hud.render_minimap_dots(world.transforms.position, world.renders.color,
                                    alive_flags.data(), world.entity_count, 1200.0f);
        }


        // === Menu / Map Select ===
        if (g_game_state.phase == GamePhase::Menu) {
            double mmx, mmy; glfwGetCursorPos(window, &mmx, &mmy);
            g_menu.screen_w = g_screen_w; g_menu.screen_h = g_screen_h;
            g_menu.render_main_menu(g_game_state, (float)mmx, (float)mmy);

            // Detect a fresh left-click edge (shared by menu + survival overlay).
            static bool menu_was_pressed = false;
            bool lmb = (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS);
            bool click_edge = lmb && !menu_was_pressed;

            if (g_game_state.survival_setup_open) {
                // --- Survival tier/seed setup overlay (drawn over the menu) ---
                int act = g_menu.render_survival_setup(
                    g_game_state, (float)mmx, (float)mmy, click_edge,
                    g_meta.unlocked_tier, g_meta.best_wave, g_meta.best_tier,
                    g_meta.runs_played, g_meta.meta_points);
                // DEBUG: auto-press START after a few frames to reproduce the
                // menu-driven survival entry without a real mouse.
                if (g_survmenu_dbg) { static int sf=0; if(++sf==30) act=1; }
                if (act == 3) { // tier -
                    audio.play_click();
                    g_game_state.survival_tier = std::max(1, g_game_state.survival_tier - 1);
                } else if (act == 4) { // tier +
                    audio.play_click();
                    g_game_state.survival_tier = std::min(g_meta.unlocked_tier, g_game_state.survival_tier + 1);
                } else if (act == 5) { // reroll seed
                    audio.play_click();
                    g_game_state.survival_seed = (uint32_t)(glfwGetTime() * 1000.0) ^ 0x9E3779B9u;
                } else if (act == 1) { // START
                    audio.play_click();
                    g_game_state.survival_setup_open = false;
                    g_game_state.mode = GameMode::Survival;
                    start_battle();
                } else if (act == 2) { // BACK
                    audio.play_click();
                    g_game_state.survival_setup_open = false;
                }
            } else if (click_edge) {
                if (g_game_state.menu_hover == 0) {
                    audio.play_click();
                    g_game_state.mode = GameMode::Skirmish;
                    start_battle(); // start with currently-selected map
                } else if (g_game_state.menu_hover == 3) {
                    audio.play_click();
                    g_game_state.survival_tier = std::min(g_game_state.survival_tier, g_meta.unlocked_tier);
                    g_game_state.survival_setup_open = true; // open tier/seed picker
                } else if (g_game_state.menu_hover == 1) {
                    audio.play_click();
                    g_game_state.phase = GamePhase::MapSelect;
                } else if (g_game_state.menu_hover == 2) {
                    audio.play_click();
                    settings_return = GamePhase::Menu; // came from the title screen
                    g_game_state.phase = GamePhase::Settings;
                }
            }
            menu_was_pressed = lmb;
        } else if (g_game_state.phase == GamePhase::MapSelect) {
            double mmx, mmy; glfwGetCursorPos(window, &mmx, &mmy);
            g_menu.screen_w = g_screen_w; g_menu.screen_h = g_screen_h;
            g_menu.render_map_select(g_game_state, (float)mmx, (float)mmy);
            static bool map_was_pressed = false;
            if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS) {
                if (!map_was_pressed) {
                    if (g_game_state.menu_hover >= 0 && g_game_state.menu_hover < MAP_COUNT) {
                        // Pick a map AND start the battle directly (async loading).
                        audio.play_click();
                        g_game_state.selected_map = g_game_state.menu_hover;
                        start_battle();
                    }
                    else if (g_game_state.menu_hover == 100) {
                        // START button: launch the currently-selected map.
                        audio.play_click();
                        start_battle();
                    }
                    else if (g_game_state.menu_hover == 101) {
                        audio.play_click();
                        audio.play_music(0); // back to menu track
                        g_game_state.phase = GamePhase::Menu; // back to main menu
                    }
                }
                map_was_pressed = true;
            } else { map_was_pressed = false; }
        } else if (g_game_state.phase == GamePhase::Settings) {
            // Countdown timer for settings confirmation
            if (g_game_state.settings_confirming) {
                g_game_state.settings_confirm_timer -= dt;
                if (g_game_state.settings_confirm_timer <= 0) {
                    // Timeout: revert settings
                    g_game_state.restore_settings();
                    apply_fullscreen(window, g_game_state);
                    glfwSwapInterval(g_game_state.settings_vsync ? 1 : 0);
                    renderer.postfx.enabled = g_game_state.settings_bloom;
                    renderer.postfx.exposure = g_game_state.settings_exposure;
                    g_game_state.settings_confirming = false;
                    g_game_state.settings_dirty = false;
                }
            }

            double smx, smy; glfwGetCursorPos(window, &smx, &smy);
            g_menu.screen_w = g_screen_w; g_menu.screen_h = g_screen_h;
            g_menu.render_settings(g_game_state, (float)smx, (float)smy);
            static bool set_was_pressed = false;
            static bool dragging_slider = false;
            static int drag_id = -1;
            if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS) {
                int hover = g_game_state.menu_hover;
                if (!set_was_pressed) {
                    // --- Navigation (category buttons + back) ---
                    if (hover == 101) g_game_state.settings_submenu = 1;       // Video
                    else if (hover == 102) g_game_state.settings_submenu = 2;  // Graphics
                    else if (hover == 103) g_game_state.settings_submenu = 3;  // Audio
                    else if (hover == 104) g_game_state.settings_submenu = 4;  // Controls
                    else if (hover == 998) g_game_state.settings_submenu = 0;  // back to settings main
                    else if (hover == 999) { // back to menu
                        if (g_game_state.settings_dirty && !g_game_state.settings_confirming) {
                            // Start confirmation countdown instead of instant exit
                            g_game_state.snapshot_settings();
                            g_game_state.settings_confirming = true;
                            g_game_state.settings_confirm_timer = 15.0f;
                        } else if (!g_game_state.settings_confirming) {
                            g_game_state.phase = settings_return; // Menu or Paused
                            g_game_state.settings_submenu = 0;
                        }
                    }
                    // APPLY button
                    else if (hover == 400 && g_game_state.settings_dirty) {
                        g_game_state.snapshot_settings();
                        g_game_state.settings_confirming = true;
                        g_game_state.settings_confirm_timer = 15.0f;
                    }
                    // Confirmation: YES (keep changes)
                    else if (hover == 401 && g_game_state.settings_confirming) {
                        g_game_state.settings_confirming = false;
                        g_game_state.settings_dirty = false; // changes accepted
                    }
                    // Confirmation: NO (revert)
                    else if (hover == 402 && g_game_state.settings_confirming) {
                        g_game_state.restore_settings();
                        apply_fullscreen(window, g_game_state);
                        glfwSwapInterval(g_game_state.settings_vsync ? 1 : 0);
                        renderer.postfx.enabled = g_game_state.settings_bloom;
                        renderer.postfx.exposure = g_game_state.settings_exposure;
                        g_game_state.settings_confirming = false;
                        g_game_state.settings_dirty = false;
                    }
                    // --- Toggles ---
                    else if (hover == 301) { // fullscreen
                        g_game_state.settings_fullscreen = !g_game_state.settings_fullscreen;
                        apply_fullscreen(window, g_game_state);
                        g_game_state.settings_dirty = true;
                    }
                    else if (hover == 302) { // vsync
                        g_game_state.settings_vsync = !g_game_state.settings_vsync;
                        glfwSwapInterval(g_game_state.settings_vsync ? 1 : 0);
                        g_game_state.settings_dirty = true;
                    }
                    else if (hover == 310) { // bloom
                        g_game_state.settings_bloom = !g_game_state.settings_bloom;
                        renderer.postfx.enabled = g_game_state.settings_bloom;
                        g_game_state.settings_dirty = true;
                    }
                    else if (hover == 331) { g_game_state.settings_edge_pan = !g_game_state.settings_edge_pan; g_game_state.settings_dirty = true; }
                    // --- Sliders: start dragging ---
                    else if (hover >= 300 && hover < 340) { dragging_slider = true; drag_id = hover; }
                }
                // Slider drag — slider track is at cx+30 .. cx+230 (sx=rx+180, rw=460, rx=cx-230)
                if (dragging_slider) {
                    float cx = g_screen_w * 0.5f;
                    float sx = cx - 230.0f + 180.0f, sw = 200.0f;
                    float frac = glm::clamp((float)(smx - sx) / sw, 0.0f, 1.0f);
                    switch (drag_id) {
                        case 311: g_game_state.settings_exposure = 0.3f + frac*1.7f; renderer.postfx.exposure = g_game_state.settings_exposure; break;
                        case 312: g_game_state.settings_fov = 50.0f + frac*50.0f; break;
                        case 313: g_game_state.settings_render_distance = 2000.0f + frac*6000.0f; break;
                        case 314: g_game_state.settings_gamma = 0.5f + frac*1.5f; break;
                        case 320: g_game_state.settings_master_volume = frac;
                                  audio.set_volumes(g_game_state.settings_master_volume, 0.6f, g_game_state.settings_sfx_volume); break;
                        case 321: g_game_state.settings_sfx_volume = frac;
                                  audio.set_volumes(g_game_state.settings_master_volume, 0.6f, g_game_state.settings_sfx_volume); break;
                        case 330: g_game_state.settings_camera_speed = 200.0f + frac*800.0f; break;
                        case 300: { // resolution: snap to preset list
                            static const int RES[][2] = {{1280,720},{1600,900},{1920,1080},{2560,1440},{3840,2160}};
                            int idx = glm::clamp((int)(frac * 5.0f), 0, 4);
                            g_game_state.settings_resolution_w = RES[idx][0];
                            g_game_state.settings_resolution_h = RES[idx][1];
                            break;
                        }
                    }
                }
                set_was_pressed = true;
            } else {
                if (dragging_slider) g_game_state.settings_dirty = true; // mark dirty on slider release
                set_was_pressed = false;
                dragging_slider = false;
                drag_id = -1;
            }
        }

        // === Loading screen (async world gen) ===
        if (g_game_state.phase == GamePhase::Loading) {
            g_menu.screen_w = g_screen_w; g_menu.screen_h = g_screen_h;
            g_menu.render_loading(g_loader.get_progress(), g_loader.get_stage());
            // When the background CPU work is done, run GL finalize on the main
            // thread (GL calls can't happen on the worker thread).
            if (g_loader.cpu_done() && !g_loader.finalized) {
                g_loader.finalized = true;
                finalize_battle();
            }
        }

        // === End screen / Menu overlay ===
        if (g_game_state.phase == GamePhase::Victory || g_game_state.phase == GamePhase::Defeat) {
            double emx, emy; glfwGetCursorPos(window, &emx, &emy);
            g_menu.screen_w = g_screen_w; g_menu.screen_h = g_screen_h;
            // 2D overlay must ignore depth so it always draws over the frozen
            // battlefield, and must blend so its alpha works.
            glDisable(GL_DEPTH_TEST);
            glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            if (g_game_state.mode == GameMode::Survival) {
                // Dedicated survival results screen with RETRY / MENU.
                GameState& gs = g_game_state;
                int act = g_menu.render_survival_results(
                    (float)emx, (float)emy, g_mouse_clicked_this_frame,
                    gs.last_run_wave, gs.last_run_tier, gs.last_run_kills,
                    gs.last_run_points, g_meta.best_wave, g_meta.unlocked_tier,
                    gs.last_run_new_unlock, gs.last_run_unlock_text);
                if (act == 1) { // RETRY: same tier, fresh seed
                    audio.play_click();
                    gs.survival_seed = (uint32_t)(glfwGetTime() * 1000.0) ^ 0x9E3779B9u;
                    gs.survival_tier = std::min(gs.survival_tier, g_meta.unlocked_tier);
                    start_battle();
                } else if (act == 2) { // MENU
                    audio.play_click();
                    gs.phase = GamePhase::Menu;
                    audio.play_music(0);
                }
            } else {
                g_menu.render_end_screen(g_game_state, g_game_state.phase == GamePhase::Victory, (float)emx, (float)emy);
                if (g_mouse_clicked_this_frame && g_game_state.menu_hover == 0) {
                    g_game_state.phase = GamePhase::Playing;
                }
            }
        }

        // === Pause overlay (ESC during a battle) ===
        // Drawn over the frozen battlefield. Buttons: 10=Resume, 11=Settings,
        // 12=Quit to Menu. Mouse is edge-detected so a single click acts once.
        if (g_game_state.phase == GamePhase::Paused) {
            double pmx, pmy; glfwGetCursorPos(window, &pmx, &pmy);
            g_menu.screen_w = g_screen_w; g_menu.screen_h = g_screen_h;
            glDisable(GL_DEPTH_TEST);
            glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            g_menu.render_pause_menu(g_game_state, (float)pmx, (float)pmy);
            static bool pause_was_pressed = false;
            if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS) {
                if (!pause_was_pressed) {
                    if (g_game_state.menu_hover == 10) {          // Resume
                        audio.play_click();
                        g_game_state.phase = GamePhase::Playing;
                    } else if (g_game_state.menu_hover == 11) {   // Settings
                        audio.play_click();
                        settings_return = GamePhase::Paused;      // come back to pause
                        g_game_state.phase = GamePhase::Settings;
                    } else if (g_game_state.menu_hover == 12) {   // Quit to Menu
                        audio.play_click();
                        audio.play_music(0);                      // menu track
                        g_game_state.phase = GamePhase::Menu;
                    }
                }
                pause_was_pressed = true;
            } else { pause_was_pressed = false; }
        }

        glDisable(GL_BLEND);
        glEnable(GL_DEPTH_TEST);

        // --- Auto-screenshot orbit ---
        if (g_shot_mode) {
            static int sframe = 0;
            static int shot_idx = 0;
            // angle presets: {yaw, pitch, distance, target_x, target_z}
            struct Shot { float yaw, pitch, dist, tx, tz; const char* name; };
            static const Shot shots[] = {
                {  0.0f, 35.0f, 1400.0f,    0,    0, "shot_0_front_far" },
                { 45.0f, 55.0f,  900.0f,    0,    0, "shot_1_high_45"   },
                { 90.0f, 12.0f,  700.0f, -800,    0, "shot_2_low_sidemtn" },
                {135.0f, 25.0f, 1000.0f,    0,  800, "shot_3_edge_mtn"  },
                {200.0f,  8.0f,  600.0f,  600,  600, "shot_4_grazing"   },
                { 30.0f, 80.0f, 1600.0f,    0,    0, "shot_5_topdown"   },
            };
            const int NSHOTS = (int)(sizeof(shots)/sizeof(shots[0]));
            // Only advance the shot sequence once the battle is actually live
            // (terrain + units uploaded). Counting during Loading captured empty
            // clear-color frames before finalize_battle ran.
            if (g_game_state.phase == GamePhase::Playing) sframe++;
            if (sframe > g_shot_warmup) {
                int local = (sframe - g_shot_warmup) % 12;
                if (local == 0 && shot_idx < NSHOTS) {
                    const Shot& s = shots[shot_idx];
                    // Camera yaw/pitch are stored in DEGREES (get_position does
                    // the radians() conversion), so assign the preset directly.
                    g_camera.yaw = s.yaw;
                    g_camera.pitch = s.pitch;
                    g_camera.distance = s.dist;
                    float gy = renderer.terrain.get_height_at(s.tx, s.tz);
                    g_camera.target = glm::vec3(s.tx, gy + 30.0f, s.tz);
                } else if (local == 8 && shot_idx < NSHOTS) {
                    char path[256];
                    std::snprintf(path, sizeof(path), "%s.png", shots[shot_idx].name);
                    save_screenshot(path, g_screen_w, g_screen_h);
                    shot_idx++;
                    if (shot_idx >= NSHOTS) { glfwSwapBuffers(window); break; }
                }
            }
        }

        // DEBUG: capture normal gameplay ~3s into Playing to inspect blue-screen.
        if (g_survmenu_dbg && g_game_state.phase == GamePhase::Playing) {
            static int dbgf = 0; dbgf++;
            if (dbgf == 180) save_screenshot("dbg_play.png", g_screen_w, g_screen_h);
        }
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
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           
