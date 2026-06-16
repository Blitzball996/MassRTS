#pragma once
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>

struct Ray { glm::vec3 origin, direction; };

class Camera {
public:
    glm::vec3 target = {0, 0, 0};  // Look-at point (free altitude, manual)
    float distance = 800.0f;        // Distance from target
    float yaw = 0.0f;              // Rotation around Y
    float pitch = 45.0f;           // Angle from horizon (deg); negative = look up
    float move_speed = 600.0f;
    float zoom_speed = 0.08f;
    float rotate_speed = 90.0f;
    float vertical_speed = 350.0f; // R/F manual rise / sink speed (world u/s)

    bool middle_dragging = false;
    double last_mx = 0, last_my = 0;

    // Terrain height sampler. Used ONLY for click-to-ground picking
    // (ray_to_ground). The camera no longer auto-sinks to follow terrain --
    // altitude is fully manual, so a normal fly-over stays level no matter how
    // deep the ground below is, and you can deliberately descend with F (and
    // tilt up to look around inside craters). Set from main after renderer init.
    float (*ground_height)(float, float) = nullptr;

    glm::mat4 get_view() const {
        glm::vec3 pos = get_position();
        return glm::lookAt(pos, target, glm::vec3(0, 1, 0));
    }

    glm::mat4 get_projection(float aspect) const {
        return glm::perspective(glm::radians(45.0f), aspect, 1.0f, 5000.0f);
    }

    glm::vec3 get_position() const {
        float p = glm::radians(pitch);
        float y = glm::radians(yaw);
        glm::vec3 offset;
        offset.x = distance * cos(p) * sin(y);
        offset.y = distance * sin(p);
        offset.z = distance * cos(p) * cos(y);
        return target + offset;
    }

    glm::vec3 get_right() const {
        float y = glm::radians(yaw);
        return glm::vec3(cos(y), 0, -sin(y));
    }

    glm::vec3 get_forward() const {
        float y = glm::radians(yaw);
        return glm::vec3(-sin(y), 0, -cos(y));
    }

    void update(GLFWwindow* window, float dt) {
        glm::vec3 right = get_right();
        glm::vec3 fwd = get_forward();
        float spd = move_speed * (distance / 500.0f) * dt;

        if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) target += fwd * spd;
        if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) target -= fwd * spd;
        if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) target -= right * spd;
        if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) target += right * spd;
        if (glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS) yaw -= rotate_speed * dt;
        if (glfwGetKey(window, GLFW_KEY_E) == GLFW_PRESS) yaw += rotate_speed * dt;

        // Manual altitude: R rises, F sinks. Scales with zoom so it feels
        // consistent close-up and far-out. The camera NEVER auto-sinks, so a
        // level fly-over stays level regardless of how deep the ground is; you
        // descend into craters/valleys only when you choose to.
        float vspd = vertical_speed * (distance / 500.0f) * dt;
        if (glfwGetKey(window, GLFW_KEY_R) == GLFW_PRESS) target.y += vspd;
        if (glfwGetKey(window, GLFW_KEY_F) == GLFW_PRESS) target.y -= vspd;

        // Middle mouse orbit
        if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS) {
            double mx, my;
            glfwGetCursorPos(window, &mx, &my);
            if (!middle_dragging) {
                middle_dragging = true;
                last_mx = mx; last_my = my;
            } else {
                double dx = mx - last_mx, dy = my - last_my;
                yaw += (float)dx * 0.3f;
                pitch += (float)dy * 0.3f;
                // Allow negative pitch so the camera can sit low and tilt UP
                // (e.g. look up out of a crater). Avoid exact +/-90 to keep the
                // up-vector well defined for lookAt.
                pitch = glm::clamp(pitch, -85.0f, 85.0f);
                last_mx = mx; last_my = my;
            }
        } else {
            middle_dragging = false;
        }
        // No auto terrain-follow: altitude is purely manual (W/A/S/D pan,
        // R/F raise/lower, scroll zoom). This keeps fly-overs level.
    }

    void on_scroll(double yoffset) {
        distance *= (1.0f - (float)yoffset * zoom_speed);
        distance = glm::clamp(distance, 5.0f, 3000.0f);
    }

    // Screen coords to ray in world space
    Ray screen_to_ray(float sx, float sy, int width, int height) const {
        float nx = (2.0f * sx / width) - 1.0f;
        float ny = 1.0f - (2.0f * sy / height);

        float aspect = (float)width / (float)height;
        glm::mat4 proj = get_projection(aspect);
        glm::mat4 view = get_view();
        glm::mat4 inv = glm::inverse(proj * view);

        glm::vec4 near_pt = inv * glm::vec4(nx, ny, -1, 1);
        glm::vec4 far_pt = inv * glm::vec4(nx, ny, 1, 1);
        near_pt /= near_pt.w;
        far_pt /= far_pt.w;

        glm::vec3 dir = glm::normalize(glm::vec3(far_pt - near_pt));
        return {glm::vec3(near_pt), dir};
    }

    // Intersect ray with the terrain. Falls back to the Y=0 plane when no
    // height sampler is set; otherwise raymarches the heightfield so clicks
    // land on the real surface (including inside craters).
    glm::vec2 ray_to_ground(Ray ray) const {
        if (ground_height) {
            // March until we cross below the terrain, then bisect for precision.
            float t = 0.0f, step = 4.0f, max_t = 8000.0f;
            float prev_t = 0.0f;
            bool prev_above = (ray.origin.y >= ground_height(ray.origin.x, ray.origin.z));
            while (t < max_t) {
                glm::vec3 p = ray.origin + ray.direction * t;
                bool above = (p.y >= ground_height(p.x, p.z));
                if (above != prev_above) {
                    // crossing between prev_t and t: bisect
                    float lo = prev_t, hi = t;
                    for (int i = 0; i < 20; i++) {
                        float mid = 0.5f * (lo + hi);
                        glm::vec3 pm = ray.origin + ray.direction * mid;
                        bool am = (pm.y >= ground_height(pm.x, pm.z));
                        if (am == prev_above) lo = mid; else hi = mid;
                    }
                    glm::vec3 hit = ray.origin + ray.direction * (0.5f * (lo + hi));
                    return {hit.x, hit.z};
                }
                prev_above = above; prev_t = t;
                t += step; step *= 1.02f; // grow step for far distances
            }
        }
        // Fallback: flat Y=0 plane
        if (fabs(ray.direction.y) < 0.0001f) return {0, 0};
        float t = -ray.origin.y / ray.direction.y;
        glm::vec3 hit = ray.origin + ray.direction * t;
        return {hit.x, hit.z};
    }
};
                                                                                                                                                                                                                                                                                                 