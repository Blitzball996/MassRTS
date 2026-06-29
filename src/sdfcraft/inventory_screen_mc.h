#pragma once
// =============================================================================
// SDFCraft - Inventory screen (EXACT Minecraft Console Edition layout)
// -----------------------------------------------------------------------------
// Directly ported from MinecraftConsoles InventoryMenu.cpp layout coordinates.
// Slot indices match MC exactly:
//   0 = result slot (craft output)
//   1-4 = 2×2 craft grid
//   5-8 = armor (helmet/chest/legs/boots)
//   9-35 = main inventory (3×9)
//   36-44 = hotbar (9 slots)
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

// Minecraft Console Edition exact slot layout from InventoryMenu.cpp
struct MCSlotLayout {
    static constexpr int RESULT_SLOT = 0;
    static constexpr int CRAFT_SLOT_START = 1;
    static constexpr int CRAFT_SLOT_END = 5;        // 1-4 (2×2)
    static constexpr int ARMOR_SLOT_START = 5;
    static constexpr int ARMOR_SLOT_END = 9;        // 5-8 (4 armor)
    static constexpr int INV_SLOT_START = 9;
    static constexpr int INV_SLOT_END = 36;         // 9-35 (3×9)
    static constexpr int USE_ROW_SLOT_START = 36;
    static constexpr int USE_ROW_SLOT_END = 45;     // 36-44 (hotbar)
};

class InventoryScreenMC {
public:
    ItemStack cursor;           // carried item (mouse)
    ItemStack craft_grid[4];    // 2×2 craft slots (indices 1-4)
    CraftResult craft_result;   // result slot (index 0)

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
        tex_font_    = load_tex("assets/textures/gui/default.png");
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

    void returnAll(Inventory& inv) {
        if(!cursor.empty()){ inv.add(cursor.id, cursor.count, item_max_stack(cursor.id)); cursor.clear(); }
        for (int i=0; i<4; i++) if (!craft_grid[i].empty()) {
            inv.add(craft_grid[i].id, craft_grid[i].count, item_max_stack(craft_grid[i].id));
            craft_grid[i].clear();
        }
        craft_result = {};
    }

    bool click(Inventory& inv, RecipeBook& recipes, int fbw, int fbh,
               float mx, float my, bool right) {
        layout(fbw, fbh);

        // MC slot index order: result, craft grid, armor, main inv, hotbar
        // Result slot (index 0)
        if (hit(slot_rects_[0], mx, my) && craft_result.id != ITEM_NONE) {
            take_craft_result(inv);
            recompute_craft(recipes);
            return true;
        }
        // Craft grid (indices 1-4)
        for (int i=0; i<4; i++) {
            if (hit(slot_rects_[1+i], mx, my)) {
                move(craft_grid[i], right);
                recompute_craft(recipes);
                return true;
            }
        }
        // Armor slots (indices 5-8)
        for (int i=0; i<4; i++) {
            if (hit(slot_rects_[5+i], mx, my)) {
                move_armor(inv.armor[i], (ArmorSlot)(i+1));
                return true;
            }
        }
        // Main inventory (indices 9-35)
        for (int i=0; i<27; i++) {
            int inv_idx = 9 + i;  // MC: slots 9..35
            if (hit(slot_rects_[inv_idx], mx, my)) {
                if (cursor.empty() && right && try_quick_equip(inv, inv.slots[inv_idx])) return true;
                move(inv.slots[inv_idx], right);
                return true;
            }
        }
        // Hotbar (indices 36-44)
        for (int i=0; i<9; i++) {
            int hb_idx = 36 + i;
            if (hit(slot_rects_[hb_idx], mx, my)) {
                if (cursor.empty() && right && try_quick_equip(inv, inv.slots[i])) return true;
                move(inv.slots[i], right);
                return true;
            }
        }
        return false;
    }

    void draw(Inventory& inv, RecipeBook& recipes, int fbw, int fbh, float mx, float my) {
        layout(fbw, fbh);
        recompute_craft(recipes);
        fbw_=(float)fbw; fbh_=(float)fbh;
        verts_.clear(); text_.clear();

        quad(0,0,(float)fbw,(float)fbh, glm::vec4(0,0,0,0.5f));
        quad(panel_.x, panel_.y, panel_.z, panel_.w, glm::vec4(0.12f,0.12f,0.14f,0.97f));

        auto draw_slot = [&](const glm::vec4& r, const ItemStack& st){
            quad(r.x, r.y, r.z, r.w, glm::vec4(0.15f,0.15f,0.17f,1.0f));
            quad(r.x+1*s_, r.y+1*s_, r.z-2*s_, r.w-2*s_, glm::vec4(0.30f,0.30f,0.33f,1.0f));
            if (!st.empty()) {
                icon(r.x+1*s_, r.y+1*s_, r.z-2*s_, r.w-2*s_, st.id);
                draw_count(r.x+1*s_, r.y+1*s_, r.z-2*s_, r.w-2*s_, st.count);
            }
        };

        // Draw all 45 slots in MC order
        draw_slot(slot_rects_[0], ItemStack{craft_result.id, craft_result.count}); // result
        for (int i=0; i<4; i++) draw_slot(slot_rects_[1+i], craft_grid[i]);        // craft
        for (int i=0; i<4; i++) draw_slot(slot_rects_[5+i], inv.armor[i]);         // armor
        for (int i=0; i<27; i++) draw_slot(slot_rects_[9+i], inv.slots[9+i]);      // main inv
        for (int i=0; i<9; i++) draw_slot(slot_rects_[36+i], inv.slots[i]);        // hotbar

        if (!cursor.empty()) {
            icon(mx-8*s_, my-8*s_, 16*s_, 16*s_, cursor.id);
            draw_count(mx-8*s_, my-8*s_, 16*s_, 16*s_, cursor.count);
        }

        flush();
    }

private:
    GLuint prog_=0, vao_=0, vbo_=0, tex_terrain_=0, tex_items_=0, white_=0, tex_font_=0;
    std::vector<float> verts_, text_;
    float fbw_=0, fbh_=0, s_=1.0f;
    glm::vec4 panel_{0,0,0,0};
    glm::vec4 slot_rects_[45];  // MC: 45 total slots

    // Layout using EXACT MC coordinates from InventoryMenu.cpp
    void layout(int fbw, int fbh) {
        s_ = std::max(1.0f, fbh / 256.0f);  // MC scale
        float cell = 18.0f * s_;             // MC slot size

        float pw = 176.0f * s_;  // MC inventory width
        float ph = 166.0f * s_;  // MC inventory height
        float px = (fbw - pw) * 0.5f;
        float py = (fbh - ph) * 0.5f;
        panel_ = {px, py, pw, ph};

        // Slot 0: Result slot (144, 36) from InventoryMenu.cpp line 35
        slot_rects_[0] = {px + 144*s_, py + 36*s_, cell, cell};

        // Slots 1-4: 2×2 craft grid (88+x*18, 26+y*18) from lines 37-42
        for (int y=0; y<2; y++) {
            for (int x=0; x<2; x++) {
                int idx = 1 + x + y*2;
                slot_rects_[idx] = {px + (88+x*18)*s_, py + (26+y*18)*s_, cell, cell};
            }
        }

        // Slots 5-8: Armor (8, 8+i*18) from lines 45-50
        for (int i=0; i<4; i++) {
            slot_rects_[5+i] = {px + 8*s_, py + (8+i*18)*s_, cell, cell};
        }

        // Slots 9-35: Main inventory 3×9 (8+x*18, 84+y*18) from lines 52-57
        for (int y=0; y<3; y++) {
            for (int x=0; x<9; x++) {
                int idx = 9 + x + y*9;
                slot_rects_[idx] = {px + (8+x*18)*s_, py + (84+y*18)*s_, cell, cell};
            }
        }

        // Slots 36-44: Hotbar (8+x*18, 142) from lines 59-61
        for (int x=0; x<9; x++) {
            slot_rects_[36+x] = {px + (8+x*18)*s_, py + 142*s_, cell, cell};
        }
    }

    void move(ItemStack& slot, bool right) {
        if (cursor.empty()) {
            if (slot.empty()) return;
            if (right) {
                uint8_t half = (uint8_t)((slot.count+1)/2);
                cursor.id = slot.id; cursor.count = half;
                slot.count -= half; if (slot.count==0) slot.clear();
            } else { cursor = slot; slot.clear(); }
        } else {
            if (slot.empty()) {
                if (right) { slot.id=cursor.id; slot.count=1; if(--cursor.count==0) cursor.clear(); }
                else { slot = cursor; cursor.clear(); }
            } else if (slot.id == cursor.id) {
                uint8_t mx = item_max_stack(slot.id);
                uint8_t room = (uint8_t)(mx - slot.count);
                uint8_t mv = right ? 1 : (uint8_t)std::min<int>(room,cursor.count);
                mv = (uint8_t)std::min<int>(mv, room);
                slot.count += mv; cursor.count -= mv; if (cursor.count==0) cursor.clear();
            } else { std::swap(slot, cursor); }
        }
    }

    void move_armor(ItemStack& slot, ArmorSlot want) {
        if (cursor.empty()) {
            if (!slot.empty()) { cursor = slot; slot.clear(); }
            return;
        }
        if (item_armor_slot(cursor.id) != want) return;
        std::swap(slot, cursor);
    }

    bool try_quick_equip(Inventory& inv, ItemStack& s) {
        if (s.empty()) return false;
        ArmorSlot sl = item_armor_slot(s.id);
        if (sl == ArmorSlot::None) return false;
        int ai = (int)sl - 1;
        if (!inv.armor[ai].empty()) std::swap(inv.armor[ai], s);
        else { inv.armor[ai] = s; s.clear(); }
        return true;
    }

    void recompute_craft(RecipeBook& recipes) {
        ItemId grid[4] = {
            craft_grid[0].empty() ? ITEM_NONE : craft_grid[0].id,
            craft_grid[1].empty() ? ITEM_NONE : craft_grid[1].id,
            craft_grid[2].empty() ? ITEM_NONE : craft_grid[2].id,
            craft_grid[3].empty() ? ITEM_NONE : craft_grid[3].id
        };
        craft_result = recipes.match(grid, 2, 2);
    }

    void take_craft_result(Inventory& inv) {
        if (craft_result.id == ITEM_NONE) return;
        if (!cursor.empty() && cursor.id != craft_result.id) return;
        uint8_t mx = item_max_stack(craft_result.id);
        if (cursor.count + craft_result.count > mx) return;
        for (int i=0; i<4; i++) {
            if (!craft_grid[i].empty()) {
                craft_grid[i].count--;
                if (craft_grid[i].count == 0) craft_grid[i].clear();
            }
        }
        if (cursor.empty()) { cursor.id = craft_result.id; cursor.count = craft_result.count; }
        else cursor.count += craft_result.count;
        craft_result = {};
    }

    static bool hit(const glm::vec4& r, float mx, float my) {
        return mx>=r.x && mx<r.x+r.z && my>=r.y && my<r.y+r.w;
    }

    void quad(float x,float y,float w,float h,glm::vec4 c){
        float x0=x/fbw_*2-1,x1=(x+w)/fbw_*2-1,y0=1-y/fbh_*2,y1=1-(y+h)/fbh_*2;
        push(x0,y0,0,0,c);push(x1,y0,0,0,c);push(x1,y1,0,0,c);
        push(x0,y0,0,0,c);push(x1,y1,0,0,c);push(x0,y1,0,0,c);
    }

    struct Pend { float x,y,w,h; IconRef ir; bool iso; BlockId b; };
    std::vector<Pend> pending_;

    void icon(float x,float y,float w,float h,ItemId id){
        if (id==ITEM_NONE) return;
        if (item_is_block(id)) { pending_.push_back({x,y,w,h,{},true,item_block(id)}); return; }
        IconRef ir=icon_for(id);
        if (!ir.ok){ glm::vec3 c(0.8f); quad(x,y,w,h,glm::vec4(c,1)); return; }
        pending_.push_back({x,y,w,h,ir,false,BLOCK_AIR});
    }

    void glyph(char ch,float dx,float dy,float px,glm::vec4 t){
        int c=(unsigned char)ch,col=c&15,row=c>>4; const float inv=1.0f/128.0f;
        float u0=(col*8)*inv,v0=(row*8)*inv,u1=(col*8+8)*inv,v1=(row*8+8)*inv;
        float x0=dx/fbw_*2-1,x1=(dx+px)/fbw_*2-1,y0=1-dy/fbh_*2,y1=1-(dy+px)/fbh_*2;
        auto v=[&](float vx,float vy,float u,float vv){ text_.insert(text_.end(),{vx,vy,u,vv,t.r,t.g,t.b,t.a}); };
        v(x0,y0,u0,v0);v(x1,y0,u1,v0);v(x1,y1,u1,v1); v(x0,y0,u0,v0);v(x1,y1,u1,v1);v(x0,y1,u0,v1);
    }

    void draw_count(float sx,float sy,float sw,float sh,int count){
        if (count<=1||!tex_font_) return;
        char d[4]; int n=0,v=count>999?999:count; do{ d[n++]=(char)('0'+v%10); v/=10; }while(v&&n<4);
        float gp=8.0f*s_*0.6f; if(gp<8)gp=8;
        float gx=sx+sw-2-gp, gy=sy+sh-2-gp;
        for(int i=0;i<n;i++){ float x=gx-i*(gp*0.75f);
            glyph(d[i],x+1,gy+1,gp,glm::vec4(0.13f,0.13f,0.13f,1)); glyph(d[i],x,gy,gp,glm::vec4(1,1,1,1)); }
    }

    void flush() {
        glUseProgram(prog_); glBindVertexArray(vao_); glBindBuffer(GL_ARRAY_BUFFER,vbo_);
        GLboolean depth_was=glIsEnabled(GL_DEPTH_TEST), cull_was=glIsEnabled(GL_CULL_FACE);
        glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
        GLint loc=glGetUniformLocation(prog_,"u_use_tex");
        glActiveTexture(GL_TEXTURE0); glUniform1i(glGetUniformLocation(prog_,"u_tex"),0);
        glUniform1f(loc,0.0f); glBindTexture(GL_TEXTURE_2D,white_);
        upload(); glDrawArrays(GL_TRIANGLES,0,(GLsizei)(verts_.size()/8));

        for (int atlas=0; atlas<2; atlas++) {
            verts_.clear();
            for (auto& p:pending_){ if(p.iso||p.ir.atlas!=atlas) continue;
                float x0=p.x/fbw_*2-1,x1=(p.x+p.w)/fbw_*2-1,y0=1-p.y/fbh_*2,y1=1-(p.y+p.h)/fbh_*2; glm::vec4 w(1);
                push(x0,y0,p.ir.u0,p.ir.v0,w);push(x1,y0,p.ir.u1,p.ir.v0,w);push(x1,y1,p.ir.u1,p.ir.v1,w);
                push(x0,y0,p.ir.u0,p.ir.v0,w);push(x1,y1,p.ir.u1,p.ir.v1,w);push(x0,y1,p.ir.u0,p.ir.v1,w); }
            if(verts_.empty()) continue;
            glUniform1f(loc,1.0f); glBindTexture(GL_TEXTURE_2D, atlas==ICON_ATLAS_TERRAIN?tex_terrain_:tex_items_);
            upload(); glDrawArrays(GL_TRIANGLES,0,(GLsizei)(verts_.size()/8));
        }

        verts_.clear();
        for (auto& p:pending_){ if(!p.iso) continue;
            std::vector<IsoTri> tris; emit_iso_cube(tris, p.x,p.y,p.w,p.h, block_face(p.b,0), block_face(p.b,1));
            for (auto& t:tris){ float x=t.x/fbw_*2-1,y=1-t.y/fbh_*2; push(x,y,t.u,t.v,t.tint); } }
        if(!verts_.empty()){ glUniform1f(loc,1.0f); glBindTexture(GL_TEXTURE_2D,tex_terrain_);
            upload(); glDrawArrays(GL_TRIANGLES,0,(GLsizei)(verts_.size()/8)); }

        if(!text_.empty()&&tex_font_){ glUniform1f(loc,1.0f); glBindTexture(GL_TEXTURE_2D,tex_font_);
            verts_=text_; upload(); glDrawArrays(GL_TRIANGLES,0,(GLsizei)(verts_.size()/8)); }
        pending_.clear(); verts_.clear(); text_.clear();
        glBindVertexArray(0);
        if(depth_was) glEnable(GL_DEPTH_TEST); if(cull_was) glEnable(GL_CULL_FACE);
    }

    void upload(){ glBufferData(GL_ARRAY_BUFFER, verts_.size()*sizeof(float), verts_.data(), GL_STREAM_DRAW); }
    void push(float x,float y,float u,float v,glm::vec4 c){ verts_.insert(verts_.end(),{x,y,u,v,c.r,c.g,c.b,c.a}); }

    static GLuint load_tex(const char* path){ int w,h,ch; stbi_set_flip_vertically_on_load(0);
        unsigned char* d=stbi_load(path,&w,&h,&ch,4); if(!d){ std::cerr<<"[sdfcraft] inv screen missing "<<path<<"\n"; return 0; }
        GLuint t; glGenTextures(1,&t); glBindTexture(GL_TEXTURE_2D,t);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,d);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        stbi_image_free(d); return t; }
    static GLuint make_white(){ GLuint t; glGenTextures(1,&t); glBindTexture(GL_TEXTURE_2D,t);
        unsigned char px[4]={255,255,255,255}; glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,px);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST); return t; }
    static GLuint compile(GLenum t,const char* s){ GLuint sh=glCreateShader(t); glShaderSource(sh,1,&s,nullptr); glCompileShader(sh);
        GLint ok=0; glGetShaderiv(sh,GL_COMPILE_STATUS,&ok); if(!ok){char l[512];glGetShaderInfoLog(sh,512,nullptr,l);std::cerr<<"[sdfcraft] inv shader: "<<l<<"\n";} return sh; }
    static GLuint link(const char* vs,const char* fs){ GLuint v=compile(GL_VERTEX_SHADER,vs),f=compile(GL_FRAGMENT_SHADER,fs);
        GLuint p=glCreateProgram(); glAttachShader(p,v);glAttachShader(p,f);glLinkProgram(p);
        GLint ok=0; glGetProgramiv(p,GL_LINK_STATUS,&ok); if(!ok){char l[512];glGetProgramInfoLog(p,512,nullptr,l);std::cerr<<"[sdfcraft] inv link: "<<l<<"\n";p=0;}
        glDeleteShader(v);glDeleteShader(f); return p; }
};

} // namespace sdfcraft
