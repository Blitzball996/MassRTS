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
#include <string>

#include "sdfcraft/mode.h"

static sdfcraft::Mode g_mode;
static double g_last_x = 0, g_last_y = 0;
static bool   g_first_mouse = true;
static float  g_look_dx = 0, g_look_dy = 0;
static int    g_wheel = 0;
static bool   g_place_edge = false;
static bool   g_fly_edge = false;
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
}

int main(int argc, char** argv) {
    uint64_t seed = 1337;
    if (argc > 1) seed = (uint64_t)strtoull(argv[1], nullptr, 10);

    if (!glfwInit()) { std::cerr << "glfwInit failed\n"; return 1; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
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

    double prev = glfwGetTime();
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
        in.look_dx = g_look_dx;         g_look_dx = 0;
        in.look_dy = g_look_dy;         g_look_dy = 0;
        in.hotbar_scroll = g_wheel;     g_wheel = 0;
        for (int k = 0; k < 9; k++)
            if (glfwGetKey(win, GLFW_KEY_1 + k) == GLFW_PRESS) in.hotbar_set = k;

        g_mode.update(in, dt);

        int fw, fh; glfwGetFramebufferSize(win, &fw, &fh);
        glViewport(0, 0, fw, fh);
        g_mode.render(fw, fh);

        glfwSwapBuffers(win);
    }

    g_mode.shutdown();
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
