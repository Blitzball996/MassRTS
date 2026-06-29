#pragma once
// =============================================================================
// SDFCraft - Minecraft-style textured box-rig model renderer (Phase A3)
// -----------------------------------------------------------------------------
// Faithful MC Legacy Console models ported from MinecraftConsoles (rig constants
// lifted verbatim from HumanoidModel/QuadrupedModel/ChickenModel/SpiderModel/
// SheepModel/SheepFurModel/CowModel/WolfModel/CreeperModel + Cube box UV unwrap).
// Each Entity draws as its proper rig, skinned with the 64x32 MC PNGs in
// assets/textures/mob/. Other players draw with the Steve humanoid + char.png.
//
// Model format (matches MinecraftConsoles exactly):
//   * Authored in "pixels", rendered at 1/16 (16px part = 1.0 world unit).
//   * MC model space is +Y DOWN; our world is +Y UP. We build every rig in MC
//     space then apply a single root flip scale(1,-1,1) and lift feet to pos.
//     Because the flip reverses winding we draw mobs double-sided.
//   * Per-face box UV unwrap is lifted verbatim from Cube.cpp / Polygon.cpp.
//
// FACING: a model's "front" is its -Z face. The world facing is yaw, where
// forward_flat(yaw) = (sin, 0, -cos). Rotating -Z about +Y by θ gives
// (-sinθ, 0, -cosθ); to match forward we rotate by θ = -yaw. (Earlier code used
// +yaw and the models faced backwards — "back of the head forwards".)
//
// ANIMATION: limbs swing only when the entity is actually walking. The server
// is authoritative for "moving" (Entity::render_moving, synced via the protocol
// moving bit); a still mob/player holds its rest pose instead of paddling.
// =============================================================================
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <vector>
#include <cmath>
#include <iostream>
#include "entity.h"
#include "../stb_image.h"   // declarations only; implementation lives in planet_renderer.h

namespace sdfcraft {

// A player to draw with the Steve model (other players; host sees its clients).
// Defined here so both mc_model.h and mode.h (which includes this) can see it.
struct PlayerRender {
    glm::vec3 pos{0,0,0};
    float yaw = 0;
    float pitch = 0;
    bool  moving = false;
};

class McModelRenderer {
public:
    bool init() {
        const char* VS =
            "#version 330 core\n"
            "layout(location=0) in vec3 a_pos;\n"
            "layout(location=1) in vec3 a_nrm;\n"
            "layout(location=2) in vec2 a_uv;\n"
            "uniform mat4 u_view, u_proj, u_model;\n"
            "out vec3 v_nrm; out vec2 v_uv;\n"
            "void main(){ v_nrm = mat3(u_model)*a_nrm; v_uv = a_uv;\n"
            "  gl_Position = u_proj*u_view*u_model*vec4(a_pos,1.0); }\n";
        const char* FS =
            "#version 330 core\n"
            "in vec3 v_nrm; in vec2 v_uv; out vec4 frag;\n"
            "uniform sampler2D u_tex;\n"
            "uniform vec3 u_sun_dir, u_color;\n"
            "uniform float u_use_tex, u_flash;\n"
            "void main(){\n"
            "  vec4 c;\n"
            "  if (u_use_tex > 0.5) { c = texture(u_tex, v_uv); if (c.a < 0.5) discard; }\n"
            "  else c = vec4(u_color, 1.0);\n"
            "  vec3 n = normalize(v_nrm);\n"
            "  float d = max(dot(n, normalize(u_sun_dir)), 0.0);\n"
            "  vec3 lit = c.rgb * (0.5 + 0.5*d);\n"
            "  lit = mix(lit, vec3(1.0,0.15,0.10), u_flash);\n"
            "  frag = vec4(lit, 1.0);\n"
            "}\n";
        prog_ = link(VS, FS);
        if (!prog_) return false;
        build_all();
        build_player();
        return true;
    }

    void shutdown() {
        for (Model& m : models_) free_model(m);
        free_model(player_model_);
        for (GLuint t : owned_tex_) if (t) glDeleteTextures(1, &t);
        owned_tex_.clear();
        if (prog_) glDeleteProgram(prog_);
        prog_ = 0;
    }

    // Draw every live mob as its rig.
    void render(const std::vector<Entity>& mobs, const glm::mat4& view,
                const glm::mat4& proj, glm::vec3 sun_dir, float anim_time = 0.0f) {
        if (!prog_ || mobs.empty()) return;
        begin(view, proj, sun_dir);
        for (const Entity& e : mobs) {
            if (!e.alive) continue;
            const Model& mdl = models_[(int)e.kind];
            if (mdl.parts.empty()) continue;
            draw_model(mdl, e.pos, e.yaw, 0.0f, e.render_moving, e.hurt_cooldown, anim_time, (float)e.id);
        }
        end();
    }

    // Draw other players with the Steve humanoid model.
    void renderPlayers(const std::vector<PlayerRender>& players, const glm::mat4& view,
                       const glm::mat4& proj, glm::vec3 sun_dir, float anim_time = 0.0f) {
        if (!prog_ || players.empty() || player_model_.parts.empty()) return;
        begin(view, proj, sun_dir);
        float idx = 0.0f;
        for (const PlayerRender& p : players)
            draw_model(player_model_, p.pos, p.yaw, p.pitch, p.moving, 0.0f, anim_time, idx++);
        end();
    }

private:
    struct Part {
        GLuint vao = 0, vbo = 0; GLsizei count = 0;
        glm::vec3 pivot{0};    // world units (px/16)
        float phase = 0.0f;    // walk-swing phase offset
        float amp = 0.0f;      // swing amplitude (rad); 0 = no swing
        float base_xrot = 0.0f;// constant rotation about X (quad body = PI/2)
        float box_maxy = 0.0f; // lowest geometry point (max +Y, MC down) in world units
        bool  is_head = false; // head parts pitch with the look direction (players)
        GLuint tex_override = 0; // per-part skin (e.g. sheep wool uses sheep_fur.png); 0 = model tex
    };
    struct Model {
        std::vector<Part> parts;
        GLuint tex = 0;
        bool   has_tex = false;
        glm::vec3 color{1};
        float  model_scale = 1.0f;
        float  feet_mc = 24.0f;
    };

    GLuint prog_ = 0;
    Model  models_[(int)MobKind::COUNT];
    Model  player_model_;
    std::vector<GLuint> owned_tex_;

    static constexpr float kPI  = 3.14159265358979323846f;
    static constexpr float kAmp = 0.55f;   // walk swing peak (rad)

    // --- shared draw path -----------------------------------------------------
    GLint loc_model_=0, loc_color_=0, loc_flash_=0, loc_usetex_=0;
    void begin(const glm::mat4& view, const glm::mat4& proj, glm::vec3 sun_dir) {
        glUseProgram(prog_);
        glUniformMatrix4fv(glGetUniformLocation(prog_, "u_view"), 1, GL_FALSE, &view[0][0]);
        glUniformMatrix4fv(glGetUniformLocation(prog_, "u_proj"), 1, GL_FALSE, &proj[0][0]);
        glm::vec3 s = glm::normalize(sun_dir);
        glUniform3fv(glGetUniformLocation(prog_, "u_sun_dir"), 1, &s[0]);
        loc_model_  = glGetUniformLocation(prog_, "u_model");
        loc_color_  = glGetUniformLocation(prog_, "u_color");
        loc_flash_  = glGetUniformLocation(prog_, "u_flash");
        loc_usetex_ = glGetUniformLocation(prog_, "u_use_tex");
        glActiveTexture(GL_TEXTURE0);
        glUniform1i(glGetUniformLocation(prog_, "u_tex"), 0);
        glEnable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_CULL_FACE);   // root flip reverses winding -> draw double-sided
    }
    void end() { glBindVertexArray(0); glEnable(GL_CULL_FACE); }

    void draw_model(const Model& mdl, glm::vec3 pos, float yaw, float pitch, bool moving,
                    float hurt_cooldown, float anim_time, float id_phase) {
        float ms = mdl.model_scale;
        glm::mat4 root(1.0f);
        root = glm::translate(root, pos);
        // Face travel direction: model -Z front aligns with forward_flat(yaw) when
        // we rotate by -yaw (see header note on facing).
        root = glm::rotate(root, glm::radians(-yaw), glm::vec3(0, 1, 0));
        root = glm::translate(root, glm::vec3(0.0f, (mdl.feet_mc / 16.0f) * ms, 0.0f));
        root = glm::scale(root, glm::vec3(ms, -ms, ms));

        glUniform1f(loc_usetex_, mdl.has_tex ? 1.0f : 0.0f);
        if (mdl.has_tex) glBindTexture(GL_TEXTURE_2D, mdl.tex);
        else             glUniform3fv(loc_color_, 1, &mdl.color[0]);
        float flash = (hurt_cooldown > 0.0f) ? glm::clamp(hurt_cooldown / 0.4f, 0.0f, 1.0f) : 0.0f;
        glUniform1f(loc_flash_, flash);

        // Head pitch: in MC-down space, looking UP (positive world pitch) tilts the
        // head back, i.e. a negative rotation about +X. (Players only; mobs pass 0.)
        float head_xrot = glm::radians(-pitch);
        // Walk clock advances only while moving; a still entity holds rest pose.
        float t = anim_time * 6.0f + id_phase;
        for (const Part& p : mdl.parts) {
            // per-part skin override (sheep wool fleece uses sheep_fur.png)
            if (mdl.has_tex) glBindTexture(GL_TEXTURE_2D, p.tex_override ? p.tex_override : mdl.tex);
            float swing = (moving && p.amp != 0.0f) ? std::cos(t + p.phase) * p.amp : 0.0f;
            glm::mat4 partM(1.0f);
            partM = glm::translate(partM, p.pivot);
            float xr = p.base_xrot + swing + (p.is_head ? head_xrot : 0.0f);
            if (xr != 0.0f) partM = glm::rotate(partM, xr, glm::vec3(1, 0, 0));
            glm::mat4 m = root * partM;
            glUniformMatrix4fv(loc_model_, 1, GL_FALSE, &m[0][0]);
            glBindVertexArray(p.vao);
            glDrawArrays(GL_TRIANGLES, 0, p.count);
        }
    }

    static void free_model(Model& m) {
        for (Part& p : m.parts) {
            if (p.vbo) glDeleteBuffers(1, &p.vbo);
            if (p.vao) glDeleteVertexArrays(1, &p.vao);
        }
        m.parts.clear();
    }

    // --- geometry: one MC box -> 36 textured verts (pos3,nrm3,uv2) in px/16 ----
    // `g` is the MC "grow"/inflate: the box geometry expands by g px on every
    // side (used for the sheep wool fleece and other overlay layers) while the
    // UVs stay mapped to the base box, so the layer puffs out around it.
    static void add_box(std::vector<float>& out, float x0, float y0, float z0,
                        int w, int h, int d, int uo, int vo, bool mirror, float g = 0.0f) {
        float gx0 = x0 - g, gy0 = y0 - g, gz0 = z0 - g;
        float x1 = x0 + w + g, y1 = y0 + h + g, z1 = z0 + d + g;
        x0 = gx0; y0 = gy0; z0 = gz0;
        const glm::vec3 u0{x0,y0,z0}, u1{x1,y0,z0}, u2{x1,y1,z0}, u3{x0,y1,z0};
        const glm::vec3 l0{x0,y0,z1}, l1{x1,y0,z1}, l2{x1,y1,z1}, l3{x0,y1,z1};
        auto quad = [&](glm::vec3 A, glm::vec3 B, glm::vec3 C, glm::vec3 D,
                        float pu0, float pv0, float pu1, float pv1,
                        float nx, float ny, float nz) {
            if (mirror) { std::swap(pu0, pu1); nx = -nx; }
            const float au=pu1,av=pv0, bu=pu0,bv=pv0, cu=pu0,cv=pv1, du=pu1,dv=pv1;
            glm::vec3 pts[4]={A,B,C,D};
            float uvs[4][2]={{au,av},{bu,bv},{cu,cv},{du,dv}};
            const int tri[6]={0,1,2,0,2,3};
            for (int i=0;i<6;++i){ int k=tri[i];
                float px=pts[k].x, py=pts[k].y, pz=pts[k].z;
                if (mirror) px=-px;
                out.push_back(px/16.0f); out.push_back(py/16.0f); out.push_back(pz/16.0f);
                out.push_back(nx); out.push_back(ny); out.push_back(nz);
                out.push_back(uvs[k][0]/64.0f); out.push_back(uvs[k][1]/32.0f);
            }
        };
        const float U=(float)uo, V=(float)vo, W=(float)w, H=(float)h, D=(float)d;
        quad(l1,u1,u2,l2, U+D+W,   V+D, U+D+W+D,   V+D+H,  1,0,0);
        quad(u0,l0,l3,u3, U,       V+D, U+D,       V+D+H, -1,0,0);
        quad(l1,l0,u0,u1, U+D,     V,   U+D+W,     V+D,    0,-1,0);
        quad(u2,u3,l3,l2, U+D+W,   V+D, U+D+W+W,   V,      0,1,0);
        quad(u1,u0,u3,u2, U+D,     V+D, U+D+W,     V+D+H,  0,0,-1);
        quad(l0,l1,l2,l3, U+D+W+D, V+D, U+D+W+D+W, V+D+H,  0,0,1);
    }

    Part make_part(const std::vector<float>& verts, glm::vec3 pivot_px,
                   float phase, float amp, float base_xrot) {
        Part p;
        p.pivot = pivot_px / 16.0f;
        p.phase = phase; p.amp = amp; p.base_xrot = base_xrot;
        p.count = (GLsizei)(verts.size() / 8);
        // Track the part's lowest geometry (largest +Y, since MC space is Y-down)
        // in world units, pivot-relative — used to auto-ground the model so no rig
        // floats or sinks regardless of its hand-authored pivots.
        float maxy = 0.0f;
        for (size_t i = 0; i + 8 <= verts.size(); i += 8)
            maxy = std::max(maxy, verts[i + 1]);   // vy at offset 1
        p.box_maxy = maxy;
        glGenVertexArrays(1, &p.vao);
        glGenBuffers(1, &p.vbo);
        glBindVertexArray(p.vao);
        glBindBuffer(GL_ARRAY_BUFFER, p.vbo);
        glBufferData(GL_ARRAY_BUFFER, verts.size()*sizeof(float), verts.data(), GL_STATIC_DRAW);
        GLsizei st = 8*sizeof(float);
        glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,st,(void*)0);                 glEnableVertexAttribArray(0);
        glVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,st,(void*)(3*sizeof(float))); glEnableVertexAttribArray(1);
        glVertexAttribPointer(2,2,GL_FLOAT,GL_FALSE,st,(void*)(6*sizeof(float))); glEnableVertexAttribArray(2);
        glBindVertexArray(0);
        return p;
    }

    // Auto-ground a finished model: lift it so its lowest leg vertex rests at y=0
    // (feet on the block surface). Replaces the error-prone hand-set feet_mc.
    static void finalize_feet(Model& m) {
        float lowest = 0.0f;   // world units, MC-down (larger = lower)
        for (const Part& p : m.parts)
            lowest = std::max(lowest, p.pivot.y + p.box_maxy);
        m.feet_mc = lowest * 16.0f;
    }

    // --- rig builders (MC pivots/boxes verbatim) -------------------------------
    void build_humanoid(Model& m) {
        std::vector<float> v;
        v.clear(); add_box(v,-4,-8,-4,8,8,8, 0,0, false);  m.parts.push_back(make_part(v,{0,0,0},     0,   0,    0));  // head
        m.parts.back().is_head = true;
        v.clear(); add_box(v,-4,0,-2,8,12,4, 16,16,false); m.parts.push_back(make_part(v,{0,0,0},     0,   0,    0));  // body
        v.clear(); add_box(v,-3,-2,-2,4,12,4,40,16,false); m.parts.push_back(make_part(v,{-5,2,0},    kPI, kAmp, 0));  // arm0
        v.clear(); add_box(v,-3,-2,-2,4,12,4,40,16,true);  m.parts.push_back(make_part(v,{5,2,0},     0,   kAmp, 0));  // arm1
        v.clear(); add_box(v,-2,0,-2,4,12,4, 0,16, false); m.parts.push_back(make_part(v,{-1.9f,12,0},0,   kAmp, 0));  // leg0
        v.clear(); add_box(v,-2,0,-2,4,12,4, 0,16, true);  m.parts.push_back(make_part(v,{1.9f,12,0}, kPI, kAmp, 0));  // leg1
        finalize_feet(m);
    }

    void build_creeper(Model& m) {
        std::vector<float> v;
        v.clear(); add_box(v,-4,-8,-4,8,8,8, 0,0, false); m.parts.push_back(make_part(v,{0,6,0},   0,   0,    0)); // head
        m.parts.back().is_head = true;
        v.clear(); add_box(v,-4,0,-2,8,12,4, 16,16,false);m.parts.push_back(make_part(v,{0,6,0},   0,   0,    0)); // body
        v.clear(); add_box(v,-2,0,-2,4,6,4, 0,16, false); m.parts.push_back(make_part(v,{-2,18,4}, 0,   kAmp, 0)); // leg0
        v.clear(); add_box(v,-2,0,-2,4,6,4, 0,16, false); m.parts.push_back(make_part(v,{2,18,4},  kPI, kAmp, 0)); // leg1
        v.clear(); add_box(v,-2,0,-2,4,6,4, 0,16, false); m.parts.push_back(make_part(v,{-2,18,-4},kPI, kAmp, 0)); // leg2
        v.clear(); add_box(v,-2,0,-2,4,6,4, 0,16, false); m.parts.push_back(make_part(v,{2,18,-4}, 0,   kAmp, 0)); // leg3
        finalize_feet(m);
    }

    // Quadruped (pig). Head/body box dims + tex offsets are passed so cow/sheep
    // can use their own (vanilla MC overrides them — they are NOT plain pigs).
    struct QuadCfg {
        int legSize;
        int hx,hy,hz,hw,hh,hd, huo,hvo;        // head box + tex
        float hpx,hpy,hpz;                     // head pivot
        int bx,by,bz,bw,bh,bd, buo,bvo;        // body box + tex
        float bpx,bpy,bpz;                     // body pivot
    };
    void build_quad(Model& m, const QuadCfg& c, bool pig_snout,
                    bool cow_horns, bool cow_udder) {
        const float yo = (float)(6 - c.legSize);
        std::vector<float> v;
        v.clear();
        add_box(v, c.hx,c.hy,c.hz, c.hw,c.hh,c.hd, c.huo,c.hvo, false);
        if (pig_snout) add_box(v,-2,0,-9,4,3,1, 16,16,false);
        if (cow_horns) { add_box(v,-5,-5,-4,1,3,1, 22,0,false); add_box(v,4,-5,-4,1,3,1, 22,0,false); }
        m.parts.push_back(make_part(v,{c.hpx,c.hpy,c.hpz}, 0,0,0));
        m.parts.back().is_head = true;
        v.clear();
        add_box(v, c.bx,c.by,c.bz, c.bw,c.bh,c.bd, c.buo,c.bvo, false);
        if (cow_udder) add_box(v,-2,2,-8,4,6,1, 52,0,false);
        m.parts.push_back(make_part(v,{c.bpx,c.bpy,c.bpz}, 0,0,kPI*0.5f));
        // four legs share box + tex (0,16), pivots at body corners
        const int L = c.legSize;
        v.clear(); add_box(v,-2,0,-2,4,L,4,0,16,false); m.parts.push_back(make_part(v,{-3,18+yo,7}, 0,  kAmp,0));
        v.clear(); add_box(v,-2,0,-2,4,L,4,0,16,false); m.parts.push_back(make_part(v,{3, 18+yo,7}, kPI,kAmp,0));
        v.clear(); add_box(v,-2,0,-2,4,L,4,0,16,false); m.parts.push_back(make_part(v,{-3,18+yo,-5},kPI,kAmp,0));
        v.clear(); add_box(v,-2,0,-2,4,L,4,0,16,false); m.parts.push_back(make_part(v,{3, 18+yo,-5},0,  kAmp,0));
        finalize_feet(m);
    }

    // Pig: legSize 6, head 8^3 @ (0,0), body 10x16x8 @ (28,8).
    void build_pig(Model& m) {
        QuadCfg c{6, -4,-4,-8,8,8,8, 0,0,  0,12,-6, -5,-10,-7,10,16,8, 28,8, 0,3,2};
        // pivots: head y=12+(6-6)=12, body y=11+(6-6)=11... use yo formula via build_quad
        c.hpy = 12 + (6 - c.legSize); c.bpy = 11 + (6 - c.legSize);
        build_quad(m, c, true, false, false);
    }
    // Cow: legSize 12, head 8x8x6 @ (0,0) (lower pivot), body 12x18x10 @ (18,4), + horns/udder.
    void build_cow(Model& m) {
        QuadCfg c{12, -4,-4,-6,8,8,6, 0,0,  0,0,-8, -6,-10,-7,12,18,10, 18,4, 0,0,2};
        c.hpy = 12 - 6 - 2; c.bpy = 11 + 6 - 12;
        build_quad(m, c, false, true, true);
    }
    // Sheep body (skin layer): legSize 12, head 6x6x8 @ (0,0), body 8x16x6 @ (28,8).
    void build_sheep(Model& m) {
        QuadCfg c{12, -3,-4,-6,6,6,8, 0,0,  0,6,-8, -4,-10,-7,8,16,6, 28,8, 0,5,2};
        c.hpy = 12 - 6; c.bpy = 11 + 6 - 12;
        build_quad(m, c, false, false, false);
        // fluffy wool overlay (SheepFurModel: head g=0.6, body g=1.75). The fleece
        // is its OWN texture (sheep_fur.png), inflated around the bare body — this
        // is what makes a sheep look woolly by default. Assigned per-part below.
        GLuint fur = load_skin("assets/textures/mob/sheep_fur.png");
        if (fur) owned_tex_.push_back(fur);
        std::vector<float> v;
        v.clear(); add_box(v,-3,-4,-4,6,6,6, 0,0, false, 0.6f);
        m.parts.push_back(make_part(v,{0,12-6,-8}, 0,0,0));
        m.parts.back().tex_override = fur;
        v.clear(); add_box(v,-4,-10,-7,8,16,6, 28,8, false, 1.75f);
        m.parts.push_back(make_part(v,{0,11+6-12,2}, 0,0,kPI*0.5f));
        m.parts.back().tex_override = fur;
        finalize_feet(m);
    }

    void build_chicken(Model& m) {
        const float yo = 0.0f;
        std::vector<float> v;
        v.clear(); add_box(v,-2,-6,-2,4,6,3, 0,0, false);  m.parts.push_back(make_part(v,{0,-1+yo,-4}, 0,0,0)); // head
        v.clear(); add_box(v,-2,-4,-4,4,2,2, 14,0,false);  m.parts.push_back(make_part(v,{0,-1+yo,-4}, 0,0,0)); // beak
        v.clear(); add_box(v,-1,-2,-3,2,2,2, 14,4,false);  m.parts.push_back(make_part(v,{0,-1+yo,-4}, 0,0,0)); // wattle
        v.clear(); add_box(v,-3,-4,-3,6,8,6, 0,9, false);  m.parts.push_back(make_part(v,{0,0+yo,0},   0,0,kPI*0.5f)); // body
        v.clear(); add_box(v,-1,0,-3,3,5,3, 26,0,false);   m.parts.push_back(make_part(v,{-2,3+yo,1}, 0,  kAmp,0)); // leg0
        v.clear(); add_box(v,-1,0,-3,3,5,3, 26,0,false);   m.parts.push_back(make_part(v,{1, 3+yo,1}, kPI,kAmp,0)); // leg1
        v.clear(); add_box(v,0,0,-3,1,4,6, 24,13,false);   m.parts.push_back(make_part(v,{-4,-3+yo,0},0,  0,0)); // wing0
        v.clear(); add_box(v,-1,0,-3,1,4,6, 24,13,false);  m.parts.push_back(make_part(v,{4,-3+yo,0}, 0,  0,0)); // wing1
        m.parts[0].is_head = true;
        finalize_feet(m);
    }

    void build_spider(Model& m) {
        const float yo = 0.0f, g = 0.0f; (void)g;
        std::vector<float> v;
        v.clear(); add_box(v,-4,-4,-8,8,8,8, 32,4,false); m.parts.push_back(make_part(v,{0,0+yo,-3}, 0,0,0)); // head
        v.clear(); add_box(v,-3,-3,-3,6,6,6, 0,0, false); m.parts.push_back(make_part(v,{0,yo,0},    0,0,0)); // body0
        v.clear(); add_box(v,-5,-4,-6,10,8,12,0,12,false);m.parts.push_back(make_part(v,{0,0+yo,9},  0,0,0)); // body1
        // 8 legs (16px long); left legs box starts at -15, right at -1.
        auto leg=[&](float px,float pz){ std::vector<float> lv; add_box(lv,-15,-1,-1,16,2,2,18,0,false); m.parts.push_back(make_part(lv,{px,0+yo,pz},0,0,0)); };
        auto legR=[&](float px,float pz){ std::vector<float> lv; add_box(lv,-1,-1,-1,16,2,2,18,0,false); m.parts.push_back(make_part(lv,{px,0+yo,pz},0,0,0)); };
        leg(-4,2); legR(4,2); leg(-4,1); legR(4,1); leg(-4,0); legR(4,0); leg(-4,-1); legR(4,-1);
        m.parts[0].is_head = true;
        finalize_feet(m);
    }

    void build_wolf(Model& m) {
        const int legSize = 8;
        const float hh = 12 + 9.5f - legSize;
        std::vector<float> v;
        v.clear();
        add_box(v,-3,-3,-2,6,6,4, 0,0, false);                 // head
        add_box(v,-3,-5,0,2,2,1, 16,14,false);                 // ear (left)
        add_box(v,1,-5,0,2,2,1, 16,14,false);                  // ear (right)
        add_box(v,-1.5f,0,-5,3,3,4, 0,10,false);               // snout
        m.parts.push_back(make_part(v,{-1,hh,-7}, 0,0,0));
        m.parts.back().is_head = true;
        v.clear(); add_box(v,-4,-2,-3,6,9,6, 18,14,false); m.parts.push_back(make_part(v,{0,11+11-legSize,2}, 0,0,kPI*0.5f)); // body
        v.clear(); add_box(v,-4,-3,-3,8,6,7, 21,0,false);  m.parts.push_back(make_part(v,{-1.0f,11+11.0f-legSize,-2}, 0,0,kPI*0.5f)); // upperBody/neck ruff (moved fwd to close the neck gap)
        v.clear(); add_box(v,-1,0,-1,2,legSize,2,0,18,false); m.parts.push_back(make_part(v,{-2.5f,18+6-legSize,7}, 0,  kAmp,0)); // leg0
        v.clear(); add_box(v,-1,0,-1,2,legSize,2,0,18,false); m.parts.push_back(make_part(v,{0.5f, 18+6-legSize,7}, kPI,kAmp,0)); // leg1
        v.clear(); add_box(v,-1,0,-1,2,legSize,2,0,18,false); m.parts.push_back(make_part(v,{-2.5f,18+6-legSize,-4},kPI,kAmp,0)); // leg2
        v.clear(); add_box(v,-1,0,-1,2,legSize,2,0,18,false); m.parts.push_back(make_part(v,{0.5f, 18+6-legSize,-4},0,  kAmp,0)); // leg3
        v.clear(); add_box(v,-1,0,-1,2,8,2, 9,18,false); m.parts.push_back(make_part(v,{-1,2+18-legSize,8}, 0,0,0)); // tail
        finalize_feet(m);
    }

    void build_cube(Model& m, int size, int uo, int vo, float pivot_y, float model_scale) {
        std::vector<float> v;
        float half = size * 0.5f;
        add_box(v,-half,-half,-half,size,size,size, uo,vo, false);
        m.parts.push_back(make_part(v,{0,pivot_y,0}, 0,0,0));
        m.model_scale = model_scale;
        m.feet_mc = pivot_y + half;
    }

    GLuint tex_or_color(Model& m, const char* file, glm::vec3 color) {
        GLuint t = load_skin(file);
        if (t) { m.tex = t; m.has_tex = true; owned_tex_.push_back(t); }
        else   { m.has_tex = false; m.color = color; }
        return t;
    }

    void build_player() {
        build_humanoid(player_model_);
        tex_or_color(player_model_, "assets/textures/mob/char.png", glm::vec3(0.85f,0.72f,0.6f));
    }

    void build_all() {
        const char* DIR = "assets/textures/mob/";
        auto path = [&](const char* f){ static std::string s; s = std::string(DIR) + f; return s.c_str(); };
        for (int k = 0; k < (int)MobKind::COUNT; ++k) {
            Model& m = models_[k];
            MobKind kind = (MobKind)k;
            const MobDef& d = mob_def(kind);
            switch (kind) {
                case MobKind::Zombie:   build_humanoid(m); tex_or_color(m, path("zombie.png"),   d.color); break;
                case MobKind::Skeleton: build_humanoid(m); tex_or_color(m, path("skeleton.png"), d.color); break;
                case MobKind::Enderman: build_humanoid(m); tex_or_color(m, path("enderman.png"), d.color); break;
                case MobKind::Creeper:  build_creeper(m);  tex_or_color(m, path("creeper.png"),  d.color); break;
                case MobKind::Pig:      build_pig(m);   tex_or_color(m, path("pig.png"),   d.color); break;
                case MobKind::Cow:      build_cow(m);   tex_or_color(m, path("cow.png"),   d.color); break;
                case MobKind::Sheep:    build_sheep(m); tex_or_color(m, path("sheep.png"), d.color); break;
                case MobKind::Wolf:     build_wolf(m);                               tex_or_color(m, path("wolf.png"),  d.color); break;
                case MobKind::Chicken:  build_chicken(m);                            tex_or_color(m, path("chicken.png"),d.color); break;
                case MobKind::Spider:   build_spider(m);                             tex_or_color(m, path("spider.png"),d.color); break;
                case MobKind::Slime:    build_cube(m, 8, 0, 0, 8.0f, 2.2f);          tex_or_color(m, path("slime.png"), d.color); break;
                default:                build_cube(m, 16, 0, 0, 8.0f, 1.5f); m.has_tex = false; m.color = d.color; break;
            }
        }
    }

    static GLuint load_skin(const char* path) {
        int w, h, ch;
        stbi_set_flip_vertically_on_load(0);
        unsigned char* data = stbi_load(path, &w, &h, &ch, 4);
        if (!data) {
            std::cerr << "[sdfcraft] mob skin missing: " << path << " (flat colour)\n";
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
        glShaderSource(sh, 1, &s, nullptr); glCompileShader(sh);
        GLint ok=0; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
        if (!ok){ char log[512]; glGetShaderInfoLog(sh,512,nullptr,log);
                  std::cerr << "[sdfcraft] mc_model shader error: " << log << "\n"; }
        return sh;
    }
    static GLuint link(const char* vs, const char* fs) {
        GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
        GLuint p = glCreateProgram();
        glAttachShader(p, v); glAttachShader(p, f); glLinkProgram(p);
        GLint ok=0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
        if (!ok){ char log[512]; glGetProgramInfoLog(p,512,nullptr,log);
                  std::cerr << "[sdfcraft] mc_model link error: " << log << "\n"; p=0; }
        glDeleteShader(v); glDeleteShader(f);
        return p;
    }
};

} // namespace sdfcraft
