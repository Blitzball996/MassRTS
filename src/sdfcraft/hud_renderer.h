#pragma once
// =============================================================================
// SDFCraft - 2D screen-space HUD overlay
// -----------------------------------------------------------------------------
// Self-contained, no external assets: builds a list of solid-colour 2D quads in
// normalized device coordinates each frame and draws them in one batched call
// with alpha blending and depth test off. Renders:
//   - crosshair (centre)
//   - health bar  (red, 10 segments, reads Player::health / max_health)
//   - hunger bar  (orange, 10 segments, reads Player::hunger)
//   - air bar     (cyan, only while underwater / air < max, reads Player::air)
//   - hotbar      (9 slots + selected highlight + per-slot block-colour swatch)
//
// Coordinate convention: callers pass pixel rects with origin at the TOP-LEFT of
// the framebuffer; we convert to NDC internally so layout math reads naturally.
// =============================================================================
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <vector>
#include <string>
#include <iostream>
#include "player.h"
#include "inventory.h"
#include "blocks.h"

namespace sdfcraft {

class HudRenderer {
public:
    bool init() {
        const char* VS =
            "#version 330 core\n"
            "layout(location=0) in vec2 a_pos;\n"   // NDC
            "layout(location=1) in vec4 a_col;\n"
            "out vec4 v_col;\n"
            "void main(){ v_col=a_col; gl_Position=vec4(a_pos,0.0,1.0); }\n";
        const char* FS =
            "#version 330 core\n"
            "in vec4 v_col; out vec4 frag;\n"
            "void main(){ frag=v_col; }\n";
        prog_ = link(VS, FS);
        if (!prog_) return false;
        glGenVertexArrays(1, &vao_);
        glGenBuffers(1, &vbo_);
        glBindVertexArray(vao_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 6*sizeof(float), (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 6*sizeof(float), (void*)(2*sizeof(float)));
        glBindVertexArray(0);
        return true;
    }

    // Draw the full HUD. fbw/fbh = framebuffer pixel size.
    void draw(int fbw, int fbh, const Player& pl, const Inventory& inv) {
        if (fbw <= 0 || fbh <= 0) return;
        fbw_ = (float)fbw; fbh_ = (float)fbh;
        verts_.clear();

        build_crosshair();
        build_hotbar(inv);
        build_stat_bars(pl);

        if (verts_.empty()) return;

        // GL state: 2D overlay, blended, no depth.
        GLboolean depth_was = glIsEnabled(GL_DEPTH_TEST);
        GLboolean blend_was = glIsEnabled(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        glUseProgram(prog_);
        glBindVertexArray(vao_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBufferData(GL_ARRAY_BUFFER, verts_.size()*sizeof(float), verts_.data(), GL_STREAM_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(verts_.size()/6));
        glBindVertexArray(0);

        if (depth_was) glEnable(GL_DEPTH_TEST);
        if (!blend_was) glDisable(GL_BLEND);
    }

    void destroy() {
        if (vbo_) glDeleteBuffers(1, &vbo_);
        if (vao_) glDeleteVertexArrays(1, &vao_);
        if (prog_) glDeleteProgram(prog_);
        vbo_ = vao_ = prog_ = 0;
    }

private:
    GLuint prog_ = 0, vao_ = 0, vbo_ = 0;
    float fbw_ = 0, fbh_ = 0;
    std::vector<float> verts_;   // x y r g b a per vertex, 6 verts per quad

    // Pixel rect (top-left origin) -> two NDC triangles.
    void quad_px(float x, float y, float w, float h, glm::vec4 c) {
        float x0 =  (x        / fbw_) * 2.0f - 1.0f;
        float x1 = ((x+w)     / fbw_) * 2.0f - 1.0f;
        // flip Y: pixel y grows down, NDC y grows up
        float y0 = 1.0f - ( y      / fbh_) * 2.0f;
        float y1 = 1.0f - ((y+h)   / fbh_) * 2.0f;
        auto v = [&](float px, float py){
            verts_.insert(verts_.end(), {px, py, c.r, c.g, c.b, c.a});
        };
        v(x0,y0); v(x1,y0); v(x1,y1);
        v(x0,y0); v(x1,y1); v(x0,y1);
    }

    void build_crosshair() {
        float cx = fbw_ * 0.5f, cy = fbh_ * 0.5f;
        float len = 10.0f, thick = 2.0f;
        glm::vec4 col(1,1,1,0.85f);
        // horizontal + vertical bars, with a small centre gap
        quad_px(cx - len - 2, cy - thick*0.5f, len, thick, col);
        quad_px(cx + 2,       cy - thick*0.5f, len, thick, col);
        quad_px(cx - thick*0.5f, cy - len - 2, thick, len, col);
        quad_px(cx - thick*0.5f, cy + 2,       thick, len, col);
    }

    // A segmented bar: `value`/`maxv` filled cells of `segments`, anchored.
    void build_bar(float x, float y, int segments, float value, float maxv,
                   glm::vec4 fill, bool right_to_left) {
        const float cell = 16.0f, gap = 2.0f;
        float filled = (maxv > 0.0f) ? (value / maxv) * segments : 0.0f;
        for (int i = 0; i < segments; i++) {
            int slot = right_to_left ? (segments - 1 - i) : i;
            float sx = x + slot * (cell + gap);
            // background socket
            quad_px(sx, y, cell, cell, glm::vec4(0,0,0,0.45f));
            float frac = glm::clamp(filled - i, 0.0f, 1.0f);
            if (frac > 0.0f) {
                float w = cell * frac;
                float fx = right_to_left ? (sx + cell - w) : sx;
                quad_px(fx + 2, y + 2, glm::max(w - 4, 1.0f), cell - 4, fill);
            }
        }
    }

    void build_stat_bars(const Player& pl) {
        const float cell = 16.0f, gap = 2.0f;
        float barw = 10 * (cell + gap);
        float cx = fbw_ * 0.5f;
        float hotbar_top = fbh_ - 64.0f;     // matches build_hotbar
        // health: bottom-left of centre, just above the hotbar
        float hy = hotbar_top - 24.0f;
        build_bar(cx - 220.0f, hy, 10, pl.health, pl.max_health,
                  glm::vec4(0.86f,0.16f,0.16f,0.95f), false);
        // hunger: bottom-right of centre, grows right-to-left toward centre
        build_bar(cx + 220.0f - barw, hy, 10, pl.hunger, 20.0f,
                  glm::vec4(0.78f,0.50f,0.16f,0.95f), true);
        // air: only when below max (underwater), a row above health
        if (pl.air < 10.0f - 0.01f) {
            build_bar(cx + 220.0f - barw, hy - (cell + 4.0f), 10, pl.air, 10.0f,
                      glm::vec4(0.30f,0.70f,0.95f,0.95f), true);
        }
    }

    void build_hotbar(const Inventory& inv) {
        const float cell = 50.0f, gap = 4.0f, pad = 4.0f;
        float total = HOTBAR_SLOTS * cell + (HOTBAR_SLOTS - 1) * gap;
        float x0 = (fbw_ - total) * 0.5f;
        float y0 = fbh_ - 64.0f;
        // backing panel
        quad_px(x0 - pad, y0 - pad, total + pad*2, cell + pad*2, glm::vec4(0,0,0,0.40f));
        for (int i = 0; i < HOTBAR_SLOTS; i++) {
            float sx = x0 + i * (cell + gap);
            // slot frame
            glm::vec4 frame = (i == inv.selected) ? glm::vec4(1,1,1,0.95f)
                                                   : glm::vec4(0.7f,0.7f,0.7f,0.55f);
            quad_px(sx, y0, cell, cell, frame);
            quad_px(sx + 2, y0 + 2, cell - 4, cell - 4, glm::vec4(0.10f,0.10f,0.12f,0.75f));
            // block-colour swatch for the held item
            const ItemStack& st = inv.slots[i];
            if (!st.empty() && item_is_block(st.id)) {
                glm::vec3 c = block_def(item_block(st.id)).color;
                quad_px(sx + 10, y0 + 10, cell - 20, cell - 20, glm::vec4(c, 1.0f));
            }
        }
    }

    static GLuint compile(GLenum t, const char* s) {
        GLuint sh = glCreateShader(t);
        glShaderSource(sh, 1, &s, nullptr);
        glCompileShader(sh);
        GLint ok=0; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
        if (!ok){ char log[512]; glGetShaderInfoLog(sh,512,nullptr,log);
                  std::cerr << "[sdfcraft] HUD shader error: " << log << "\n"; }
        return sh;
    }
    static GLuint link(const char* vs, const char* fs) {
        GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
        GLuint p = glCreateProgram();
        glAttachShader(p, v); glAttachShader(p, f); glLinkProgram(p);
        GLint ok=0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
        if (!ok){ char log[512]; glGetProgramInfoLog(p,512,nullptr,log);
                  std::cerr << "[sdfcraft] HUD link error: " << log << "\n"; p=0; }
        glDeleteShader(v); glDeleteShader(f);
        return p;
    }
};

} // namespace sdfcraft
