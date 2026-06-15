#pragma once
// ============================================================================
// SKINNED MODEL  —  runtime loader for the offline asset pipeline output.
//
// Loads .mesh (geometry + bone weights), .anim (animation texture), and .meta
// (clip table) produced by tools/asset_pipeline.py. GPU skinning is done in
// the vertex shader by sampling the animation texture, so 50k animated units
// cost almost nothing on the CPU.
//
// EVERYTHING IS OPTIONAL: if a unit has no skinned model, the renderer keeps
// using the existing procedural mesh. Missing/invalid files load nothing.
// ============================================================================
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <map>
#include <fstream>
#include <cstdint>
#include <iostream>
#include "../core/json.h"

struct AnimClip {
    int start = 0;     // first frame row in the animation texture
    int frames = 1;    // number of frames in this clip
    float fps = 24.0f;
};

// Skinned vertex layout matches the .mesh file (16 floats).
struct SkinnedVertex {
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
    float b0, b1, b2, b3;   // bone indices (stored as float for vertex attrib)
    float w0, w1, w2, w3;   // bone weights
};

class SkinnedModel {
public:
    GLuint vao = 0, vbo = 0, ebo = 0;
    GLuint anim_tex = 0;        // RGBA32F animation texture (bones*4 wide, frames tall)
    uint32_t index_count = 0;
    uint32_t bone_count = 0;
    int anim_tex_width = 0, anim_tex_height = 0;
    bool has_bones = false;
    bool valid = false;
    std::map<std::string, AnimClip> clips;

    // Load <base>.mesh (+ optional .anim/.meta). Returns false if mesh missing.
    bool load(const std::string& base) {
        if (!load_mesh(base + ".mesh")) return false;
        if (has_bones) {
            load_anim_texture(base + ".anim");
            load_meta(base + ".meta");
        }
        valid = true;
        return true;
    }

    const AnimClip* clip(const std::string& name) const {
        auto it = clips.find(name);
        return it == clips.end() ? nullptr : &it->second;
    }

    void destroy() {
        if (vao) glDeleteVertexArrays(1, &vao);
        if (vbo) glDeleteBuffers(1, &vbo);
        if (ebo) glDeleteBuffers(1, &ebo);
        if (anim_tex) glDeleteTextures(1, &anim_tex);
        vao = vbo = ebo = anim_tex = 0;
        valid = false;
    }

private:
    bool load_mesh(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f.good()) return false;

        char magic[4];
        f.read(magic, 4);
        if (std::string(magic, 4) != "MRTS") {
            std::cerr << "[skinned] bad magic in " << path << "\n";
            return false;
        }
        uint32_t version, vert_count, idx_count, bones, has_b;
        f.read((char*)&version, 4);
        f.read((char*)&vert_count, 4);
        f.read((char*)&idx_count, 4);
        f.read((char*)&bones, 4);
        f.read((char*)&has_b, 4);

        std::vector<SkinnedVertex> verts(vert_count);
        f.read((char*)verts.data(), vert_count * sizeof(SkinnedVertex));
        std::vector<uint32_t> indices(idx_count);
        f.read((char*)indices.data(), idx_count * sizeof(uint32_t));
        if (!f) { std::cerr << "[skinned] truncated " << path << "\n"; return false; }

        index_count = idx_count;
        bone_count = bones;
        has_bones = (has_b != 0);

        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glGenBuffers(1, &ebo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(SkinnedVertex), verts.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(uint32_t), indices.data(), GL_STATIC_DRAW);

        GLsizei stride = sizeof(SkinnedVertex);
        // 0:pos 1:norm 2:uv 3:bone_ids 4:bone_weights
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)(3*sizeof(float)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (void*)(6*sizeof(float)));
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, stride, (void*)(8*sizeof(float)));
        glEnableVertexAttribArray(4);
        glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, stride, (void*)(12*sizeof(float)));
        glBindVertexArray(0);

        std::cout << "[skinned] loaded " << path << " (" << vert_count << " verts, "
                  << bone_count << " bones, " << (has_bones ? "skinned" : "static") << ")\n";
        return true;
    }

    void load_anim_texture(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f.good()) return;
        uint32_t w, h;
        f.read((char*)&w, 4);
        f.read((char*)&h, 4);
        std::vector<float> data(w * h * 4);
        f.read((char*)data.data(), data.size() * sizeof(float));
        if (!f) { std::cerr << "[skinned] truncated anim " << path << "\n"; return; }

        anim_tex_width = (int)w;
        anim_tex_height = (int)h;
        glGenTextures(1, &anim_tex);
        glBindTexture(GL_TEXTURE_2D, anim_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, w, h, 0, GL_RGBA, GL_FLOAT, data.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
        std::cout << "[skinned] anim texture " << w << "x" << h << "\n";
    }

    void load_meta(const std::string& path) {
        JsonValue root = JsonParser::parse_file(path);
        if (root.is_null()) return;
        const JsonValue& c = root["clips"];
        if (!c.is_object()) return;
        for (const auto& kv : c.obj) {
            AnimClip clip;
            clip.start = (int)kv.second["start"].as_number(0);
            clip.frames = (int)kv.second["frames"].as_number(1);
            clip.fps = (float)kv.second["fps"].as_number(24);
            clips[kv.first] = clip;
        }
        std::cout << "[skinned] " << clips.size() << " clips from " << path << "\n";
    }
};
