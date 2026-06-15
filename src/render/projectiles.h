#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <vector>
#include <cmath>
#include <random>
#include "../ecs/components.h"

enum class ProjectileType : uint8_t { Arrow = 0, Cannonball = 1 };

struct Projectile {
    glm::vec3 position;
    glm::vec3 velocity;
    glm::vec3 target;
    ProjectileType type;
    Faction faction;
    float life;
    float size;
};

struct ProjectileHit {
    glm::vec3 position;
    float radius; // 0 = single target (arrow), >0 = AOE (cannonball)
    float damage;
    Faction source_faction;
    float knockback_force;
    int kind; // 0=arrow (no explosion), 1=cannon (explosion), 2=nuke (big explosion)
};

struct ProjectileGPU {
    glm::vec3 pos;
    glm::vec3 color;
    glm::vec3 dir;
    float size;
    float stretch;
};

class ProjectileSystem {
public:
    static constexpr int MAX_PROJECTILES = 60000; // raised for 60k armies (visible arrow volleys)
    std::vector<Projectile> projectiles;
    std::vector<ProjectileHit> pending_hits;

    GLuint vao = 0, vbo_quad = 0, vbo_inst = 0;
    std::vector<ProjectileGPU> gpu_data;
    std::mt19937 rng{123};

    void init() {
        projectiles.reserve(MAX_PROJECTILES);
        gpu_data.reserve(MAX_PROJECTILES);
        pending_hits.reserve(256);

        float quad[] = {-0.5f,-0.5f, 0.5f,-0.5f, -0.5f,0.5f, 0.5f,0.5f};
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);

        glGenBuffers(1, &vbo_quad);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_quad);
        glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        glEnableVertexAttribArray(0);

        glGenBuffers(1, &vbo_inst);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_inst);
        glBufferData(GL_ARRAY_BUFFER, MAX_PROJECTILES * sizeof(ProjectileGPU), nullptr, GL_DYNAMIC_DRAW);
        glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(ProjectileGPU), (void*)0);
        glEnableVertexAttribArray(2); glVertexAttribDivisor(2, 1);
        glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(ProjectileGPU), (void*)(3*sizeof(float)));
        glEnableVertexAttribArray(3); glVertexAttribDivisor(3, 1);
        glVertexAttribPointer(4, 3, GL_FLOAT, GL_FALSE, sizeof(ProjectileGPU), (void*)(6*sizeof(float)));
        glEnableVertexAttribArray(4); glVertexAttribDivisor(4, 1);
        glVertexAttribPointer(5, 1, GL_FLOAT, GL_FALSE, sizeof(ProjectileGPU), (void*)(9*sizeof(float)));
        glEnableVertexAttribArray(5); glVertexAttribDivisor(5, 1);
        glVertexAttribPointer(6, 1, GL_FLOAT, GL_FALSE, sizeof(ProjectileGPU), (void*)(10*sizeof(float)));
        glEnableVertexAttribArray(6); glVertexAttribDivisor(6, 1);

        glBindVertexArray(0);
    }

    void spawn_arrow(glm::vec3 from, glm::vec3 to, Faction faction) {
        if (projectiles.size() >= MAX_PROJECTILES) return;
        Projectile p;
        p.position = from + glm::vec3(0, 3.0f, 0);
        p.target = to;
        glm::vec3 diff = to - from;
        float dist = glm::length(diff);
        if (dist < 1.0f) return;
        float flight_time = dist / 75.0f;            // slower horizontal -> taller arc
        if (flight_time < 0.6f) flight_time = 0.6f;  // min hang time so short shots still lob
        p.velocity = diff / flight_time;
        // vy to return to launch height under gravity (60/s), plus a lob boost
        p.velocity.y += 30.0f * flight_time + 40.0f; // arc high, rain down on target
        p.type = ProjectileType::Arrow;
        p.faction = faction;
        p.life = flight_time + 1.5f;                 // live long enough to complete the arc
        p.size = 1.6f;                               // bigger arrows -> volley clearly visible
        projectiles.push_back(p);
    }

    void spawn_cannonball(glm::vec3 from, glm::vec3 to, Faction faction) {
        if (projectiles.size() >= MAX_PROJECTILES) return;
        Projectile p;
        p.position = from + glm::vec3(0, 6.0f, 0);
        p.target = to;
        glm::vec3 diff = to - from;
        float dist = glm::length(diff);
        if (dist < 1.0f) return;
        float flight_time = dist / 45.0f; // SLOW: very visible trajectory
        p.velocity = diff / flight_time;
        p.velocity.y += 80.0f * flight_time * 0.5f; // HIGH arc
        p.type = ProjectileType::Cannonball;
        p.faction = faction;
        p.life = flight_time + 2.0f;
        p.size = 3.5f; // bigger, more visible
        projectiles.push_back(p);
    }

    void spawn_nuke(glm::vec3 from, glm::vec3 to, Faction faction) {
        if (projectiles.size() >= MAX_PROJECTILES) return;
        Projectile p;
        p.position = glm::vec3(to.x, 400.0f, to.z);
        p.target = to;
        p.velocity = glm::vec3(0, -200.0f, 0);
        p.type = ProjectileType::Cannonball;
        p.faction = faction;
        p.life = 4.0f;
        p.size = 8.0f;
        projectiles.push_back(p);
    }

    void update(float dt, float (*get_height)(float, float)) {
        pending_hits.clear();
        for (int i = (int)projectiles.size() - 1; i >= 0; i--) {
            auto& p = projectiles[i];
            p.life -= dt;
            p.velocity.y -= 60.0f * dt;
            p.position += p.velocity * dt;

            float ground_y = get_height ? get_height(p.position.x, p.position.z) : 0.0f;
            bool hit_ground = p.position.y <= ground_y;
            bool expired = p.life <= 0;

            if (hit_ground || expired) {
                if (hit_ground) {
                    ProjectileHit h;
                    h.position = p.position;
                    h.source_faction = p.faction;
                    if (p.type == ProjectileType::Arrow) {
                        h.kind = 0;          // arrow: NO explosion, small dust puff
                        h.radius = 6.0f;     // small splash, no knockback
                        h.damage = 22.0f;
                        h.knockback_force = 0;
                    } else if (p.size > 5.0f) {
                        h.kind = 2;          // nuke: huge explosion
                        h.radius = 120.0f;
                        h.damage = 200.0f;
                        h.knockback_force = 300.0f;
                    } else {
                        h.kind = 1;          // cannon: explosion + knockback
                        h.radius = 30.0f;
                        h.damage = 50.0f;
                        h.knockback_force = 80.0f;
                    }
                    pending_hits.push_back(h);
                }
                projectiles[i] = projectiles.back();
                projectiles.pop_back();
            }
        }
    }

    void render(const glm::mat4& view, const glm::mat4& proj, GLuint shader) {
        if (projectiles.empty()) return;
        gpu_data.clear();
        for (auto& p : projectiles) {
            ProjectileGPU g;
            g.pos = p.position;
            float vlen = glm::length(p.velocity);
            g.dir = vlen > 0.1f ? p.velocity / vlen : glm::vec3(0, 1, 0);
            g.size = p.size;
            if (p.type == ProjectileType::Arrow) {
                g.color = glm::vec3(0.45f, 0.28f, 0.1f);
                g.stretch = 4.0f;
            } else {
                g.color = glm::vec3(0.15f, 0.15f, 0.15f);
                if (p.size > 5.0f) g.color = glm::vec3(1.0f, 0.4f, 0.0f);
                g.stretch = 1.0f;
            }
            gpu_data.push_back(g);
        }

        glUseProgram(shader);
        glUniformMatrix4fv(glGetUniformLocation(shader, "u_view"), 1, GL_FALSE, &view[0][0]);
        glUniformMatrix4fv(glGetUniformLocation(shader, "u_proj"), 1, GL_FALSE, &proj[0][0]);

        glBindBuffer(GL_ARRAY_BUFFER, vbo_inst);
        glBufferSubData(GL_ARRAY_BUFFER, 0, gpu_data.size() * sizeof(ProjectileGPU), gpu_data.data());

        glBindVertexArray(vao);
        glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, (int)gpu_data.size());
        glBindVertexArray(0);
    }

    void cleanup() {
        glDeleteVertexArrays(1, &vao);
        glDeleteBuffers(1, &vbo_quad);
        glDeleteBuffers(1, &vbo_inst);
    }
};
