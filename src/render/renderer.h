#pragma once
#include <glad/glad.h>
#include <GLFW/glfw3.h> // glfwGetTime for sky animation (glad must precede)
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "../ecs/world.h"
#include "mesh_gen.h"
#include "terrain.h"
#include "decor.h"
#include "base_system.h"
#include "model_library.h"
#include "../core/asset_manifest.h"
#include "skinned_model.h"
#include "particles.h"
#include "projectiles.h"
#include "gpu_compute.h"
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <iostream>
#include <functional>
#include <algorithm>

struct InstanceData {
    glm::vec3 position;
    glm::vec3 color;
    float scale;
    float rotation;
    float state;
    float _pad;
};

struct Frustum {
    glm::vec4 planes[6];
    void extract(const glm::mat4& vp) {
        planes[0] = glm::vec4(vp[0][3]+vp[0][0], vp[1][3]+vp[1][0], vp[2][3]+vp[2][0], vp[3][3]+vp[3][0]);
        planes[1] = glm::vec4(vp[0][3]-vp[0][0], vp[1][3]-vp[1][0], vp[2][3]-vp[2][0], vp[3][3]-vp[3][0]);
        planes[2] = glm::vec4(vp[0][3]+vp[0][1], vp[1][3]+vp[1][1], vp[2][3]+vp[2][1], vp[3][3]+vp[3][1]);
        planes[3] = glm::vec4(vp[0][3]-vp[0][1], vp[1][3]-vp[1][1], vp[2][3]-vp[2][1], vp[3][3]-vp[3][1]);
        planes[4] = glm::vec4(vp[0][3]+vp[0][2], vp[1][3]+vp[1][2], vp[2][3]+vp[2][2], vp[3][3]+vp[3][2]);
        planes[5] = glm::vec4(vp[0][3]-vp[0][2], vp[1][3]-vp[1][2], vp[2][3]-vp[2][2], vp[3][3]-vp[3][2]);
        for (int i = 0; i < 6; i++) {
            float len = glm::length(glm::vec3(planes[i]));
            if (len > 0.0001f) planes[i] /= len;
        }
    }
    bool point_inside(glm::vec3 p, float radius = 5.0f) const {
        for (int i = 0; i < 6; i++)
            if (glm::dot(glm::vec3(planes[i]), p) + planes[i].w < -radius) return false;
        return true;
    }
};

// Number of distinct mesh types we render (matches UnitType enum)
static constexpr int NUM_MESH_TYPES = 10;

class Renderer {
public:
    GLuint unit_shader = 0, terrain_shader = 0, select_shader = 0;
    GLuint particle_shader = 0, billboard_shader = 0, projectile_shader = 0;
    GLuint skinned_shader = 0;
    GLuint sky_shader = 0; GLuint sky_vao = 0; // procedural atmospheric sky
    glm::vec3 sun_dir = glm::normalize(glm::vec3(0.4f, 0.85f, 0.3f)); // matches scene light dir
    SkinnedModel skinned_models[NUM_MESH_TYPES]; // optional GPU-skinned override per bucket

    Mesh meshes[NUM_MESH_TYPES]; // Infantry,Cavalry,Archer,Bomber,Artillery,Shield,Samurai
    float model_render_scale[NUM_MESH_TYPES] = {1,1,1,1,1,1,1,1,1,1}; // manifest per-unit scale
    GLuint inst_vbo[NUM_MESH_TYPES] = {};
    std::vector<InstanceData> inst_data[NUM_MESH_TYPES];

    GLuint bb_vao = 0, bb_quad_vbo = 0, bb_inst_vbo = 0;
    std::vector<InstanceData> bb_data;

    float lod_distance = 400.0f;
    float game_time = 0.0f;
    float frame_dt = 0.016f;
    glm::vec3 camera_pos_world = {0,0,0};

    Terrain terrain;
    BattlefieldDecor decor;
    BaseSystem bases;
    ModelLibrary models;
    ParticleSystem particles;
    ProjectileSystem projectiles;
    GPUCompute gpu_compute;

    GLuint select_vao = 0, select_vbo_q = 0;
    std::string base_path;

    void set_base_path(const std::string& exe) {
        size_t p = exe.find_last_of("/\\");
        base_path = (p != std::string::npos) ? exe.substr(0, p + 1) : "./";
    }

    bool init(int w, int h) {
        std::string sd = find_shader_dir();
        if (sd.empty()) { std::cerr << "No shaders!\n"; return false; }

        unit_shader = load_shader(sd+"unit.vert", sd+"unit.frag");
        terrain_shader = load_shader(sd+"terrain.vert", sd+"terrain.frag");
        select_shader = load_shader(sd+"select.vert", sd+"select.frag");
        particle_shader = load_shader(sd+"particle.vert", sd+"particle.frag");
        billboard_shader = load_shader(sd+"billboard.vert", sd+"billboard.frag");
        skinned_shader = load_shader(sd+"skinned.vert", sd+"skinned.frag");
        projectile_shader = load_shader(sd+"projectile.vert", sd+"projectile.frag");
        sky_shader = load_shader(sd+"sky.vert", sd+"sky.frag");
        glGenVertexArrays(1, &sky_vao); // empty VAO; sky is a gl_VertexID triangle
        if (!unit_shader || !terrain_shader) return false;

        // Create all meshes (index matches UnitType enum)
        meshes[0] = MeshGenerator::create_infantry();
        meshes[1] = MeshGenerator::create_cavalry();
        meshes[2] = MeshGenerator::create_archer();
        meshes[3] = MeshGenerator::create_infantry(); // Bomber uses infantry mesh
        meshes[4] = MeshGenerator::create_artillery();
        meshes[5] = MeshGenerator::create_shield();
        meshes[6] = MeshGenerator::create_samurai();
        meshes[7] = MeshGenerator::create_militia();
        meshes[8] = MeshGenerator::create_wall();
        meshes[9] = MeshGenerator::create_turret();

        // --- Optional CC0 model override ---
        // Drop Kenney/Quaternius .obj files in assets/models/ with these names
        // and they replace the matching procedural mesh automatically. Missing
        // files are silently skipped (procedural mesh stays). Index = UnitType.
        const char* model_files[NUM_MESH_TYPES] = {
            "infantry", "cavalry", "archer", "bomber", "artillery",
            "shield", "samurai", "militia", "wall", "turret"
        };
        // Candidate roots (cwd may be project root or build/Release).
        const char* asset_roots[] = {
            "assets/models/", "../assets/models/",
            "../../assets/models/", "../../../assets/models/"
        };
        for (int i = 0; i < NUM_MESH_TYPES; i++) {
            // Manifest override takes priority: assets/manifest.json may map this
            // unit to a specific model file + render scale. Falls back to the
            // default "<name>.obj" scan if no manifest entry exists.
            const ModelOverride* mo = g_manifest.model(model_files[i]);
            std::string filename = (mo && mo->present) ? mo->file
                                                       : (std::string(model_files[i]) + ".obj");
            if (mo && mo->present) model_render_scale[i] = mo->scale;
            for (const char* root : asset_roots) {
                std::string path = std::string(root) + filename;
                std::ifstream probe(path);
                if (!probe.good()) continue;       // file not here, try next root
                probe.close();
                if (models.load(model_files[i], path)) {
                    const ModelEntry* me = models.get(model_files[i]);
                    if (me && me->mesh.vao) meshes[i] = me->mesh; // override
                }
                break; // found this model, stop scanning roots
            }
        }

        // --- Optional GPU-skinned models (asset pipeline output) ---
        // If manifest model "file" ends in .mesh (or a "<name>.mesh" exists),
        // load it as a skinned model. The skinned draw path then animates it.
        for (int i = 0; i < NUM_MESH_TYPES; i++) {
            const ModelOverride* mo = g_manifest.model(model_files[i]);
            std::string base;
            if (mo && mo->present) {
                std::string f = mo->file;
                size_t dot = f.find_last_of('.');
                std::string stem = (dot == std::string::npos) ? f : f.substr(0, dot);
                base = stem;
            } else {
                base = model_files[i];
            }
            for (const char* root : asset_roots) {
                std::string mpath = std::string(root) + base + ".mesh";
                std::ifstream probe(mpath, std::ios::binary);
                if (!probe.good()) continue;
                probe.close();
                if (skinned_models[i].load(std::string(root) + base)) {
                    if (mo && mo->present) model_render_scale[i] = mo->scale;
                }
                break;
            }
        }

        for (int i = 0; i < NUM_MESH_TYPES; i++)
            setup_instancing(meshes[i], inst_vbo[i]);

        // Billboard VAO
        float quad[] = {-0.5f,-0.5f, 0.5f,-0.5f, -0.5f,0.5f, 0.5f,0.5f};
        glGenVertexArrays(1, &bb_vao);
        glBindVertexArray(bb_vao);
        glGenBuffers(1, &bb_quad_vbo);
        glBindBuffer(GL_ARRAY_BUFFER, bb_quad_vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        glEnableVertexAttribArray(0);
        glGenBuffers(1, &bb_inst_vbo);
        glBindBuffer(GL_ARRAY_BUFFER, bb_inst_vbo);
        glBufferData(GL_ARRAY_BUFFER, MAX_ENTITIES * sizeof(InstanceData), nullptr, GL_DYNAMIC_DRAW);
        size_t s = sizeof(InstanceData);
        glVertexAttribPointer(2,3,GL_FLOAT,GL_FALSE,s,(void*)offsetof(InstanceData,position));
        glEnableVertexAttribArray(2); glVertexAttribDivisor(2,1);
        glVertexAttribPointer(3,3,GL_FLOAT,GL_FALSE,s,(void*)offsetof(InstanceData,color));
        glEnableVertexAttribArray(3); glVertexAttribDivisor(3,1);
        glVertexAttribPointer(4,1,GL_FLOAT,GL_FALSE,s,(void*)offsetof(InstanceData,scale));
        glEnableVertexAttribArray(4); glVertexAttribDivisor(4,1);
        glVertexAttribPointer(5,1,GL_FLOAT,GL_FALSE,s,(void*)offsetof(InstanceData,rotation));
        glEnableVertexAttribArray(5); glVertexAttribDivisor(5,1);
        glVertexAttribPointer(6,1,GL_FLOAT,GL_FALSE,s,(void*)offsetof(InstanceData,state));
        glEnableVertexAttribArray(6); glVertexAttribDivisor(6,1);
        glVertexAttribPointer(10,1,GL_FLOAT,GL_FALSE,s,(void*)offsetof(InstanceData,_pad));
        glEnableVertexAttribArray(10); glVertexAttribDivisor(10,1);
        glBindVertexArray(0);

        terrain.generate();
        decor.generate(3000.0f, [this](float x, float z){ return terrain.get_height_at(x, z); });
        bases.init({-550, 0}, {550, 0}, [this](float x, float z){ return terrain.get_height_at(x, z); });
        particles.init();
        projectiles.init();
        gpu_compute.init(sd, terrain);

        if (select_shader) {
            float sq[] = {-1,-1, 1,-1, -1,1, 1,1};
            glGenVertexArrays(1, &select_vao);
            glBindVertexArray(select_vao);
            glGenBuffers(1, &select_vbo_q);
            glBindBuffer(GL_ARRAY_BUFFER, select_vbo_q);
            glBufferData(GL_ARRAY_BUFFER, sizeof(sq), sq, GL_STATIC_DRAW);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
            glEnableVertexAttribArray(0);
            glBindVertexArray(0);
        }

        glEnable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        return true;
    }

    void update(float dt) { particles.update(dt); game_time += dt; frame_dt = dt; }

    void spawn_hit_particles(glm::vec2 pos_xz, glm::vec3 color) {
        float y = terrain.get_height_at(pos_xz.x, pos_xz.y) + 1.0f;
        particles.spawn_combat({pos_xz.x, y, pos_xz.y}, color);
    }

    void spawn_explosion_particles(glm::vec3 center, float radius, bool is_nuke) {
        int count = is_nuke ? 200 : 30;
        for (int i = 0; i < count && particles.particles.size() < ParticleSystem::MAX_PARTICLES; i++) {
            std::uniform_real_distribution<float> ang(0, 6.28f);
            std::uniform_real_distribution<float> spd(20.0f, is_nuke ? 150.0f : 60.0f);
            std::uniform_real_distribution<float> up(30.0f, is_nuke ? 200.0f : 80.0f);
            std::uniform_real_distribution<float> life(0.5f, is_nuke ? 3.0f : 1.5f);
            std::uniform_real_distribution<float> sz(is_nuke ? 3.0f : 1.0f, is_nuke ? 8.0f : 3.0f);
            float a = ang(particles.rng);
            float s = spd(particles.rng);
            Particle p;
            p.position = center + glm::vec3(cos(a)*3, 2, sin(a)*3);
            p.velocity = glm::vec3(cos(a)*s, up(particles.rng), sin(a)*s);
            p.color = is_nuke ? glm::vec3(1.0f, 0.4f + i*0.003f, 0.0f) : glm::vec3(0.8f, 0.4f, 0.1f);
            p.life = life(particles.rng);
            p.size = sz(particles.rng);
            particles.particles.push_back(p);
        }
    }

    void render(const World& world, const glm::mat4& view, const glm::mat4& proj, glm::vec3 cam_pos) {
        camera_pos_world = cam_pos;
        // Balance quality vs cost: real 3D meshes up close, cheap imposters in
        // the far field. The spherical billboard now reads as a proper humanoid,
        // so the switch can come in fairly close to keep the mesh count low.
        lod_distance = glm::clamp(cam_pos.y * 0.7f, 350.0f, 1100.0f);

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);

        // === Procedural sky (atmospheric scattering + volumetric clouds) ===
        // Drawn first as a fullscreen triangle at the far plane; depth test
        // LEQUAL so terrain/units overwrite it. No depth write so it never
        // occludes the scene.
        if (sky_shader) {
            glDepthMask(GL_FALSE);
            glDepthFunc(GL_LEQUAL);
            glUseProgram(sky_shader);
            glm::mat4 inv_view = glm::inverse(view);
            glm::mat4 inv_proj = glm::inverse(proj);
            glUniformMatrix4fv(glGetUniformLocation(sky_shader,"u_inv_view"),1,GL_FALSE,&inv_view[0][0]);
            glUniformMatrix4fv(glGetUniformLocation(sky_shader,"u_inv_proj"),1,GL_FALSE,&inv_proj[0][0]);
            glUniform3f(glGetUniformLocation(sky_shader,"u_sun_dir"), sun_dir.x, sun_dir.y, sun_dir.z);
            glUniform3f(glGetUniformLocation(sky_shader,"u_cam_pos"), cam_pos.x, cam_pos.y, cam_pos.z);
            glUniform1f(glGetUniformLocation(sky_shader,"u_time"), (float)glfwGetTime());
            glBindVertexArray(sky_vao);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glBindVertexArray(0);
            glDepthMask(GL_TRUE);
            glDepthFunc(GL_LESS);
        } else {
            glClearColor(0.45f, 0.55f, 0.6f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
        }

        // Terrain
        glUseProgram(terrain_shader);
        glUniformMatrix4fv(glGetUniformLocation(terrain_shader,"u_view"),1,GL_FALSE,&view[0][0]);
        glUniformMatrix4fv(glGetUniformLocation(terrain_shader,"u_proj"),1,GL_FALSE,&proj[0][0]);
        glUniform1f(glGetUniformLocation(terrain_shader,"u_time"), game_time);
        glUniform3f(glGetUniformLocation(terrain_shader,"u_cam_pos"), cam_pos.x, cam_pos.y, cam_pos.z);
        terrain.render();

        // Decorations
        glUseProgram(unit_shader);
        glUniformMatrix4fv(glGetUniformLocation(unit_shader,"u_view"),1,GL_FALSE,&view[0][0]);
        glUniform1f(glGetUniformLocation(unit_shader,"u_time"), game_time);
        glUniformMatrix4fv(glGetUniformLocation(unit_shader,"u_proj"),1,GL_FALSE,&proj[0][0]);
        glUniform1f(glGetUniformLocation(unit_shader,"u_model_scale"), 1.0f); // decor/bases use full scale
        decor.render(unit_shader);
        bases.render(unit_shader);

        // Units (CPU instancing - reliable, GPU handles combat/movement only)
        render_cpu_path(world, view, proj, cam_pos);

        // Projectiles
        if (projectile_shader && !projectiles.projectiles.empty()) {
            glDepthMask(GL_FALSE);
            projectiles.render(view, proj, projectile_shader);
            glDepthMask(GL_TRUE);
        }

        // Particles
        if (particle_shader) {
            glDepthMask(GL_FALSE);
            particles.render(view, proj, particle_shader);
            glDepthMask(GL_TRUE);
        }
    }

    void render_selection_box(glm::vec2 min_ndc, glm::vec2 max_ndc) {
        if (!select_shader) return;
        glDisable(GL_DEPTH_TEST);
        glUseProgram(select_shader);
        glUniform4f(glGetUniformLocation(select_shader,"u_box"), min_ndc.x,min_ndc.y,max_ndc.x,max_ndc.y);
        glBindVertexArray(select_vao);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindVertexArray(0);
        glEnable(GL_DEPTH_TEST);
    }

    float get_terrain_height(float x, float z) const { return terrain.get_height_at(x, z); }

    void gpu_upload_units(const World& world) {
        if (gpu_compute.supported) gpu_compute.upload_units(world);
    }
    void gpu_upload_batch(const World& world, uint32_t start, uint32_t count) {
        if (gpu_compute.supported) gpu_compute.upload_unit_batch(world, start, count);
    }
    void gpu_readback_positions(World& world, uint32_t start, uint32_t count) {
        if (gpu_compute.supported) gpu_compute.readback_positions(world, start, count);
    }
    bool is_gpu_compute() const { return gpu_compute.supported; }

    void cleanup() {
        for (int i = 0; i < NUM_MESH_TYPES; i++) {
            MeshGenerator::destroy_mesh(meshes[i]);
            glDeleteBuffers(1, &inst_vbo[i]);
        }
        terrain.cleanup(); decor.cleanup(); particles.cleanup(); projectiles.cleanup();
        gpu_compute.cleanup();
        glDeleteBuffers(1, &bb_inst_vbo); glDeleteBuffers(1, &bb_quad_vbo);
        glDeleteVertexArrays(1, &bb_vao);
        glDeleteProgram(unit_shader); glDeleteProgram(terrain_shader);
        glDeleteProgram(select_shader); glDeleteProgram(particle_shader);
        glDeleteProgram(billboard_shader); glDeleteProgram(projectile_shader);
    }

private:
    // Draw one bucket using the GPU-skinned shader + animation texture.
    // Instance data comes from the same ssbo_instances, bound at locations 5-9
    // (skinned vertex data occupies 0-4). Clip selection: we feed the unit's
    // state via a_inst_state and pick the matching clip range below.
    void draw_skinned_bucket(int t, uint32_t clamped, const glm::mat4& view, const glm::mat4& proj) {
        SkinnedModel& sm = skinned_models[t];
        glUseProgram(skinned_shader);
        glUniformMatrix4fv(glGetUniformLocation(skinned_shader,"u_view"),1,GL_FALSE,&view[0][0]);
        glUniformMatrix4fv(glGetUniformLocation(skinned_shader,"u_proj"),1,GL_FALSE,&proj[0][0]);
        glUniform1f(glGetUniformLocation(skinned_shader,"u_time"), game_time);
        glUniform1f(glGetUniformLocation(skinned_shader,"u_model_scale"), model_render_scale[t]);
        glm::vec3 ld = glm::normalize(glm::vec3(0.4f, 1.0f, 0.3f));
        glUniform3f(glGetUniformLocation(skinned_shader,"u_light_dir"), ld.x, ld.y, ld.z);
        glUniform1i(glGetUniformLocation(skinned_shader,"u_use_texture"), 0);

        // Animation texture + dimensions
        if (sm.anim_tex) {
            glActiveTexture(GL_TEXTURE3);
            glBindTexture(GL_TEXTURE_2D, sm.anim_tex);
            glUniform1i(glGetUniformLocation(skinned_shader,"u_anim_tex"), 3);
        }
        glUniform1i(glGetUniformLocation(skinned_shader,"u_bone_count"), (int)sm.bone_count);
        glUniform1i(glGetUniformLocation(skinned_shader,"u_anim_width"), sm.anim_tex_width);
        glUniform1i(glGetUniformLocation(skinned_shader,"u_anim_height"), sm.anim_tex_height);

        // Pick a clip (prefer "walk", else "idle", else first). Per-unit state-based
        // clip switching can be added by drawing sub-ranges; for now whole bucket
        // shares the locomotion clip which already de-syncs per unit via a seed.
        // Default fallback clip (walk -> idle -> first available)
        const AnimClip* def = sm.clip("walk");
        if (!def) def = sm.clip("idle");
        if (!def && !sm.clips.empty()) def = &sm.clips.begin()->second;
        if (def) {
            glUniform1i(glGetUniformLocation(skinned_shader,"u_clip_start"), def->start);
            glUniform1i(glGetUniformLocation(skinned_shader,"u_clip_frames"), def->frames);
            glUniform1f(glGetUniformLocation(skinned_shader,"u_clip_fps"), def->fps);
        } else {
            glUniform1i(glGetUniformLocation(skinned_shader,"u_clip_frames"), 0);
        }
        glUniform1f(glGetUniformLocation(skinned_shader,"u_anim_phase"), 0.0f);

        // Per-state clip table: state 0=idle 1=walk(moving) 2=attack 3=death.
        // Maps clip names to unit states; -1 start means "use default clip".
        const char* state_clip[4] = { "idle", "walk", "attack", "death" };
        int   st_start[4]; int st_frames[4]; float st_fps[4]; int st_loop[4];
        for (int s = 0; s < 4; s++) {
            const AnimClip* c = sm.clip(state_clip[s]);
            if (c) { st_start[s]=c->start; st_frames[s]=c->frames; st_fps[s]=c->fps; }
            else   { st_start[s]=-1;       st_frames[s]=0;         st_fps[s]=24.0f; }
            st_loop[s] = (s == 3) ? 0 : 1;   // death plays once and holds
        }
        glUniform1iv(glGetUniformLocation(skinned_shader,"u_state_start"), 4, st_start);
        glUniform1iv(glGetUniformLocation(skinned_shader,"u_state_frames"), 4, st_frames);
        glUniform1fv(glGetUniformLocation(skinned_shader,"u_state_fps"), 4, st_fps);
        glUniform1iv(glGetUniformLocation(skinned_shader,"u_state_loop"), 4, st_loop);

        glBindVertexArray(sm.vao); // provides attribs 0-4 (pos/norm/uv/bones/weights)

        // Instance data at locations 5-9 from ssbo_instances bucket offset
        glBindBuffer(GL_ARRAY_BUFFER, gpu_compute.ssbo_instances);
        size_t offset = (size_t)t * GPUCompute::MAX_INSTANCES_PER_TYPE * sizeof(InstanceData);
        size_t s = sizeof(InstanceData);
        glVertexAttribPointer(5,3,GL_FLOAT,GL_FALSE,s,(void*)(offset + offsetof(InstanceData,position)));
        glEnableVertexAttribArray(5); glVertexAttribDivisor(5,1);
        glVertexAttribPointer(6,3,GL_FLOAT,GL_FALSE,s,(void*)(offset + offsetof(InstanceData,color)));
        glEnableVertexAttribArray(6); glVertexAttribDivisor(6,1);
        glVertexAttribPointer(7,1,GL_FLOAT,GL_FALSE,s,(void*)(offset + offsetof(InstanceData,scale)));
        glEnableVertexAttribArray(7); glVertexAttribDivisor(7,1);
        glVertexAttribPointer(8,1,GL_FLOAT,GL_FALSE,s,(void*)(offset + offsetof(InstanceData,rotation)));
        glEnableVertexAttribArray(8); glVertexAttribDivisor(8,1);
        glVertexAttribPointer(9,1,GL_FLOAT,GL_FALSE,s,(void*)(offset + offsetof(InstanceData,state)));
        glEnableVertexAttribArray(9); glVertexAttribDivisor(9,1);
        glVertexAttribPointer(10,1,GL_FLOAT,GL_FALSE,s,(void*)(offset + offsetof(InstanceData,_pad)));
        glEnableVertexAttribArray(10); glVertexAttribDivisor(10,1);

        glDrawElementsInstanced(GL_TRIANGLES, sm.index_count, GL_UNSIGNED_INT, 0, (int)clamped);
        glBindVertexArray(0);
        glActiveTexture(GL_TEXTURE0);
    }

    void render_gpu_path(const World& world, const glm::mat4& view, const glm::mat4& proj, glm::vec3 cam_pos) {
        Frustum frustum;
        frustum.extract(proj * view);

        // Dispatch GPU instance generation compute shader
        gpu_compute.dispatch_instances(world.entity_count, cam_pos,
            lod_distance, 3000.0f, frustum.planes, game_time);

        // Read back instance counts for draw
        uint32_t counts[4];
        gpu_compute.get_instance_counts(counts);

        // Draw each mesh type from GPU-generated instance buffer
        glUseProgram(unit_shader);
        glUniformMatrix4fv(glGetUniformLocation(unit_shader,"u_view"),1,GL_FALSE,&view[0][0]);
        glUniformMatrix4fv(glGetUniformLocation(unit_shader,"u_proj"),1,GL_FALSE,&proj[0][0]);
        glUniform1f(glGetUniformLocation(unit_shader,"u_time"), game_time);

        for (int t = 0; t < 3; t++) {
            if (counts[t] == 0) continue;
            uint32_t clamped = std::min(counts[t], (uint32_t)GPUCompute::MAX_INSTANCES_PER_TYPE);

            // --- GPU-skinned path (asset pipeline models with animations) ---
            if (skinned_models[t].valid && skinned_shader) {
                draw_skinned_bucket(t, clamped, view, proj);
                glUseProgram(unit_shader); // restore for next non-skinned bucket
                continue;
            }

            // Manifest per-unit render scale (bucket 0/1/2 = infantry/cavalry/archer mesh)
            glUniform1f(glGetUniformLocation(unit_shader,"u_model_scale"), model_render_scale[t]);
            glBindVertexArray(meshes[t].vao);

            // Bind GPU instance data from the correct offset in ssbo_instances
            // Offset: bucket * MAX_INSTANCES_PER_TYPE * sizeof(InstanceData)
            glBindBuffer(GL_ARRAY_BUFFER, gpu_compute.ssbo_instances);
            size_t offset = (size_t)t * GPUCompute::MAX_INSTANCES_PER_TYPE * sizeof(InstanceData);
            size_t s = sizeof(InstanceData);
            glVertexAttribPointer(2,3,GL_FLOAT,GL_FALSE,s,(void*)(offset + offsetof(InstanceData,position)));
            glEnableVertexAttribArray(2); glVertexAttribDivisor(2,1);
            glVertexAttribPointer(3,3,GL_FLOAT,GL_FALSE,s,(void*)(offset + offsetof(InstanceData,color)));
            glEnableVertexAttribArray(3); glVertexAttribDivisor(3,1);
            glVertexAttribPointer(4,1,GL_FLOAT,GL_FALSE,s,(void*)(offset + offsetof(InstanceData,scale)));
            glEnableVertexAttribArray(4); glVertexAttribDivisor(4,1);
            glVertexAttribPointer(5,1,GL_FLOAT,GL_FALSE,s,(void*)(offset + offsetof(InstanceData,rotation)));
            glEnableVertexAttribArray(5); glVertexAttribDivisor(5,1);
            glVertexAttribPointer(6,1,GL_FLOAT,GL_FALSE,s,(void*)(offset + offsetof(InstanceData,state)));
            glEnableVertexAttribArray(6); glVertexAttribDivisor(6,1);
            glVertexAttribPointer(10,1,GL_FLOAT,GL_FALSE,s,(void*)(offset + offsetof(InstanceData,_pad)));
            glEnableVertexAttribArray(10); glVertexAttribDivisor(10,1);

            glDrawElementsInstanced(GL_TRIANGLES, meshes[t].index_count, GL_UNSIGNED_INT, 0, (int)clamped);
        }
        glBindVertexArray(0);

        // Billboard LOD pass
        if (counts[3] > 0 && billboard_shader) {
            uint32_t bb_count = std::min(counts[3], (uint32_t)GPUCompute::MAX_INSTANCES_PER_TYPE);
            glUseProgram(billboard_shader);
            glUniformMatrix4fv(glGetUniformLocation(billboard_shader,"u_view"),1,GL_FALSE,&view[0][0]);
            glUniformMatrix4fv(glGetUniformLocation(billboard_shader,"u_proj"),1,GL_FALSE,&proj[0][0]);

            glBindVertexArray(bb_vao);
            // Bind billboard instances from bucket 3 offset
            glBindBuffer(GL_ARRAY_BUFFER, gpu_compute.ssbo_instances);
            size_t bb_offset = (size_t)3 * GPUCompute::MAX_INSTANCES_PER_TYPE * sizeof(InstanceData);
            size_t s2 = sizeof(InstanceData);
            glVertexAttribPointer(2,3,GL_FLOAT,GL_FALSE,s2,(void*)(bb_offset + offsetof(InstanceData,position)));
            glEnableVertexAttribArray(2); glVertexAttribDivisor(2,1);
            glVertexAttribPointer(3,3,GL_FLOAT,GL_FALSE,s2,(void*)(bb_offset + offsetof(InstanceData,color)));
            glEnableVertexAttribArray(3); glVertexAttribDivisor(3,1);
            glVertexAttribPointer(4,1,GL_FLOAT,GL_FALSE,s2,(void*)(bb_offset + offsetof(InstanceData,scale)));
            glEnableVertexAttribArray(4); glVertexAttribDivisor(4,1);
            glVertexAttribPointer(5,1,GL_FLOAT,GL_FALSE,s2,(void*)(bb_offset + offsetof(InstanceData,rotation)));
            glEnableVertexAttribArray(5); glVertexAttribDivisor(5,1);
            glVertexAttribPointer(6,1,GL_FLOAT,GL_FALSE,s2,(void*)(bb_offset + offsetof(InstanceData,state)));
            glEnableVertexAttribArray(6); glVertexAttribDivisor(6,1);
            glVertexAttribPointer(10,1,GL_FLOAT,GL_FALSE,s2,(void*)(bb_offset + offsetof(InstanceData,_pad)));
            glEnableVertexAttribArray(10); glVertexAttribDivisor(10,1);

            glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, (int)bb_count);
            glBindVertexArray(0);
        }
    }

    void render_cpu_path(const World& world, const glm::mat4& view, const glm::mat4& proj, glm::vec3 cam_pos) {
        Frustum frustum;
        frustum.extract(proj * view);

        for (int t = 0; t < NUM_MESH_TYPES; t++) inst_data[t].clear();
        bb_data.clear();

        for (uint32_t i = 0; i < world.entity_count; i++) {
            if (!world.is_visible(i)) continue;
            glm::vec2 p = world.transforms.position[i];
            float y = terrain.get_height_at(p.x, p.y) + world.transforms.y_offset[i];
            glm::vec3 wp = {p.x, y, p.y};

            float dist = glm::length(wp - cam_pos);
            if (dist > 2500.0f) continue;
            if (!frustum.point_inside(wp, world.renders.scale[i] * 3.0f)) continue;

            InstanceData d;
            d.position = wp;
            d.color = world.renders.color[i];
            d.scale = world.renders.scale[i];
            d.rotation = world.transforms.rotation[i];

            float anim_state = 0.0f;
            auto ustate = world.units.state[i];
            if (ustate == UnitState::Dead) {
                anim_state = 3.0f;
                float fade = glm::max(world.units.hit_timer[i] / 4.0f, 0.0f);
                d.color *= fade;
            } else if (ustate == UnitState::Ragdoll) {
                anim_state = 3.0f; // also use dead/tilt animation
            } else if (ustate == UnitState::Moving || ustate == UnitState::Retreating) {
                anim_state = 1.0f;
            } else if (ustate == UnitState::Attacking) {
                anim_state = 2.0f;
            }
            d.state = anim_state;
            if (ustate != UnitState::Dead && ustate != UnitState::Ragdoll && world.units.hit_timer[i] > 0)
                d.state += 4.0f;

            if (dist < lod_distance) {
                int ti = (int)world.units.type[i];
                if (ti < 0 || ti >= NUM_MESH_TYPES) ti = 0;
                inst_data[ti].push_back(d);
            } else {
                bb_data.push_back(d);
            }
        }

        glUseProgram(unit_shader);
        glUniformMatrix4fv(glGetUniformLocation(unit_shader,"u_view"),1,GL_FALSE,&view[0][0]);
        glUniformMatrix4fv(glGetUniformLocation(unit_shader,"u_proj"),1,GL_FALSE,&proj[0][0]);
        glUniform1f(glGetUniformLocation(unit_shader,"u_time"), game_time);

        for (int t = 0; t < NUM_MESH_TYPES; t++) {
            if (inst_data[t].empty()) continue;
            // FIX: u_model_scale must be set here too, else it defaults to 0
            // and units shrink to nothing (steve/villager became invisible).
            glUniform1f(glGetUniformLocation(unit_shader,"u_model_scale"), model_render_scale[t]);
            glBindVertexArray(meshes[t].vao);
            glBindBuffer(GL_ARRAY_BUFFER, inst_vbo[t]);
            glBufferSubData(GL_ARRAY_BUFFER, 0, inst_data[t].size()*sizeof(InstanceData), inst_data[t].data());
            glDrawElementsInstanced(GL_TRIANGLES, meshes[t].index_count, GL_UNSIGNED_INT, 0, (int)inst_data[t].size());
        }
        glBindVertexArray(0);

        if (!bb_data.empty() && billboard_shader) {
            glUseProgram(billboard_shader);
            glUniformMatrix4fv(glGetUniformLocation(billboard_shader,"u_view"),1,GL_FALSE,&view[0][0]);
            glUniformMatrix4fv(glGetUniformLocation(billboard_shader,"u_proj"),1,GL_FALSE,&proj[0][0]);
            glBindVertexArray(bb_vao);
            glBindBuffer(GL_ARRAY_BUFFER, bb_inst_vbo);
            glBufferSubData(GL_ARRAY_BUFFER, 0, bb_data.size()*sizeof(InstanceData), bb_data.data());
            glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, (int)bb_data.size());
            glBindVertexArray(0);
        }
    }

    void setup_instancing(Mesh& mesh, GLuint& ivbo) {
        glBindVertexArray(mesh.vao);
        glGenBuffers(1, &ivbo);
        glBindBuffer(GL_ARRAY_BUFFER, ivbo);
        glBufferData(GL_ARRAY_BUFFER, MAX_ENTITIES * sizeof(InstanceData), nullptr, GL_DYNAMIC_DRAW);
        size_t s = sizeof(InstanceData);
        glVertexAttribPointer(2,3,GL_FLOAT,GL_FALSE,s,(void*)offsetof(InstanceData,position));
        glEnableVertexAttribArray(2); glVertexAttribDivisor(2,1);
        glVertexAttribPointer(3,3,GL_FLOAT,GL_FALSE,s,(void*)offsetof(InstanceData,color));
        glEnableVertexAttribArray(3); glVertexAttribDivisor(3,1);
        glVertexAttribPointer(4,1,GL_FLOAT,GL_FALSE,s,(void*)offsetof(InstanceData,scale));
        glEnableVertexAttribArray(4); glVertexAttribDivisor(4,1);
        glVertexAttribPointer(5,1,GL_FLOAT,GL_FALSE,s,(void*)offsetof(InstanceData,rotation));
        glEnableVertexAttribArray(5); glVertexAttribDivisor(5,1);
        glVertexAttribPointer(6,1,GL_FLOAT,GL_FALSE,s,(void*)offsetof(InstanceData,state));
        glEnableVertexAttribArray(6); glVertexAttribDivisor(6,1);
        glBindVertexArray(0);
    }

    std::string find_shader_dir() {
        std::vector<std::string> paths = {base_path+"shaders/","shaders/","../shaders/"};
        for (auto& p : paths) { std::ifstream f(p+"unit.vert"); if(f.good()){std::cout<<"Shaders: "<<p<<"\n"; return p;} }
        return "";
    }
    std::string load_file(const std::string& path) { std::ifstream f(path); if(!f.is_open()) return ""; std::stringstream ss; ss<<f.rdbuf(); return ss.str(); }
    GLuint compile(const std::string& src, GLenum type) {
        GLuint s=glCreateShader(type); const char*c=src.c_str(); glShaderSource(s,1,&c,nullptr); glCompileShader(s);
        int ok; glGetShaderiv(s,GL_COMPILE_STATUS,&ok);
        if(!ok){char log[512]; glGetShaderInfoLog(s,512,0,log); std::cerr<<"Shader: "<<log<<"\n"; return 0;} return s;
    }
    GLuint load_shader(const std::string& vp, const std::string& fp) {
        std::string vs=load_file(vp),fs=load_file(fp); if(vs.empty()||fs.empty()) return 0;
        GLuint v=compile(vs,GL_VERTEX_SHADER),f=compile(fs,GL_FRAGMENT_SHADER); if(!v||!f) return 0;
        GLuint p=glCreateProgram(); glAttachShader(p,v); glAttachShader(p,f); glLinkProgram(p);
        int ok; glGetProgramiv(p,GL_LINK_STATUS,&ok);
        if(!ok){char log[512]; glGetProgramInfoLog(p,512,0,log); std::cerr<<"Link: "<<log<<"\n"; return 0;}
        glDeleteShader(v); glDeleteShader(f); return p;
    }
};
