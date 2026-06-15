#pragma once
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>

struct Ray { glm::vec3 origin, direction; };

class Camera {
public:
    glm::vec3 target = {0, 0, 0};  // Look-at point on ground
    float distance = 800.0f;        // Distance from target
    float yaw = 0.0f;              // Rotation around Y
    float pitch = 45.0f;           // Angle from horizon (degrees)
    float move_speed = 600.0f;
    float zoom_speed = 0.08f;
    float rotate_speed = 90.0f;

    bool middle_dragging = false;
    double last_mx = 0, last_my = 0;

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
                pitch = glm::clamp(pitch, 10.0f, 85.0f);
                last_mx = mx; last_my = my;
            }
        } else {
            middle_dragging = false;
        }
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

    // Intersect ray with ground plane (Y=0)
    glm::vec2 ray_to_ground(Ray ray) const {
        if (abs(ray.direction.y) < 0.0001f) return {0, 0};
        float t = -ray.origin.y / ray.direction.y;
        glm::vec3 hit = ray.origin + ray.direction * t;
        return {hit.x, hit.z};
    }
};
