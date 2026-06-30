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
#include <glm/gtc/matrix_transform.hpp>
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

// stb_image already implemented in planet_renderer.h
#include "../stb_image.h"

namespace sdfcraft {

class ChunkRenderer {
public:
    // When true, the chunk shader emits LINEAR HDR for the PostFX composite to
    // tonemap; when false it tonemaps + gammas inline (PostFX disabled fallback).
    bool hdr_out = false;

    bool init(const std::string& shader_dir) {
        chunk_prog_  = load_program(shader_dir + "sdfcraft_chunk.vert",  shader_dir + "sdfcraft_chunk.frag");
        select_prog_ = load_program(shader_dir + "sdfcraft_select.vert", shader_dir + "sdfcraft_select.frag");
        depth_prog_  = load_program(shader_dir + "sdfcraft_depth.vert",  shader_dir + "sdfcraft_depth.frag");
        if (!chunk_prog_ || !select_prog_) return false;
        init_select_box();
        if (depth_prog_) init_shadow_fbo();
        
        // Load block textures (3DWorld assets)
        tex_grass_      = load_texture("assets/textures/blocks/grass.png");
        tex_dirt_       = load_texture("assets/textures/blocks/dirt.png");
        tex_rock_       = load_texture("assets/textures/blocks/rock.png");
        tex_rock2_      = load_texture("assets/textures/blocks/rock2.png");
        tex_sand_       = load_texture("assets/textures/blocks/desert_sand.jpg");
        tex_snow_       = load_texture("assets/textures/blocks/snow2.jpg");
        tex_gravel_     = load_texture("assets/textures/blocks/gravel.jpg");
        tex_mossy_rock_ = load_texture("assets/textures/blocks/mossy_rock.jpg");
        tex_wood_       = load_texture("assets/textures/blocks/wood.jpg");
        tex_bark_       = load_texture("assets/textures/blocks/bark.jpg");
        
        return true;
    }

    void shutdown() {
        for (auto& kv : gpu_) free_gpu(kv.second);
        gpu_.clear();
        if (select_vao_) { glDeleteVertexArrays(1, &select_vao_); glDeleteBuffers(1, &select_vbo_); }
        if (chunk_prog_)  glDeleteProgram(chunk_prog_);
        if (select_prog_) glDeleteProgram(select_prog_);
        if (depth_prog_)  glDeleteProgram(depth_prog_);
        if (shadow_fbo_)  glDeleteFramebuffers(1, &shadow_fbo_);
        if (shadow_tex_)  glDeleteTextures(1, &shadow_tex_);

        // Clean up textures
        GLuint textures[] = {tex_grass_, tex_dirt_, tex_rock_, tex_rock2_, tex_sand_,
                             tex_snow_, tex_gravel_, tex_mossy_rock_, tex_wood_, tex_bark_};
        glDeleteTextures(10, textures);
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

    // Render all loaded chunk geometry from the sun's POV into the shadow depth
    // map. Call once per frame BEFORE render(). The light frustum is an ortho box
    // fitted around the camera so shadow resolution stays concentrated near the
    // player. Returns false (and render() falls back to no shadows) if the FBO
    // wasn't created.
    bool shadow_pass(glm::vec3 cam, glm::vec3 sun_dir, int restore_vp_w, int restore_vp_h) {
        if (!depth_prog_ || !shadow_fbo_) return false;
        // Light looks from the sun toward the camera region. Ortho half-extent
        // covers a generous radius around the player; depth range spans the world
        // height so tall mountains still cast.
        glm::vec3 L = glm::normalize(sun_dir);
        if (L.y < 0.05f) return false;   // sun at/below horizon: skip (night)
        float R = 140.0f;                // XZ coverage radius around camera
        glm::vec3 center = glm::vec3(cam.x, 64.0f, cam.z);
        glm::vec3 eye = center + L * 300.0f;
        glm::vec3 up = std::fabs(L.y) > 0.99f ? glm::vec3(1,0,0) : glm::vec3(0,1,0);
        glm::mat4 lview = glm::lookAt(eye, center, up);
        glm::mat4 lproj = glm::ortho(-R, R, -R, R, 10.0f, 600.0f);
        light_vp_ = lproj * lview;

        glViewport(0, 0, SHADOW_SIZE, SHADOW_SIZE);
        glBindFramebuffer(GL_FRAMEBUFFER, shadow_fbo_);
        glClear(GL_DEPTH_BUFFER_BIT);
        glUseProgram(depth_prog_);
        glUniformMatrix4fv(glGetUniformLocation(depth_prog_, "u_light_vp"), 1, GL_FALSE, &light_vp_[0][0]);
        // Front-face cull during the depth pass reduces peter-panning/acne on the
        // lit surfaces (standard shadow-map trick).
        glEnable(GL_DEPTH_TEST);
        GLboolean cull_was = glIsEnabled(GL_CULL_FACE);
        glEnable(GL_CULL_FACE); glCullFace(GL_FRONT);
        for (auto& kv : gpu_) {
            if (kv.second.opaque_count == 0) continue;
            glBindVertexArray(kv.second.opaque_vao);
            glDrawArrays(GL_TRIANGLES, 0, kv.second.opaque_count);
        }
        glCullFace(GL_BACK);
        if (!cull_was) glDisable(GL_CULL_FACE);
        glBindVertexArray(0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        // CRITICAL: restore viewport to framebuffer size, else main pass renders
        // at shadow-map resolution (2048x2048) into the window and only fills the
        // left half (or less).
        glViewport(0, 0, restore_vp_w, restore_vp_h);
        return true;
    }

    void render(World& world, const glm::mat4& view, const glm::mat4& proj,
                glm::vec3 cam, glm::vec3 sun_dir, glm::vec3 fog_color,
                float fog_start, float fog_end, float time = 0.0f) {
        glUseProgram(chunk_prog_);
        glUniformMatrix4fv(glGetUniformLocation(chunk_prog_, "u_view"), 1, GL_FALSE, &view[0][0]);
        glUniformMatrix4fv(glGetUniformLocation(chunk_prog_, "u_proj"), 1, GL_FALSE, &proj[0][0]);
        glUniform3fv(glGetUniformLocation(chunk_prog_, "u_sun_dir"), 1, &sun_dir[0]);
        glUniform3fv(glGetUniformLocation(chunk_prog_, "u_cam"), 1, &cam[0]);
        glUniform3fv(glGetUniformLocation(chunk_prog_, "u_fog_color"), 1, &fog_color[0]);
        glUniform1f(glGetUniformLocation(chunk_prog_, "u_fog_start"), fog_start);
        glUniform1f(glGetUniformLocation(chunk_prog_, "u_fog_end"), fog_end);
        glUniform1f(glGetUniformLocation(chunk_prog_, "u_time"), time);
        glUniform1i(glGetUniformLocation(chunk_prog_, "u_hdr_out"), hdr_out ? 1 : 0);

        // Shadow map: bind to texture unit 10, hand the shader the light matrix.
        // u_shadow_on=0 disables sampling (night / FBO missing) so the branch is
        // free of artifacts when there's no valid depth map.
        int shadow_on = (depth_prog_ && shadow_tex_) ? 1 : 0;
        glUniformMatrix4fv(glGetUniformLocation(chunk_prog_, "u_light_vp"), 1, GL_FALSE, &light_vp_[0][0]);
        glUniform1i(glGetUniformLocation(chunk_prog_, "u_shadow_on"), shadow_on);
        if (shadow_on) {
            glActiveTexture(GL_TEXTURE10);
            glBindTexture(GL_TEXTURE_2D, shadow_tex_);
            glUniform1i(glGetUniformLocation(chunk_prog_, "u_shadow_map"), 10);
        }
        
        // Bind block textures (all materials use these)
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tex_grass_);
        glUniform1i(glGetUniformLocation(chunk_prog_, "u_tex_grass"), 0);
        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, tex_dirt_);
        glUniform1i(glGetUniformLocation(chunk_prog_, "u_tex_dirt"), 1);
        glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, tex_rock_);
        glUniform1i(glGetUniformLocation(chunk_prog_, "u_tex_rock"), 2);
        glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, tex_rock2_);
        glUniform1i(glGetUniformLocation(chunk_prog_, "u_tex_rock2"), 3);
        glActiveTexture(GL_TEXTURE4); glBindTexture(GL_TEXTURE_2D, tex_sand_);
        glUniform1i(glGetUniformLocation(chunk_prog_, "u_tex_sand"), 4);
        glActiveTexture(GL_TEXTURE5); glBindTexture(GL_TEXTURE_2D, tex_snow_);
        glUniform1i(glGetUniformLocation(chunk_prog_, "u_tex_snow"), 5);
        glActiveTexture(GL_TEXTURE6); glBindTexture(GL_TEXTURE_2D, tex_gravel_);
        glUniform1i(glGetUniformLocation(chunk_prog_, "u_tex_gravel"), 6);
        glActiveTexture(GL_TEXTURE7); glBindTexture(GL_TEXTURE_2D, tex_mossy_rock_);
        glUniform1i(glGetUniformLocation(chunk_prog_, "u_tex_mossy_rock"), 7);
        glActiveTexture(GL_TEXTURE8); glBindTexture(GL_TEXTURE_2D, tex_wood_);
        glUniform1i(glGetUniformLocation(chunk_prog_, "u_tex_wood"), 8);
        glActiveTexture(GL_TEXTURE9); glBindTexture(GL_TEXTURE_2D, tex_bark_);
        glUniform1i(glGetUniformLocation(chunk_prog_, "u_tex_bark"), 9);

        // opaque pass
        glUniform1f(glGetUniformLocation(chunk_prog_, "u_alpha"), 1.0f);
        glEnable(GL_DEPTH_TEST); glDisable(GL_BLEND);
        // Backface culling is ON. The MC mesher now corrects every triangle's
        // winding against its outward SDF-gradient normal (see emit_tri), so the
        // complex carved configs (overhangs, caves, thin dug walls) are wound
        // consistently outward. Culling the back faces removes the "folded paper"
        // double surfaces that showed on deep digs when we drew terrain
        // double-sided, without re-introducing see-through holes.
        glEnable(GL_CULL_FACE); glCullFace(GL_BACK); glFrontFace(GL_CCW);
        for (auto& kv : gpu_) {
            if (kv.second.opaque_count == 0) continue;
            glBindVertexArray(kv.second.opaque_vao);
            glDrawArrays(GL_TRIANGLES, 0, kv.second.opaque_count);
        }
        // transparent pass (water/glass/leaves) — draw double-sided so thin
        // water/leaf surfaces are visible from inside too; depth-write off.
        glDisable(GL_CULL_FACE);
        glUniform1f(glGetUniformLocation(chunk_prog_, "u_alpha"), 0.72f);
        glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
        for (auto& kv : gpu_) {
            if (kv.second.trans_count == 0) continue;
            glBindVertexArray(kv.second.trans_vao);
            glDrawArrays(GL_TRIANGLES, 0, kv.second.trans_count);
        }
        glDepthMask(GL_TRUE); glDisable(GL_BLEND);
        glEnable(GL_CULL_FACE);   // restore (we disabled it for the terrain pass)
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

    // --- shadow mapping (directional sun) ---
    GLuint depth_prog_=0, shadow_fbo_=0, shadow_tex_=0;
    static constexpr int SHADOW_SIZE = 2048;
    glm::mat4 light_vp_{1.0f};   // last light-space matrix (for the main pass)
    
    // Block textures (3DWorld assets for realistic terrain)
    GLuint tex_grass_=0, tex_dirt_=0, tex_rock_=0, tex_rock2_=0, 
           tex_sand_=0, tex_snow_=0, tex_gravel_=0, tex_mossy_rock_=0,
           tex_wood_=0, tex_bark_=0;

    static void make_buffer(GLuint& vao, GLuint& vbo, const std::vector<float>& data) {
        if (!vao) glGenVertexArrays(1, &vao);
        if (!vbo) glGenBuffers(1, &vbo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, data.size()*sizeof(float), data.data(), GL_DYNAMIC_DRAW);
        GLsizei stride = 11 * sizeof(float);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)0);                  glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)(3*sizeof(float)));  glEnableVertexAttribArray(1);
        glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, stride, (void*)(6*sizeof(float)));  glEnableVertexAttribArray(2);
        glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, stride, (void*)(9*sizeof(float)));  glEnableVertexAttribArray(3);
        glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, stride, (void*)(10*sizeof(float))); glEnableVertexAttribArray(4);
        glBindVertexArray(0);
    }

    void upload(ChunkKey k, const ChunkMesh& m) {
        Gpu& g = gpu_[k];
        if (!m.opaque.empty()) { make_buffer(g.opaque_vao, g.opaque_vbo, m.opaque); g.opaque_count = (GLsizei)(m.opaque.size()/11); }
        else g.opaque_count = 0;
        if (!m.transparent.empty()) { make_buffer(g.trans_vao, g.trans_vbo, m.transparent); g.trans_count = (GLsizei)(m.transparent.size()/11); }
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

    void init_shadow_fbo() {
        glGenFramebuffers(1, &shadow_fbo_);
        glBindFramebuffer(GL_FRAMEBUFFER, shadow_fbo_);
        // Depth texture: stores light-space Z for shadow comparison. No color
        // attachment needed (depth-only pass). CLAMP_TO_BORDER with white border
        // so any sample outside the shadow frustum reads as lit (depth=1>fragZ).
        glGenTextures(1, &shadow_tex_);
        glBindTexture(GL_TEXTURE_2D, shadow_tex_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, SHADOW_SIZE, SHADOW_SIZE, 0,
                     GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
        float border[] = {1.0f, 1.0f, 1.0f, 1.0f};
        glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, shadow_tex_, 0);
        glDrawBuffer(GL_NONE); glReadBuffer(GL_NONE);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            std::cerr << "[sdfcraft] shadow FBO incomplete\n";
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
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
    
    // Load texture from disk (uses stb_image, same as planet_renderer)
    static GLuint load_texture(const char* path) {
        int w, h, ch;
        stbi_set_flip_vertically_on_load(0);
        unsigned char* data = stbi_load(path, &w, &h, &ch, 0);
        if (!data) {
            fprintf(stderr, "Failed to load block texture: %s\n", path);
            return 0;
        }
        GLuint tex;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        GLenum fmt = (ch == 3) ? GL_RGB : GL_RGBA;
        glTexImage2D(GL_TEXTURE_2D, 0, fmt, w, h, 0, fmt, GL_UNSIGNED_BYTE, data);
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        // Enable anisotropic filtering for crisp textures at oblique angles
        float aniso = 0.0f;
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &aniso);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, std::min(aniso, 8.0f));
        stbi_image_free(data);
        return tex;
    }
};

} // namespace sdfcraft
