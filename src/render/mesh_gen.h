#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <vector>
#include <cmath>

struct Mesh {
    GLuint vao = 0, vbo = 0, ebo = 0;
    int index_count = 0;
};

struct AnimVertex { float x,y,z, nx,ny,nz, part_id, pivot_y; };

class MeshGenerator {
public:
    static Mesh create_infantry() {
        std::vector<AnimVertex> verts;
        std::vector<unsigned int> idx;
        add_box(verts, idx, {0,18,0}, {4,6,2}, 0, 24);
        add_box(verts, idx, {0,28,0}, {4,4,4}, 1, 24);
        add_box(verts, idx, {-6,18,0}, {2,6,2}, 2, 24);
        add_box(verts, idx, {6,18,0}, {2,6,2}, 3, 24);
        add_box(verts, idx, {-2,6,0}, {2,6,2}, 4, 12);
        add_box(verts, idx, {2,6,0}, {2,6,2}, 5, 12);
        add_box(verts, idx, {7,18,5}, {0.5f,0.5f,8}, 6, 24);
        return upload(verts, idx);
    }

    static Mesh create_cavalry() {
        std::vector<AnimVertex> verts;
        std::vector<unsigned int> idx;
        add_box(verts, idx, {0,13,0}, {6,5,9}, 8, 13);
        add_box(verts, idx, {0,16,12}, {4,4,3}, 9, 16);
        add_box(verts, idx, {-4.5f,20,11}, {0.5f,2,0.5f}, 9, 20);
        add_box(verts, idx, {4.5f,20,11}, {0.5f,2,0.5f}, 9, 20);
        add_box(verts, idx, {-3.5f,4,6}, {2,4,2}, 10, 8);
        add_box(verts, idx, {3.5f,4,6}, {2,4,2}, 10, 8);
        add_box(verts, idx, {-3.5f,4,-6}, {2,4,2}, 10, 8);
        add_box(verts, idx, {3.5f,4,-6}, {2,4,2}, 10, 8);
        add_box(verts, idx, {0,9,-2}, {2,1.5f,2}, 8, 9);
        add_box(verts, idx, {0,23,0}, {3.5f,5,2}, 0, 23);
        add_box(verts, idx, {0,31,0}, {3.5f,3.5f,3.5f}, 1, 28);
        add_box(verts, idx, {-5.5f,23,0}, {1.5f,5,1.5f}, 2, 28);
        add_box(verts, idx, {5.5f,23,0}, {1.5f,5,1.5f}, 3, 28);
        add_box(verts, idx, {7,27,5}, {0.4f,0.4f,10}, 6, 28);
        add_box(verts, idx, {0,35,0}, {5,0.5f,5}, 7, 35);
        return upload(verts, idx);
    }

    static Mesh create_archer() {
        std::vector<AnimVertex> verts;
        std::vector<unsigned int> idx;
        add_box(verts, idx, {0,18,0}, {4,6,2}, 0, 24);
        add_box(verts, idx, {0,11,0}, {4.5f,3,2.5f}, 0, 12);
        add_box(verts, idx, {0,29,0}, {4,5,4}, 1, 24);
        add_box(verts, idx, {0,27,5.5f}, {1,2,1.5f}, 1, 27);
        add_box(verts, idx, {-5,18,1.5f}, {1.5f,6,1.5f}, 2, 24);
        add_box(verts, idx, {5,18,-1.5f}, {1.5f,6,1.5f}, 3, 24);
        add_box(verts, idx, {-2,5,0}, {2,5,2}, 4, 12);
        add_box(verts, idx, {2,5,0}, {2,5,2}, 5, 12);
        add_box(verts, idx, {-7,20,3}, {0.3f,8,0.3f}, 6, 20);
        add_box(verts, idx, {-7,28,4.5f}, {0.3f,0.5f,1.5f}, 6, 28);
        add_box(verts, idx, {-7,12,4.5f}, {0.3f,0.5f,1.5f}, 6, 12);
        add_box(verts, idx, {-6.6f,20,4.5f}, {0.1f,8,0.1f}, 6, 20);
        add_box(verts, idx, {1.5f,23,-4}, {1.5f,6,1.5f}, 7, 24);
        return upload(verts, idx);
    }

    static Mesh create_shield() {
        std::vector<AnimVertex> verts;
        std::vector<unsigned int> idx;
        add_box(verts, idx, {0,18,0}, {4.5f,6,2.5f}, 0, 24);
        add_box(verts, idx, {0,28,0}, {4,4,4}, 1, 24);
        add_box(verts, idx, {0,33,0}, {4.5f,1,4.5f}, 7, 33);
        add_box(verts, idx, {0,28,4.5f}, {3.5f,2,0.5f}, 7, 28);
        add_box(verts, idx, {-6,18,0}, {2,6,2}, 2, 24);
        add_box(verts, idx, {6,18,0}, {2,6,2}, 3, 24);
        add_box(verts, idx, {-2,6,0}, {2.5f,6,2.5f}, 4, 12);
        add_box(verts, idx, {2,6,0}, {2.5f,6,2.5f}, 5, 12);
        add_box(verts, idx, {-9,18,2}, {0.5f,10,7}, 11, 18);
        add_box(verts, idx, {7,16,4}, {0.5f,0.5f,6}, 6, 24);
        return upload(verts, idx);
    }

    static Mesh create_samurai() {
        std::vector<AnimVertex> verts;
        std::vector<unsigned int> idx;
        add_box(verts, idx, {0,18,0}, {4,6,2}, 0, 24);
        add_box(verts, idx, {-5.5f,23,0}, {2.5f,1.5f,3}, 7, 23);
        add_box(verts, idx, {5.5f,23,0}, {2.5f,1.5f,3}, 7, 23);
        add_box(verts, idx, {0,28,0}, {4,4,4}, 1, 24);
        add_box(verts, idx, {0,33,0}, {5,1.5f,5}, 7, 33);
        add_box(verts, idx, {0,36,2}, {0.5f,3,0.5f}, 7, 36);
        add_box(verts, idx, {0,26,4.5f}, {3,1.5f,0.5f}, 7, 26);
        add_box(verts, idx, {-6,18,0}, {2,6,2}, 2, 24);
        add_box(verts, idx, {6,18,0}, {2,6,2}, 3, 24);
        add_box(verts, idx, {0,12,0}, {5,2,3}, 0, 12);
        add_box(verts, idx, {-2,5,0}, {2,5,2}, 4, 12);
        add_box(verts, idx, {2,5,0}, {2,5,2}, 5, 12);
        add_box(verts, idx, {7,20,6}, {0.3f,0.3f,12}, 6, 24);
        add_box(verts, idx, {7,20,0}, {1.2f,1.2f,0.3f}, 6, 20);
        add_box(verts, idx, {-2,16,-5}, {0.5f,0.5f,10}, 7, 16);
        return upload(verts, idx);
    }

    static Mesh create_artillery() {
        std::vector<AnimVertex> verts;
        std::vector<unsigned int> idx;
        add_box(verts, idx, {0,8,8}, {2,2,12}, 6, 8);
        add_box(verts, idx, {0,8,21}, {2.5f,2.5f,1}, 6, 8);
        add_box(verts, idx, {0,5,0}, {5,3,8}, 0, 5);
        add_box(verts, idx, {-6,5,0}, {1,4,4}, 10, 5);
        add_box(verts, idx, {6,5,0}, {1,4,4}, 10, 5);
        add_box(verts, idx, {0,16,-8}, {3,5,2}, 0, 16);
        add_box(verts, idx, {0,24,-8}, {3,3,3}, 1, 22);
        add_box(verts, idx, {-4,16,-8}, {1.5f,5,1.5f}, 2, 22);
        add_box(verts, idx, {4,16,-8}, {1.5f,5,1.5f}, 3, 22);
        return upload(verts, idx);
    }

    // Militia: simpler Steve with no weapon, tattered look
    static Mesh create_militia() {
        std::vector<AnimVertex> verts;
        std::vector<unsigned int> idx;
        add_box(verts, idx, {0,18,0}, {3.5f,5.5f,2}, 0, 24);
        add_box(verts, idx, {0,27,0}, {4,4,4}, 1, 24);
        add_box(verts, idx, {-5.5f,18,0}, {1.5f,5.5f,1.5f}, 2, 24);
        add_box(verts, idx, {5.5f,18,0}, {1.5f,5.5f,1.5f}, 3, 24);
        add_box(verts, idx, {-2,6,0}, {2,6,2}, 4, 12);
        add_box(verts, idx, {2,6,0}, {2,6,2}, 5, 12);
        // Short stick weapon
        add_box(verts, idx, {6,16,3}, {0.4f,0.4f,5}, 6, 24);
        return upload(verts, idx);
    }

    // Wall: thick stone block
    static Mesh create_wall() {
        std::vector<AnimVertex> verts;
        std::vector<unsigned int> idx;
        add_box(verts, idx, {0,10,0}, {8,10,4}, 12, 10);
        // Crenellations on top
        add_box(verts, idx, {-5,22,0}, {2,2,4}, 12, 22);
        add_box(verts, idx, {5,22,0}, {2,2,4}, 12, 22);
        add_box(verts, idx, {0,22,-3}, {8,2,1}, 12, 22);
        add_box(verts, idx, {0,22,3}, {8,2,1}, 12, 22);
        return upload(verts, idx);
    }

    // Turret: tower with crossbow on top
    static Mesh create_turret() {
        std::vector<AnimVertex> verts;
        std::vector<unsigned int> idx;
        // Base tower
        add_box(verts, idx, {0,12,0}, {5,12,5}, 12, 12);
        // Platform
        add_box(verts, idx, {0,25,0}, {7,1,7}, 12, 25);
        // Crossbow mechanism
        add_box(verts, idx, {0,28,0}, {2,2,6}, 6, 28);
        add_box(verts, idx, {-3,28,3}, {0.5f,3,0.5f}, 6, 28);
        add_box(verts, idx, {3,28,3}, {0.5f,3,0.5f}, 6, 28);
        // Roof peak
        add_box(verts, idx, {0,30,0}, {4,2,4}, 7, 30);
        return upload(verts, idx);
    }

    static void destroy_mesh(Mesh& m) {
        if (m.vao) glDeleteVertexArrays(1, &m.vao);
        if (m.vbo) glDeleteBuffers(1, &m.vbo);
        if (m.ebo) glDeleteBuffers(1, &m.ebo);
        m = {};
    }

private:
    static void add_box(std::vector<AnimVertex>& verts, std::vector<unsigned int>& idx,
                        glm::vec3 c, glm::vec3 h, float part_id, float pivot_y) {
        unsigned int base = (unsigned int)verts.size();
        struct F { float nx,ny,nz; glm::vec3 o[4]; };
        F faces[6] = {
            { 0, 0, 1, {{-h.x,-h.y,h.z},{h.x,-h.y,h.z},{h.x,h.y,h.z},{-h.x,h.y,h.z}}},
            { 0, 0,-1, {{h.x,-h.y,-h.z},{-h.x,-h.y,-h.z},{-h.x,h.y,-h.z},{h.x,h.y,-h.z}}},
            { 0, 1, 0, {{-h.x,h.y,h.z},{h.x,h.y,h.z},{h.x,h.y,-h.z},{-h.x,h.y,-h.z}}},
            { 0,-1, 0, {{-h.x,-h.y,-h.z},{h.x,-h.y,-h.z},{h.x,-h.y,h.z},{-h.x,-h.y,h.z}}},
            { 1, 0, 0, {{h.x,-h.y,h.z},{h.x,-h.y,-h.z},{h.x,h.y,-h.z},{h.x,h.y,h.z}}},
            {-1, 0, 0, {{-h.x,-h.y,-h.z},{-h.x,-h.y,h.z},{-h.x,h.y,h.z},{-h.x,h.y,-h.z}}},
        };
        for (int f = 0; f < 6; f++) {
            for (int v = 0; v < 4; v++) {
                verts.push_back({
                    c.x+faces[f].o[v].x, c.y+faces[f].o[v].y, c.z+faces[f].o[v].z,
                    faces[f].nx, faces[f].ny, faces[f].nz, part_id, pivot_y
                });
            }
            unsigned int b = base + f*4;
            idx.insert(idx.end(), {b,b+1,b+2, b,b+2,b+3});
        }
    }

    static Mesh upload(const std::vector<AnimVertex>& verts, const std::vector<unsigned int>& indices) {
        Mesh m;
        m.index_count = (int)indices.size();
        glGenVertexArrays(1, &m.vao);
        glBindVertexArray(m.vao);
        glGenBuffers(1, &m.vbo);
        glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
        glBufferData(GL_ARRAY_BUFFER, verts.size()*sizeof(AnimVertex), verts.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(AnimVertex), (void*)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(AnimVertex), (void*)(3*sizeof(float)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(7, 2, GL_FLOAT, GL_FALSE, sizeof(AnimVertex), (void*)(6*sizeof(float)));
        glEnableVertexAttribArray(7);
        glGenBuffers(1, &m.ebo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m.ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size()*sizeof(unsigned int), indices.data(), GL_STATIC_DRAW);
        glBindVertexArray(0);
        return m;
    }
};
