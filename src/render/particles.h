#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <vector>
#include <cmath>
#include <random>

// Simple combat particle system (sparks, blood splats)
struct Particle {
    glm::vec3 position;
    glm::vec3 velocity;
    glm::vec3 color;
    float life;            // seconds remaining
    float size;
    float max_life = 0.8f; // for correct per-effect alpha fade
    float gravity = 80.0f; // per-particle gravity (smoke small, sparks high)
    float drag = 0.0f;     // velocity damping/sec (smoke & rings slow down)
    float grow = 0.0f;     // size growth/sec (puffs & shockwave rings expand)
};

class ParticleSystem {
public:
    static constexpr int MAX_PARTICLES = 30000;
    std::vector<Particle> particles;
    GLuint vao = 0, vbo_quad = 0, vbo_inst = 0;

    struct ParticleGPU {
        glm::vec3 pos;
        glm::vec3 col;
        float size;
    };
    std::vector<ParticleGPU> gpu_data;

    std::mt19937 rng{42};

    // Terrain height sampler so particles settle on the real ground (crater
    // floors) instead of the flat Z=0 plane. Set from main after renderer init.
    float (*ground_height)(float, float) = nullptr;

    void init() {
        particles.reserve(MAX_PARTICLES);
        gpu_data.reserve(MAX_PARTICLES);

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
        glBufferData(GL_ARRAY_BUFFER, MAX_PARTICLES * sizeof(ParticleGPU), nullptr, GL_DYNAMIC_DRAW);
        // pos
        glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(ParticleGPU), (void*)0);
        glEnableVertexAttribArray(2); glVertexAttribDivisor(2, 1);
        // color
        glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(ParticleGPU), (void*)(3*sizeof(float)));
        glEnableVertexAttribArray(3); glVertexAttribDivisor(3, 1);
        // size
        glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, sizeof(ParticleGPU), (void*)(6*sizeof(float)));
        glEnableVertexAttribArray(4); glVertexAttribDivisor(4, 1);

        glBindVertexArray(0);
    }

    void spawn_combat(glm::vec3 pos, glm::vec3 color) {
        if (particles.size() >= MAX_PARTICLES) return;
        std::uniform_real_distribution<float> vel(-30.0f, 30.0f);
        std::uniform_real_distribution<float> up(10.0f, 50.0f);
        std::uniform_real_distribution<float> life(0.3f, 0.8f);
        std::uniform_real_distribution<float> sz(0.5f, 2.0f);

        int count = 3;
        for (int i = 0; i < count && particles.size() < MAX_PARTICLES; i++) {
            Particle p;
            p.position = pos + glm::vec3(vel(rng)*0.1f, 1.0f, vel(rng)*0.1f);
            p.velocity = {vel(rng), up(rng), vel(rng)};
            p.color = color;
            p.life = life(rng);
            p.size = sz(rng);
            particles.push_back(p);
        }
    }


    // --- helper to push one configured particle ---
    void push(glm::vec3 pos, glm::vec3 vel, glm::vec3 col, float life,
              float size, float grav, float drag, float grow) {
        if (particles.size() >= MAX_PARTICLES) return;
        Particle p; p.position=pos; p.velocity=vel; p.color=col;
        p.life=life; p.max_life=life; p.size=size;
        p.gravity=grav; p.drag=drag; p.grow=grow;
        particles.push_back(p);
    }

    // ARROW IMPACT: small puff of dust + a couple of skidding splinters.
    void spawn_arrow_impact(glm::vec3 pos) {
        std::uniform_real_distribution<float> a(-1.0f,1.0f);
        for (int i=0;i<4 && particles.size()<MAX_PARTICLES;i++)
            push(pos+glm::vec3(0,0.5f,0), glm::vec3(a(rng)*8,5+a(rng)*4,a(rng)*8),
                 glm::vec3(0.55f,0.5f,0.42f), 0.35f, 0.6f, 40.0f, 2.0f, 0.0f);
        // tiny brown splinters
        for (int i=0;i<3 && particles.size()<MAX_PARTICLES;i++)
            push(pos+glm::vec3(0,0.3f,0), glm::vec3(a(rng)*14,a(rng)*6,a(rng)*14),
                 glm::vec3(0.45f,0.3f,0.15f), 0.5f, 0.4f, 90.0f, 0.0f, 0.0f);
    }

    // CANNON BLAST: orange fireball, fast sparks, black smoke, ground shockwave ring.
    void spawn_cannon_blast(glm::vec3 c) {
        std::uniform_real_distribution<float> a(-1.0f,1.0f);
        std::uniform_real_distribution<float> u(0.0f,1.0f);
        // fireball core
        for (int i=0;i<14 && particles.size()<MAX_PARTICLES;i++)
            push(c+glm::vec3(a(rng)*2,1+u(rng)*2,a(rng)*2),
                 glm::vec3(a(rng)*18, 8+u(rng)*22, a(rng)*18),
                 glm::vec3(1.0f, 0.5f+u(rng)*0.3f, 0.1f), 0.4f+u(rng)*0.25f,
                 2.0f+u(rng)*2.0f, 10.0f, 2.0f, 6.0f);
        // bright sparks (fall fast)
        for (int i=0;i<20 && particles.size()<MAX_PARTICLES;i++)
            push(c+glm::vec3(0,1,0), glm::vec3(a(rng)*45, 10+u(rng)*40, a(rng)*45),
                 glm::vec3(1.0f,0.85f,0.4f), 0.5f, 0.7f, 130.0f, 0.0f, 0.0f);
        // rising black smoke
        for (int i=0;i<12 && particles.size()<MAX_PARTICLES;i++)
            push(c+glm::vec3(a(rng)*3,2,a(rng)*3), glm::vec3(a(rng)*6, 14+u(rng)*10, a(rng)*6),
                 glm::vec3(0.12f,0.11f,0.1f), 1.4f, 3.0f, -8.0f, 1.5f, 7.0f);
        // ground shockwave ring (expanding flat dust)
        for (int i=0;i<24 && particles.size()<MAX_PARTICLES;i++){
            float ang=(float)i/24.0f*6.2831853f;
            push(c+glm::vec3(cosf(ang)*3,0.3f,sinf(ang)*3),
                 glm::vec3(cosf(ang)*55,2,sinf(ang)*55),
                 glm::vec3(0.6f,0.55f,0.45f), 0.45f, 1.5f, 8.0f, 6.0f, 8.0f);
        }
    }

    // NUKE: blinding white flash, towering mushroom cloud, huge shockwave ring, embers.
    void spawn_nuke_blast(glm::vec3 c) {
        std::uniform_real_distribution<float> a(-1.0f,1.0f);
        std::uniform_real_distribution<float> u(0.0f,1.0f);
        // white-hot flash core
        for (int i=0;i<30 && particles.size()<MAX_PARTICLES;i++)
            push(c+glm::vec3(a(rng)*4,3+u(rng)*4,a(rng)*4),
                 glm::vec3(a(rng)*10, u(rng)*15, a(rng)*10),
                 glm::vec3(1.0f,1.0f,0.92f), 0.5f, 6.0f+u(rng)*6.0f, 0.0f, 4.0f, 18.0f);
        // mushroom stem (rises straight up)
        for (int i=0;i<40 && particles.size()<MAX_PARTICLES;i++)
            push(c+glm::vec3(a(rng)*4,2,a(rng)*4),
                 glm::vec3(a(rng)*4, 40+u(rng)*40, a(rng)*4),
                 glm::vec3(0.9f,0.5f+u(rng)*0.3f,0.2f), 1.8f+u(rng)*0.8f,
                 5.0f, -4.0f, 0.5f, 10.0f);
        // mushroom cap (billows out high up)
        for (int i=0;i<50 && particles.size()<MAX_PARTICLES;i++){
            float ang=u(rng)*6.2831853f, rad=u(rng)*30.0f;
            push(c+glm::vec3(cosf(ang)*rad, 55+u(rng)*30, sinf(ang)*rad),
                 glm::vec3(cosf(ang)*20, 8+u(rng)*8, sinf(ang)*20),
                 glm::vec3(0.35f,0.32f,0.3f), 3.0f+u(rng)*1.5f, 8.0f, -2.0f, 1.0f, 14.0f);
        }
        // massive ground shockwave ring
        for (int i=0;i<60 && particles.size()<MAX_PARTICLES;i++){
            float ang=(float)i/60.0f*6.2831853f;
            push(c+glm::vec3(cosf(ang)*5,0.5f,sinf(ang)*5),
                 glm::vec3(cosf(ang)*140,3,sinf(ang)*140),
                 glm::vec3(0.85f,0.8f,0.6f), 0.8f, 3.0f, 6.0f, 5.0f, 16.0f);
        }
        // glowing embers raining outward
        for (int i=0;i<50 && particles.size()<MAX_PARTICLES;i++)
            push(c+glm::vec3(0,3,0), glm::vec3(a(rng)*60, 20+u(rng)*60, a(rng)*60),
                 glm::vec3(1.0f,0.6f,0.15f), 1.2f, 1.2f, 110.0f, 0.0f, 0.0f);
    }

    void update(float dt) {
        for (int i = (int)particles.size()-1; i >= 0; i--) {
            particles[i].life -= dt;
            if (particles[i].life <= 0) {
                particles[i] = particles.back();
                particles.pop_back();
                continue;
            }
            Particle& p = particles[i];
            p.velocity.y -= p.gravity * dt;
            if (p.drag > 0) p.velocity *= glm::max(0.0f, 1.0f - p.drag * dt);
            p.position += p.velocity * dt;
            p.size += p.grow * dt;
            float floor_y = ground_height ? ground_height(p.position.x, p.position.z) : 0.0f;
            if (p.position.y < floor_y) {
                p.position.y = floor_y;
                p.velocity = glm::vec3(0);
            }
        }
    }

    void render(const glm::mat4& view, const glm::mat4& proj, GLuint shader) {
        if (particles.empty()) return;

        gpu_data.clear();
        for (auto& p : particles) {
            float f = p.max_life > 0 ? p.life / p.max_life : 1.0f;
            gpu_data.push_back({p.position, p.color * glm::clamp(f, 0.0f, 1.0f), p.size});
        }

        glUseProgram(shader);
        glUniformMatrix4fv(glGetUniformLocation(shader, "u_view"), 1, GL_FALSE, &view[0][0]);
        glUniformMatrix4fv(glGetUniformLocation(shader, "u_proj"), 1, GL_FALSE, &proj[0][0]);

        glBindBuffer(GL_ARRAY_BUFFER, vbo_inst);
        glBufferSubData(GL_ARRAY_BUFFER, 0, gpu_data.size()*sizeof(ParticleGPU), gpu_data.data());

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
         