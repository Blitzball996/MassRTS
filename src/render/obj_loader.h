#pragma once
// Minimal OBJ model loader - supports v, vn, f
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <iostream>
#include <unordered_map>
#include "mesh_gen.h" // for Mesh struct

class OBJLoader {
public:
    struct Vertex { float x,y,z,nx,ny,nz; };
    // Load OBJ file and return a Mesh ready for instanced rendering
    // Mesh has: location 0 = vec3 pos, location 1 = vec3 normal
    static Mesh load(const std::string& path) {
        std::ifstream file(path);
        if (!file.is_open()) {
            std::cerr << "OBJ: cannot open " << path << "\n";
            return {};
        }

        std::vector<glm::vec3> positions;
        std::vector<glm::vec3> normals;

        std::vector<Vertex> out_verts;
        std::vector<unsigned int> out_indices;
        std::unordered_map<std::string, unsigned int> vert_cache;

        std::string line;
        while (std::getline(file, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream ss(line);
            std::string prefix;
            ss >> prefix;

            if (prefix == "v") {
                glm::vec3 v;
                ss >> v.x >> v.y >> v.z;
                positions.push_back(v);
            } else if (prefix == "vn") {
                glm::vec3 n;
                ss >> n.x >> n.y >> n.z;
                normals.push_back(n);
            } else if (prefix == "f") {
                // Parse face (triangulate quads)
                std::vector<unsigned int> face_indices;
                std::string token;
                while (ss >> token) {
                    unsigned int idx = get_or_create_vertex(token, positions, normals, out_verts, vert_cache);
                    face_indices.push_back(idx);
                }
                // Triangulate (fan)
                for (size_t i = 2; i < face_indices.size(); i++) {
                    out_indices.push_back(face_indices[0]);
                    out_indices.push_back(face_indices[i-1]);
                    out_indices.push_back(face_indices[i]);
                }
            }
        }

        if (out_verts.empty()) {
            std::cerr << "OBJ: no vertices in " << path << "\n";
            return {};
        }

        // If no normals, generate flat normals
        if (normals.empty()) {
            for (size_t i = 0; i < out_indices.size(); i += 3) {
                auto& v0 = out_verts[out_indices[i]];
                auto& v1 = out_verts[out_indices[i+1]];
                auto& v2 = out_verts[out_indices[i+2]];
                glm::vec3 p0 = {v0.x,v0.y,v0.z};
                glm::vec3 p1 = {v1.x,v1.y,v1.z};
                glm::vec3 p2 = {v2.x,v2.y,v2.z};
                glm::vec3 n = glm::normalize(glm::cross(p1-p0, p2-p0));
                v0.nx=n.x; v0.ny=n.y; v0.nz=n.z;
                v1.nx=n.x; v1.ny=n.y; v1.nz=n.z;
                v2.nx=n.x; v2.ny=n.y; v2.nz=n.z;
            }
        }

        // Upload to GPU
        Mesh m;
        m.index_count = (int)out_indices.size();
        glGenVertexArrays(1, &m.vao);
        glBindVertexArray(m.vao);

        glGenBuffers(1, &m.vbo);
        glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
        glBufferData(GL_ARRAY_BUFFER, out_verts.size()*sizeof(Vertex), out_verts.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)(3*sizeof(float)));
        glEnableVertexAttribArray(1);

        glGenBuffers(1, &m.ebo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m.ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, out_indices.size()*sizeof(unsigned int), out_indices.data(), GL_STATIC_DRAW);

        glBindVertexArray(0);

        std::cout << "OBJ loaded: " << path << " (" << out_verts.size() << " verts, "
                  << out_indices.size()/3 << " tris)\n";
        return m;
    }

private:
    static unsigned int get_or_create_vertex(
        const std::string& token,
        const std::vector<glm::vec3>& positions,
        const std::vector<glm::vec3>& normals,
        std::vector<Vertex>& out_verts,
        std::unordered_map<std::string, unsigned int>& cache)
    {
        auto it = cache.find(token);
        if (it != cache.end()) return it->second;

        // Parse v/vt/vn or v//vn or v
        int vi = 0, ti = 0, ni = 0;
        if (token.find("//") != std::string::npos) {
            sscanf(token.c_str(), "%d//%d", &vi, &ni);
        } else if (token.find('/') != std::string::npos) {
            sscanf(token.c_str(), "%d/%d/%d", &vi, &ti, &ni);
        } else {
            vi = std::stoi(token);
        }

        Vertex v = {0,0,0, 0,1,0};
        if (vi > 0 && vi <= (int)positions.size()) {
            v.x = positions[vi-1].x;
            v.y = positions[vi-1].y;
            v.z = positions[vi-1].z;
        }
        if (ni > 0 && ni <= (int)normals.size()) {
            v.nx = normals[ni-1].x;
            v.ny = normals[ni-1].y;
            v.nz = normals[ni-1].z;
        }

        unsigned int idx = (unsigned int)out_verts.size();
        out_verts.push_back(v);
        cache[token] = idx;
        return idx;
    }
};
