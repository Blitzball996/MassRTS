#pragma once
// =============================================================================
// SDFCraft - Stylized Sky Renderer (Genshin Impact / 原神 style)
// -----------------------------------------------------------------------------
// Renders a bright gradient sky dome with volumetric clouds, sunset/sunrise
// warm glows, and sun disc. Replaces the flat glClearColor background.
// =============================================================================
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <string>
#include <fstream>
#include <sstream>
#include <vector>
#include <cmath>

namespace sdfcraft {

class SkyRenderer {
public:
    // When true, the sky shader emits LINEAR HDR for the PostFX composite to
    // tonemap; when false it tonemaps + gammas inline (PostFX disabled fallback).
    bool hdr_out = false;

    bool init(const std::string& shader_dir) {
        prog_ = load_program(shader_dir + "sdfcraft_sky.vert", shader_dir + "sdfcraft_sky.frag");
        if (!prog_) return false;
        
        // Create sky dome (icosphere)
        std::vector<float> verts;
        generate_sphere(verts, 2); // 2 subdivisions = 80 triangles
        
        glGenVertexArrays(1, &vao_);
        glGenBuffers(1, &vbo_);
        glBindVertexArray(vao_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glBindVertexArray(0);
        
        count_ = (GLsizei)(verts.size() / 3);
        return true;
    }
    
    void shutdown() {
        if (vbo_) glDeleteBuffers(1, &vbo_);
        if (vao_) glDeleteVertexArrays(1, &vao_);
        if (prog_) glDeleteProgram(prog_);
    }
    
    void render(const glm::mat4& view, const glm::mat4& proj, glm::vec3 sun_dir, float time) {
        glUseProgram(prog_);
        glUniformMatrix4fv(glGetUniformLocation(prog_, "u_view"), 1, GL_FALSE, &view[0][0]);
        glUniformMatrix4fv(glGetUniformLocation(prog_, "u_proj"), 1, GL_FALSE, &proj[0][0]);
        glUniform3fv(glGetUniformLocation(prog_, "u_sun_dir"), 1, &sun_dir[0]);
        glUniform1f(glGetUniformLocation(prog_, "u_time"), time);
        glUniform1i(glGetUniformLocation(prog_, "u_hdr_out"), hdr_out ? 1 : 0);

        // The dome is viewed from the inside, so its outward-wound triangles are
        // back-facing to the camera — with GL_CULL_FACE(BACK) enabled they'd all
        // be culled and the sky would never draw. Disable culling for the dome,
        // draw it at the far plane (LEQUAL so it survives the cleared depth), then
        // restore state for the terrain pass.
        glDisable(GL_CULL_FACE);
        glDepthFunc(GL_LEQUAL);
        glBindVertexArray(vao_);
        glDrawArrays(GL_TRIANGLES, 0, count_);
        glBindVertexArray(0);
        glDepthFunc(GL_LESS);
        glEnable(GL_CULL_FACE);
    }
    
private:
    GLuint prog_ = 0, vao_ = 0, vbo_ = 0;
    GLsizei count_ = 0;
    
    // Generate icosphere vertices (unit sphere)
    void generate_sphere(std::vector<float>& verts, int subdivisions) {
        // Start with icosahedron
        const float t = (1.0f + std::sqrt(5.0f)) / 2.0f;
        std::vector<glm::vec3> positions = {
            {-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0},
            {0, -1, t}, {0, 1, t}, {0, -1, -t}, {0, 1, -t},
            {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}
        };
        for (auto& p : positions) p = glm::normalize(p);
        
        std::vector<glm::ivec3> indices = {
            {0,11,5}, {0,5,1}, {0,1,7}, {0,7,10}, {0,10,11},
            {1,5,9}, {5,11,4}, {11,10,2}, {10,7,6}, {7,1,8},
            {3,9,4}, {3,4,2}, {3,2,6}, {3,6,8}, {3,8,9},
            {4,9,5}, {2,4,11}, {6,2,10}, {8,6,7}, {9,8,1}
        };
        
        // Subdivide
        for (int s = 0; s < subdivisions; s++) {
            std::vector<glm::ivec3> new_indices;
            for (auto tri : indices) {
                glm::vec3 a = positions[tri.x];
                glm::vec3 b = positions[tri.y];
                glm::vec3 c = positions[tri.z];
                
                glm::vec3 ab = glm::normalize((a + b) * 0.5f);
                glm::vec3 bc = glm::normalize((b + c) * 0.5f);
                glm::vec3 ca = glm::normalize((c + a) * 0.5f);
                
                int ia = tri.x, ib = tri.y, ic = tri.z;
                int iab = (int)positions.size(); positions.push_back(ab);
                int ibc = (int)positions.size(); positions.push_back(bc);
                int ica = (int)positions.size(); positions.push_back(ca);
                
                new_indices.push_back({ia, iab, ica});
                new_indices.push_back({ib, ibc, iab});
                new_indices.push_back({ic, ica, ibc});
                new_indices.push_back({iab, ibc, ica});
            }
            indices = new_indices;
        }
        
        // Flatten to vertex array
        for (auto tri : indices) {
            verts.push_back(positions[tri.x].x); verts.push_back(positions[tri.x].y); verts.push_back(positions[tri.x].z);
            verts.push_back(positions[tri.y].x); verts.push_back(positions[tri.y].y); verts.push_back(positions[tri.y].z);
            verts.push_back(positions[tri.z].x); verts.push_back(positions[tri.z].y); verts.push_back(positions[tri.z].z);
        }
    }
    
    static GLuint load_program(const std::string& vs_path, const std::string& fs_path) {
        auto read_file = [](const std::string& path) -> std::string {
            std::ifstream f(path);
            if (!f) return "";
            std::stringstream ss; ss << f.rdbuf();
            return ss.str();
        };
        
        auto compile = [](GLenum type, const std::string& src) -> GLuint {
            GLuint sh = glCreateShader(type);
            const char* c = src.c_str();
            glShaderSource(sh, 1, &c, nullptr);
            glCompileShader(sh);
            GLint ok = 0;
            glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
            if (!ok) {
                char log[512];
                glGetShaderInfoLog(sh, 512, nullptr, log);
                fprintf(stderr, "Shader compile error: %s\n", log);
                return 0;
            }
            return sh;
        };
        
        std::string vs_src = read_file(vs_path);
        std::string fs_src = read_file(fs_path);
        if (vs_src.empty() || fs_src.empty()) return 0;
        
        GLuint vs = compile(GL_VERTEX_SHADER, vs_src);
        GLuint fs = compile(GL_FRAGMENT_SHADER, fs_src);
        if (!vs || !fs) return 0;
        
        GLuint prog = glCreateProgram();
        glAttachShader(prog, vs);
        glAttachShader(prog, fs);
        glLinkProgram(prog);
        
        GLint ok = 0;
        glGetProgramiv(prog, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[512];
            glGetProgramInfoLog(prog, 512, nullptr, log);
            fprintf(stderr, "Program link error: %s\n", log);
            return 0;
        }
        
        glDeleteShader(vs);
        glDeleteShader(fs);
        return prog;
    }
};

} // namespace sdfcraft
