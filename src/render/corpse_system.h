#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <vector>
#include <cmath>
#include <functional>

// ============================================================================
// CorpseSystem — persistent battlefield gore as flat ground decals.
//
// Why decals (not ECS entities): at ~190k units, keeping dead bodies as live
// ECS slots would starve the spawn pool and inflate the per-frame GPU sim.
// Instead, kill_entity() recycles the slot immediately and queues a DeathEvent;
// we turn that into two cheap, flat, ground-hugging quads:
//   * a BLOOD splat (drawn first, under feet) — dense splats visually merge
//     into pools and "rivers of blood".
//   * a CORPSE body  (drawn on top of blood, still flat on the ground) — living
//     units render above it, producing the "soldiers trample the dead" look.
//
// Both fade only at the very end of their (long) lifetime and the oldest are
// retired first when we hit the cap, so the field stays carpeted during a fight
// but never grows unbounded.
// ============================================================================

struct GroundDecal {
    glm::vec3 pos;     // world position (y = terrain height)
    glm::vec3 color;   // tint
    float     radius;  // half-size in world units
    float     rotation;// yaw
    float     age;     // seconds alive
    float     life;    // total lifetime (seconds); fade kicks in near the end
    float     kind;    // 0 = blood splat, 1 = corpse body
    float     seed;    // per-decal random for shape variation
};

class CorpseSystem {
public:
    static constexpr size_t MAX_CORPSES = 24000;
    static constexpr size_t MAX_BLOOD   = 24000;

    std::vector<GroundDecal> corpses;
    std::vector<GroundDecal> blood;

    // Blood splats are OFF by default (corpses still drawn). Toggle at runtime.
    bool blood_enabled = false;

    GLuint vao = 0, quad_vbo = 0, inst_vbo = 0;
    GLuint shader = 0;
    uint32_t rng = 0x1234567u;

    float frand() { rng = rng * 1664525u + 1013904223u; return (float)(rng >> 8) / 16777216.0f; }

    void init(GLuint decal_shader) {
        shader = decal_shader;
        corpses.reserve(MAX_CORPSES);
        blood.reserve(MAX_BLOOD);
        // Unit quad in XZ plane, centered, [-0.5,0.5].
        float quad[] = {
            -0.5f, -0.5f,  0.5f, -0.5f,  -0.5f, 0.5f,
             0.5f, -0.5f,  0.5f,  0.5f,  -0.5f, 0.5f
        };
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glGenBuffers(1, &quad_vbo);
        glBindBuffer(GL_ARRAY_BUFFER, quad_vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        glEnableVertexAttribArray(0);
        // Per-instance buffer (GroundDecal layout).
        glGenBuffers(1, &inst_vbo);
        glBindBuffer(GL_ARRAY_BUFFER, inst_vbo);
        glBufferData(GL_ARRAY_BUFFER, MAX_CORPSES * sizeof(GroundDecal), nullptr, GL_DYNAMIC_DRAW);
        size_t s = sizeof(GroundDecal);
        glVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,s,(void*)offsetof(GroundDecal,pos));      glEnableVertexAttribArray(1); glVertexAttribDivisor(1,1);
        glVertexAttribPointer(2,3,GL_FLOAT,GL_FALSE,s,(void*)offsetof(GroundDecal,color));    glEnableVertexAttribArray(2); glVertexAttribDivisor(2,1);
        glVertexAttribPointer(3,1,GL_FLOAT,GL_FALSE,s,(void*)offsetof(GroundDecal,radius));   glEnableVertexAttribArray(3); glVertexAttribDivisor(3,1);
        glVertexAttribPointer(4,1,GL_FLOAT,GL_FALSE,s,(void*)offsetof(GroundDecal,rotation)); glEnableVertexAttribArray(4); glVertexAttribDivisor(4,1);
        glVertexAttribPointer(5,1,GL_FLOAT,GL_FALSE,s,(void*)offsetof(GroundDecal,age));      glEnableVertexAttribArray(5); glVertexAttribDivisor(5,1);
        glVertexAttribPointer(6,1,GL_FLOAT,GL_FALSE,s,(void*)offsetof(GroundDecal,life));     glEnableVertexAttribArray(6); glVertexAttribDivisor(6,1);
        glVertexAttribPointer(7,1,GL_FLOAT,GL_FALSE,s,(void*)offsetof(GroundDecal,kind));     glEnableVertexAttribArray(7); glVertexAttribDivisor(7,1);
        glVertexAttribPointer(8,1,GL_FLOAT,GL_FALSE,s,(void*)offsetof(GroundDecal,seed));     glEnableVertexAttribArray(8); glVertexAttribDivisor(8,1);
        glBindVertexArray(0);
    }

    // Spawn a corpse + blood splat at a death site. ground_y supplies terrain Y.
    void spawn(glm::vec2 p, glm::vec3 unit_color, float rotation, uint8_t type,
               float ground_y) {
        // --- Blood: a few overlapping splats so dense kills merge into pools ---
        // Dark crimson base (the frag shader refines wet/dry); never bright red.
        if (blood_enabled) {
        glm::vec3 blood_col(0.22f + frand() * 0.08f, 0.02f, 0.02f);
        int n_splats = 1 + (int)(frand() * 3.0f); // 1..3 splats per death
        for (int k = 0; k < n_splats; k++) {
            if (blood.size() >= MAX_BLOOD) blood.erase(blood.begin()); // retire oldest
            GroundDecal b;
            float ang = frand() * 6.2831853f;
            float off = frand() * 1.6f;
            b.pos = glm::vec3(p.x + cosf(ang) * off, ground_y + 0.05f, p.y + sinf(ang) * off);
            b.color = blood_col;
            b.radius = 1.4f + frand() * 1.8f; // small & dense; overlaps merge into pools
            b.rotation = frand() * 6.2831853f;
            b.age = 0.0f;
            b.life = 90.0f + frand() * 30.0f; // blood lingers a long time
            b.kind = 0.0f;
            b.seed = frand();
            blood.push_back(b);
        }
        }
        // --- Corpse: one flat body decal ---
        if (corpses.size() >= MAX_CORPSES) corpses.erase(corpses.begin());
        GroundDecal c;
        c.pos = glm::vec3(p.x, ground_y + 0.10f, p.y); // just above blood
        // Pass the raw unit color; the fragment shader desaturates/darkens it
        // into a lifeless corpse tone and adds fake volume + contact shadow.
        c.color = unit_color;
        c.radius = 3.5f + frand() * 1.5f;
        c.rotation = rotation + (frand() - 0.5f) * 1.2f;
        c.age = 0.0f;
        c.life = 60.0f + frand() * 30.0f;
        c.kind = 1.0f;
        c.seed = frand();
        (void)type;
        corpses.push_back(c);
    }

    void update(float dt) {
        // Age decals; remove only when fully expired. erase-by-swap keeps it O(n).
        auto age_list = [dt](std::vector<GroundDecal>& v) {
            for (size_t i = 0; i < v.size(); ) {
                v[i].age += dt;
                if (v[i].age >= v[i].life) { v[i] = v.back(); v.pop_back(); }
                else ++i;
            }
        };
        age_list(blood);
        age_list(corpses);
    }

    // Draw blood first (under units), then corpses. Both are flat ground decals.
    // Depth test on, depth write off so decals sit on terrain without z-fighting
    // each other and living units always render over them.
    void render(const glm::mat4& view, const glm::mat4& proj) {
        if (!shader) return;
        glUseProgram(shader);
        glUniformMatrix4fv(glGetUniformLocation(shader,"u_view"),1,GL_FALSE,&view[0][0]);
        glUniformMatrix4fv(glGetUniformLocation(shader,"u_proj"),1,GL_FALSE,&proj[0][0]);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
        glBindVertexArray(vao);
        auto draw = [&](std::vector<GroundDecal>& v) {
            if (v.empty()) return;
            size_t count = v.size();
            glBindBuffer(GL_ARRAY_BUFFER, inst_vbo);
            glBufferSubData(GL_ARRAY_BUFFER, 0, count * sizeof(GroundDecal), v.data());
            glDrawArraysInstanced(GL_TRIANGLES, 0, 6, (int)count);
        };
        draw(blood);    // pools/rivers under everything
        draw(corpses);  // bodies on top of blood, under living units
        glBindVertexArray(0);
        glDepthMask(GL_TRUE);
    }
};
