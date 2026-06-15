#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <vector>
#include <cmath>
#include <functional>

// ============================================================================
// BASE SYSTEM - Each faction has a home base. Units spawn at the owner's base.
// Bases have health and can be destroyed (destroying the enemy base = win).
// Rendered as a fortified keep using the same instanced mesh path as decor,
// driven by the existing unit_shader (loc0=pos, loc1=normal, loc2=inst pos,
// loc3=color, loc4=scale, loc5=rotation, loc6=selected).
// ============================================================================

struct BaseInstance {
    glm::vec3 position;
    float rotation;
    float scale;
    glm::vec3 color;
};

class BaseSystem {
public:
    struct Base {
        glm::vec2 position{0, 0};
        float radius = 60.0f;       // spawn ring radius
        float max_health = 50000.0f;
        float health = 50000.0f;
        int faction = 0;            // 0=red, 1=blue
        bool alive = true;
        glm::vec2 rally{0, 0};      // where spawned units gather
    };

    Base bases[2];

    GLuint vao = 0, vbo = 0, ebo = 0, inst_vbo = 0;
    int idx_count = 0;

    void init(glm::vec2 red_pos, glm::vec2 blue_pos,
              std::function<float(float,float)> get_height) {
        bases[0] = {red_pos, 60.0f, 50000.0f, 50000.0f, 0, true, red_pos + glm::vec2(120, 0)};
        bases[1] = {blue_pos, 60.0f, 50000.0f, 50000.0f, 1, true, blue_pos - glm::vec2(120, 0)};
        get_height_fn = get_height;
        if (!vao) build_mesh();
        upload_instances();
    }

    void reset(glm::vec2 red_pos, glm::vec2 blue_pos) {
        bases[0].position = red_pos; bases[0].rally = red_pos + glm::vec2(120, 0);
        bases[1].position = blue_pos; bases[1].rally = blue_pos - glm::vec2(120, 0);
        for (int i = 0; i < 2; i++) { bases[i].health = bases[i].max_health; bases[i].alive = true; }
        upload_instances();
    }

    // Returns a spawn position inside the faction's base ring (spiral packing).
    glm::vec2 spawn_point(int faction, int index) {
        const Base& b = bases[faction];
        // golden-angle spiral so successive spawns fan out around the base
        float ga = 2.39996323f;
        float r = b.radius * 0.35f + sqrtf((float)index) * 3.2f;
        float a = index * ga;
        return b.position + glm::vec2(cosf(a), sinf(a)) * r;
    }

    bool point_in_base(int faction, glm::vec2 p) const {
        return glm::length(p - bases[faction].position) < bases[faction].radius;
    }

    // Damage a base; returns true if it was just destroyed this call.
    bool damage(int faction, float dmg) {
        Base& b = bases[faction];
        if (!b.alive) return false;
        b.health -= dmg;
        if (b.health <= 0) { b.health = 0; b.alive = false; return true; }
        return false;
    }

    void render(GLuint shader) {
        if (!vao) return;
        glBindVertexArray(vao);
        glDrawElementsInstanced(GL_TRIANGLES, idx_count, GL_UNSIGNED_INT, 0, 2);
        glBindVertexArray(0);
    }

    void cleanup() {
        if (vao) glDeleteVertexArrays(1, &vao);
        if (vbo) glDeleteBuffers(1, &vbo);
        if (ebo) glDeleteBuffers(1, &ebo);
        if (inst_vbo) glDeleteBuffers(1, &inst_vbo);
        vao = vbo = ebo = inst_vbo = 0;
    }

private:
    struct V { float x,y,z,nx,ny,nz; };
    std::function<float(float,float)> get_height_fn;

    void upload_instances() {
        BaseInstance inst[2];
        for (int i = 0; i < 2; i++) {
            float h = get_height_fn ? get_height_fn(bases[i].position.x, bases[i].position.y) : 0.0f;
            inst[i].position = glm::vec3(bases[i].position.x, h, bases[i].position.y);
            inst[i].rotation = (i == 0) ? 0.0f : 3.14159f;
            inst[i].scale = 14.0f;
            // dim the keep as it loses health (visual damage feedback)
            float hp = bases[i].max_health > 0 ? bases[i].health / bases[i].max_health : 0.0f;
            glm::vec3 base_col = (i == 0) ? glm::vec3(0.55f, 0.18f, 0.15f)
                                          : glm::vec3(0.18f, 0.28f, 0.55f);
            inst[i].color = base_col * (0.4f + 0.6f * hp);
        }
        glBindVertexArray(vao);
        if (!inst_vbo) glGenBuffers(1, &inst_vbo);
        glBindBuffer(GL_ARRAY_BUFFER, inst_vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(inst), inst, GL_DYNAMIC_DRAW);
        glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(BaseInstance), (void*)offsetof(BaseInstance, position));
        glEnableVertexAttribArray(2); glVertexAttribDivisor(2, 1);
        glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(BaseInstance), (void*)offsetof(BaseInstance, color));
        glEnableVertexAttribArray(3); glVertexAttribDivisor(3, 1);
        glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, sizeof(BaseInstance), (void*)offsetof(BaseInstance, scale));
        glEnableVertexAttribArray(4); glVertexAttribDivisor(4, 1);
        glVertexAttribPointer(5, 1, GL_FLOAT, GL_FALSE, sizeof(BaseInstance), (void*)offsetof(BaseInstance, rotation));
        glEnableVertexAttribArray(5); glVertexAttribDivisor(5, 1);
        glVertexAttrib1f(6, 0.0f);
        glBindVertexArray(0);
    }

    void build_mesh() {
        std::vector<V> verts;
        std::vector<unsigned int> idx;
        // --- central keep (box) ---
        add_box(verts, idx, glm::vec3(0, 0, 0), glm::vec3(3.0f, 4.0f, 3.0f));
        // --- four corner towers ---
        float t = 4.2f;
        add_box(verts, idx, glm::vec3( t, 0,  t), glm::vec3(1.0f, 5.5f, 1.0f));
        add_box(verts, idx, glm::vec3(-t, 0,  t), glm::vec3(1.0f, 5.5f, 1.0f));
        add_box(verts, idx, glm::vec3( t, 0, -t), glm::vec3(1.0f, 5.5f, 1.0f));
        add_box(verts, idx, glm::vec3(-t, 0, -t), glm::vec3(1.0f, 5.5f, 1.0f));
        // --- perimeter walls (4 thin slabs) ---
        add_box(verts, idx, glm::vec3(0, 0,  t), glm::vec3(t, 2.0f, 0.4f));
        add_box(verts, idx, glm::vec3(0, 0, -t), glm::vec3(t, 2.0f, 0.4f));
        add_box(verts, idx, glm::vec3( t, 0, 0), glm::vec3(0.4f, 2.0f, t));
        add_box(verts, idx, glm::vec3(-t, 0, 0), glm::vec3(0.4f, 2.0f, t));

        idx_count = (int)idx.size();
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, verts.size()*sizeof(V), verts.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(V), (void*)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(V), (void*)(3*sizeof(float)));
        glEnableVertexAttribArray(1);
        glGenBuffers(1, &ebo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx.size()*sizeof(unsigned int), idx.data(), GL_STATIC_DRAW);
        glBindVertexArray(0);
    }

    // Append a box centered at c (base sitting on y=0) with half-extents he.
    void add_box(std::vector<V>& verts, std::vector<unsigned int>& idx,
                 glm::vec3 c, glm::vec3 he) {
        unsigned b = (unsigned)verts.size();
        // 8 corners; y from 0 (ground) to 2*he.y
        float x0 = c.x-he.x, x1 = c.x+he.x;
        float y0 = c.y,      y1 = c.y+he.y*2.0f;
        float z0 = c.z-he.z, z1 = c.z+he.z;
        glm::vec3 P[8] = {
            {x0,y0,z0},{x1,y0,z0},{x1,y0,z1},{x0,y0,z1},
            {x0,y1,z0},{x1,y1,z0},{x1,y1,z1},{x0,y1,z1}
        };
        struct F { int a,b,c,d; glm::vec3 n; };
        F faces[6] = {
            {0,1,2,3, {0,-1,0}}, {4,5,6,7, {0,1,0}},
            {0,1,5,4, {0,0,-1}}, {3,2,6,7, {0,0,1}},
            {1,2,6,5, {1,0,0}},  {0,3,7,4, {-1,0,0}}
        };
        for (auto& f : faces) {
            unsigned s = (unsigned)verts.size();
            int ix[4] = {f.a, f.b, f.c, f.d};
            for (int k = 0; k < 4; k++)
                verts.push_back({P[ix[k]].x, P[ix[k]].y, P[ix[k]].z, f.n.x, f.n.y, f.n.z});
            idx.insert(idx.end(), {s, s+1, s+2, s, s+2, s+3});
        }
    }
};
