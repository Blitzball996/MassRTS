#pragma once
// =============================================================================
// SDFCraft - Player Preview (3D model in GUI, like Minecraft Console Edition)
// -----------------------------------------------------------------------------
// Renders the player's 3D model inside the inventory screen UI window.
// Shows current armor/equipment and follows cursor for rotation (like MC).
//
// Based on MinecraftConsoles UIControl_MinecraftPlayer.cpp
// =============================================================================
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "mc_model.h"
#include "player.h"
#include "inventory.h"

namespace sdfcraft {

class PlayerPreviewRenderer {
public:
    // Render player model in a GUI rect (x, y, w, h in screen pixels)
    // Mouse (mx, my) controls model rotation (like MC Console Edition)
    void render(const Player& player, const Inventory& inv,
                McModelRenderer& model_rend,
                int fbw, int fbh,
                float x, float y, float w, float h,
                float mx, float my, float anim_time) {

        // Set up viewport scissor to clip to the preview region
        GLint old_vp[4];
        glGetIntegerv(GL_VIEWPORT, old_vp);

        int px = (int)x;
        int py = (int)(fbh - y - h);  // OpenGL Y is bottom-up
        int pw = (int)w;
        int ph = (int)h;

        glEnable(GL_SCISSOR_TEST);
        glScissor(px, py, pw, ph);
        glViewport(px, py, pw, ph);

        // Create projection for 3D model (perspective)
        float aspect = w / h;
        glm::mat4 proj = glm::perspective(glm::radians(30.0f), aspect, 0.1f, 100.0f);

        // Camera positioned to frame the player model
        glm::vec3 cam_pos(0, 0, 5.0f);
        glm::vec3 look_at(0, 0, 0);
        glm::mat4 view = glm::lookAt(cam_pos, look_at, glm::vec3(0,1,0));

        // Calculate model rotation based on mouse position (MC style)
        // Mouse in center = facing forward, moving mouse rotates model
        float center_x = x + w * 0.5f;
        float center_y = y + h * 0.5f;
        float dx = mx - center_x;
        float dy = my - center_y;

        // Body yaw follows horizontal mouse movement
        float body_yaw = (dx / w) * 60.0f;  // degrees

        // Head pitch follows vertical mouse movement (subtle)
        float head_pitch = -(dy / h) * 20.0f;  // degrees

        // Build PlayerRender for the model renderer
        PlayerRender pr;
        pr.pos = glm::vec3(0, -0.9f, 0);  // position model slightly lower
        pr.yaw = body_yaw;
        pr.pitch = head_pitch;
        pr.moving = false;  // static pose in inventory

        // Render player model
        glm::vec3 sun_dir(0.5f, 1.0f, 0.3f);
        std::vector<PlayerRender> players = {pr};

        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
        glClear(GL_DEPTH_BUFFER_BIT);  // clear depth for this region only

        model_rend.renderPlayers(players, view, proj, sun_dir, anim_time);

        glDisable(GL_DEPTH_TEST);

        // Restore viewport and scissor
        glViewport(old_vp[0], old_vp[1], old_vp[2], old_vp[3]);
        glDisable(GL_SCISSOR_TEST);
    }
};

} // namespace sdfcraft
