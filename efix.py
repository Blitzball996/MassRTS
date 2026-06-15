#!/usr/bin/env python3
# Distinct effects for cannon / arrow / nuke. Rename-workaround for mmap lock.
import os
def patch(path, repls, tag):
    t=open(path,'r',encoding='utf-8',newline='').read(); orig=t
    nl='\r\n' if t.count('\r\n')>t.count('\n')-t.count('\r\n') else '\n'
    for old,new in repls:
        o=old.replace('\n',nl); n=new.replace('\n',nl)
        if o not in t: print(f"  SKIP[{tag}]: {old[:48]!r}"); continue
        t=t.replace(o,n,1); print(f"  OK[{tag}]: {old[:40]!r}")
    if t==orig: print(f"  no change {path}"); return
    tmp=path+'.t'; open(tmp,'w',encoding='utf-8',newline='').write(t)
    bak=path+'.'+tag+'bak'
    i=0
    while os.path.exists(bak):
        i+=1; bak=path+'.'+tag+'bak'+str(i)
    os.rename(path,bak); os.rename(tmp,path); print(f"  WROTE {path}")

P=r"G:\CMakePJ\MassRTS\src\render\particles.h"

# 1) Particle struct: add per-effect fields
patch(P, [(
"""struct Particle {
    glm::vec3 position;
    glm::vec3 velocity;
    glm::vec3 color;
    float life; // seconds remaining
    float size;
};""",
"""struct Particle {
    glm::vec3 position;
    glm::vec3 velocity;
    glm::vec3 color;
    float life;            // seconds remaining
    float size;
    float max_life = 0.8f; // for correct per-effect alpha fade
    float gravity = 80.0f; // per-particle gravity (smoke small, sparks high)
    float drag = 0.0f;     // velocity damping/sec (smoke & rings slow down)
    float grow = 0.0f;     // size growth/sec (puffs & shockwave rings expand)
};""")], "struct")

# 2) bump particle cap for big nuke bursts
patch(P, [("    static constexpr int MAX_PARTICLES = 5000;",
           "    static constexpr int MAX_PARTICLES = 30000;")], "cap")

# 3) physics update -> per-particle gravity/drag/grow
patch(P, [(
"""            particles[i].velocity.y -= 80.0f * dt; // gravity
            particles[i].position += particles[i].velocity * dt;
            if (particles[i].position.y < 0) {
                particles[i].position.y = 0;
                particles[i].velocity = glm::vec3(0);
            }""",
"""            Particle& p = particles[i];
            p.velocity.y -= p.gravity * dt;
            if (p.drag > 0) p.velocity *= glm::max(0.0f, 1.0f - p.drag * dt);
            p.position += p.velocity * dt;
            p.size += p.grow * dt;
            if (p.position.y < 0) {
                p.position.y = 0;
                p.velocity = glm::vec3(0);
            }""")], "phys")

# 4) Add distinct effect spawners after spawn_combat
EFFECTS = r'''
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
'''

patch(P, [(
"""    void update(float dt) {""",
EFFECTS + "\n    void update(float dt) {"
)], "spawners")

# 5) fade by per-particle max_life (so long-lived smoke isn't over-bright)
patch(P, [(
"            gpu_data.push_back({p.position, p.color * (p.life / 0.8f), p.size});",
"            float f = p.max_life > 0 ? p.life / p.max_life : 1.0f;\n"
"            gpu_data.push_back({p.position, p.color * glm::clamp(f, 0.0f, 1.0f), p.size});"
)], "fade")

print("DONE")

