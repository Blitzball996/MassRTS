#pragma once
// =============================================================================
// SDFCraft - 2D screen-space HUD overlay (Minecraft Legacy Console style)
// -----------------------------------------------------------------------------
// Renders the classic MC survival HUD from the real sprite sheets:
//   assets/textures/gui/icons.png (256x256) - hearts, hunger, air, crosshair
//   assets/textures/gui/gui.png   (256x256) - hotbar widget + selection box
//
// Pipeline: a single textured shader (pos2 + uv2 + rgba tint). Sprites sample
// their sheet; solid quads (per-slot block swatch, fallback bars) bind a 1x1
// white texture so the same shader/program covers everything. Geometry is
// batched per source texture to minimise binds:
//   - gui.png   batch : hotbar widget, selection highlight
//   - white     batch : per-slot block swatches (tinted)
//   - icons.png batch : hearts, hunger, air bubbles, crosshair
//
// Coordinate convention: layout math uses pixel rects with origin at the
// TOP-LEFT of the framebuffer; quads are converted to NDC internally. Sprite
// source rects are MC pixel coords into the 256x256 sheet.
//
// If a sheet fails to load we fall back to the old solid-colour quads for the
// affected part so a missing asset still shows *something* instead of crashing.
//
// GL state: depth test OFF, blend ON (src_alpha / one_minus_src_alpha); prior
// GL_DEPTH_TEST / GL_BLEND state is saved and restored.
// =============================================================================
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/common.hpp>
#include <vector>
#include <iostream>
#include "player.h"
#include "inventory.h"
#include "blocks.h"
#include "item_icons.h"

// stb_image already implemented in planet_renderer.h; declarations only here.
#include "../stb_image.h"

namespace sdfcraft {

class HudRenderer {
public:
    bool init() {
        const char* VS =
            "#version 330 core\n"
            "layout(location=0) in vec2 a_pos;\n"   // NDC
            "layout(location=1) in vec2 a_uv;\n"
            "layout(location=2) in vec4 a_col;\n"
            "out vec2 v_uv; out vec4 v_col;\n"
            "void main(){ v_uv=a_uv; v_col=a_col; gl_Position=vec4(a_pos,0.0,1.0); }\n";
        const char* FS =
            "#version 330 core\n"
            "in vec2 v_uv; in vec4 v_col; out vec4 frag;\n"
            "uniform sampler2D u_tex;\n"
            "void main(){ frag = texture(u_tex, v_uv) * v_col; }\n";
        prog_ = link(VS, FS);
        if (!prog_) return false;

        glGenVertexArrays(1, &vao_);
        glGenBuffers(1, &vbo_);
        glBindVertexArray(vao_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        const GLsizei stride = 8 * sizeof(float);  // x y u v r g b a
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (void*)(2*sizeof(float)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, (void*)(4*sizeof(float)));
        glBindVertexArray(0);

        u_tex_ = glGetUniformLocation(prog_, "u_tex");

        // 1x1 white texture: lets solid-colour quads reuse the textured shader.
        tex_white_ = make_white_();

        // MC sprite sheets (relative to the exe CWD, same as chunk_renderer).
        tex_icons_   = load_sprite_sheet("assets/textures/gui/icons.png");
        tex_gui_     = load_sprite_sheet("assets/textures/gui/gui.png");
        tex_terrain_ = load_sprite_sheet("assets/textures/gui/terrain.png");
        tex_items_   = load_sprite_sheet("assets/textures/gui/items.png");
        tex_font_    = load_sprite_sheet("assets/textures/gui/default.png");  // 128x128 ASCII font
        if (!tex_icons_ || !tex_gui_)
            std::cerr << "[sdfcraft] HUD: sprite sheet(s) missing, using solid fallback\n";
        if (!tex_terrain_ || !tex_items_)
            std::cerr << "[sdfcraft] HUD: item-icon sheet(s) missing, using colour swatch\n";
        return true;
    }

    // Draw the full HUD. fbw/fbh = framebuffer pixel size.
    void draw(int fbw, int fbh, const Player& pl, const Inventory& inv) {
        if (fbw <= 0 || fbh <= 0 || !prog_) return;
        fbw_ = (float)fbw; fbh_ = (float)fbh;
        v_gui_.clear(); v_solid_.clear(); v_icons_.clear();
        v_terrain_.clear(); v_items_.clear(); v_font_.clear();

        // Integer GUI scale derived from height (~MC auto-scale). At 900p -> 3.
        scale_ = (float)std::max(2, fbh / 240);

        build_hotbar(inv);     // -> v_gui_ (widget+selection) + v_solid_ (swatches)
        build_stat_bars(pl);   // -> v_icons_ (or v_solid_ fallback)
        build_crosshair();     // -> v_icons_ (or v_solid_ fallback)

        if (v_gui_.empty() && v_solid_.empty() && v_icons_.empty()
            && v_terrain_.empty() && v_items_.empty() && v_font_.empty()) return;

        // 2D overlay GL state: blended, no depth, no face culling. The world
        // pass leaves GL_CULL_FACE enabled (GL_CCW front / GL_BACK cull); our
        // screen-space quads wind clockwise in NDC, so they'd be culled as
        // back-faces if we left it on. Save/restore all three toggles.
        GLboolean depth_was = glIsEnabled(GL_DEPTH_TEST);
        GLboolean blend_was = glIsEnabled(GL_BLEND);
        GLboolean cull_was  = glIsEnabled(GL_CULL_FACE);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        glUseProgram(prog_);
        glActiveTexture(GL_TEXTURE0);
        glUniform1i(u_tex_, 0);
        glBindVertexArray(vao_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);

        // Back-to-front: hotbar widget, then swatches on top, then item icons
        // from their two atlases, then stat icons/crosshair.
        GLuint gui_tex = tex_gui_ ? tex_gui_ : tex_white_;
        draw_batch(v_gui_,   gui_tex);
        draw_batch(v_solid_, tex_white_);
        if (tex_terrain_) draw_batch(v_terrain_, tex_terrain_);
        if (tex_items_)   draw_batch(v_items_,   tex_items_);
        GLuint icon_tex = tex_icons_ ? tex_icons_ : tex_white_;
        draw_batch(v_icons_, icon_tex);
        if (tex_font_) draw_batch(v_font_, tex_font_);   // stack-count digits on top

        glBindVertexArray(0);
        if (depth_was) glEnable(GL_DEPTH_TEST);
        if (cull_was)  glEnable(GL_CULL_FACE);
        if (!blend_was) glDisable(GL_BLEND);
    }

    void destroy() {
        if (vbo_) glDeleteBuffers(1, &vbo_);
        if (vao_) glDeleteVertexArrays(1, &vao_);
        if (prog_) glDeleteProgram(prog_);
        GLuint texs[] = { tex_white_, tex_icons_, tex_gui_, tex_terrain_, tex_items_, tex_font_ };
        glDeleteTextures(6, texs);
        vbo_ = vao_ = prog_ = 0;
        tex_white_ = tex_icons_ = tex_gui_ = 0;
        tex_terrain_ = tex_items_ = tex_font_ = 0;
    }

private:
    GLuint prog_ = 0, vao_ = 0, vbo_ = 0;
    GLint  u_tex_ = -1;
    GLuint tex_white_ = 0, tex_icons_ = 0, tex_gui_ = 0;
    GLuint tex_terrain_ = 0, tex_items_ = 0;   // block / item icon atlases
    GLuint tex_font_ = 0;                       // 128x128 ASCII font (8x8 glyphs)
    float  fbw_ = 0, fbh_ = 0, scale_ = 2.0f;
    std::vector<float> v_gui_, v_solid_, v_icons_;     // x y u v r g b a per vertex
    std::vector<float> v_terrain_, v_items_;           // item-icon atlas batches
    std::vector<float> v_font_;                        // glyph batch (stack counts)

    // ---- geometry helpers ---------------------------------------------------

    // Push a textured quad. Pixel dst rect (top-left origin) -> two NDC tris;
    // sprite source rect (sheet pixels, top-left origin) -> uv in [0,1].
    void sprite(std::vector<float>& buf, float dx, float dy, float dw, float dh,
                float su, float sv, float sw, float sh, glm::vec4 tint) {
        float x0 =  (dx        / fbw_) * 2.0f - 1.0f;
        float x1 = ((dx+dw)    / fbw_) * 2.0f - 1.0f;
        float y0 = 1.0f - ( dy      / fbh_) * 2.0f;   // top (higher NDC y)
        float y1 = 1.0f - ((dy+dh)  / fbh_) * 2.0f;   // bottom
        const float inv = 1.0f / 256.0f;
        float u0 = su * inv,        v0 = sv * inv;        // sprite top-left
        float u1 = (su+sw) * inv,   v1 = (sv+sh) * inv;   // sprite bottom-right
        auto vert = [&](float px, float py, float u, float v){
            buf.insert(buf.end(), {px, py, u, v, tint.r, tint.g, tint.b, tint.a});
        };
        // top-left, top-right, bottom-right / top-left, bottom-right, bottom-left
        vert(x0,y0,u0,v0); vert(x1,y0,u1,v0); vert(x1,y1,u1,v1);
        vert(x0,y0,u0,v0); vert(x1,y1,u1,v1); vert(x0,y1,u0,v1);
    }

    // Solid-colour quad (binds the white texture; tint carries the colour).
    void solid(std::vector<float>& buf, float dx, float dy, float dw, float dh, glm::vec4 c) {
        sprite(buf, dx, dy, dw, dh, 0, 0, 256, 256, c);  // sample white texel
    }

    // Textured quad with UVs already in [0,1] (item-icon atlases). Pixel dst
    // rect (top-left origin) -> two NDC tris.
    void sprite_uv(std::vector<float>& buf, float dx, float dy, float dw, float dh,
                   float u0, float v0, float u1, float v1, glm::vec4 tint) {
        float x0 =  (dx       / fbw_) * 2.0f - 1.0f;
        float x1 = ((dx+dw)   / fbw_) * 2.0f - 1.0f;
        float y0 = 1.0f - ( dy     / fbh_) * 2.0f;   // top (higher NDC y)
        float y1 = 1.0f - ((dy+dh) / fbh_) * 2.0f;   // bottom
        auto vert = [&](float px, float py, float u, float v){
            buf.insert(buf.end(), {px, py, u, v, tint.r, tint.g, tint.b, tint.a});
        };
        vert(x0,y0,u0,v0); vert(x1,y0,u1,v0); vert(x1,y1,u1,v1);
        vert(x0,y0,u0,v0); vert(x1,y1,u1,v1); vert(x0,y1,u0,v1);
    }

    // Emit a Minecraft-style iso cube for a block item into v_terrain_ (terrain.png).
    // Builds the geometry in pixel space via emit_iso_cube, then converts each
    // IsoTri (px + uv + shade tint) into our NDC vertex format.
    void iso_cube(float dx, float dy, float dw, float dh, BlockId b) {
        std::vector<IsoTri> tris;
        emit_iso_cube(tris, dx, dy, dw, dh, block_face(b, 0), block_face(b, 1));
        for (const IsoTri& t : tris) {
            float px = (t.x / fbw_) * 2.0f - 1.0f;
            float py = 1.0f - (t.y / fbh_) * 2.0f;
            v_terrain_.insert(v_terrain_.end(),
                { px, py, t.u, t.v, t.tint.r, t.tint.g, t.tint.b, t.tint.a });
        }
    }

    void draw_batch(const std::vector<float>& buf, GLuint tex) {
        if (buf.empty()) return;
        glBindTexture(GL_TEXTURE_2D, tex);
        glBufferData(GL_ARRAY_BUFFER, buf.size()*sizeof(float), buf.data(), GL_STREAM_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(buf.size()/8));
    }

    // ---- bitmap text (MC font default.png: 128x128, 16x16 grid of 8px glyphs) -
    // ASCII code c -> cell (c&15, c>>4). Glyphs are drawn at `px` size per cell.
    // Used for stack-count numbers in the bottom-right of item slots.
    void glyph(std::vector<float>& buf, char ch, float dx, float dy, float px, glm::vec4 tint) {
        int c = (unsigned char)ch;
        int col = c & 15, row = c >> 4;
        const float inv = 1.0f / 128.0f;        // font sheet is 128x128
        float u0 = (col*8) * inv, v0 = (row*8) * inv;
        float u1 = (col*8+8) * inv, v1 = (row*8+8) * inv;
        float x0 =  (dx     / fbw_) * 2.0f - 1.0f;
        float x1 = ((dx+px) / fbw_) * 2.0f - 1.0f;
        float y0 = 1.0f - ( dy     / fbh_) * 2.0f;
        float y1 = 1.0f - ((dy+px) / fbh_) * 2.0f;
        auto vert = [&](float vx, float vy, float u, float v){
            buf.insert(buf.end(), {vx, vy, u, v, tint.r, tint.g, tint.b, tint.a});
        };
        vert(x0,y0,u0,v0); vert(x1,y0,u1,v0); vert(x1,y1,u1,v1);
        vert(x0,y0,u0,v0); vert(x1,y1,u1,v1); vert(x0,y1,u0,v1);
    }

    // Draw a stack count in a slot's bottom-right corner (MC-style: white digits
    // with a 1px dark drop-shadow). Right-aligned so multi-digit reads naturally.
    // (sx,sy) = slot top-left, (sw,sh) = slot size in px. Skips counts <= 1.
    void draw_count(float sx, float sy, float sw, float sh, int count) {
        if (count <= 1 || !tex_font_) return;
        char digits[4]; int n = 0;
        int v = count > 999 ? 999 : count;
        do { digits[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 4);
        float gp = 8.0f * (scale_ * 0.5f + 0.5f);   // glyph size ~ slot scale
        if (gp < 8.0f) gp = 8.0f;
        float gx = sx + sw - 2.0f - gp;             // right edge, last digit
        float gy = sy + sh - 2.0f - gp;             // bottom edge
        for (int i = 0; i < n; i++) {               // digits[] is least-significant first
            float x = gx - i * (gp * 0.75f);
            glyph(v_font_, digits[i], x + 1.0f, gy + 1.0f, gp, glm::vec4(0.13f,0.13f,0.13f,1)); // shadow
            glyph(v_font_, digits[i], x, gy, gp, glm::vec4(1,1,1,1));                            // white
        }
    }

    // ---- hotbar -------------------------------------------------------------
    // gui.png: widget src (0,0,182,22); selection src (0,22,24,24). Slots are
    // 20px wide; item icon sits at (3 + i*20, 3) inside the widget, 16x16.
    void build_hotbar(const Inventory& inv) {
        const float s = scale_;
        float w = 182.0f * s, h = 22.0f * s;
        float x0 = (fbw_ - w) * 0.5f;
        float y0 = fbh_ - h - 2.0f * s;              // small bottom margin

        if (tex_gui_) {
            sprite(v_gui_, x0, y0, w, h, 0, 0, 182, 22, glm::vec4(1));
            int sel = glm::clamp(inv.selected, 0, HOTBAR_SLOTS - 1);
            float selx = x0 + (sel * 20.0f - 1.0f) * s;
            float sely = y0 - 1.0f * s;
            sprite(v_gui_, selx, sely, 24.0f*s, 24.0f*s, 0, 22, 24, 24, glm::vec4(1));
        } else {
            // fallback: dark panel + per-slot frames
            solid(v_solid_, x0, y0, w, h, glm::vec4(0,0,0,0.45f));
            for (int i = 0; i < HOTBAR_SLOTS; i++) {
                float sx = x0 + (1.0f + i*20.0f) * s;
                glm::vec4 fr = (i == inv.selected) ? glm::vec4(1,1,1,0.9f)
                                                   : glm::vec4(0.7f,0.7f,0.7f,0.5f);
                solid(v_solid_, sx, y0 + s, 20.0f*s - 2.0f*s, 20.0f*s, fr);
            }
        }

        // Per-slot held item: draw the real MC sprite from its atlas. Falls back
        // to the flat block-colour swatch only if the icon is unmapped or the
        // icon atlases failed to load.
        for (int i = 0; i < HOTBAR_SLOTS; i++) {
            const ItemStack& st = inv.slots[i];
            if (st.empty()) continue;
            float ix = x0 + (3.0f + i*20.0f) * s;
            float iy = y0 + 3.0f * s;
            float sz = 16.0f * s;

            IconRef ic = icon_for(st.id);
            bool have_atlas = (ic.atlas == ICON_ATLAS_TERRAIN) ? (tex_terrain_ != 0)
                                                               : (tex_items_   != 0);
            if (item_is_block(st.id) && tex_terrain_ && ic.ok) {
                // 3D iso cube for blocks (samples terrain.png faces, shaded).
                iso_cube(ix, iy, sz, sz, item_block(st.id));
            } else if (ic.ok && have_atlas) {
                std::vector<float>& buf = (ic.atlas == ICON_ATLAS_TERRAIN) ? v_terrain_ : v_items_;
                sprite_uv(buf, ix, iy, sz, sz, ic.u0, ic.v0, ic.u1, ic.v1, glm::vec4(1));
            } else if (item_is_block(st.id)) {
                glm::vec3 c = block_def(item_block(st.id)).color;
                solid(v_solid_, ix, iy, sz, sz, glm::vec4(c, 1.0f));
            }
            // stack count in the slot's bottom-right (skips 1)
            draw_count(ix, iy, sz, sz, st.count);
        }
    }

    // ---- hearts / hunger / air ---------------------------------------------
    // icons.png rows: hearts v=0, air v=18, hunger v=27. Each icon 9x9, drawn
    // 8px apart (they overlap slightly, as in MC). Bars sit just above the
    // hotbar: health on the left, hunger on the right, air a row above hunger.
    void build_stat_bars(const Player& pl) {
        const float s = scale_;
        float bw = 182.0f * s;
        float left = (fbw_ - bw) * 0.5f;             // hotbar left edge
        float hotbar_top = fbh_ - 22.0f * s - 2.0f * s;
        float row_y  = hotbar_top - 10.0f * s;       // hearts / hunger row
        float air_y  = row_y - 10.0f * s;            // air row (above hunger)
        const float icon = 9.0f * s, step = 8.0f * s;

        if (!tex_icons_) { build_stat_bars_solid(pl, left, bw, row_y, air_y); return; }

        // Health: 10 hearts, left-to-right. Each heart = 2 HP.
        float hp = glm::clamp(pl.health, 0.0f, pl.max_health);
        for (int i = 0; i < 10; i++) {
            float hx = left + i * step;
            sprite(v_icons_, hx, row_y, icon, icon, 16, 0, 9, 9, glm::vec4(1)); // container
            float v = hp - i * 2.0f;
            if (v >= 2.0f)
                sprite(v_icons_, hx, row_y, icon, icon, 52, 0, 9, 9, glm::vec4(1)); // full
            else if (v >= 1.0f)
                sprite(v_icons_, hx, row_y, icon, icon, 61, 0, 9, 9, glm::vec4(1)); // half
        }

        // Hunger: 10 drumsticks, right-to-left from the hotbar right edge.
        float food = glm::clamp(pl.hunger, 0.0f, 20.0f);
        float fright = left + bw - icon;             // right-aligned start
        for (int i = 0; i < 10; i++) {
            float fx = fright - i * step;
            sprite(v_icons_, fx, row_y, icon, icon, 16, 27, 9, 9, glm::vec4(1)); // container
            float v = food - i * 2.0f;
            if (v >= 2.0f)
                sprite(v_icons_, fx, row_y, icon, icon, 52, 27, 9, 9, glm::vec4(1)); // full
            else if (v >= 1.0f)
                sprite(v_icons_, fx, row_y, icon, icon, 61, 27, 9, 9, glm::vec4(1)); // half
        }

        // Air: only while underwater (air below max). Right-to-left above hunger.
        if (pl.air < 10.0f - 0.01f) {
            float air = glm::clamp(pl.air, 0.0f, 10.0f);
            int full = (int)glm::ceil(air);          // bubbles still present
            for (int i = 0; i < 10; i++) {
                float ax = fright - i * step;
                if (i < full)
                    sprite(v_icons_, ax, air_y, icon, icon, 16, 18, 9, 9, glm::vec4(1)); // bubble
                else if (i == full)
                    sprite(v_icons_, ax, air_y, icon, icon, 25, 18, 9, 9, glm::vec4(1)); // popping
            }
        }
    }

    // Solid-colour fallback bars (only used if icons.png is missing).
    void build_stat_bars_solid(const Player& pl, float left, float bw,
                               float row_y, float air_y) {
        auto bar = [&](float x, float y, float value, float maxv, glm::vec4 col, bool rtl){
            const float cell = 9.0f * scale_, step = 8.0f * scale_;
            float filled = maxv > 0 ? (value / maxv) * 10.0f : 0.0f;
            for (int i = 0; i < 10; i++) {
                int slot = rtl ? (9 - i) : i;
                float sx = x + slot * step;
                solid(v_solid_, sx, y, cell, cell, glm::vec4(0,0,0,0.4f));
                float frac = glm::clamp(filled - i, 0.0f, 1.0f);
                if (frac > 0) solid(v_solid_, sx+scale_, y+scale_, (cell-2*scale_)*frac, cell-2*scale_, col);
            }
        };
        bar(left, row_y, pl.health, pl.max_health, glm::vec4(0.86f,0.16f,0.16f,0.95f), false);
        bar(left + bw - 9.0f*scale_ - 9*8.0f*scale_, row_y, pl.hunger, 20.0f,
            glm::vec4(0.78f,0.50f,0.16f,0.95f), false);
        if (pl.air < 10.0f - 0.01f)
            bar(left + bw - 9.0f*scale_ - 9*8.0f*scale_, air_y, pl.air, 10.0f,
                glm::vec4(0.30f,0.70f,0.95f,0.95f), false);
    }

    // ---- crosshair ----------------------------------------------------------
    // icons.png src (0,0,16,16), centred. Plain alpha blend (good enough).
    void build_crosshair() {
        float sz = 16.0f * std::max(1.0f, scale_ - 1.0f);   // a touch smaller than HUD scale
        float cx = fbw_ * 0.5f - sz * 0.5f;
        float cy = fbh_ * 0.5f - sz * 0.5f;
        if (tex_icons_) {
            sprite(v_icons_, cx, cy, sz, sz, 0, 0, 16, 16, glm::vec4(1));
        } else {
            float len = 10.0f, th = 2.0f, ccx = fbw_*0.5f, ccy = fbh_*0.5f;
            glm::vec4 c(1,1,1,0.85f);
            solid(v_solid_, ccx-len-2, ccy-th*0.5f, len, th, c);
            solid(v_solid_, ccx+2,     ccy-th*0.5f, len, th, c);
            solid(v_solid_, ccx-th*0.5f, ccy-len-2, th, len, c);
            solid(v_solid_, ccx-th*0.5f, ccy+2,     th, len, c);
        }
    }

    // ---- resources ----------------------------------------------------------
    static GLuint make_white_() {
        GLuint t; glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        const unsigned char px[4] = {255,255,255,255};
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        return t;
    }

    // Load a GUI sprite sheet: forced RGBA, GL_NEAREST, no mips (crisp pixels).
    static GLuint load_sprite_sheet(const char* path) {
        int w, h, ch;
        stbi_set_flip_vertically_on_load(0);   // sheet stored top-row-first
        unsigned char* data = stbi_load(path, &w, &h, &ch, 4);
        if (!data) {
            std::cerr << "[sdfcraft] HUD: failed to load " << path << "\n";
            return 0;
        }
        GLuint tex; glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        stbi_image_free(data);
        return tex;
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
