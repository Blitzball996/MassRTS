#pragma once
// =============================================================================
// SDFCraft - R Key Crafting Screen (Exact Minecraft Console Edition Layout)
// -----------------------------------------------------------------------------
// EXACT layout from MinecraftConsoles HowToPlay_Crafting.png:
//  - Top: Horizontal scrolling recipe cards (3-4 visible at once)
//  - Each card shows: ingredient grid layout + arrow + result item
//  - Cards are color-coded: bright = craftable, dim = missing materials
//  - Bottom: Player inventory + hotbar (for material reference)
//  - Left/Right buttons or scroll to navigate recipes
//
// Based on MinecraftConsoles UIScene_CraftingMenu horizontal slot system.
// =============================================================================
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <vector>
#include <algorithm>
#include <iostream>
#include "inventory.h"
#include "items.h"
#include "blocks.h"
#include "crafting.h"
#include "item_icons.h"
#include "../stb_image.h"

namespace sdfcraft {

class CraftingScreenExact {
public:
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

    void scroll(int dir) {
        scroll_ = std::max(0, scroll_ - dir);
    }

    bool click(Inventory& inv, RecipeBook& recipes, int fbw, int fbh, float mx, float my) {
        layout(fbw, fbh);
        const auto& rs = recipes.all();

        // Check if clicked on a recipe card
        for (int vis = 0; vis < cards_visible_; vis++) {
            int ri = scroll_ + vis;
            if (ri >= (int)rs.size()) break;
            glm::vec4 card = card_rect_(vis);
            if (mx>=card.x && mx<card.x+card.z && my>=card.y && my<card.y+card.w) {
                if (affordable(inv, rs[ri])) {
                    craft(inv, rs[ri]);
                    return true;
                }
            }
        }
        return false;
    }

    void draw(Inventory& inv, RecipeBook& recipes, int fbw, int fbh, float mx, float my) {
        layout(fbw, fbh);
        fbw_=(float)fbw; fbh_=(float)fbh;
        verts_.clear(); text_.clear(); pending_.clear();

        // Background
        quad(0,0,(float)fbw,(float)fbh, glm::vec4(0,0,0,0.6f));

        // Main panel
        quad(panel_.x, panel_.y, panel_.z, panel_.w, glm::vec4(0.1f,0.1f,0.12f,0.95f));

        // Title
        quad(panel_.x, panel_.y, panel_.z, 20*s_, glm::vec4(0.18f,0.18f,0.22f,1));
        draw_text("CRAFTING", panel_.x + 8*s_, panel_.y + 6*s_, 1.2f, glm::vec4(1,1,1,1));

        const auto& rs = recipes.all();

        // Draw recipe cards (horizontal scrolling)
        for (int vis = 0; vis < cards_visible_; vis++) {
            int ri = scroll_ + vis;
            if (ri >= (int)rs.size()) break;
            const Recipe& rc = rs[ri];
            glm::vec4 card = card_rect_(vis);
            bool ok = affordable(inv, rc);
            bool hover = (mx>=card.x && mx<card.x+card.z && my>=card.y && my<card.y+card.w);

            // Card background (bright if craftable, dim if not)
            glm::vec4 bg = ok ? (hover ? glm::vec4(0.3f,0.35f,0.3f,1) : glm::vec4(0.2f,0.25f,0.22f,1))
                              : glm::vec4(0.14f,0.12f,0.12f,0.8f);
            quad(card.x, card.y, card.z, card.w, bg);

            // Card border
            float bw = 2*s_;
            glm::vec4 border_col = ok ? glm::vec4(0.5f,0.6f,0.5f,1) : glm::vec4(0.3f,0.3f,0.3f,1);
            quad(card.x, card.y, card.z, bw, border_col); // top
            quad(card.x, card.y+card.w-bw, card.z, bw, border_col); // bottom
            quad(card.x, card.y, bw, card.w, border_col); // left
            quad(card.x+card.z-bw, card.y, bw, card.w, border_col); // right

            float cx = card.x + 8*s_;
            float cy = card.y + 8*s_;
            float cell = 14*s_;

            // Determine grid size (2×2 or 3×3)
            int grid_w = 3, grid_h = 3;
            if (!rc.shapeless) {
                // Use actual recipe dimensions
                grid_w = rc.w;
                grid_h = rc.h;
            }

            // Draw ingredient grid
            for (int y=0; y<grid_h; y++) {
                for (int x=0; x<grid_w; x++) {
                    int idx = x + y*grid_w;
                    if (idx >= 9) break;
                    ItemId item = (idx < (int)rc.pattern.size()) ? rc.pattern[idx] : ITEM_NONE;
                    float gx = cx + x*16*s_;
                    float gy = cy + y*16*s_;

                    // Slot background
                    quad(gx, gy, cell, cell, glm::vec4(0.2f,0.2f,0.22f,0.8f));

                    if (item != ITEM_NONE) {
                        icon(gx+1*s_, gy+1*s_, cell-2*s_, cell-2*s_, item);
                    }
                }
            }

            // Arrow
            float arrow_x = cx + grid_w*16*s_ + 4*s_;
            float arrow_y = cy + grid_h*8*s_ - 4*s_;
            draw_text("->", arrow_x, arrow_y, 0.8f, glm::vec4(0.8f,0.8f,0.8f,1));

            // Result
            float res_x = arrow_x + 16*s_;
            float res_y = cy + grid_h*8*s_ - 8*s_;
            quad(res_x, res_y, cell, cell, glm::vec4(0.25f,0.25f,0.27f,1));
            icon(res_x+1*s_, res_y+1*s_, cell-2*s_, cell-2*s_, rc.result.id);
            draw_count(res_x+1*s_, res_y+1*s_, cell-2*s_, cell-2*s_, rc.result.count);
        }

        // Scroll indicators
        if (scroll_ > 0) {
            draw_text("<", panel_.x + 4*s_, panel_.y + 50*s_, 1.5f, glm::vec4(1,1,0,1));
        }
        if (scroll_ + cards_visible_ < (int)rs.size()) {
            draw_text(">", panel_.x + panel_.z - 12*s_, panel_.y + 50*s_, 1.5f, glm::vec4(1,1,0,1));
        }

        // Bottom: player inventory display (read-only reference)
        float inv_y = panel_.y + panel_.w - 60*s_;
        draw_text("INVENTORY", panel_.x + 8*s_, inv_y - 12*s_, 0.8f, glm::vec4(0.7f,0.7f,0.7f,1));

        // Show first 9 hotbar slots as reference
        float hb_x = panel_.x + 8*s_;
        float hb_y = inv_y;
        float cell = 14*s_;
        for (int i=0; i<9; i++) {
            float sx = hb_x + i*16*s_;
            quad(sx, hb_y, cell, cell, glm::vec4(0.15f,0.15f,0.17f,0.6f));
            if (!inv.slots[i].empty()) {
                icon(sx+1*s_, hb_y+1*s_, cell-2*s_, cell-2*s_, inv.slots[i].id);
                draw_count(sx+1*s_, hb_y+1*s_, cell-2*s_, cell-2*s_, inv.slots[i].count);
            }
        }

        flush();
    }

private:
    GLuint prog_=0, vao_=0, vbo_=0, tex_terrain_=0, tex_items_=0, white_=0, tex_font_=0;
    std::vector<float> verts_, text_;
    float fbw_=0, fbh_=0, s_=2.0f;
    glm::vec4 panel_{0,0,0,0};
    int scroll_=0, cards_visible_=0;
    float card_w_=0, card_h_=0;

    void layout(int fbw, int fbh) {
        s_ = std::max(2.0f, fbh/240.0f);
        float pw = 280*s_, ph = 160*s_;
        panel_ = {(fbw-pw)*0.5f, (fbh-ph)*0.5f, pw, ph};

        // Recipe cards (horizontal)
        card_w_ = 80*s_;
        card_h_ = 70*s_;
        cards_visible_ = 3;  // show 3 cards at once
    }

    glm::vec4 card_rect_(int vis) const {
        float x = panel_.x + 10*s_ + vis*(card_w_ + 8*s_);
        float y = panel_.y + 25*s_;
        return {x, y, card_w_, card_h_};
    }

    bool affordable(const Inventory& inv, const Recipe& rc) const {
        for (auto& nd : RecipeBook::recipe_needs(rc))
            if (inv.count(nd.id) < nd.count) return false;
        return true;
    }

    void craft(Inventory& inv, const Recipe& rc) {
        for (auto& nd : RecipeBook::recipe_needs(rc)) inv.remove(nd.id, nd.count);
        inv.add(rc.result.id, rc.result.count, item_max_stack(rc.result.id));
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

    void draw_text(const char* str, float x, float y, float scale, glm::vec4 c) {
        float gp = 8.0f * s_ * scale;
        for (int i=0; str[i]; i++) {
            glyph(str[i], x + i*gp*0.6f, y, gp, c);
        }
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
        unsigned char* d=stbi_load(path,&w,&h,&ch,4); if(!d){ std::cerr<<"[sdfcraft] craft screen missing "<<path<<"\n"; return 0; }
        GLuint t; glGenTextures(1,&t); glBindTexture(GL_TEXTURE_2D,t);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,d);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        stbi_image_free(d); return t; }
    static GLuint make_white(){ GLuint t; glGenTextures(1,&t); glBindTexture(GL_TEXTURE_2D,t);
        unsigned char px[4]={255,255,255,255}; glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,px);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST); return t; }
    static GLuint compile(GLenum t,const char* s){ GLuint sh=glCreateShader(t); glShaderSource(sh,1,&s,nullptr); glCompileShader(sh);
        GLint ok=0; glGetShaderiv(sh,GL_COMPILE_STATUS,&ok); if(!ok){char l[512];glGetShaderInfoLog(sh,512,nullptr,l);std::cerr<<"[sdfcraft] craft shader: "<<l<<"\n";} return sh; }
    static GLuint link(const char* vs,const char* fs){ GLuint v=compile(GL_VERTEX_SHADER,vs),f=compile(GL_FRAGMENT_SHADER,fs);
        GLuint p=glCreateProgram(); glAttachShader(p,v);glAttachShader(p,f);glLinkProgram(p);
        GLint ok=0; glGetProgramiv(p,GL_LINK_STATUS,&ok); if(!ok){char l[512];glGetProgramInfoLog(p,512,nullptr,l);std::cerr<<"[sdfcraft] craft link: "<<l<<"\n";p=0;}
        glDeleteShader(v);glDeleteShader(f); return p; }
};

} // namespace sdfcraft
