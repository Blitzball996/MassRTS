#pragma once
// =============================================================================
// SDFCraft - Mob renderer (Phase D placeholder)
// -----------------------------------------------------------------------------
// Draws every live entity as a lit, solid-colour box sized to its AABB
// (MobDef.width/height) and tinted by MobDef.color. This is a deliberate
// placeholder: A3 will replace it with the MinecraftConsoles humanoid /
// animal models + skins, but a coloured body is enough to *see* the mobs the
// Phase D AI/spawn loop produces, walk up to them, fight them and watch the
// day/night spawn rules work.
//
// Self-contained like hud_renderer.h: shaders are embedded so the module needs
// no external assets and can't silently fail on a missing file. One draw call
// per mob (population is capped at ~70) keeps it trivially cheap. A short red
// "hurt flash" tints a mob that was just damaged so combat reads clearly.
// =============================================================================
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <vector>
#include <iostream>
#include "entity.h"

namespace sdfcraft {

class MobRenderer {
public:
    bool init() {
        const char* VS =
            "#version 330 core\n"
            "layout(location=0) in vec3 a_pos;\n"
            "layout(location=1) in vec3 a_nrm;\n"
            "uniform mat4 u_view, u_proj, u_model;\n"
            "out vec3 v_nrm;\n"
            "void main(){ v_nrm = mat3(u_model)*a_nrm;\n"
            "  gl_Position = u_proj*u_view*u_model*vec4(a_pos,1.0); }\n";
        const char* FS =
            "#version 330 core\n"
            "in vec3 v_nrm; out vec4 frag;\n"
            "uniform vec3 u_color, u_sun_dir;\n"
            "uniform float u_flash;\n"   // 0..1 hurt flash
            "void main(){\n"
            "  vec3 n = normalize(v_nrm);\n"
            "  float d = max(dot(n, normalize(u_sun_dir)), 0.0);\n"
            "  vec3 lit = u_color * (0.45 + 0.65*d);\n"
            "  lit = mix(lit, vec3(1.0,0.15,0.10), u_flash);\n"
            "  frag = vec4(lit, 1.0);\n"
            "}\n";
        prog_ = link(VS, FS);
        if (!prog_) return false;
        build_cube();
        return true;
    }

    void shutdown() {
        if (vbo_) glDeleteBuffers(1, &vbo_);
        if (vao_) glDeleteVertexArrays(1, &vao_);
        if (prog_) glDeleteProgram(prog_);
        vbo_ = vao_ = prog_ = 0;
    }

    // Draw all live entities. `cam` is used only to skip far/behind mobs cheaply.
    void render(const std::vector<Entity>& mobs, const glm::mat4& view,
                const glm::mat4& proj, glm::vec3 sun_dir) {
        if (!prog_ || mobs.empty()) return;
        glUseProgram(prog_);
        glUniformMatrix4fv(glGetUniformLocation(prog_, "u_view"), 1, GL_FALSE, &view[0][0]);
        glUniformMatrix4fv(glGetUniformLocation(prog_, "u_proj"), 1, GL_FALSE, &proj[0][0]);
        glm::vec3 s = glm::normalize(sun_dir);
        glUniform3fv(glGetUniformLocation(prog_, "u_sun_dir"), 1, &s[0]);
        GLint loc_model = glGetUniformLocation(prog_, "u_model");
        GLint loc_color = glGetUniformLocation(prog_, "u_color");
        GLint loc_flash = glGetUniformLocation(prog_, "u_flash");

        glEnable(GL_DEPTH_TEST);
        glBindVertexArray(vao_);
        for (const Entity& e : mobs) {
            if (!e.alive) continue;
            const MobDef& d = e.def();
            // Box spans [pos.x-w, pos.x+w] x [pos.y, pos.y+h] x [pos.z-w, pos.z+w].
            // Unit cube is 0..1, so scale by (2w, h, 2w) and shift to the corner.
            glm::mat4 m(1.0f);
            m = glm::translate(m, glm::vec3(e.pos.x - d.width, e.pos.y, e.pos.z - d.width));
            m = glm::scale(m, glm::vec3(d.width * 2.0f, d.height, d.width * 2.0f));
            glUniformMatrix4fv(loc_model, 1, GL_FALSE, &m[0][0]);
            glUniform3fv(loc_color, 1, &d.color[0]);
            float flash = (e.hurt_cooldown > 0.0f) ? glm::clamp(e.hurt_cooldown / 0.4f, 0.0f, 1.0f) : 0.0f;
            glUniform1f(loc_flash, flash);
            glDrawArrays(GL_TRIANGLES, 0, 36);
        }
        glBindVertexArray(0);
    }

private:
    GLuint prog_ = 0, vao_ = 0, vbo_ = 0;

    void build_cube() {
        // 36 verts (pos3 + nrm3), unit cube 0..1, outward normals, CCW from outside.
        const float v[] = {
            // -Z face (n=0,0,-1)
            0,0,0, 0,0,-1,  1,1,0, 0,0,-1,  1,0,0, 0,0,-1,
            0,0,0, 0,0,-1,  0,1,0, 0,0,-1,  1,1,0, 0,0,-1,
            // +Z face (n=0,0,1)
            0,0,1, 0,0,1,   1,0,1, 0,0,1,   1,1,1, 0,0,1,
            0,0,1, 0,0,1,   1,1,1, 0,0,1,   0,1,1, 0,0,1,
            // -X face (n=-1,0,0)
            0,0,0, -1,0,0,  0,0,1, -1,0,0,  0,1,1, -1,0,0,
            0,0,0, -1,0,0,  0,1,1, -1,0,0,  0,1,0, -1,0,0,
            // +X face (n=1,0,0)
            1,0,0, 1,0,0,   1,1,0, 1,0,0,   1,1,1, 1,0,0,
            1,0,0, 1,0,0,   1,1,1, 1,0,0,   1,0,1, 1,0,0,
            // -Y face (n=0,-1,0)
            0,0,0, 0,-1,0,  1,0,0, 0,-1,0,  1,0,1, 0,-1,0,
            0,0,0, 0,-1,0,  1,0,1, 0,-1,0,  0,0,1, 0,-1,0,
            // +Y face (n=0,1,0)
            0,1,0, 0,1,0,   0,1,1, 0,1,0,   1,1,1, 0,1,0,
            0,1,0, 0,1,0,   1,1,1, 0,1,0,   1,1,0, 0,1,0,
        };
        glGenVertexArrays(1, &vao_);
        glGenBuffers(1, &vbo_);
        glBindVertexArray(vao_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6*sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6*sizeof(float), (void*)(3*sizeof(float)));
        glEnableVertexAttribArray(1);
        glBindVertexArray(0);
    }

    static GLuint compile(GLenum t, const char* s) {
        GLuint sh = glCreateShader(t);
        glShaderSource(sh, 1, &s, nullptr);
        glCompileShader(sh);
        GLint ok=0; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
        if (!ok){ char log[512]; glGetShaderInfoLog(sh,512,nullptr,log);
                  std::cerr << "[sdfcraft] mob shader error: " << log << "\n"; }
        return sh;
    }
    static GLuint link(const char* vs, const char* fs) {
        GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
        GLuint p = glCreateProgram();
        glAttachShader(p, v); glAttachShader(p, f); glLinkProgram(p);
        GLint ok=0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
        if (!ok){ char log[512]; glGetProgramInfoLog(p,512,nullptr,log);
                  std::cerr << "[sdfcraft] mob link error: " << log << "\n"; p=0; }
        glDeleteShader(v); glDeleteShader(f);
        return p;
    }
};

} // namespace sdfcraft
