// =============================================================================
// SDFCraft - Planet viewer (Phase P1): fly around a real-scale 6371 km planet
// -----------------------------------------------------------------------------
// Standalone executable. Free-fly camera in DOUBLE precision; the cube-sphere
// LOD planet refines under you and is drawn in floating-origin space. Controls:
//   WASD fly, mouse look, Space up, Shift down, Ctrl boost, R reset to orbit.
// =============================================================================
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "sdfcraft/planet.h"
#include "sdfcraft/planet_mesh.h"
#include "sdfcraft/planet_renderer.h"
#include <cstdio>
#include <cmath>

using namespace sdfcraft;

static double g_yaw = 0, g_pitch = 0;
static double g_lastx = 0, g_lasty = 0; static bool g_first = true;
static void cursor_cb(GLFWwindow*, double x, double y) {
    if (g_first) { g_lastx=x; g_lasty=y; g_first=false; }
    double dx = x-g_lastx, dy = y-g_lasty; g_lastx=x; g_lasty=y;
    g_yaw += dx*0.1; g_pitch -= dy*0.1;
    if (g_pitch>89) g_pitch=89; if (g_pitch<-89) g_pitch=-89;
}

int main() {
    if (!glfwInit()) { fprintf(stderr,"glfwInit failed\n"); return 1; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR,3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR,3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    GLFWwindow* win = glfwCreateWindow(1600,900,"SDFCraft Planet Viewer",nullptr,nullptr);
    if (!win){ glfwTerminate(); return 1; }
    glfwMakeContextCurrent(win); glfwSwapInterval(1);
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)){ return 1; }
    glfwSetInputMode(win, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    glfwSetCursorPosCallback(win, cursor_cb);

    PlanetMesh planet;
    // a gentle procedural height so the globe reads as land/ocean/mountains
    planet.height = [&](const dvec3& dir)->double {
        double n = std::sin(dir.x*8.0)*std::cos(dir.y*6.0)*std::sin(dir.z*7.0);
        n += 0.5*std::sin(dir.x*23.0+1.0)*std::sin(dir.z*19.0);
        return n * 3000.0;   // +-~4.5 km relief
    };
    PlanetRenderer rend;
    if (!rend.init()){ fprintf(stderr,"planet renderer init failed\n"); return 1; }

    // PLACEHOLDER_LOOP
    // start in low orbit looking at the planet
    dvec3 cam = dvec3(1,0,0) * (planet.cfg.radius_m + 2.0e6);  // 2000 km up
    double prev = glfwGetTime();
    int rebuild_timer = 0;
    std::vector<PlanetVertex> verts;

    while (!glfwWindowShouldClose(win)) {
        glfwPollEvents();
        double now = glfwGetTime(); float dt=(float)(now-prev); prev=now;
        if (dt>0.1f) dt=0.1f;
        if (glfwGetKey(win,GLFW_KEY_ESCAPE)==GLFW_PRESS) break;
        if (glfwGetKey(win,GLFW_KEY_R)==GLFW_PRESS)
            cam = dvec3(1,0,0)*(planet.cfg.radius_m+2.0e6);

        // look direction from yaw/pitch
        double cy=cos(glm::radians(g_yaw)), sy=sin(glm::radians(g_yaw));
        double cp=cos(glm::radians(g_pitch)), sp=sin(glm::radians(g_pitch));
        dvec3 fwd = glm::normalize(dvec3(cy*cp, sp, sy*cp));
        dvec3 up0(0,1,0);
        dvec3 rightv = glm::normalize(glm::cross(fwd, up0));
        dvec3 upv = glm::cross(rightv, fwd);

        // speed scales with altitude so flight feels right from orbit to ground
        double alt = glm::length(cam) - planet.cfg.radius_m;
        double speed = std::max(50.0, alt*0.5);
        if (glfwGetKey(win,GLFW_KEY_LEFT_CONTROL)==GLFW_PRESS) speed *= 20.0;
        double m = speed * dt;
        if (glfwGetKey(win,GLFW_KEY_W)==GLFW_PRESS) cam += fwd*m;
        if (glfwGetKey(win,GLFW_KEY_S)==GLFW_PRESS) cam -= fwd*m;
        if (glfwGetKey(win,GLFW_KEY_D)==GLFW_PRESS) cam += rightv*m;
        if (glfwGetKey(win,GLFW_KEY_A)==GLFW_PRESS) cam -= rightv*m;
        if (glfwGetKey(win,GLFW_KEY_SPACE)==GLFW_PRESS) cam += up0*m;
        if (glfwGetKey(win,GLFW_KEY_LEFT_SHIFT)==GLFW_PRESS) cam -= up0*m;

        // refresh LOD + rebuild mesh a few times per second (cheap enough)
        if (rebuild_timer-- <= 0) {
            planet.update_lod(cam);
            planet.build(cam, verts, 8);
            rend.upload(verts);
            rebuild_timer = 6;
        }

        // floating-origin view: camera at origin, looking along fwd
        glm::mat4 view = glm::lookAt(glm::vec3(0), glm::vec3(fwd), glm::vec3(upv));
        int fw,fh; glfwGetFramebufferSize(win,&fw,&fh);
        glViewport(0,0,fw,fh);
        // near/far must span orbit-to-surface; use a big far plane
        glm::mat4 proj = glm::perspective(glm::radians(60.0f),
                            (float)fw/(float)fh, 1.0f, 5.0e7f);
        glClearColor(0.02f,0.02f,0.05f,1.0f);
        glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_CULL_FACE); glCullFace(GL_BACK);
        glm::vec3 sun = glm::normalize(glm::vec3(0.5f,0.7f,0.4f));
        rend.render(view, proj, sun, glm::vec3(upv));
        glfwSwapBuffers(win);
    }

    rend.shutdown();
    glfwDestroyWindow(win); glfwTerminate();
    return 0;
}
