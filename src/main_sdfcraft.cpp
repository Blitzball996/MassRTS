// =============================================================================
// SDFCraft - standalone entry point (Phase A skeleton + Phase B core loop)
// -----------------------------------------------------------------------------
// A separate executable so the survival/build mode never interferes with the
// RTS build. Maps GLFW input to sdfcraft::FrameInput and runs the mode loop.
// Controls: WASD move, mouse look, Space jump, Shift down, F fly toggle,
//   LMB dig, RMB place, 1-9 hotbar, wheel cycle hotbar, Esc quit.
// =============================================================================
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <chrono>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include "sdfcraft/mode.h"

static sdfcraft::Mode g_mode;

// =============================================================================
// Dedicated server: headless authoritative loop (declared in game_server.h).
// No GL, no window, no local player. Pre-generates a spawn region so newly
// joined clients see populated terrain + mobs immediately, then ticks the sim
// at a fixed 20 Hz until killed (Ctrl-C). Same ServerSim brain as the host.
// =============================================================================
namespace sdfcraft {
int runDedicatedServer(uint64_t seed, uint16_t port) {
    NetInit net_guard;                       // WSAStartup on Windows (no-op POSIX)
    if (!net_guard.ok()) { std::cerr << "[sdfcraft] WSAStartup failed\n"; return 1; }
    World world(seed);
    GameServer server(world, seed, port, /*local host player*/ false);
    if (!server.ok()) {
        std::cerr << "[sdfcraft] dedicated server failed to bind port " << port << "\n";
        return 1;
    }
    // Pre-generate a spawn-area disk so mobs have ground to spawn on and the
    // first joiner streams real terrain rather than empty air.
    ChunkKey c0 = World::world_to_chunk(0, 0);
    for (int dz = -6; dz <= 6; dz++)
    for (int dx = -6; dx <= 6; dx++)
        if (dx*dx + dz*dz <= 36) world.get_chunk({c0.cx + dx, c0.cz + dz}, true);

    std::cerr << "[sdfcraft] dedicated server listening on port " << port
              << " (seed " << seed << "), 20Hz. Ctrl-C to stop.\n";
    const double TICK = 1.0 / 20.0;
    auto next = std::chrono::steady_clock::now();
    uint64_t ticks = 0;
    for (;;) {
        server.update((float)TICK);
        if (++ticks % 200 == 0)   // ~every 10s
            std::cerr << "[sdfcraft] players=" << server.sim().players().size()
                      << " mobs=" << server.sim().mobs.entities.size()
                      << " day=" << server.sim().day << "\n";
        next += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(TICK));
        std::this_thread::sleep_until(next);
    }
    return 0;
}
} // namespace sdfcraft

// Capture the current framebuffer to a PNG (flips rows; GL origin is bottom-left).
static void save_screenshot(const std::string& path, int w, int h) {
    std::vector<unsigned char> px((size_t)w * h * 3);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px.data());
    std::vector<unsigned char> flip((size_t)w * h * 3);
    for (int y = 0; y < h; y++)
        memcpy(&flip[(size_t)(h-1-y)*w*3], &px[(size_t)y*w*3], (size_t)w*3);
    stbi_write_png(path.c_str(), w, h, 3, flip.data(), w * 3);
    std::cerr << "wrote " << path << "\n";
}

static double g_last_x = 0, g_last_y = 0;
static bool   g_first_mouse = true;
static float  g_look_dx = 0, g_look_dy = 0;
static int    g_wheel = 0;
static bool   g_place_edge = false;
static bool   g_attack_edge = false;   // LMB edge: melee swing at a mob
static bool   g_eat_edge = false;      // Q: eat held food (moved off E)
static bool   g_inv_edge = false;      // E: toggle inventory screen
static bool   g_craft_edge = false;    // R: toggle crafting recipe list (Console Edition)
static bool   g_lclick_edge = false;   // raw left-click edge (GUI when inv open)
static bool   g_rclick_edge = false;   // raw right-click edge (GUI when inv open)
static bool   g_fly_edge = false;
static bool   g_takeoff_edge = false;   // V: enter fly mode
static bool   g_land_edge = false;      // C: enter walk mode
static bool   g_planet_edge = false;
static const float MOUSE_SENS = 0.12f;

static void cursor_cb(GLFWwindow*, double x, double y) {
    if (g_first_mouse) { g_last_x = x; g_last_y = y; g_first_mouse = false; return; }
    g_look_dx += (float)(x - g_last_x) * MOUSE_SENS;
    g_look_dy += (float)(y - g_last_y) * MOUSE_SENS;
    g_last_x = x; g_last_y = y;
}
static void scroll_cb(GLFWwindow*, double, double dy) {
    g_wheel += (dy > 0 ? 1 : (dy < 0 ? -1 : 0));
}
static void mouse_btn_cb(GLFWwindow*, int button, int action, int) {
    if (button == GLFW_MOUSE_BUTTON_RIGHT && action == GLFW_PRESS) { g_place_edge = true; g_rclick_edge = true; }
    if (button == GLFW_MOUSE_BUTTON_LEFT  && action == GLFW_PRESS) { g_attack_edge = true; g_lclick_edge = true; }
}
static void key_cb(GLFWwindow* w, int key, int, int action, int) {
    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) glfwSetWindowShouldClose(w, 1);
    if (key == GLFW_KEY_F && action == GLFW_PRESS) g_fly_edge = true;
    if (key == GLFW_KEY_V && action == GLFW_PRESS) g_takeoff_edge = true;  // take off (V key)
    if (key == GLFW_KEY_C && action == GLFW_PRESS) g_land_edge = true;
    if (key == GLFW_KEY_G && action == GLFW_PRESS) g_planet_edge = true;
    if (key == GLFW_KEY_Q && action == GLFW_PRESS) g_eat_edge = true;   // eat (moved off E)
    if (key == GLFW_KEY_E && action == GLFW_PRESS) g_inv_edge = true;   // inventory (E key)
    if (key == GLFW_KEY_R && action == GLFW_PRESS) g_craft_edge = true; // crafting list (R key, Console Edition)
}

int main(int argc, char** argv) {
    uint64_t seed = 1337;
    // --- offscreen test harness flags ---
    std::string shot_path; int shot_frames = 120;
    float tod_override = -1.0f;   // --tod <0..1>: pin time-of-day (QA / screenshots)
    bool want_fly = false, want_planet = false, want_dig = false;
    int  qa_mob = -1;   // QA closeup: MobKind index to spawn directly in front
    bool qa_mob_side = false, qa_mob_walk = false, qa_openinv = false, qa_opencraft = false;
    float px=0, py=1e9f, pz=0, yaw=0, pitch=-20;
    // --- multiplayer flags ---
    enum class Net { Solo, Host, Client, Server } net_mode = Net::Solo;
    std::string connect_ip = "127.0.0.1";
    uint16_t port = sdfcraft::SDFCRAFT_DEFAULT_PORT;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--shot" && i+1 < argc) shot_path = argv[++i];
        else if (a == "--frames" && i+1 < argc) shot_frames = atoi(argv[++i]);
        else if (a == "--tod" && i+1 < argc) tod_override = (float)atof(argv[++i]);
        else if (a == "--fly") want_fly = true;
        else if (a == "--planet") want_planet = true;
        else if (a == "--dig") want_dig = true;
        else if (a == "--mob" && i+1 < argc) qa_mob = atoi(argv[++i]);   // QA: spawn one mob ahead
        else if (a == "--mobside") qa_mob_side = true;   // QA: show mob's side profile
        else if (a == "--mobwalk") qa_mob_walk = true;   // QA: animate walk
        else if (a == "--openinv") qa_openinv = true;     // QA: open inventory screen
        else if (a == "--opencraft") qa_opencraft = true; // QA: open crafting recipe list
        else if (a == "--pos" && i+3 < argc) { px=atof(argv[++i]); py=atof(argv[++i]); pz=atof(argv[++i]); }
        else if (a == "--look" && i+2 < argc) { yaw=atof(argv[++i]); pitch=atof(argv[++i]); }
        else if (a == "--host")  net_mode = Net::Host;
        else if (a == "--server") net_mode = Net::Server;   // dedicated, headless
        else if (a == "--connect") { net_mode = Net::Client; if (i+1 < argc && argv[i+1][0] != '-') connect_ip = argv[++i]; }
        else if (a == "--port" && i+1 < argc) port = (uint16_t)atoi(argv[++i]);
        else if (a[0] != '-') seed = (uint64_t)strtoull(argv[i], nullptr, 10);
    }
    bool headless = !shot_path.empty();

    // --- dedicated server: no GL, no window, just the authoritative loop ---
    if (net_mode == Net::Server)
        return sdfcraft::runDedicatedServer(seed, port);

    if (!glfwInit()) { std::cerr << "glfwInit failed\n"; return 1; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    if (headless) glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    const char* title = net_mode == Net::Host ? "SDFCraft [HOST]"
                      : net_mode == Net::Client ? "SDFCraft [CLIENT]" : "SDFCraft";
    GLFWwindow* win = glfwCreateWindow(1600, 900, title, nullptr, nullptr);
    if (!win) { std::cerr << "window failed\n"; glfwTerminate(); return 1; }
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) { std::cerr << "glad failed\n"; return 1; }

    glfwSetInputMode(win, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    glfwSetCursorPosCallback(win, cursor_cb);
    glfwSetScrollCallback(win, scroll_cb);
    glfwSetMouseButtonCallback(win, mouse_btn_cb);
    glfwSetKeyCallback(win, key_cb);

    // shaders live next to the executable's working dir under shaders/
    std::string shader_dir = "shaders/";
    bool init_ok = false;
    switch (net_mode) {
        case Net::Host:   init_ok = g_mode.initHost(seed, shader_dir, port); break;
        case Net::Client: init_ok = g_mode.initClient(connect_ip, port, shader_dir); break;
        default:          init_ok = g_mode.init(seed, shader_dir); break;
    }
    if (!init_ok) {
        std::cerr << "sdfcraft init failed (shaders missing? server unreachable? run from project root)\n";
        return 1;
    }
    if (net_mode == Net::Host)
        std::cerr << "[sdfcraft] hosting on port " << port << " — others connect with --connect <your-ip>\n";
    else if (net_mode == Net::Client)
        std::cerr << "[sdfcraft] connecting to " << connect_ip << ":" << port << "\n";

    // --- harness: position the player and set view before the loop ---
    if (want_fly) g_mode.player.flying = true;
    if (py < 1e8f) g_mode.player.pos = glm::vec3(px, py, pz);
    g_mode.player.yaw = yaw; g_mode.player.pitch = pitch;
    if (tod_override >= 0.0f)
        if (sdfcraft::ServerSim* s = g_mode.sim()) s->time_of_day = tod_override;
    if (want_planet) { /* toggled below once mode is ready */ }

    double prev = glfwGetTime();
    int frame = 0;
    bool planet_done = false;
    while (!glfwWindowShouldClose(win)) {
        glfwPollEvents();
        double now = glfwGetTime();
        float dt = (float)(now - prev); prev = now;
        if (dt > 0.1f) dt = 0.1f;

        sdfcraft::FrameInput in;
        bool gui = g_mode.invOpen();   // inventory screen open this frame?
        in.move_z = gui ? 0.0f : (glfwGetKey(win, GLFW_KEY_W)==GLFW_PRESS ? 1.0f : 0.0f) - (glfwGetKey(win, GLFW_KEY_S)==GLFW_PRESS ? 1.0f : 0.0f);
        in.move_x = gui ? 0.0f : (glfwGetKey(win, GLFW_KEY_D)==GLFW_PRESS ? 1.0f : 0.0f) - (glfwGetKey(win, GLFW_KEY_A)==GLFW_PRESS ? 1.0f : 0.0f);
        in.jump   = !gui && glfwGetKey(win, GLFW_KEY_SPACE)==GLFW_PRESS;
        in.crouch = !gui && glfwGetKey(win, GLFW_KEY_LEFT_SHIFT)==GLFW_PRESS;
        in.dig    = !gui && glfwGetMouseButton(win, GLFW_MOUSE_BUTTON_LEFT)==GLFW_PRESS;
        in.inv_toggle = g_inv_edge;     g_inv_edge = false;
        in.craft_toggle = g_craft_edge; g_craft_edge = false;
        // mouse clicks: while GUI is open they drive the inventory; otherwise they
        // dig/place/attack the world.
        if (gui) {
            in.mouse_click = g_lclick_edge;
            in.mouse_right = g_rclick_edge;
            in.place = in.attack = false;
            double cxp, cyp; glfwGetCursorPos(win, &cxp, &cyp);
            in.mouse_x = (float)cxp; in.mouse_y = (float)cyp;
        } else {
            in.place  = g_place_edge;
            in.attack = g_attack_edge;
        }
        g_place_edge = g_attack_edge = g_lclick_edge = g_rclick_edge = false;
        in.eat    = g_eat_edge;         g_eat_edge = false;
        in.toggle_fly = !gui && g_fly_edge;     g_fly_edge = false;
        in.request_fly  = g_takeoff_edge; g_takeoff_edge = false;
        in.request_walk = g_land_edge;    g_land_edge = false;
        in.toggle_planet = g_planet_edge; g_planet_edge = false;
        in.fly_boost  = !gui && glfwGetKey(win, GLFW_KEY_LEFT_CONTROL)==GLFW_PRESS;
        in.look_dx = gui ? 0.0f : g_look_dx;  g_look_dx = 0;
        in.look_dy = gui ? 0.0f : g_look_dy;  g_look_dy = 0;
        in.hotbar_scroll = g_wheel; g_wheel = 0;   // GUI uses it to scroll lists; playing uses it for hotbar
        for (int k = 0; k < 9; k++)
            if (glfwGetKey(win, GLFW_KEY_1 + k) == GLFW_PRESS) in.hotbar_set = k;
        
        // Terrain sculpting mode keys (hold Ctrl to use sculpt modes instead of hotbar)
        bool ctrl_held = glfwGetKey(win, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS || 
                         glfwGetKey(win, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
        if (ctrl_held) {
            in.key_1 = glfwGetKey(win, GLFW_KEY_1) == GLFW_PRESS;
            in.key_2 = glfwGetKey(win, GLFW_KEY_2) == GLFW_PRESS;
            in.key_3 = glfwGetKey(win, GLFW_KEY_3) == GLFW_PRESS;
            in.key_4 = glfwGetKey(win, GLFW_KEY_4) == GLFW_PRESS;
            in.key_5 = glfwGetKey(win, GLFW_KEY_5) == GLFW_PRESS;
        }
        // R/T for radius/strength ([ ] for decrease/increase)
        in.key_r_down = glfwGetKey(win, GLFW_KEY_LEFT_BRACKET) == GLFW_PRESS;
        in.key_r_up = glfwGetKey(win, GLFW_KEY_RIGHT_BRACKET) == GLFW_PRESS;
        in.key_t_down = glfwGetKey(win, GLFW_KEY_MINUS) == GLFW_PRESS;
        in.key_t_up = glfwGetKey(win, GLFW_KEY_EQUAL) == GLFW_PRESS;

        // In headless harness mode, drive inputs programmatically and keep the
        // camera pinned so chunks stream in around the fixed viewpoint.
        if (headless) {
            in = sdfcraft::FrameInput{};
            in.dig = want_dig;
            if (want_planet && !planet_done) { in.toggle_planet = true; planet_done = true; }
            // QA: open the inventory screen on frame 1, park a cursor over a slot
            if (qa_openinv) {
                if (frame == 0) in.inv_toggle = true;
                in.mouse_x = 820.0f; in.mouse_y = 470.0f;
            }
            if (qa_opencraft) {
                if (frame == 0) in.craft_toggle = true;
                in.mouse_x = 820.0f; in.mouse_y = 470.0f;
            }
            dt = 1.0f / 60.0f;
            if (want_fly) g_mode.player.flying = true;
            if (py < 1e8f) g_mode.player.pos = glm::vec3(px, py, pz);
            g_mode.player.yaw = yaw; g_mode.player.pitch = pitch;
            // pin time-of-day so the lighting in --shot is deterministic
            if (tod_override >= 0.0f)
                if (sdfcraft::ServerSim* s = g_mode.sim()) s->time_of_day = tod_override;
        }

        g_mode.update(in, dt);

        // Cursor mode follows the inventory screen: free pointer for clicking
        // slots when open, locked for mouse-look when playing. Reset first_mouse
        // on the transition so closing the screen doesn't snap the view.
        if (!headless) {
            static bool was_gui = false;
            bool now_gui = g_mode.invOpen();
            if (now_gui != was_gui) {
                glfwSetInputMode(win, GLFW_CURSOR, now_gui ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_DISABLED);
                g_first_mouse = true;
                was_gui = now_gui;
            }
        }

        // QA closeup: keep one mob of the requested kind pinned ~3.5m in front of
        // the camera so a headless --shot captures the model up close (the normal
        // spawn ring is 24m+ away, too far to inspect rigs). Refreshed each frame
        // so it can't wander/fall out of view.
        if (qa_mob >= 0) {
            if (sdfcraft::ServerSim* s = g_mode.sim()) {
                glm::vec3 fwd = g_mode.player.forward();
                glm::vec3 eye = g_mode.player.eye();
                glm::vec3 fp(eye.x + fwd.x*3.0f, eye.y - 1.4f, eye.z + fwd.z*3.0f);
                // Keep EXACTLY one mob of the requested kind (the natural spawner
                // also adds mobs, so we clear them and pin our own each frame —
                // otherwise entities[0] is a random natural mob of the wrong kind).
                s->mobs.entities.clear();
                auto& e = s->mobs.spawn((sdfcraft::MobKind)qa_mob, fp);
                e.pos = fp;
                e.yaw = g_mode.player.yaw + (qa_mob_side ? 90.0f : 180.0f);  // side or face camera
                e.render_moving = qa_mob_walk;
            }
        }

        int fw, fh; glfwGetFramebufferSize(win, &fw, &fh);
        glViewport(0, 0, fw, fh);
        g_mode.render(fw, fh);

        glfwSwapBuffers(win);

        if (headless && ++frame >= shot_frames) {
            save_screenshot(shot_path, fw, fh);
            break;
        }
    }

    g_mode.shutdown();
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
