#pragma once
// ============================================================================
// MODEL LIBRARY - drop-in registry for CC0 art (Kenney / Quaternius OBJ models)
//
// HOW TO USE:
//   1. Put .obj files in  assets/models/   (e.g. assets/models/soldier.obj)
//   2. At startup call  g_models.load("soldier", "assets/models/soldier.obj");
//   3. Fetch with        const ModelEntry* m = g_models.get("soldier");
//      then render m->mesh with your instanced unit shader.
//
// Models are auto-centered on the ground (min-Y = 0) and uniformly scaled so
// their largest horizontal footprint == 1.0, so any CC0 model (whatever its
// original units) drops in at a predictable size. Multiply by your per-unit
// scale at render time.
//
// Recommended free CC0 packs (no attribution required):
//   - Quaternius "Ultimate Modular Men" / "RTS units"  (quaternius.com)
//   - Kenney "Tower Defense Kit" / "Castle Kit" / "Medieval Town" (kenney.nl)
//   These ship as .obj+.mtl. This loader uses the .obj geometry; tint is
//   applied per-instance via vertex color, so untextured models still look right.
// ============================================================================
#include "obj_loader.h"
#include "mesh_gen.h"
#include <glm/glm.hpp>
#include <string>
#include <unordered_map>
#include <iostream>

struct ModelEntry {
    Mesh mesh;
    glm::vec3 center{0};   // original geometric center (pre-normalize)
    float norm_scale = 1.0f; // scale that was applied to fit footprint==1
    bool valid = false;
};

class ModelLibrary {
public:
    // Load an OBJ, normalize it, and register under `name`. Returns success.
    bool load(const std::string& name, const std::string& path,
              bool normalize = true) {
        if (models.count(name)) return true; // already loaded (cache)

        ModelEntry e;
        // Reuse the existing OBJLoader but post-process the raw geometry so we
        // can normalize. OBJLoader uploads directly, so we load + measure via a
        // lightweight second pass here.
        if (!load_normalized(path, e, normalize)) {
            std::cerr << "ModelLibrary: failed to load '" << name << "' from " << path << "\n";
            return false;
        }
        e.valid = true;
        models[name] = e;
        std::cout << "ModelLibrary: '" << name << "' ready ("
                  << e.mesh.index_count/3 << " tris, fit-scale "
                  << e.norm_scale << ")\n";
        return true;
    }

    const ModelEntry* get(const std::string& name) const {
        auto it = models.find(name);
        if (it == models.end() || !it->second.valid) return nullptr;
        return &it->second;
    }

    bool has(const std::string& name) const {
        auto it = models.find(name);
        return it != models.end() && it->second.valid;
    }

    void cleanup() {
        for (auto& kv : models) {
            Mesh& m = kv.second.mesh;
            if (m.vao) glDeleteVertexArrays(1, &m.vao);
            if (m.vbo) glDeleteBuffers(1, &m.vbo);
            if (m.ebo) glDeleteBuffers(1, &m.ebo);
        }
        models.clear();
    }

private:
    std::unordered_map<std::string, ModelEntry> models;

    struct V { float x,y,z,nx,ny,nz; };

    bool load_normalized(const std::string& path, ModelEntry& out, bool normalize) {
        std::ifstream file(path);
        if (!file.is_open()) return false;

        std::vector<glm::vec3> positions, normals;
        std::vector<V> verts;
        std::vector<unsigned int> indices;
        std::unordered_map<std::string, unsigned int> cache;

        std::string line;
        while (std::getline(file, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream ss(line);
            std::string p; ss >> p;
            if (p == "v") { glm::vec3 v; ss >> v.x >> v.y >> v.z; positions.push_back(v); }
            else if (p == "vn") { glm::vec3 n; ss >> n.x >> n.y >> n.z; normals.push_back(n); }
            else if (p == "f") {
                std::vector<unsigned int> fi;
                std::string tok;
                while (ss >> tok) fi.push_back(emit(tok, positions, normals, verts, cache));
                for (size_t i = 2; i < fi.size(); i++) {
                    indices.push_back(fi[0]); indices.push_back(fi[i-1]); indices.push_back(fi[i]);
                }
            }
        }
        if (verts.empty()) return false;

        // Generate flat normals if the file had none
        if (normals.empty()) {
            for (size_t i = 0; i + 2 < indices.size(); i += 3) {
                V& a = verts[indices[i]]; V& b = verts[indices[i+1]]; V& c = verts[indices[i+2]];
                glm::vec3 n = glm::normalize(glm::cross(
                    glm::vec3(b.x-a.x,b.y-a.y,b.z-a.z),
                    glm::vec3(c.x-a.x,c.y-a.y,c.z-a.z)));
                a.nx=n.x;a.ny=n.y;a.nz=n.z; b.nx=n.x;b.ny=n.y;b.nz=n.z; c.nx=n.x;c.ny=n.y;c.nz=n.z;
            }
        }

        // --- normalize: center on XZ, sit on ground (minY=0), fit footprint to 1 ---
        if (normalize) {
            glm::vec3 mn(1e9f), mx(-1e9f);
            for (auto& v : verts) {
                mn = glm::min(mn, glm::vec3(v.x,v.y,v.z));
                mx = glm::max(mx, glm::vec3(v.x,v.y,v.z));
            }
            glm::vec3 ctr = (mn + mx) * 0.5f;
            float footprint = glm::max(mx.x - mn.x, mx.z - mn.z);
            if (footprint < 1e-4f) footprint = 1.0f;
            float s = 1.0f / footprint;
            out.center = ctr;
            out.norm_scale = s;
            for (auto& v : verts) {
                v.x = (v.x - ctr.x) * s;
                v.y = (v.y - mn.y) * s;   // ground = 0
                v.z = (v.z - ctr.z) * s;
            }
        }

        // upload (loc0 pos, loc1 normal) - matches unit_shader
        Mesh& m = out.mesh;
        m.index_count = (int)indices.size();
        glGenVertexArrays(1, &m.vao);
        glBindVertexArray(m.vao);
        glGenBuffers(1, &m.vbo);
        glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
        glBufferData(GL_ARRAY_BUFFER, verts.size()*sizeof(V), verts.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(V), (void*)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(V), (void*)(3*sizeof(float)));
        glEnableVertexAttribArray(1);
        glGenBuffers(1, &m.ebo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m.ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size()*sizeof(unsigned int), indices.data(), GL_STATIC_DRAW);
        glBindVertexArray(0);
        return true;
    }

    unsigned int emit(const std::string& tok,
                      const std::vector<glm::vec3>& pos, const std::vector<glm::vec3>& nrm,
                      std::vector<V>& verts, std::unordered_map<std::string,unsigned int>& cache) {
        auto it = cache.find(tok);
        if (it != cache.end()) return it->second;
        int vi=0, ti=0, ni=0;
        if (tok.find("//") != std::string::npos) sscanf(tok.c_str(), "%d//%d", &vi, &ni);
        else if (tok.find('/') != std::string::npos) sscanf(tok.c_str(), "%d/%d/%d", &vi, &ti, &ni);
        else vi = std::atoi(tok.c_str());
        V v = {0,0,0, 0,1,0};
        if (vi > 0 && vi <= (int)pos.size()) { v.x=pos[vi-1].x; v.y=pos[vi-1].y; v.z=pos[vi-1].z; }
        if (ni > 0 && ni <= (int)nrm.size()) { v.nx=nrm[ni-1].x; v.ny=nrm[ni-1].y; v.nz=nrm[ni-1].z; }
        unsigned int idx = (unsigned int)verts.size();
        verts.push_back(v);
        cache[tok] = idx;
        return idx;
    }
};
