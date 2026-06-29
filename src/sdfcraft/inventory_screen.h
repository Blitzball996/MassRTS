#pragma once
// =============================================================================
// SDFCraft - Inventory screen (E key) — Minecraft classic layout
// -----------------------------------------------------------------------------
// MC-standard inventory overlay (按 E 打开):
//   • 左上：2×2 crafting grid + arrow + result slot (player crafting)
//   • 左侧：4 armor slots VERTICAL (head/chest/legs/feet) ← 竖排！
//   • 中间：3×9 main inventory
//   • 底部：9 hotbar slots
//   • 右上（可选）：player model preview
//
// Interaction: manual click-drag items MC-style (no auto-craft button list).
// The 2x2 grid continuously evaluates recipes via RecipeBook::match; clicking
// the result slot consumes one of each ingredient and gives the result.
//
// Self-contained rendering like hud_renderer.h: own shader + MC texture atlases
// (terrain.png, items.png, default.png font). Item icons via item_icons.h.
// =============================================================================
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <vector>
#include <array>
#include <algorithm>
#include <iostream>
#include "inventory.h"
#include "items.h"
#include "blocks.h"
#include "crafting.h"
#include "item_icons.h"
#include "../stb_image.h"

namespace sdfcraft {

class InventoryScreen {
public:
    ItemStack cursor;          // the stack currently held on the mouse (drag)
    ItemStack craft_grid[4];   // 2×2 crafting grid (row-major: 0=TL, 1=TR, 2=BL, 3=BR)
    CraftResult craft_result;  // auto-evaluated result from craft_grid

    bool init() {
        const char* VS =
            "#version 330 core\n"
            "layout(location=0) in vec2 a_pos;\n"
            "layout(location=1) in vec2 a_uv;\n"
            "layout(location=2) in vec4 a_col;\n"
            "out vec2 v_uv; out vec4 v_col;\n"
            "void main(){ v_uv=a_uv; v_col=a_col; gl_Position=vec4(a_pos,0,1); }\n";
        const char* FS =
            "#version 330 core\n"
            "in vec2 v_uv; in vec4 v_col; out vec4 frag;\n"
            "uniform sampler2D u_tex; uniform float u_use_tex;\n"
            "void main(){ if(u_use_tex>0.5){ vec4 c=texture(u_tex,v_uv); if(c.a<0.05) discard; frag=c*v_col; }\n"
            "             else frag=v_col; }\n";
        prog_ = link(VS, FS);
        if (!prog_) return false;
        glGenVertexArrays(1,&vao_); glGenBuffers(1,&vbo_);
        glBindVertexArray(vao_); glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,8*sizeof(float),(void*)0);                 glEnableVertexAttribArray(0);
        glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,8*sizeof(float),(void*)(2*sizeof(float))); glEnableVertexAttribArray(1);
        glVertexAttribPointer(2,4,GL_FLOAT,GL_FALSE,8*sizeof(float),(void*)(4*sizeof(float))); glEnableVertexAttribArray(2);
        glBindVertexArray(0);
        tex_terrain_ = load_tex("assets/textures/gui/terrain.png");
        tex_items_   = load_tex("assets/textures/gui/items.png");
        tex_font_    = load_tex("assets/textures/gui/default.png");  // 128x128 ASCII font
        white_       = make_white();
        return true;
    }

    void destroy() {
        if (vbo_) glDeleteBuffers(1,&vbo_);
        if (vao_) glDeleteVertexArrays(1,&vao_);
        if (prog_) glDeleteProgram(prog_);
        GLuint t[4]={tex_terrain_,tex_items_,white_,tex_font_};
        glDeleteTextures(4,t);
        prog_=vao_=vbo_=0;
    }

    // Drop the held cursor stack + craft grid items back into inventory (called
    // on close so dragged/gridded items aren't lost). Worn armor stays equipped.
    void returnAll(Inventory& inv) {
        if(!cursor.empty()){ inv.add(cursor.id, cursor.count, item_max_stack(cursor.id)); cursor.clear(); }
        for (int i=0; i<4; i++) if (!craft_grid[i].empty()) {
            inv.add(craft_grid[i].id, craft_grid[i].count, item_max_stack(craft_grid[i].id));
            craft_grid[i].clear();
        }
        craft_result = {};
    }

    // Handle one mouse click at pixel (mx,my) (top-left origin). right=true for
    // right-click (place one / pick half). Returns true if it hit a slot.
    bool click(Inventory& inv, RecipeBook& recipes, int fbw, int fbh,
               float mx, float my, bool right) {
        layout(fbw, fbh);
        // 2×2 craft grid slots
        for (int i=0;i<4;i++) if (hit(craft_rect_[i], mx, my)) {
            move(craft_grid[i], right);
            recompute_craft(recipes);
            return true;
        }
        // craft result slot: take the result, consume one of each ingredient
        if (hit(craft_result_rect_, mx, my) && craft_result.id != ITEM_NONE) {
            take_craft_result(inv);
            recompute_craft(recipes);
            return true;
        }
        // armor equipment slots (head/chest/legs/feet). Clicking moves the cursor
        // stack in/out; a held armor piece only goes into its matching slot.
        for (int i=0;i<4;i++) if (hit(armor_rect_[i], mx, my)) { move_armor(inv.armor[i], (ArmorSlot)(i+1)); return true; }
        // inventory slots (hotbar + main). Right-click on an armor item with an
        // empty cursor auto-equips it (MC quick-equip).
        for (int i=0;i<INV_SLOTS;i++) if (hit(slot_rect_[i], mx, my)) {
            if (cursor.empty() && right && try_quick_equip(inv, inv.slots[i])) return true;
            move(inv.slots[i], right); return true;
        }
        return false;
    }

    // Draw the whole screen. mx/my = cursor pos so the held stack follows it.
    void draw(Inventory& inv, RecipeBook& recipes, int fbw, int fbh, float mx, float my) {
        layout(fbw, fbh);
        recompute_craft(recipes);
        fbw_=(float)fbw; fbh_=(float)fbh;
        verts_.clear(); text_.clear();

        // dim the world behind the panel
        quad(0,0,(float)fbw,(float)fbh, glm::vec4(0,0,0,0.55f));
        // panel
        quad(panel_.x, panel_.y, panel_.z, panel_.w, glm::vec4(0.12f,0.12f,0.14f,0.96f));
        quad(panel_.x+2*s_, panel_.y+2*s_, panel_.z-4*s_, panel_.w-4*s_, glm::vec4(0.20f,0.20f,0.24f,1.0f));

        auto draw_slot = [&](const glm::vec4& r, const ItemStack& st){
            quad(r.x, r.y, r.z, r.w, glm::vec4(0.10f,0.10f,0.12f,1.0f));      // socket
            quad(r.x+s_, r.y+s_, r.z-2*s_, r.w-2*s_, glm::vec4(0.32f,0.32f,0.36f,1.0f));
            if (!st.empty()) { icon(r.x+2*s_, r.y+2*s_, r.z-4*s_, r.w-4*s_, st.id);
                               draw_count(r.x+2*s_, r.y+2*s_, r.z-4*s_, r.w-4*s_, st.count); }
        };
        // 2×2 crafting grid
        for (int i=0;i<4;i++) draw_slot(craft_rect_[i], craft_grid[i]);
        // craft arrow (simple rightward arrow)
        float ax = craft_rect_[1].x + craft_rect_[1].z + 3*s_;
        float ay = craft_rect_[0].y + (craft_rect_[2].y - craft_rect_[0].y)*0.5f;
        quad(ax, ay, 12*s_, 4*s_, glm::vec4(0.7f,0.7f,0.7f,1.0f));
        // craft result slot
        draw_slot(craft_result_rect_, ItemStack{craft_result.id, craft_result.count});

        for (int i=0;i<INV_SLOTS;i++) draw_slot(slot_rect_[i], inv.slots[i]);
        // armor equipment slots (head/chest/legs/feet) VERTICAL down the left
        for (int i=0;i<4;i++) draw_slot(armor_rect_[i], inv.armor[i]);

        // the held (cursor) stack floats under the pointer
        if (!cursor.empty()) { icon(mx-8*s_, my-8*s_, 16*s_, 16*s_, cursor.id);
                               draw_count(mx-8*s_, my-8*s_, 16*s_, 16*s_, cursor.count); }

        flush();
    }

private:
    GLuint prog_=0, vao_=0, vbo_=0, tex_terrain_=0, tex_items_=0, white_=0, tex_font_=0;
    std::vector<float> verts_;
    std::vector<float> text_;   // glyph quads (count numbers), drawn last
    float fbw_=0, fbh_=0, s_=2.0f;
    glm::vec4 panel_{0,0,0,0};
    glm::vec4 slot_rect_[INV_SLOTS];
    glm::vec4 armor_rect_[4];   // head/chest/legs/feet equipment slots (VERTICAL)
    glm::vec4 craft_rect_[4];   // 2×2 crafting grid slots
    glm::vec4 craft_result_rect_{0,0,0,0};

    // --- slot move logic (pick up / drop / merge) ---------------------------
    void move(ItemStack& slot, bool right) {
        if (cursor.empty()) {
            if (slot.empty()) return;
            if (right) {                       // pick up half
                uint8_t half = (uint8_t)((slot.count+1)/2);
                cursor.id = slot.id; cursor.count = half;
                slot.count -= half; if (slot.count==0) slot.clear();
            } else { cursor = slot; slot.clear(); }   // pick up all
        } else {
            if (slot.empty()) {
                if (right) { slot.id=cursor.id; slot.count=1; if(--cursor.count==0) cursor.clear(); }
                else { slot = cursor; cursor.clear(); }
            } else if (slot.id == cursor.id) {  // merge
                uint8_t mx = item_max_stack(slot.id);
                uint8_t room = (uint8_t)(mx - slot.count);
                uint8_t mv = right ? (uint8_t)std::min<int>(1,cursor.count) : (uint8_t)std::min<int>(room,cursor.count);
                mv = (uint8_t)std::min<int>(mv, room);
                slot.count += mv; cursor.count -= mv; if (cursor.count==0) cursor.clear();
            } else { std::swap(slot, cursor); }  // swap different items
        }
    }

    // --- crafting logic -----------------------------------------------------
    void recompute_craft(RecipeBook& recipes) {
        ItemId grid[4] = { craft_grid[0].empty() ? ITEM_NONE : craft_grid[0].id,
                           craft_grid[1].empty() ? ITEM_NONE : craft_grid[1].id,
                           craft_grid[2].empty() ? ITEM_NONE : craft_grid[2].id,
                           craft_grid[3].empty() ? ITEM_NONE : craft_grid[3].id };
        craft_result = recipes.match(grid, 2, 2);
    }
    void take_craft_result(Inventory& inv) {
        if (craft_result.id == ITEM_NONE) return;
        // try to merge into cursor; if full, do nothing (MC behavior: can't take if cursor full of different item)
        if (!cursor.empty() && cursor.id != craft_result.id) return;
        uint8_t mx = item_max_stack(craft_result.id);
        if (cursor.count + craft_result.count > mx) return;
        // consume one of each ingredient in the grid
        for (int i=0; i<4; i++) {
            if (!craft_grid[i].empty()) {
                craft_grid[i].count--;
                if (craft_grid[i].count == 0) craft_grid[i].clear();
            }
        }
        // give result to cursor
        if (cursor.empty()) { cursor.id = craft_result.id; cursor.count = craft_result.count; }
        else cursor.count += craft_result.count;
        craft_result = {};
    }

    // Move in/out of an armor slot. The cursor may only DROP an item whose
    // armor_slot matches this slot; picking up an equipped piece is always ok.
    void move_armor(ItemStack& slot, ArmorSlot want) {
        if (cursor.empty()) {                  // pick the worn piece up
            if (!slot.empty()) { cursor = slot; slot.clear(); }
            return;
        }
        if (item_armor_slot(cursor.id) != want) return;   // wrong piece for this slot
        std::swap(slot, cursor);               // equip (swap with whatever's worn)
    }

    // Right-click quick-equip from an inventory slot into its armor slot.
    bool try_quick_equip(Inventory& inv, ItemStack& s) {
        if (s.empty()) return false;
        ArmorSlot sl = item_armor_slot(s.id);
        if (sl == ArmorSlot::None) return false;
        int ai = (int)sl - 1;                  // Head=1..Feet=4 -> 0..3
        if (!inv.armor[ai].empty()) std::swap(inv.armor[ai], s);   // swap with worn
        else { inv.armor[ai] = s; s.clear(); }
        return true;
    }

    // --- layout (virtual px scaled by s_) -----------------------------------
    void layout(int fbw, int fbh) {
        s_ = std::max(2.0f, fbh / 240.0f);
        const float cell = 20.0f * s_;          // slot pitch
        float pw = INV_COLS * cell + 40*s_;     // wider to fit craft grid on left
        float ph = (INV_ROWS + 1) * cell + 80*s_;
        float px = (fbw - pw) * 0.5f;
        float py = (fbh - ph) * 0.5f;
        panel_ = {px, py, pw, ph};

        float gx = px + 50*s_;                  // main grid starts after craft area
        float top = py + 50*s_;
        // main inventory 3x9 (slots 9..35)
        for (int r=0;r<INV_ROWS;r++) for (int c=0;c<INV_COLS;c++) {
            int idx = HOTBAR_SLOTS + r*INV_COLS + c;
            slot_rect_[idx] = {gx + c*cell, top + r*cell, cell-2*s_, cell-2*s_};
        }
        // hotbar row (slots 0..8) below, with a small gap
        float hb = top + INV_ROWS*cell + 6*s_;
        for (int c=0;c<HOTBAR_SLOTS;c++)
            slot_rect_[c] = {gx + c*cell, hb, cell-2*s_, cell-2*s_};

        // --- LEFT SIDE: armor (VERTICAL) + 2×2 craft grid ---
        // Armor slots: VERTICAL column (head/chest/legs/feet top-to-bottom)
        float agx = px + 8*s_, agy = py + 8*s_;
        for (int i=0;i<4;i++)
            armor_rect_[i] = {agx, agy + i*cell, cell-2*s_, cell-2*s_};

        // 2×2 crafting grid below armor (or beside, your choice; below is cleaner)
        float cgx = agx, cgy = agy + 4*cell + 8*s_;
        craft_rect_[0] = {cgx,          cgy,          cell-2*s_, cell-2*s_};  // TL
        craft_rect_[1] = {cgx + cell,   cgy,          cell-2*s_, cell-2*s_};  // TR
        craft_rect_[2] = {cgx,          cgy + cell,   cell-2*s_, cell-2*s_};  // BL
        craft_rect_[3] = {cgx + cell,   cgy + cell,   cell-2*s_, cell-2*s_};  // BR
        // craft result slot to the right of the 2×2 grid
        craft_result_rect_ = {cgx + 2*cell + 8*s_, cgy + cell*0.5f, cell-2*s_, cell-2*s_};
    }

    static bool hit(const glm::vec4& r, float mx, float my) {
        return mx>=r.x && mx<r.x+r.z && my>=r.y && my<r.y+r.w;
    }

    // --- drawing primitives (pixel rects -> NDC) ----------------------------
    void quad(float x, float y, float w, float h, glm::vec4 c) {
        float x0=x/fbw_*2-1, x1=(x+w)/fbw_*2-1;
        float y0=1-y/fbh_*2, y1=1-(y+h)/fbh_*2;
        push(x0,y0,0,0,c); push(x1,y0,0,0,c); push(x1,y1,0,0,c);
        push(x0,y0,0,0,c); push(x1,y1,0,0,c); push(x0,y1,0,0,c);
    }
    void icon(float x, float y, float w, float h, ItemId id) {
        IconRef ir = icon_for(id);
        if (!ir.ok) { // fallback: block colour swatch
            glm::vec3 col = item_is_block(id) ? block_def(item_block(id)).color : glm::vec3(0.8f);
            quad(x,y,w,h, glm::vec4(col,1)); return;
        }
        // Blocks render as a 3D iso cube (terrain.png faces, shaded); items flat.
        if (item_is_block(id) && tex_terrain_) {
            emit_iso_cube(pending_iso_, x, y, w, h,
                          block_face(item_block(id), 0), block_face(item_block(id), 1));
            return;
        }
        // tag verts with atlas via the .a channel sign? simpler: flush per-atlas.
        // We bucket by atlas using two passes in flush(); encode atlas in w of uv.
        pending_.push_back({x,y,w,h,ir});
    }

    struct Pend { float x,y,w,h; IconRef ir; };
    std::vector<Pend> pending_;
    std::vector<IsoTri> pending_iso_;   // 3D block cube tris (terrain.png, shaded)

    // Append a glyph quad (MC font default.png: 128x128, 16x16 grid of 8px cells).
    void glyph(char ch, float dx, float dy, float px, glm::vec4 tint) {
        int c=(unsigned char)ch, col=c&15, row=c>>4;
        const float inv=1.0f/128.0f;
        float u0=(col*8)*inv, v0=(row*8)*inv, u1=(col*8+8)*inv, v1=(row*8+8)*inv;
        float x0=dx/fbw_*2-1, x1=(dx+px)/fbw_*2-1;
        float y0=1-dy/fbh_*2, y1=1-(dy+px)/fbh_*2;
        auto v=[&](float vx,float vy,float u,float vv){ text_.insert(text_.end(),{vx,vy,u,vv,tint.r,tint.g,tint.b,tint.a}); };
        v(x0,y0,u0,v0); v(x1,y0,u1,v0); v(x1,y1,u1,v1);
        v(x0,y0,u0,v0); v(x1,y1,u1,v1); v(x0,y1,u0,v1);
    }
    // Stack count in a slot's bottom-right (white digits + dark shadow), skips <=1.
    void draw_count(float sx, float sy, float sw, float sh, int count) {
        if (count <= 1 || !tex_font_) return;
        char d[4]; int n=0, v=count>999?999:count;
        do { d[n++]=(char)('0'+v%10); v/=10; } while(v && n<4);
        float gp = 8.0f * s_ * 0.6f; if (gp < 8.0f) gp = 8.0f;
        float gx = sx + sw - 2.0f - gp, gy = sy + sh - 2.0f - gp;
        for (int i=0;i<n;i++){ float x=gx - i*(gp*0.75f);
            glyph(d[i], x+1, gy+1, gp, glm::vec4(0.13f,0.13f,0.13f,1));
            glyph(d[i], x, gy, gp, glm::vec4(1,1,1,1)); }
    }

    void flush() {
        glUseProgram(prog_);
        glBindVertexArray(vao_); glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        GLboolean depth_was = glIsEnabled(GL_DEPTH_TEST);
        GLboolean cull_was  = glIsEnabled(GL_CULL_FACE);
        glDisable(GL_DEPTH_TEST); glEnable(GL_BLEND);
        glDisable(GL_CULL_FACE);   // 2D quads wind CW in NDC; the world pass leaves
                                   // back-face culling on, which would cull them all.
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        GLint loc_use = glGetUniformLocation(prog_, "u_use_tex");
        glActiveTexture(GL_TEXTURE0); glUniform1i(glGetUniformLocation(prog_,"u_tex"),0);

        // pass 1: untextured panel/slots
        glUniform1f(loc_use, 0.0f);
        glBindTexture(GL_TEXTURE_2D, white_);
        upload(); glDrawArrays(GL_TRIANGLES,0,(GLsizei)(verts_.size()/8));

        // pass 2: icons, bucketed by atlas
        for (int atlas=0; atlas<2; atlas++) {
            verts_.clear();
            for (auto& p : pending_) {
                if (p.ir.atlas != atlas) continue;
                float x0=p.x/fbw_*2-1, x1=(p.x+p.w)/fbw_*2-1;
                float y0=1-p.y/fbh_*2, y1=1-(p.y+p.h)/fbh_*2;
                glm::vec4 w(1);
                push(x0,y0,p.ir.u0,p.ir.v0,w); push(x1,y0,p.ir.u1,p.ir.v0,w); push(x1,y1,p.ir.u1,p.ir.v1,w);
                push(x0,y0,p.ir.u0,p.ir.v0,w); push(x1,y1,p.ir.u1,p.ir.v1,w); push(x0,y1,p.ir.u0,p.ir.v1,w);
            }
            // 3D block cubes share the terrain atlas; emit their shaded tris here.
            if (atlas == ICON_ATLAS_TERRAIN) {
                for (const IsoTri& t : pending_iso_) {
                    float px = t.x/fbw_*2-1, py = 1-t.y/fbh_*2;
                    push(px, py, t.u, t.v, t.tint);
                }
            }
            if (verts_.empty()) continue;
            glUniform1f(loc_use, 1.0f);
            glBindTexture(GL_TEXTURE_2D, atlas==ICON_ATLAS_TERRAIN ? tex_terrain_ : tex_items_);
            upload(); glDrawArrays(GL_TRIANGLES,0,(GLsizei)(verts_.size()/8));
        }
        // pass 3: stack-count glyphs (font sheet) on top of everything
        if (!text_.empty() && tex_font_) {
            glUniform1f(loc_use, 1.0f);
            glBindTexture(GL_TEXTURE_2D, tex_font_);
            verts_ = text_;
            upload(); glDrawArrays(GL_TRIANGLES,0,(GLsizei)(verts_.size()/8));
        }
        pending_.clear(); verts_.clear(); text_.clear(); pending_iso_.clear();
        glBindVertexArray(0);
        if (depth_was) glEnable(GL_DEPTH_TEST);
        if (cull_was)  glEnable(GL_CULL_FACE);
    }

    void upload() {
        glBufferData(GL_ARRAY_BUFFER, verts_.size()*sizeof(float), verts_.data(), GL_STREAM_DRAW);
    }
    void push(float x,float y,float u,float v,glm::vec4 c){
        verts_.insert(verts_.end(), {x,y,u,v,c.r,c.g,c.b,c.a});
    }

    static GLuint load_tex(const char* path) {
        int w,h,ch; stbi_set_flip_vertically_on_load(0);
        unsigned char* d = stbi_load(path,&w,&h,&ch,4);
        if (!d){ std::cerr<<"[sdfcraft] inv screen missing "<<path<<"\n"; return 0; }
        GLuint t; glGenTextures(1,&t); glBindTexture(GL_TEXTURE_2D,t);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,d);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        stbi_image_free(d); return t;
    }
    static GLuint make_white() {
        GLuint t; glGenTextures(1,&t); glBindTexture(GL_TEXTURE_2D,t);
        unsigned char px[4]={255,255,255,255};
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,px);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        return t;
    }
    static GLuint compile(GLenum t,const char* s){ GLuint sh=glCreateShader(t);
        glShaderSource(sh,1,&s,nullptr); glCompileShader(sh);
        GLint ok=0; glGetShaderiv(sh,GL_COMPILE_STATUS,&ok);
        if(!ok){char log[512];glGetShaderInfoLog(sh,512,nullptr,log);std::cerr<<"[sdfcraft] inv shader: "<<log<<"\n";}
        return sh; }
    static GLuint link(const char* vs,const char* fs){ GLuint v=compile(GL_VERTEX_SHADER,vs),f=compile(GL_FRAGMENT_SHADER,fs);
        GLuint p=glCreateProgram(); glAttachShader(p,v); glAttachShader(p,f); glLinkProgram(p);
        GLint ok=0; glGetProgramiv(p,GL_LINK_STATUS,&ok);
        if(!ok){char log[512];glGetProgramInfoLog(p,512,nullptr,log);std::cerr<<"[sdfcraft] inv link: "<<log<<"\n";p=0;}
        glDeleteShader(v);glDeleteShader(f); return p; }
};

} // namespace sdfcraft
