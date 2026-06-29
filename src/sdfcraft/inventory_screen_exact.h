#pragma once
// Exact InventoryMenu.cpp coordinates with proper scaling
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <vector>
#include "inventory.h"
#include "items.h"
#include "blocks.h"
#include "crafting.h"
#include "item_icons.h"
#include "player_preview.h"
#include "mc_model.h"
#include "../stb_image.h"

namespace sdfcraft {

class InventoryScreenExact {
public:
    ItemStack cursor, craft_grid[4];
    CraftResult craft_result;

    bool init() {
        const char* VS="#version 330 core\nlayout(location=0)in vec2 a_pos;layout(location=1)in vec2 a_uv;layout(location=2)in vec4 a_col;out vec2 v_uv;out vec4 v_col;void main(){v_uv=a_uv;v_col=a_col;gl_Position=vec4(a_pos,0,1);}";
        const char* FS="#version 330 core\nin vec2 v_uv;in vec4 v_col;out vec4 frag;uniform sampler2D u_tex;uniform float u_use_tex;void main(){if(u_use_tex>0.5){vec4 c=texture(u_tex,v_uv);if(c.a<0.05)discard;frag=c*v_col;}else frag=v_col;}";
        prog_=link(VS,FS);if(!prog_)return false;
        glGenVertexArrays(1,&vao_);glGenBuffers(1,&vbo_);
        glBindVertexArray(vao_);glBindBuffer(GL_ARRAY_BUFFER,vbo_);
        glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,8*sizeof(float),(void*)0);glEnableVertexAttribArray(0);
        glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,8*sizeof(float),(void*)(2*sizeof(float)));glEnableVertexAttribArray(1);
        glVertexAttribPointer(2,4,GL_FLOAT,GL_FALSE,8*sizeof(float),(void*)(4*sizeof(float)));glEnableVertexAttribArray(2);
        glBindVertexArray(0);
        tex_t_=load_tex("assets/textures/gui/terrain.png");
        tex_i_=load_tex("assets/textures/gui/items.png");
        tex_f_=load_tex("assets/textures/gui/default.png");
        white_=make_white();
        return true;
    }

    void destroy(){if(vbo_)glDeleteBuffers(1,&vbo_);if(vao_)glDeleteVertexArrays(1,&vao_);if(prog_)glDeleteProgram(prog_);GLuint t[4]={tex_t_,tex_i_,white_,tex_f_};glDeleteTextures(4,t);prog_=vao_=vbo_=0;}
    void returnAll(Inventory& inv){if(!cursor.empty()){inv.add(cursor.id,cursor.count,item_max_stack(cursor.id));cursor.clear();}for(int i=0;i<4;i++)if(!craft_grid[i].empty()){inv.add(craft_grid[i].id,craft_grid[i].count,item_max_stack(craft_grid[i].id));craft_grid[i].clear();}craft_result={};}

    bool click(Inventory& inv,RecipeBook& recipes,int fbw,int fbh,float mx,float my,bool right){
        layout(fbw,fbh);
        if(hit(r_res_,mx,my)&&craft_result.id!=ITEM_NONE){take_craft(inv);recomp(recipes);return true;}
        for(int i=0;i<4;i++)if(hit(r_craft_[i],mx,my)){mov(craft_grid[i],right);recomp(recipes);return true;}
        for(int i=0;i<4;i++)if(hit(r_armor_[i],mx,my)){mov_armor(inv.armor[i],(ArmorSlot)(i+1));return true;}
        for(int i=0;i<27;i++)if(hit(r_inv_[i],mx,my)){if(cursor.empty()&&right&&quick_eq(inv,inv.slots[9+i]))return true;mov(inv.slots[9+i],right);return true;}
        for(int i=0;i<9;i++)if(hit(r_hb_[i],mx,my)){if(cursor.empty()&&right&&quick_eq(inv,inv.slots[i]))return true;mov(inv.slots[i],right);return true;}
        return false;
    }

    void draw(Player& player,Inventory& inv,RecipeBook& recipes,McModelRenderer& model_rend,int fbw,int fbh,float mx,float my,float anim_time){
        layout(fbw,fbh);recomp(recipes);fbw_=(float)fbw;fbh_=(float)fbh;
        v_.clear();t_.clear();p_.clear();

        // Dim the world, then draw the solid MC inventory panel (light grey slab
        // with a bevel border), so slots sit on a panel exactly like real MC.
        quad(0,0,(float)fbw,(float)fbh,glm::vec4(0,0,0,0.55f));
        quad(px_-3,py_-3,pw_+6,ph_+6,glm::vec4(0.20f,0.20f,0.22f,1.0f));   // dark outer frame
        quad(px_,py_,pw_,ph_,glm::vec4(0.78f,0.78f,0.80f,1.0f));            // panel face
        quad(px_,py_,pw_,2*s_,glm::vec4(1,1,1,1));                          // top highlight
        quad(px_,py_,2*s_,ph_,glm::vec4(1,1,1,1));                          // left highlight
        quad(px_,py_+ph_-2*s_,pw_,2*s_,glm::vec4(0.42f,0.42f,0.45f,1));     // bottom shadow
        quad(px_+pw_-2*s_,py_,2*s_,ph_,glm::vec4(0.42f,0.42f,0.45f,1));     // right shadow

        // Recessed slot: MC slots are sunken dark squares with a light bottom-right
        // edge. Drawn on the light panel so they read as inset cells.
        auto ds=[&](const glm::vec4& r,const ItemStack& st){
            quad(r.x-1,r.y-1,r.z+2,r.w+2,glm::vec4(0.30f,0.30f,0.33f,1));   // dark inset border
            quad(r.x,r.y,r.z,r.w,glm::vec4(0.54f,0.54f,0.57f,1));            // slot fill
            quad(r.x+r.z-s_,r.y,s_,r.w,glm::vec4(1,1,1,0.65f));              // light edge R
            quad(r.x,r.y+r.w-s_,r.z,s_,glm::vec4(1,1,1,0.65f));             // light edge B
            if(!st.empty()){icon(r.x+1,r.y+1,r.z-2,r.w-2,st.id);draw_cnt(r.x+1,r.y+1,r.z-2,r.w-2,st.count);}
        };

        for(int i=0;i<4;i++)ds(r_armor_[i],inv.armor[i]);
        for(int i=0;i<4;i++)ds(r_craft_[i],craft_grid[i]);
        ds(r_res_,ItemStack{craft_result.id,craft_result.count});
        for(int i=0;i<27;i++)ds(r_inv_[i],inv.slots[9+i]);
        for(int i=0;i<9;i++)ds(r_hb_[i],inv.slots[i]);

        if(!cursor.empty()){icon(mx-8*s_,my-8*s_,16*s_,16*s_,cursor.id);draw_cnt(mx-8*s_,my-8*s_,16*s_,16*s_,cursor.count);}
        flush();
        preview_.render(player,inv,model_rend,fbw,fbh,prx_,pry_,prw_,prh_,mx,my,anim_time);
    }

private:
    GLuint prog_=0,vao_=0,vbo_=0,tex_t_=0,tex_i_=0,white_=0,tex_f_=0;
    std::vector<float> v_,t_;
    float fbw_=0,fbh_=0,px_=0,py_=0,pw_=0,ph_=0,prx_=0,pry_=0,prw_=0,prh_=0,s_=2.0f;
    glm::vec4 r_res_,r_craft_[4],r_armor_[4],r_inv_[27],r_hb_[9];
    PlayerPreviewRenderer preview_;

    // EXACT MinecraftConsoles InventoryMenu.cpp slot coordinates, drawn on a
    // centered 176x166 panel scaled by an integer GUI factor (MC auto-scale).
    // Every offset below is the canonical MC icon top-left in panel-local pixels:
    //   ResultSlot(144,36)  craft(88+c*18, 26+r*18)  armor(8, 8+i*18)
    //   inv(8+c*18, 84+r*18)  hotbar(8+c*18, 142)
    // The whole inventory is one compact slab (like real MC), not floating slots.
    void layout(int fbw,int fbh){
        float W=(float)fbw, H=(float)fbh;
        s_ = std::max(2.0f, std::floor(H/240.0f));   // integer GUI scale
        float pw=176.0f*s_, ph=166.0f*s_;
        px_=std::floor((W-pw)*0.5f); py_=std::floor((H-ph)*0.5f);
        pw_=pw; ph_=ph;
        float slot=16.0f*s_;                          // 16px icon; cell drawn 1px larger
        auto P=[&](float mx,float my){ return glm::vec2(px_+mx*s_, py_+my*s_); };

        for(int i=0;i<4;i++){ glm::vec2 q=P(8.0f, 8.0f+i*18); r_armor_[i]={q.x,q.y,slot,slot}; }
        for(int r=0;r<2;r++)for(int c=0;c<2;c++){ glm::vec2 q=P(88.0f+c*18, 26.0f+r*18); r_craft_[c+r*2]={q.x,q.y,slot,slot}; }
        { glm::vec2 q=P(144.0f,36.0f); r_res_={q.x,q.y,slot,slot}; }
        for(int r=0;r<3;r++)for(int c=0;c<9;c++){ glm::vec2 q=P(8.0f+c*18, 84.0f+r*18); r_inv_[c+r*9]={q.x,q.y,slot,slot}; }
        for(int c=0;c<9;c++){ glm::vec2 q=P(8.0f+c*18, 142.0f); r_hb_[c]={q.x,q.y,slot,slot}; }

        // Player model viewport: the gap between the armor column and the craft
        // grid (MC shows the live avatar here). x 26..76, y 8..78 in panel px.
        glm::vec2 pv=P(26.0f,8.0f); prx_=pv.x; pry_=pv.y; prw_=50.0f*s_; prh_=70.0f*s_;
    }

    void mov(ItemStack& slot,bool right){if(cursor.empty()){if(slot.empty())return;if(right){uint8_t half=(uint8_t)((slot.count+1)/2);cursor.id=slot.id;cursor.count=half;slot.count-=half;if(slot.count==0)slot.clear();}else{cursor=slot;slot.clear();}}else{if(slot.empty()){if(right){slot.id=cursor.id;slot.count=1;if(--cursor.count==0)cursor.clear();}else{slot=cursor;cursor.clear();}}else if(slot.id==cursor.id){uint8_t mx=item_max_stack(slot.id),room=(uint8_t)(mx-slot.count);uint8_t mv=right?1:(uint8_t)std::min<int>(room,cursor.count);mv=(uint8_t)std::min<int>(mv,room);slot.count+=mv;cursor.count-=mv;if(cursor.count==0)cursor.clear();}else{std::swap(slot,cursor);}}}
    void mov_armor(ItemStack& slot,ArmorSlot want){if(cursor.empty()){if(!slot.empty()){cursor=slot;slot.clear();}return;}if(item_armor_slot(cursor.id)!=want)return;std::swap(slot,cursor);}
    bool quick_eq(Inventory& inv,ItemStack& s){if(s.empty())return false;ArmorSlot sl=item_armor_slot(s.id);if(sl==ArmorSlot::None)return false;int ai=(int)sl-1;if(!inv.armor[ai].empty())std::swap(inv.armor[ai],s);else{inv.armor[ai]=s;s.clear();}return true;}
    void recomp(RecipeBook& recipes){ItemId grid[4]={craft_grid[0].empty()?ITEM_NONE:craft_grid[0].id,craft_grid[1].empty()?ITEM_NONE:craft_grid[1].id,craft_grid[2].empty()?ITEM_NONE:craft_grid[2].id,craft_grid[3].empty()?ITEM_NONE:craft_grid[3].id};craft_result=recipes.match(grid,2,2);}
    void take_craft(Inventory& inv){if(craft_result.id==ITEM_NONE)return;if(!cursor.empty()&&cursor.id!=craft_result.id)return;uint8_t mx=item_max_stack(craft_result.id);if(cursor.count+craft_result.count>mx)return;for(int i=0;i<4;i++){if(!craft_grid[i].empty()){craft_grid[i].count--;if(craft_grid[i].count==0)craft_grid[i].clear();}}if(cursor.empty()){cursor.id=craft_result.id;cursor.count=craft_result.count;}else cursor.count+=craft_result.count;craft_result={};}
    static bool hit(const glm::vec4& r,float mx,float my){return mx>=r.x&&mx<r.x+r.z&&my>=r.y&&my<r.y+r.w;}

    void quad(float x,float y,float w,float h,glm::vec4 c){float x0=x/fbw_*2-1,x1=(x+w)/fbw_*2-1,y0=1-y/fbh_*2,y1=1-(y+h)/fbh_*2;push(x0,y0,0,0,c);push(x1,y0,0,0,c);push(x1,y1,0,0,c);push(x0,y0,0,0,c);push(x1,y1,0,0,c);push(x0,y1,0,0,c);}
    struct P{float x,y,w,h;IconRef ir;bool iso;BlockId b;};
    std::vector<P> p_;
    void icon(float x,float y,float w,float h,ItemId id){if(id==ITEM_NONE)return;if(item_is_block(id)){p_.push_back({x,y,w,h,{},true,item_block(id)});return;}IconRef ir=icon_for(id);if(!ir.ok){quad(x,y,w,h,glm::vec4(0.8f));return;}p_.push_back({x,y,w,h,ir,false,BLOCK_AIR});}
    void glyph(char ch,float dx,float dy,float px,glm::vec4 t){int c=(unsigned char)ch,col=c&15,row=c>>4;const float inv=1.0f/128.0f;float u0=(col*8)*inv,v0=(row*8)*inv,u1=(col*8+8)*inv,v1=(row*8+8)*inv;float x0=dx/fbw_*2-1,x1=(dx+px)/fbw_*2-1,y0=1-dy/fbh_*2,y1=1-(dy+px)/fbh_*2;auto v=[&](float vx,float vy,float u,float vv){t_.insert(t_.end(),{vx,vy,u,vv,t.r,t.g,t.b,t.a});};v(x0,y0,u0,v0);v(x1,y0,u1,v0);v(x1,y1,u1,v1);v(x0,y0,u0,v0);v(x1,y1,u1,v1);v(x0,y1,u0,v1);}
    void draw_cnt(float sx,float sy,float sw,float sh,int count){if(count<=1||!tex_f_)return;char d[4];int n=0,v=count>999?999:count;do{d[n++]=(char)('0'+v%10);v/=10;}while(v&&n<4);float gp=std::max(8.0f,sh*0.3f);float gx=sx+sw-gp-2,gy=sy+sh-gp-2;for(int i=0;i<n;i++){float x=gx-i*(gp*0.75f);glyph(d[i],x+1,gy+1,gp,glm::vec4(0.13f));glyph(d[i],x,gy,gp,glm::vec4(1));}}

    void flush(){glUseProgram(prog_);glBindVertexArray(vao_);glBindBuffer(GL_ARRAY_BUFFER,vbo_);GLboolean d=glIsEnabled(GL_DEPTH_TEST),c=glIsEnabled(GL_CULL_FACE);glDisable(GL_DEPTH_TEST);glDisable(GL_CULL_FACE);glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);GLint loc=glGetUniformLocation(prog_,"u_use_tex");glActiveTexture(GL_TEXTURE0);glUniform1i(glGetUniformLocation(prog_,"u_tex"),0);glUniform1f(loc,0.0f);glBindTexture(GL_TEXTURE_2D,white_);upload();glDrawArrays(GL_TRIANGLES,0,(GLsizei)(v_.size()/8));for(int atlas=0;atlas<2;atlas++){v_.clear();for(auto& pp:p_){if(pp.iso||pp.ir.atlas!=atlas)continue;float x0=pp.x/fbw_*2-1,x1=(pp.x+pp.w)/fbw_*2-1,y0=1-pp.y/fbh_*2,y1=1-(pp.y+pp.h)/fbh_*2;glm::vec4 w(1);push(x0,y0,pp.ir.u0,pp.ir.v0,w);push(x1,y0,pp.ir.u1,pp.ir.v0,w);push(x1,y1,pp.ir.u1,pp.ir.v1,w);push(x0,y0,pp.ir.u0,pp.ir.v0,w);push(x1,y1,pp.ir.u1,pp.ir.v1,w);push(x0,y1,pp.ir.u0,pp.ir.v1,w);}if(v_.empty())continue;glUniform1f(loc,1.0f);glBindTexture(GL_TEXTURE_2D,atlas==ICON_ATLAS_TERRAIN?tex_t_:tex_i_);upload();glDrawArrays(GL_TRIANGLES,0,(GLsizei)(v_.size()/8));}v_.clear();for(auto& pp:p_){if(!pp.iso)continue;std::vector<IsoTri> tris;emit_iso_cube(tris,pp.x,pp.y,pp.w,pp.h,block_face(pp.b,0),block_face(pp.b,1));for(auto& tt:tris){float x=tt.x/fbw_*2-1,y=1-tt.y/fbh_*2;push(x,y,tt.u,tt.v,tt.tint);}}if(!v_.empty()){glUniform1f(loc,1.0f);glBindTexture(GL_TEXTURE_2D,tex_t_);upload();glDrawArrays(GL_TRIANGLES,0,(GLsizei)(v_.size()/8));}if(!t_.empty()&&tex_f_){glUniform1f(loc,1.0f);glBindTexture(GL_TEXTURE_2D,tex_f_);v_=t_;upload();glDrawArrays(GL_TRIANGLES,0,(GLsizei)(v_.size()/8));}p_.clear();v_.clear();t_.clear();glBindVertexArray(0);if(d)glEnable(GL_DEPTH_TEST);if(c)glEnable(GL_CULL_FACE);}
    void upload(){glBufferData(GL_ARRAY_BUFFER,v_.size()*sizeof(float),v_.data(),GL_STREAM_DRAW);}
    void push(float x,float y,float u,float v,glm::vec4 c){v_.insert(v_.end(),{x,y,u,v,c.r,c.g,c.b,c.a});}

    static GLuint load_tex(const char* path){int w,h,ch;stbi_set_flip_vertically_on_load(0);unsigned char* d=stbi_load(path,&w,&h,&ch,4);if(!d)return 0;GLuint t;glGenTextures(1,&t);glBindTexture(GL_TEXTURE_2D,t);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,d);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);stbi_image_free(d);return t;}
    static GLuint make_white(){GLuint t;glGenTextures(1,&t);glBindTexture(GL_TEXTURE_2D,t);unsigned char px[4]={255,255,255,255};glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,px);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);return t;}
    static GLuint compile(GLenum t,const char* s){GLuint sh=glCreateShader(t);glShaderSource(sh,1,&s,nullptr);glCompileShader(sh);return sh;}
    static GLuint link(const char* vs,const char* fs){GLuint v=compile(GL_VERTEX_SHADER,vs),f=compile(GL_FRAGMENT_SHADER,fs);GLuint p=glCreateProgram();glAttachShader(p,v);glAttachShader(p,f);glLinkProgram(p);GLint ok=0;glGetProgramiv(p,GL_LINK_STATUS,&ok);if(!ok)p=0;glDeleteShader(v);glDeleteShader(f);return p;}
};

} // namespace sdfcraft
