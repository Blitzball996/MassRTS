#pragma once
// =============================================================================
// SDFCraft - OpenGL chunk renderer
// -----------------------------------------------------------------------------
// Owns the GPU buffers for chunk meshes, a wireframe block-selection box, and
// the shaders for both. Self-contained shader loading keeps the module
// decoupled from the RTS Renderer. Meshes are rebuilt lazily when a chunk's
// dirty_mesh flag is set (after generation or a player edit).
// =============================================================================
#include <glad/glad.h>
#include <glm/glm.hpp>
#include "world.h"
#include "mesher.h"
#include "mc_mesher.h"
#include <unordered_map>
#include <string>
#include <fstream>
#include <sstream>
#include <iostream>
#include <vector>
#include <algorithm>
#include <chrono>

namespace sdfcraft {

class ChunkRenderer {
public:
    bool init(const std::string& shader_dir) {
        chunk_prog_  = load_program(shader_dir + "sdfcraft_chunk.vert",  shader_dir + "sdfcraft_chunk.frag");
        select_prog_ = load_program(shader_dir + "sdfcraft_select.vert", shader_dir + "sdfcraft_select.frag");
        if (!chunk_prog_ || !select_prog_) return false;
        init_select_box();
        return true;
    }

    void shutdown() {
        for (auto& kv : gpu_) free_gpu(kv.second);
        gpu_.clear();
        if (select_vao_) { glDeleteVertexArrays(1, &select_vao_); glDeleteBuffers(1, &select_vbo_); }
        if (chunk_prog_)  glDeleteProgram(chunk_prog_);
        if (select_prog_) glDeleteProgram(select_prog_);
    }

    // Rebuild GPU meshes for any dirty/loaded chunks (bounded per frame).
    // Re-mesh dirty chunks under a TIME BUDGET (default ~4ms/frame). The old
    // fixed-count limit still spiked when several heavy chunks landed in one
    // frame; a wall-clock budget instead guarantees a flat frame time no matter
    // how many chunks flooded in when crossing into a new region — the rest
    // simply finish over the next few frames. Nearest-to-camera chunks are
    // meshed first so the ground around the player resolves immediately.
    void sync(World& world, glm::vec3 cam, double budget_ms = 4.0) {
        // collect dirty chunks, nearest first
        struct D { Chunk* c; float d2; };
        std::vector<D> dirty;
        for (auto& kv : world.chunks())
            if (kv.second.dirty_mesh) {
                float cx = (kv.first.cx + 0.5f) * CHUNK_SX;
                float cz = (kv.first.cz + 0.5f) * CHUNK_SZ;
                float dx = cx - cam.x, dz = cz - cam.z;
                dirty.push_back({ &kv.second, dx*dx + dz*dz });
            }
        std::sort(dirty.begin(), dirty.end(),
                  [](const D& a, const D& b){ return a.d2 < b.d2; });

        auto t0 = std::chrono::steady_clock::now();
        for (D& d : dirty) {
            Chunk& c = *d.c;
            // Natural terrain → smooth Marching-Cubes isosurface (true SDF look).
            // Object blocks (logs/leaves/planks/glass/placed) → discrete cubes.
            ChunkMesh m;
            MCMesher::build(world, c, m);   // fills m.opaque (smooth ground)
            ChunkMesh cube;
            Mesher::build(world, c, cube);  // fills cube.opaque (trees/objects) + cube.transparent
            // Merge the cube objects' opaque faces into the terrain opaque buffer
            // so trees actually render — previously cube.opaque was dropped and
            // trunks/leaves never drew.
            m.opaque.insert(m.opaque.end(), cube.opaque.begin(), cube.opaque.end());
            m.transparent = std::move(cube.transparent);
            upload(c.key, m);
            c.dirty_mesh = false;
            double el = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - t0).count();
            if (el >= budget_ms) break;     // out of time; continue next frame
        }
        // drop GPU buffers for chunks the world no longer holds
        for (auto it = gpu_.begin(); it != gpu_.end();) {
            if (!world.chunk_loaded(it->first)) { free_gpu(it->second); it = gpu_.erase(it); }
            else ++it;
        }
    }

    void render(World& world, const glm::mat4& view, const glm::mat4& proj,
                glm::vec3 cam, glm::vec3 sun_dir, glm::vec3 fog_color,
                float fog_start, float fog_end) {
        glUseProgram(chunk_prog_);
        glUniformMatrix4fv(glGetUniformLocation(chunk_prog_, "u_view"), 1, GL_FALSE, &view[0][0]);
        glUniformMatrix4fv(glGetUniformLocation(chunk_prog_, "u_proj"), 1, GL_FALSE, &proj[0][0]);
        glUniform3fv(glGetUniformLocation(chunk_prog_, "u_sun_dir"), 1, &sun_dir[0]);
        glUniform3fv(glGetUniformLocation(chunk_prog_, "u_cam"), 1, &cam[0]);
        glUniform3fv(glGetUniformLocation(chunk_prog_, "u_fog_color"), 1, &fog_color[0]);
        glUniform1f(glGetUniformLocation(chunk_prog_, "u_fog_start"), fog_start);
        glUniform1f(glGetUniformLocation(chunk_prog_, "u_fog_end"), fog_end);

        // opaque pass
        glUniform1f(glGetUniformLocation(chunk_prog_, "u_alpha"), 1.0f);
        glEnable(GL_DEPTH_TEST); glDisable(GL_BLEND);
        for (auto& kv : gpu_) {
            if (kv.second.opaque_count == 0) continue;
            glBindVertexArray(kv.second.opaque_vao);
            glDrawArrays(GL_TRIANGLES, 0, kv.second.opaque_count);
        }
        // transparent pass (water/glass/leaves), depth-write off
        glUniform1f(glGetUniformLocation(chunk_prog_, "u_alpha"), 0.72f);
        glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
        for (auto& kv : gpu_) {
            if (kv.second.trans_count == 0) continue;
            glBindVertexArray(kv.second.trans_vao);
            glDrawArrays(GL_TRIANGLES, 0, kv.second.trans_count);
        }
        glDepthMask(GL_TRUE); glDisable(GL_BLEND);
        glBindVertexArray(0);
    }

    void render_selection(const glm::mat4& view, const glm::mat4& proj, glm::ivec3 block) {
        glUseProgram(select_prog_);
        glUniformMatrix4fv(glGetUniformLocation(select_prog_, "u_view"), 1, GL_FALSE, &view[0][0]);
        glUniformMatrix4fv(glGetUniformLocation(select_prog_, "u_proj"), 1, GL_FALSE, &proj[0][0]);
        glm::vec3 b((float)block.x, (float)block.y, (float)block.z);
        glUniform3fv(glGetUniformLocation(select_prog_, "u_block"), 1, &b[0]);
        glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glLineWidth(2.0f);
        glBindVertexArray(select_vao_);
        glDrawArrays(GL_LINES, 0, 24);
        glBindVertexArray(0);
        glDisable(GL_BLEND);
    }

private:
    struct Gpu {
        GLuint opaque_vao=0, opaque_vbo=0; GLsizei opaque_count=0;
        GLuint trans_vao=0,  trans_vbo=0;  GLsizei trans_count=0;
    };
    std::unordered_map<ChunkKey, Gpu, ChunkKeyHash> gpu_;
    GLuint chunk_prog_=0, select_prog_=0;
    GLuint select_vao_=0, select_vbo_=0;

    static void make_buffer(GLuint& vao, GLuint& vbo, const std::vector<float>& data) {
        if (!vao) glGenVertexArrays(1, &vao);
        if (!vbo) glGenBuffers(1, &vbo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, data.size()*sizeof(float), data.data(), GL_DYNAMIC_DRAW);
        GLsizei stride = 10 * sizeof(float);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)0);                  glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)(3*sizeof(float)));  glEnableVertexAttribArray(1);
        glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, stride, (void*)(6*sizeof(float)));  glEnableVertexAttribArray(2);
        glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, stride, (void*)(9*sizeof(float)));  glEnableVertexAttribArray(3);
        glBindVertexArray(0);
    }

    void upload(ChunkKey k, const ChunkMesh& m) {
        Gpu& g = gpu_[k];
        if (!m.opaque.empty()) { make_buffer(g.opaque_vao, g.opaque_vbo, m.opaque); g.opaque_count = (GLsizei)(m.opaque.size()/10); }
        else g.opaque_count = 0;
        if (!m.transparent.empty()) { make_buffer(g.trans_vao, g.trans_vbo, m.transparent); g.trans_count = (GLsizei)(m.transparent.size()/10); }
        else g.trans_count = 0;
    }

    static void free_gpu(Gpu& g) {
        if (g.opaque_vao) { glDeleteVertexArrays(1,&g.opaque_vao); glDeleteBuffers(1,&g.opaque_vbo); }
        if (g.trans_vao)  { glDeleteVertexArrays(1,&g.trans_vao);  glDeleteBuffers(1,&g.trans_vbo); }
        g = Gpu{};
    }

    void init_select_box() {
        // 12 edges of unit cube as line list
        static const float v[8][3] = {
            {0,0,0},{1,0,0},{1,1,0},{0,1,0},{0,0,1},{1,0,1},{1,1,1},{0,1,1}
        };
        static const int e[12][2] = {
            {0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}
        };
        std::vector<float> data;
        for (auto& ed : e) for (int i=0;i<2;i++) { data.push_back(v[ed[i]][0]); data.push_back(v[ed[i]][1]); data.push_back(v[ed[i]][2]); }
        glGenVertexArrays(1, &select_vao_);
        glGenBuffers(1, &select_vbo_);
        glBindVertexArray(select_vao_);
        glBindBuffer(GL_ARRAY_BUFFER, select_vbo_);
        glBufferData(GL_ARRAY_BUFFER, data.size()*sizeof(float), data.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3*sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glBindVertexArray(0);
    }

    // --- minimal shader compilation (self-contained) ---
    static std::string read_file(const std::string& path) {
        std::ifstream f(path);
        if (!f) { std::cerr << "[sdfcraft] missing shader: " << path << "\n"; return ""; }
        std::stringstream ss; ss << f.rdbuf(); return ss.str();
    }
    static GLuint compile(GLenum type, const std::string& src) {
        GLuint s = glCreateShader(type);
        const char* c = src.c_str();
        glShaderSource(s, 1, &c, nullptr);
        glCompileShader(s);
        GLint ok=0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) { char log[1024]; glGetShaderInfoLog(s, 1024, nullptr, log); std::cerr << "[sdfcraft] shader error: " << log << "\n"; }
        return s;
    }
    static GLuint load_program(const std::string& vs, const std::string& fs) {
        std::string vsrc = read_file(vs), fsrc = read_file(fs);
        if (vsrc.empty() || fsrc.empty()) return 0;
        GLuint v = compile(GL_VERTEX_SHADER, vsrc), f = compile(GL_FRAGMENT_SHADER, fsrc);
        GLuint p = glCreateProgram();
        glAttachShader(p, v); glAttachShader(p, f); glLinkProgram(p);
        GLint ok=0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
        if (!ok) { char log[1024]; glGetProgramInfoLog(p, 1024, nullptr, log); std::cerr << "[sdfcraft] link error: " << log << "\n"; }
        glDeleteShader(v); glDeleteShader(f);
        return ok ? p : 0;
    }
};

} // namespace sdfcraft
