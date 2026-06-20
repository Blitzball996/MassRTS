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

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include "sdfcraft/mode.h"

static sdfcraft::Mode g_mode;

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
static bool   g_fly_edge = false;
static bool   g_takeoff_edge = false;   // R: enter fly mode
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
    if (button == GLFW_MOUSE_BUTTON_RIGHT && action == GLFW_PRESS) g_place_edge = true;
}
static void key_cb(GLFWwindow* w, int key, int, int action, int) {
    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) glfwSetWindowShouldClose(w, 1);
    if (key == GLFW_KEY_F && action == GLFW_PRESS) g_fly_edge = true;
    if (key == GLFW_KEY_R && action == GLFW_PRESS) g_takeoff_edge = true;
    if (key == GLFW_KEY_C && action == GLFW_PRESS) g_land_edge = true;
    if (key == GLFW_KEY_G && action == GLFW_PRESS) g_planet_edge = true;
}

int main(int argc, char** argv) {
    uint64_t seed = 1337;
    // --- offscreen test harness flags ---
    std::string shot_path; int shot_frames = 120;
    bool want_fly = false, want_planet = false, want_dig = false;
    float px=0, py=1e9f, pz=0, yaw=0, pitch=-20;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--shot" && i+1 < argc) shot_path = argv[++i];
        else if (a == "--frames" && i+1 < argc) shot_frames = atoi(argv[++i]);
        else if (a == "--fly") want_fly = true;
        else if (a == "--planet") want_planet = true;
        else if (a == "--dig") want_dig = true;
        else if (a == "--pos" && i+3 < argc) { px=atof(argv[++i]); py=atof(argv[++i]); pz=atof(argv[++i]); }
        else if (a == "--look" && i+2 < argc) { yaw=atof(argv[++i]); pitch=atof(argv[++i]); }
        else if (a[0] != '-') seed = (uint64_t)strtoull(argv[i], nullptr, 10);
    }
    bool headless = !shot_path.empty();

    if (!glfwInit()) { std::cerr << "glfwInit failed\n"; return 1; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    if (headless) glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* win = glfwCreateWindow(1600, 900, "SDFCraft", nullptr, nullptr);
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
    if (!g_mode.init(seed, shader_dir)) {
        std::cerr << "sdfcraft init failed (shaders missing? run from project root)\n";
        return 1;
    }

    // --- harness: position the player and set view before the loop ---
    if (want_fly) g_mode.player.flying = true;
    if (py < 1e8f) g_mode.player.pos = glm::vec3(px, py, pz);
    g_mode.player.yaw = yaw; g_mode.player.pitch = pitch;
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
        in.move_z = (glfwGetKey(win, GLFW_KEY_W)==GLFW_PRESS ? 1.0f : 0.0f) - (glfwGetKey(win, GLFW_KEY_S)==GLFW_PRESS ? 1.0f : 0.0f);
        in.move_x = (glfwGetKey(win, GLFW_KEY_D)==GLFW_PRESS ? 1.0f : 0.0f) - (glfwGetKey(win, GLFW_KEY_A)==GLFW_PRESS ? 1.0f : 0.0f);
        in.jump   = glfwGetKey(win, GLFW_KEY_SPACE)==GLFW_PRESS;
        in.crouch = glfwGetKey(win, GLFW_KEY_LEFT_SHIFT)==GLFW_PRESS;
        in.dig    = glfwGetMouseButton(win, GLFW_MOUSE_BUTTON_LEFT)==GLFW_PRESS;
        in.place  = g_place_edge;       g_place_edge = false;
        in.toggle_fly = g_fly_edge;     g_fly_edge = false;
        in.request_fly  = g_takeoff_edge; g_takeoff_edge = false;
        in.request_walk = g_land_edge;    g_land_edge = false;
        in.toggle_planet = g_planet_edge; g_planet_edge = false;
        in.fly_boost  = glfwGetKey(win, GLFW_KEY_LEFT_CONTROL)==GLFW_PRESS;
        in.look_dx = g_look_dx;         g_look_dx = 0;
        in.look_dy = g_look_dy;         g_look_dy = 0;
        in.hotbar_scroll = g_wheel;     g_wheel = 0;
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
            dt = 1.0f / 60.0f;
            if (want_fly) g_mode.player.flying = true;
            if (py < 1e8f) g_mode.player.pos = glm::vec3(px, py, pz);
            g_mode.player.yaw = yaw; g_mode.player.pitch = pitch;
        }

        g_mode.update(in, dt);

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
