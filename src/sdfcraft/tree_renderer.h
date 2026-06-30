#pragma once
// ============================================================================
// SDFCraft - Smooth procedural tree renderer
// ----------------------------------------------------------------------------
// Replaces the old voxel LOG/LEAVES trees (which read as ugly stepped blobs
// next to the marching-cubes-smoothed terrain) with rounded meshes: a tapered
// trunk + a clustered-sphere canopy (oak) or stacked cones (pine). Two static
// template meshes are built once; each TreeInstance is drawn with its own model
// matrix (placement + yaw + size scale). Output is LINEAR HDR so the shared
// PostFX composite tonemaps/grades it with the rest of the frame; falls back to
// an inline tonemap when PostFX is off. Lighting matches sdfcraft_chunk.frag
// (day/night ambient + sun NdotL + distance fog) so trees sit in the scene.
// ============================================================================
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <vector>
#include <cmath>
#include <string>
#include <iostream>
#include "world.h"   // TreeInstance

namespace sdfcraft {

class TreeRenderer {
public:
    bool hdr_out = false;   // mirror of PostFX-enabled (linear HDR vs inline tonemap)

    bool init() {
        prog_ = link(VS(), FS());
        if (!prog_) { std::cerr << "[sdfcraft] tree shader failed\n"; return false; }
        build_template(/*pine=*/false, oak_);
        build_template(/*pine=*/true,  pine_);
        return oak_.count > 0 && pine_.count > 0;
    }

    void shutdown() {
        for (Mesh* m : {&oak_, &pine_}) {
            if (m->vbo) glDeleteBuffers(1, &m->vbo);
            if (m->vao) glDeleteVertexArrays(1, &m->vao);
            *m = Mesh{};
        }
        if (prog_) glDeleteProgram(prog_);
        prog_ = 0;
    }

    // Draw every visible tree. `trees` is the collected instance list (world
    // space). View/proj place them; sun_dir + fog match the terrain pass.
    void render(const std::vector<TreeInstance>& trees,
                const glm::mat4& view, const glm::mat4& proj,
                glm::vec3 cam, glm::vec3 sun_dir,
                glm::vec3 fog_color, float fog_start, float fog_end, float time) {
        if (!prog_ || trees.empty()) return;
        // Trees are few; draw double-sided so a wrong winding (or seeing a canopy
        // sphere from inside) can't punch holes in the crown.
        GLboolean cull_was = glIsEnabled(GL_CULL_FACE);
        glDisable(GL_CULL_FACE);
        glUseProgram(prog_);
        glUniformMatrix4fv(glGetUniformLocation(prog_, "u_view"), 1, GL_FALSE, &view[0][0]);
        glUniformMatrix4fv(glGetUniformLocation(prog_, "u_proj"), 1, GL_FALSE, &proj[0][0]);
        glUniform3fv(glGetUniformLocation(prog_, "u_sun_dir"), 1, &sun_dir[0]);
        glUniform3fv(glGetUniformLocation(prog_, "u_cam"), 1, &cam[0]);
        glUniform3fv(glGetUniformLocation(prog_, "u_fog_color"), 1, &fog_color[0]);
        glUniform1f(glGetUniformLocation(prog_, "u_fog_start"), fog_start);
        glUniform1f(glGetUniformLocation(prog_, "u_fog_end"), fog_end);
        glUniform1f(glGetUniformLocation(prog_, "u_time"), time);
        glUniform1i(glGetUniformLocation(prog_, "u_hdr_out"), hdr_out ? 1 : 0);
        GLint loc_model = glGetUniformLocation(prog_, "u_model");

        for (const TreeInstance& t : trees) {
            const Mesh& m = (t.species == 1) ? pine_ : oak_;
            if (!m.vao) continue;
            // Per-tree model matrix: translate to base, yaw from seed, and scale
            // the unit-ish template so its trunk matches this tree's height. The
            // template is built at nominal_h, so scale = trunk_h / nominal_h.
            float s = t.trunk_h / m.nominal_h;
            float yaw = (float)(t.seed & 1023) / 1023.0f * 6.2831853f;
            glm::mat4 model(1.0f);
            model = glm::translate(model, glm::vec3(t.wx, t.base_y, t.wz));
            model = glm::rotate(model, yaw, glm::vec3(0, 1, 0));
            // lean: shear the canopy slightly by tilting the whole mesh a touch
            model = glm::rotate(model, t.lean_x * 0.05f, glm::vec3(0, 0, 1));
            model = glm::rotate(model, t.lean_z * 0.05f, glm::vec3(1, 0, 0));
            model = glm::scale(model, glm::vec3(s));
            glUniformMatrix4fv(loc_model, 1, GL_FALSE, &model[0][0]);
            glBindVertexArray(m.vao);
            glDrawArrays(GL_TRIANGLES, 0, m.count);
        }
        glBindVertexArray(0);
        if (cull_was) glEnable(GL_CULL_FACE);   // restore caller's cull state
    }


private:
    struct Mesh { GLuint vao = 0, vbo = 0; GLsizei count = 0; float nominal_h = 1.0f; };
    GLuint prog_ = 0;
    Mesh oak_, pine_;

    // --- geometry helpers ---------------------------------------------------
    // Vertex layout: pos(3) nrm(3) mat(1). mat 0 = bark, 1 = leaves.
    static void push_v(std::vector<float>& v, glm::vec3 p, glm::vec3 n, float mat) {
        v.insert(v.end(), { p.x, p.y, p.z, n.x, n.y, n.z, mat });
    }
    static void push_tri(std::vector<float>& v, glm::vec3 a, glm::vec3 b, glm::vec3 c, float mat) {
        glm::vec3 n = glm::normalize(glm::cross(b - a, c - a));
        push_v(v, a, n, mat); push_v(v, b, n, mat); push_v(v, c, n, mat);
    }

    // Tapered cylinder trunk from y0..y1, radius r0 (bottom) -> r1 (top).
    static void add_trunk(std::vector<float>& v, float y0, float y1, float r0, float r1) {
        const int seg = 9;
        for (int i = 0; i < seg; i++) {
            float a0 = (float)i / seg * 6.2831853f, a1 = (float)(i + 1) / seg * 6.2831853f;
            glm::vec3 b0(std::cos(a0) * r0, y0, std::sin(a0) * r0);
            glm::vec3 b1(std::cos(a1) * r0, y0, std::sin(a1) * r0);
            glm::vec3 t0(std::cos(a0) * r1, y1, std::sin(a0) * r1);
            glm::vec3 t1(std::cos(a1) * r1, y1, std::sin(a1) * r1);
            // side normals point radially out (smooth-ish, per-tri here)
            glm::vec3 nb0(std::cos(a0), 0.25f, std::sin(a0)), nb1(std::cos(a1), 0.25f, std::sin(a1));
            push_v(v, b0, nb0, 0); push_v(v, b1, nb1, 0); push_v(v, t1, nb1, 0);
            push_v(v, b0, nb0, 0); push_v(v, t1, nb1, 0); push_v(v, t0, nb0, 0);
        }
    }

    // A leaf sphere (icosphere-ish via lat/long) centred at c, radius r. Normals
    // are radial so the canopy catches the sun smoothly like a rounded crown.
    static void add_leaf_sphere(std::vector<float>& v, glm::vec3 c, float r) {
        const int rings = 6, secs = 8;
        for (int y = 0; y < rings; y++) {
            float v0 = (float)y / rings * 3.1415926f, v1 = (float)(y + 1) / rings * 3.1415926f;
            for (int x = 0; x < secs; x++) {
                float u0 = (float)x / secs * 6.2831853f, u1 = (float)(x + 1) / secs * 6.2831853f;
                auto P = [&](float uu, float vv) {
                    glm::vec3 d(std::sin(vv) * std::cos(uu), std::cos(vv), std::sin(vv) * std::sin(uu));
                    return std::make_pair(c + d * r, d);
                };
                auto a = P(u0, v0), b = P(u1, v0), cc = P(u1, v1), dd = P(u0, v1);
                push_v(v, a.first, a.second, 1); push_v(v, b.first, b.second, 1); push_v(v, cc.first, cc.second, 1);
                push_v(v, a.first, a.second, 1); push_v(v, cc.first, cc.second, 1); push_v(v, dd.first, dd.second, 1);
            }
        }
    }

    // Build one species template at a NOMINAL trunk height (instances scale it
    // to their own trunk_h). Oak: tapered trunk + a cluster of overlapping leaf
    // spheres for a bushy rounded crown. Pine: taller thin trunk + stacked
    // shrinking spheres for a conical evergreen silhouette.
    void build_template(bool pine, Mesh& out) {
        std::vector<float> v;
        if (!pine) {
            // --- Oak: nominal trunk 7 tall, bushy crown ---
            float th = 7.0f;
            add_trunk(v, 0.0f, th, 0.42f, 0.26f);
            // crown: a big central sphere + 5 offset lobes so the silhouette is
            // round but broken up (not a perfect ball).
            float cr = 2.9f;                 // nominal canopy radius
            glm::vec3 top(0.0f, th + 0.6f, 0.0f);
            add_leaf_sphere(v, top, cr);
            const glm::vec3 lobes[5] = {
                { cr*0.75f, th+0.1f,  0.10f}, {-cr*0.70f, th+0.25f, 0.30f},
                { 0.15f,    th+0.2f,  cr*0.75f}, { 0.20f,   th+0.35f,-cr*0.70f},
                { 0.0f,     th+1.5f,  0.0f}
            };
            for (auto& L : lobes) add_leaf_sphere(v, L, cr * 0.62f);
            out.nominal_h = th;
        } else {
            // --- Pine: nominal trunk 11 tall, conical stacked spheres ---
            float th = 11.0f;
            add_trunk(v, 0.0f, th + 1.0f, 0.34f, 0.16f);
            // stacked, shrinking spheres from mid-trunk to a pointed crown
            int layers = 5;
            float base_y = th * 0.42f, topY = th + 1.4f;
            for (int i = 0; i < layers; i++) {
                float f = (float)i / (layers - 1);
                float y = base_y + (topY - base_y) * f;
                float r = (1.0f - f) * 2.5f + 0.35f;
                add_leaf_sphere(v, glm::vec3(0, y, 0), r);
            }
            out.nominal_h = th;
        }

        out.count = (GLsizei)(v.size() / 7);
        glGenVertexArrays(1, &out.vao);
        glGenBuffers(1, &out.vbo);
        glBindVertexArray(out.vao);
        glBindBuffer(GL_ARRAY_BUFFER, out.vbo);
        glBufferData(GL_ARRAY_BUFFER, v.size() * sizeof(float), v.data(), GL_STATIC_DRAW);
        GLsizei st = 7 * sizeof(float);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, st, (void*)0);                 glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, st, (void*)(3*sizeof(float))); glEnableVertexAttribArray(1);
        glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, st, (void*)(6*sizeof(float))); glEnableVertexAttribArray(2);
        glBindVertexArray(0);
    }

    // --- shaders ------------------------------------------------------------
    static const char* VS() {
        return
        "#version 330 core\n"
        "layout(location=0) in vec3 a_pos;\n"
        "layout(location=1) in vec3 a_nrm;\n"
        "layout(location=2) in float a_mat;\n"
        "uniform mat4 u_model, u_view, u_proj;\n"
        "out vec3 v_world; out vec3 v_nrm; out float v_mat; out vec3 v_local;\n"
        "void main(){\n"
        "  vec4 wp = u_model * vec4(a_pos,1.0);\n"
        "  v_world = wp.xyz;\n"
        "  v_local = a_pos;\n"
        "  v_nrm = normalize(mat3(u_model) * a_nrm);\n"
        "  v_mat = a_mat;\n"
        "  gl_Position = u_proj * u_view * wp;\n"
        "}\n";
    }

    static const char* FS() {
        return
        "#version 330 core\n"
        "in vec3 v_world; in vec3 v_nrm; in float v_mat; in vec3 v_local;\n"
        "uniform vec3 u_sun_dir, u_cam, u_fog_color;\n"
        "uniform float u_fog_start, u_fog_end, u_time;\n"
        "uniform int u_hdr_out;\n"
        "out vec4 frag;\n"
        // cheap value noise so leaves/bark aren't flat fills
        "float hash(vec2 p){ p=fract(p*vec2(127.1,311.7)); p+=dot(p,p+34.5); return fract(p.x*p.y);}\n"
        "float noise(vec2 p){ vec2 i=floor(p),f=fract(p); f=f*f*(3.0-2.0*f);\n"
        "  return mix(mix(hash(i),hash(i+vec2(1,0)),f.x),mix(hash(i+vec2(0,1)),hash(i+vec2(1,1)),f.x),f.y);}\n"
        "void main(){\n"
        "  vec3 n = normalize(v_nrm);\n"
        "  vec3 to_sun = normalize(u_sun_dir);\n"
        // base colour per material
        "  vec3 base;\n"
        "  if (v_mat > 0.5) {\n"
        // leaves: dappled green clumps + sky gaps, brighter on sun side
        "    float n1 = noise(v_local.xz*1.3 + v_local.y*0.8);\n"
        "    float n2 = noise(v_local.xz*3.7 - v_local.y*1.5);\n"
        "    float clump = n1*0.65 + n2*0.35;\n"
        "    vec3 dark=vec3(0.05,0.18,0.04), mid=vec3(0.12,0.34,0.09), lit=vec3(0.33,0.58,0.18);\n"
        "    base = mix(dark, mid, smoothstep(0.25,0.6,clump));\n"
        "    base = mix(base, lit, smoothstep(0.65,0.95,clump));\n"
        "  } else {\n"
        // bark: vertical streaky brown
        "    float streak = noise(vec2(atan(v_local.z,v_local.x)*2.2, v_local.y*2.5));\n"
        "    base = mix(vec3(0.20,0.13,0.07), vec3(0.34,0.24,0.14), streak);\n"
        "  }\n"
        // day/night ambient (same curve as terrain) + sun diffuse
        "  float dayf = smoothstep(-0.15, 0.20, to_sun.y);\n"
        "  float skyf = n.y*0.5+0.5;\n"
        "  vec3 sky_amb = mix(vec3(0.46,0.46,0.48), vec3(0.58,0.68,0.88), skyf);\n"
        "  sky_amb = mix(sky_amb*vec3(0.40,0.50,0.85), sky_amb, dayf);\n"
        "  float amb = mix(0.12, 0.70, dayf);\n"
        "  float NdotL = max(dot(n, to_sun), 0.0);\n"
        "  vec3 sun_col = vec3(1.1,1.0,0.9);\n"
        "  vec3 color = base*amb*sky_amb + base*NdotL*sun_col;\n"
        // leaves get a soft self-AO toward the underside so the crown reads round
        "  if (v_mat > 0.5) color *= mix(0.55, 1.0, n.y*0.5+0.5);\n"
        // distance fog
        "  float d = length(v_world - u_cam);\n"
        "  float fog = clamp((d-u_fog_start)/max(u_fog_end-u_fog_start,1.0),0.0,1.0); fog*=fog;\n"
        "  color = mix(color, u_fog_color, fog);\n"
        "  if (u_hdr_out==1){ frag=vec4(color,1.0); }\n"
        "  else {\n"
        "    color*=1.05; vec3 x=color;\n"
        "    color = clamp((x*(2.51*x+0.03))/(x*(2.43*x+0.59)+0.14),0.0,1.0);\n"
        "    color = pow(color, vec3(1.0/2.2));\n"
        "    frag=vec4(color,1.0);\n"
        "  }\n"
        "}\n";
    }

    static GLuint compile(GLenum type, const char* src) {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr); glCompileShader(s);
        GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) { char log[512]; glGetShaderInfoLog(s, 512, nullptr, log);
                   std::cerr << "[sdfcraft] tree shader: " << log << "\n"; }
        return s;
    }
    static GLuint link(const char* vs, const char* fs) {
        GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
        GLuint p = glCreateProgram();
        glAttachShader(p, v); glAttachShader(p, f); glLinkProgram(p);
        GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
        glDeleteShader(v); glDeleteShader(f);
        if (!ok) { char log[512]; glGetProgramInfoLog(p, 512, nullptr, log);
                   std::cerr << "[sdfcraft] tree link: " << log << "\n"; return 0; }
        return p;
    }

};

} // namespace sdfcraft
