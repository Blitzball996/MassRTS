#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <vector>
#include <cmath>
#include <random>
#include <functional>

// Battlefield decoration: trees, rocks, banners, dead bodies
struct DecorInstance {
    glm::vec3 position;
    float rotation;
    float scale;
    glm::vec3 color;
};

struct Mesh; // forward

class BattlefieldDecor {
public:
    // Decor meshes
    GLuint tree_vao = 0, tree_vbo = 0, tree_ebo = 0;
    int tree_idx_count = 0;
    GLuint rock_vao = 0, rock_vbo = 0, rock_ebo = 0;
    int rock_idx_count = 0;
    GLuint banner_vao = 0, banner_vbo = 0, banner_ebo = 0;
    int banner_idx_count = 0;

    // Instance data
    GLuint tree_inst_vbo = 0, rock_inst_vbo = 0, banner_inst_vbo = 0;
    std::vector<DecorInstance> trees, rocks, banners;

    void generate(float world_size, std::function<float(float,float)> get_height) {
        std::mt19937 rng(12345);
        std::uniform_real_distribution<float> pos_dist(-world_size * 0.45f, world_size * 0.45f);
        std::uniform_real_distribution<float> rot_dist(0.0f, 6.28f);
        std::uniform_real_distribution<float> scale_dist(0.7f, 1.5f);

        // Trees (avoid center battlefield area)
        for (int i = 0; i < 400; i++) {
            float x = pos_dist(rng), z = pos_dist(rng);
            float center_dist = sqrt(x*x + z*z);
            if (center_dist < 500.0f) continue; // keep battlefield clear
            float y = get_height(x, z);
            float s = scale_dist(rng) * 8.0f;
            trees.push_back({{x, y, z}, rot_dist(rng), s, {0.15f, 0.35f + scale_dist(rng)*0.1f, 0.1f}});
        }

        // Rocks scattered more near edges
        for (int i = 0; i < 300; i++) {
            float x = pos_dist(rng), z = pos_dist(rng);
            float center_dist = sqrt(x*x + z*z);
            if (center_dist < 300.0f) continue;
            float y = get_height(x, z);
            float s = scale_dist(rng) * 4.0f;
            rocks.push_back({{x, y, z}, rot_dist(rng), s, {0.4f, 0.38f, 0.35f}});
        }

        // War banners along the front lines
        for (int i = 0; i < 20; i++) {
            float x = -200.0f + i * 20.0f;
            float z = (i % 2 == 0) ? -5.0f : 5.0f;
            float y = get_height(x, z);
            glm::vec3 col = (i < 10) ? glm::vec3(0.8f, 0.1f, 0.1f) : glm::vec3(0.1f, 0.2f, 0.8f);
            banners.push_back({{x, y, z}, 0, 6.0f, col});
        }

        // Build meshes
        build_tree_mesh();
        build_rock_mesh();
        build_banner_mesh();

        // Upload instances
        tree_inst_vbo = upload_instances(tree_vao, trees);
        rock_inst_vbo = upload_instances(rock_vao, rocks);
        banner_inst_vbo = upload_instances(banner_vao, banners);
    }

    void render(GLuint shader) {
        if (!trees.empty()) {
            glBindVertexArray(tree_vao);
            glDrawElementsInstanced(GL_TRIANGLES, tree_idx_count, GL_UNSIGNED_INT, 0, (int)trees.size());
        }
        if (!rocks.empty()) {
            glBindVertexArray(rock_vao);
            glDrawElementsInstanced(GL_TRIANGLES, rock_idx_count, GL_UNSIGNED_INT, 0, (int)rocks.size());
        }
        if (!banners.empty()) {
            glBindVertexArray(banner_vao);
            glDrawElementsInstanced(GL_TRIANGLES, banner_idx_count, GL_UNSIGNED_INT, 0, (int)banners.size());
        }
        glBindVertexArray(0);
    }

    void cleanup() {
        glDeleteVertexArrays(1, &tree_vao);
        glDeleteBuffers(1, &tree_vbo);
        glDeleteBuffers(1, &tree_ebo);
        glDeleteBuffers(1, &tree_inst_vbo);
        glDeleteVertexArrays(1, &rock_vao);
        glDeleteBuffers(1, &rock_vbo);
        glDeleteBuffers(1, &rock_ebo);
        glDeleteBuffers(1, &rock_inst_vbo);
        glDeleteVertexArrays(1, &banner_vao);
        glDeleteBuffers(1, &banner_vbo);
        glDeleteBuffers(1, &banner_ebo);
        glDeleteBuffers(1, &banner_inst_vbo);
    }

private:
    struct V { float x,y,z,nx,ny,nz,part_id,pivot_y; };

    // Helper: add a cylindrical branch segment
    void add_branch_segment(std::vector<V>& verts, std::vector<unsigned int>& idx,
                           glm::vec3 start, glm::vec3 end, float radius_start, float radius_end, int segs = 6) {
        glm::vec3 dir = glm::normalize(end - start);
        glm::vec3 right = glm::normalize(glm::cross(dir, glm::vec3(0,1,0) + glm::vec3(0.01f, 0, 0)));
        glm::vec3 up = glm::cross(right, dir);
        
        unsigned base = (unsigned)verts.size();
        for (int i = 0; i < segs; i++) {
            float angle = 6.28318f * i / segs;
            glm::vec3 offset = (cosf(angle) * right + sinf(angle) * up);
            glm::vec3 normal = glm::normalize(offset);
            verts.push_back({start.x + offset.x * radius_start, start.y + offset.y * radius_start, start.z + offset.z * radius_start,
                           normal.x, normal.y, normal.z, 0, 0});
            verts.push_back({end.x + offset.x * radius_end, end.y + offset.y * radius_end, end.z + offset.z * radius_end,
                           normal.x, normal.y, normal.z, 0, 0});
        }
        for (int i = 0; i < segs; i++) {
            int n = (i+1)%segs;
            unsigned a = base + i*2, b = base + i*2+1, c = base + n*2, d = base + n*2+1;
            idx.insert(idx.end(), {a,c,b, b,c,d});
        }
    }

    // Recursive branching tree generation
    void add_tree_branch(std::vector<V>& verts, std::vector<unsigned int>& idx,
                        glm::vec3 start, glm::vec3 direction, float length, float radius,
                        int depth, int max_depth, std::mt19937& rng) {
        if (depth > max_depth || length < 0.05f) return;
        
        glm::vec3 end = start + direction * length;
        add_branch_segment(verts, idx, start, end, radius, radius * 0.7f, 5);
        
        if (depth < max_depth) {
            // Add 2-4 child branches
            std::uniform_int_distribution<int> branch_count(2, 4);
            std::uniform_real_distribution<float> angle_dist(0.3f, 0.8f);
            std::uniform_real_distribution<float> twist_dist(0.0f, 6.28f);
            
            int num_branches = branch_count(rng);
            for (int i = 0; i < num_branches; i++) {
                float split_point = 0.6f + angle_dist(rng) * 0.3f;
                glm::vec3 branch_start = start + direction * length * split_point;
                
                // Diverge from parent direction
                float twist = twist_dist(rng);
                float diverge = angle_dist(rng);
                glm::vec3 perp = glm::normalize(glm::cross(direction, glm::vec3(0,1,0) + glm::vec3(0.1f,0,0)));
                glm::vec3 new_dir = glm::normalize(
                    direction * cosf(diverge) + 
                    (cosf(twist) * perp + sinf(twist) * glm::cross(direction, perp)) * sinf(diverge)
                );
                
                float child_length = length * (0.5f + angle_dist(rng) * 0.3f);
                float child_radius = radius * 0.6f;
                
                add_tree_branch(verts, idx, branch_start, new_dir, child_length, child_radius, depth+1, max_depth, rng);
            }
        }
        
        // Add leaf clusters at branch tips (simple spherical canopy approximation)
        if (depth >= max_depth - 1) {
            unsigned leaf_base = (unsigned)verts.size();
            int leaf_segs = 8;
            float leaf_radius = length * 1.5f;
            for (int i = 0; i < leaf_segs; i++) {
                for (int j = 0; j < leaf_segs; j++) {
                    float theta = 3.14159f * i / (leaf_segs - 1);
                    float phi = 6.28318f * j / leaf_segs;
                    glm::vec3 p = end + glm::vec3(
                        sinf(theta) * cosf(phi) * leaf_radius,
                        cosf(theta) * leaf_radius * 0.8f,
                        sinf(theta) * sinf(phi) * leaf_radius
                    );
                    glm::vec3 n = glm::normalize(p - end);
                    verts.push_back({p.x, p.y, p.z, n.x, n.y, n.z, 1, 0});
                }
            }
            // Generate sphere triangles
            for (int i = 0; i < leaf_segs - 1; i++) {
                for (int j = 0; j < leaf_segs; j++) {
                    int next_j = (j + 1) % leaf_segs;
                    unsigned a = leaf_base + i * leaf_segs + j;
                    unsigned b = leaf_base + i * leaf_segs + next_j;
                    unsigned c = leaf_base + (i+1) * leaf_segs + j;
                    unsigned d = leaf_base + (i+1) * leaf_segs + next_j;
                    idx.insert(idx.end(), {a,c,b, b,c,d});
                }
            }
        }
    }

    void build_tree_mesh() {
        std::vector<V> verts;
        std::vector<unsigned int> idx;
        
        std::mt19937 rng(42);
        
        // Main trunk
        glm::vec3 trunk_start(0, 0, 0);
        glm::vec3 trunk_dir(0, 1, 0.05f); // Slight lean
        trunk_dir = glm::normalize(trunk_dir);
        
        add_tree_branch(verts, idx, trunk_start, trunk_dir, 0.6f, 0.08f, 0, 2, rng);

        upload_mesh(tree_vao, tree_vbo, tree_ebo, tree_idx_count, verts, idx);
    }

    void build_rock_mesh() {
        std::vector<V> verts;
        std::vector<unsigned int> idx;
        // Irregular icosahedron-ish shape
        int segs = 6;
        unsigned base = 0;
        // Bottom ring
        for (int i = 0; i < segs; i++) {
            float a = 6.28318f * i / segs;
            float r = 0.3f + 0.1f * sin(a * 3);
            verts.push_back({cos(a)*r, 0, sin(a)*r, cos(a), -0.3f, sin(a), 2, 0});
        }
        // Top ring (smaller, offset)
        unsigned top_base = (unsigned)verts.size();
        for (int i = 0; i < segs; i++) {
            float a = 6.28318f * i / segs + 0.3f;
            float r = 0.2f + 0.05f * cos(a * 2);
            verts.push_back({cos(a)*r, 0.25f, sin(a)*r, cos(a), 0.5f, sin(a), 2, 0});
        }
        // Top point
        unsigned top_pt = (unsigned)verts.size();
        verts.push_back({0.05f, 0.35f, 0, 0, 1, 0, 2, 0});
        // Sides
        for (int i = 0; i < segs; i++) {
            int n = (i+1)%segs;
            idx.insert(idx.end(), {(unsigned)i, (unsigned)n, top_base+(unsigned)i});
            idx.insert(idx.end(), {(unsigned)n, top_base+(unsigned)n, top_base+(unsigned)i});
        }
        // Top cap
        for (int i = 0; i < segs; i++) {
            int n = (i+1)%segs;
            idx.insert(idx.end(), {top_base+(unsigned)i, top_base+(unsigned)n, top_pt});
        }

        upload_mesh(rock_vao, rock_vbo, rock_ebo, rock_idx_count, verts, idx);
    }

    void build_banner_mesh() {
        std::vector<V> verts;
        std::vector<unsigned int> idx;
        // Pole
        verts.push_back({0, 0, 0, 0, 0, 1, 2, 0});
        verts.push_back({0.02f, 0, 0, 0, 0, 1, 2, 0});
        verts.push_back({0.02f, 1.5f, 0, 0, 0, 1, 2, 0});
        verts.push_back({0, 1.5f, 0, 0, 0, 1, 2, 0});
        idx.insert(idx.end(), {0,1,2, 0,2,3});
        // Flag (rectangle, slightly angled for waving feel)
        unsigned fb = (unsigned)verts.size();
        verts.push_back({0.02f, 1.2f, 0, 0, 0, 1, 2, 0});
        verts.push_back({0.5f, 1.15f, 0.05f, 0, 0, 1, 2, 0});
        verts.push_back({0.5f, 1.45f, 0.05f, 0, 0, 1, 2, 0});
        verts.push_back({0.02f, 1.5f, 0, 0, 0, 1, 2, 0});
        idx.insert(idx.end(), {fb, fb+1, fb+2, fb, fb+2, fb+3});
        // Back face
        idx.insert(idx.end(), {fb, fb+2, fb+1, fb, fb+3, fb+2});

        upload_mesh(banner_vao, banner_vbo, banner_ebo, banner_idx_count, verts, idx);
    }

    void upload_mesh(GLuint& vao, GLuint& vbo, GLuint& ebo, int& idx_count,
                     const std::vector<V>& verts, const std::vector<unsigned int>& indices) {
        idx_count = (int)indices.size();
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, verts.size()*sizeof(V), verts.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(V), (void*)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(V), (void*)(3*sizeof(float)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(7, 2, GL_FLOAT, GL_FALSE, sizeof(V), (void*)(6*sizeof(float)));
        glEnableVertexAttribArray(7);
        glGenBuffers(1, &ebo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size()*sizeof(unsigned int), indices.data(), GL_STATIC_DRAW);
        glBindVertexArray(0);
    }

    GLuint upload_instances(GLuint vao, const std::vector<DecorInstance>& data) {
        GLuint ivbo;
        glBindVertexArray(vao);
        glGenBuffers(1, &ivbo);
        glBindBuffer(GL_ARRAY_BUFFER, ivbo);
        glBufferData(GL_ARRAY_BUFFER, data.size()*sizeof(DecorInstance), data.data(), GL_STATIC_DRAW);
        // loc 2: position (vec3)
        glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(DecorInstance), (void*)offsetof(DecorInstance, position));
        glEnableVertexAttribArray(2); glVertexAttribDivisor(2, 1);
        // loc 3: color (use offset to color field) -- we pack rotation+scale+color
        // Actually remap: loc3=color(vec3), loc4=scale, loc5=rotation
        glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(DecorInstance), (void*)offsetof(DecorInstance, color));
        glEnableVertexAttribArray(3); glVertexAttribDivisor(3, 1);
        glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, sizeof(DecorInstance), (void*)offsetof(DecorInstance, scale));
        glEnableVertexAttribArray(4); glVertexAttribDivisor(4, 1);
        glVertexAttribPointer(5, 1, GL_FLOAT, GL_FALSE, sizeof(DecorInstance), (void*)offsetof(DecorInstance, rotation));
        glEnableVertexAttribArray(5); glVertexAttribDivisor(5, 1);
        // loc 6: selected = 0 always for decor
        glVertexAttrib1f(6, 0.0f);
        // loc 10: fade = 1.0 (fully opaque)
        glVertexAttrib1f(10, 1.0f);
        glBindVertexArray(0);
        return ivbo;
    }
};
