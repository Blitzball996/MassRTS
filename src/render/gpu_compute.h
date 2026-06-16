#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include "../ecs/world.h"
#include "terrain.h"
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <iostream>

// GPU-side unit data (must match compute shader struct)
struct UnitGPU {
    glm::vec2 position;
    glm::vec2 velocity;
    float rotation;
    float scale;
    float state;
    uint32_t type;
    glm::vec3 color;
    float _pad;
};

// Instance output from compute (must match InstanceData in renderer.h)
struct InstanceGPU {
    glm::vec3 position;
    glm::vec3 color;
    float scale;
    float rotation;
    float state;
    float _pad; // align to 40 bytes for SSBO
};

// Manages GPU compute for unit movement + instance generation
class GPUCompute {
public:
    GLuint compute_shader = 0;
    GLuint ssbo_units = 0;       // binding 0: unit data
    GLuint ssbo_instances = 0;   // binding 1: instance output
    GLuint ssbo_indirect = 0;    // binding 2: indirect draw commands
    GLuint ssbo_counters = 0;    // binding 3: atomic counters
    GLuint heightmap_tex = 0;    // terrain heightmap as texture
    const Terrain* terrain_ptr = nullptr; // for per-unit slope speed (uphill slow)

    // Combat + Movement GPU pipeline
    GLuint combat_shader = 0;
    GLuint movement_shader = 0;
    GLuint spatial_shader = 0;
    GLuint ssbo_cell_counts = 0;
    GLuint ssbo_cell_entries = 0;
    GLuint ssbo_damage = 0;
    GLuint ssbo_move_cmds = 0;
    GLuint ssbo_colors = 0;        // binding 8: per-unit color+scale
    bool combat_gpu_ready = false;
    bool gpu_instancing_ready = false;
    // Non-blocking readback fence. After the combat/movement dispatch we insert
    // a fence; readback waits on it with a tiny timeout and SKIPS this frame if
    // the GPU isn't done, reusing last frame's CPU data. This guarantees the CPU
    // never blocks long enough to cascade into a driver TDR -> black screen.
    GLsync combat_fence = 0;
    GLuint gpu_timer = 0; double last_gpu_ms = 0.0; bool timer_pending = false;
    // Triple-buffered async readback. After each dispatch we GPU-copy ssbo_units
    // into rb_buf[head] and set rb_fence[head]; readback maps the OLDEST buffer
    // (already finished -> no GPU stall). This removes the ~24ms/frame blocking
    // map that was 78% of frame time and the cause of the long-play black screen.
    static constexpr int RB_COUNT = 3;
    GLuint rb_buf[RB_COUNT] = {0,0,0};
    GLsync rb_fence[RB_COUNT] = {0,0,0};
    int rb_head = 0;        // buffer we copy INTO this frame
    bool rb_inited = false;
    uint32_t rb_count_at[RB_COUNT] = {0,0,0}; // entity_count snapshot per buffer

    static constexpr uint32_t MAX_INSTANCES_PER_TYPE = 80000;
    static constexpr uint32_t NUM_BUCKETS = 4; // infantry, cavalry, archer, billboard

    bool supported = false;

    bool init(const std::string& shader_dir, const Terrain& terrain) {
        terrain_ptr = &terrain;
        // Clear any pending GL errors
        while (glGetError() != GL_NO_ERROR) {}
        // Check GL 4.3+ compute support
        int major = 0, minor = 0;
        glGetIntegerv(GL_MAJOR_VERSION, &major);
        glGetIntegerv(GL_MINOR_VERSION, &minor);
        if (major < 4 || (major == 4 && minor < 3)) {
            std::cout << "GPU compute not supported (need GL 4.3+), falling back to CPU\n";
            supported = false;
            return false;
        }

        // Load compute shader
        std::string src = load_file(shader_dir + "compute_units.glsl");
        if (src.empty()) {
            std::cerr << "Failed to load compute shader\n";
            supported = false;
            return false;
        }

        GLuint cs = glCreateShader(GL_COMPUTE_SHADER);
        const char* c = src.c_str();
        glShaderSource(cs, 1, &c, nullptr);
        glCompileShader(cs);
        int ok;
        glGetShaderiv(cs, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[1024];
            glGetShaderInfoLog(cs, 1024, nullptr, log);
            std::cerr << "Compute shader compile: " << log << "\n";
            supported = false;
            return false;
        }

        compute_shader = glCreateProgram();
        glAttachShader(compute_shader, cs);
        glLinkProgram(compute_shader);
        glGetProgramiv(compute_shader, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[1024];
            glGetProgramInfoLog(compute_shader, 1024, nullptr, log);
            std::cerr << "Compute shader link: " << log << "\n";
            supported = false;
            return false;
        }
        glDeleteShader(cs);

        // Create SSBOs
        glGenBuffers(1, &ssbo_units);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_units);
        glBufferData(GL_SHADER_STORAGE_BUFFER, MAX_ENTITIES * 64, nullptr, GL_DYNAMIC_DRAW); // 64 bytes = UC combat struct size

        // Async readback ring buffers (GPU-side copy targets, mapped read-only).
        for (int b = 0; b < RB_COUNT; b++) {
            glGenBuffers(1, &rb_buf[b]);
            glBindBuffer(GL_COPY_WRITE_BUFFER, rb_buf[b]);
            glBufferData(GL_COPY_WRITE_BUFFER, MAX_ENTITIES * 64, nullptr, GL_STREAM_READ);
        }
        rb_inited = true;

        glGenBuffers(1, &ssbo_instances);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_instances);
        glBufferData(GL_SHADER_STORAGE_BUFFER, MAX_INSTANCES_PER_TYPE * NUM_BUCKETS * sizeof(InstanceGPU), nullptr, GL_DYNAMIC_DRAW);

        // Indirect draw commands: 4 commands (infantry, cavalry, archer, billboard)
        glGenBuffers(1, &ssbo_indirect);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_indirect);
        glBufferData(GL_SHADER_STORAGE_BUFFER, NUM_BUCKETS * 4 * sizeof(uint32_t), nullptr, GL_DYNAMIC_DRAW);

        glGenBuffers(1, &ssbo_counters);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_counters);
        glBufferData(GL_SHADER_STORAGE_BUFFER, NUM_BUCKETS * sizeof(uint32_t), nullptr, GL_DYNAMIC_DRAW);

        // Upload terrain heightmap as R32F texture
        upload_heightmap(terrain);

        supported = true;
        std::cout << "GPU compute initialized (GL " << major << "." << minor << ")\n";

        // Init combat + movement GPU pipeline
        combat_shader = load_compute_program(shader_dir + "compute_combat.glsl");
        movement_shader = load_compute_program(shader_dir + "compute_movement.glsl");
        spatial_shader = load_compute_program(shader_dir + "compute_spatial_hash.glsl");
        if (combat_shader && movement_shader && spatial_shader) {
            glGenBuffers(1, &ssbo_cell_counts);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_cell_counts);
            glBufferData(GL_SHADER_STORAGE_BUFFER, 22500*sizeof(uint32_t), nullptr, GL_DYNAMIC_DRAW);
            glGenBuffers(1, &ssbo_cell_entries);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_cell_entries);
            glBufferData(GL_SHADER_STORAGE_BUFFER, 22500*64*sizeof(uint32_t), nullptr, GL_DYNAMIC_DRAW);
            glGenBuffers(1, &ssbo_damage);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_damage);
            glBufferData(GL_SHADER_STORAGE_BUFFER, MAX_ENTITIES*sizeof(uint32_t), nullptr, GL_DYNAMIC_DRAW);
            glGenBuffers(1, &ssbo_move_cmds);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_move_cmds);
            glBufferData(GL_SHADER_STORAGE_BUFFER, MAX_ENTITIES*4*sizeof(float), nullptr, GL_DYNAMIC_DRAW);
            glGenBuffers(1, &ssbo_colors);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_colors);
            glBufferData(GL_SHADER_STORAGE_BUFFER, MAX_ENTITIES*4*sizeof(float), nullptr, GL_DYNAMIC_DRAW);
            combat_gpu_ready = true;
            gpu_instancing_ready = true;
            std::cout << "GPU combat pipeline ready!\n";
        }
        return true;
    }

    GLuint load_compute_program(const std::string& path) {
        std::string src = load_file(path);
        if (src.empty()) return 0;
        GLuint cs = glCreateShader(GL_COMPUTE_SHADER);
        const char* c = src.c_str();
        glShaderSource(cs, 1, &c, nullptr); glCompileShader(cs);
        int ok; glGetShaderiv(cs, GL_COMPILE_STATUS, &ok);
        if (!ok) { char log[512]; glGetShaderInfoLog(cs, 512, nullptr, log); std::cerr << path << ": " << log << "\n"; return 0; }
        GLuint prog = glCreateProgram();
        glAttachShader(prog, cs); glLinkProgram(prog);
        glGetProgramiv(prog, GL_LINK_STATUS, &ok);
        if (!ok) { char log[512]; glGetProgramInfoLog(prog, 512, nullptr, log); std::cerr << path << " link: " << log << "\n"; return 0; }
        glDeleteShader(cs);
        return prog;
    }

    // Upload unit data from CPU to GPU (only when state changes, not every frame!)
    void upload_units(const World& world) {
        std::vector<UnitGPU> gpu_units(world.entity_count);
        for (uint32_t i = 0; i < world.entity_count; i++) {
            gpu_units[i].position = world.transforms.position[i];
            gpu_units[i].velocity = world.transforms.velocity[i];
            gpu_units[i].rotation = world.transforms.rotation[i];
            gpu_units[i].scale = world.renders.scale[i];
            gpu_units[i].state = encode_state(world, i);
            gpu_units[i].type = (uint32_t)world.units.type[i];
            gpu_units[i].color = world.renders.color[i];
            gpu_units[i]._pad = 0;
        }
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_units);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, world.entity_count * sizeof(UnitGPU), gpu_units.data());
    }

    // Partial upload: only velocity/state/rotation for units in AI batch
    void upload_unit_batch(const World& world, uint32_t start, uint32_t count) {
        if (start + count > world.entity_count) count = world.entity_count - start;
        std::vector<UnitGPU> batch(count);
        for (uint32_t i = 0; i < count; i++) {
            uint32_t idx = start + i;
            batch[i].position = world.transforms.position[idx];
            batch[i].velocity = world.transforms.velocity[idx];
            batch[i].rotation = world.transforms.rotation[idx];
            batch[i].scale = world.renders.scale[idx];
            batch[i].state = encode_state(world, idx);
            batch[i].type = (uint32_t)world.units.type[idx];
            batch[i].color = world.renders.color[idx];
            batch[i]._pad = 0;
        }
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_units);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, start * sizeof(UnitGPU), count * sizeof(UnitGPU), batch.data());
    }

    // Dispatch compute: moves units, builds instance buffers, does frustum cull
    void dispatch(uint32_t unit_count, float dt, glm::vec3 cam_pos,
                  float lod_dist, float terrain_size, const glm::vec4 frustum_planes[6],
                  float game_time) {
        // Reset counters to 0
        uint32_t zeros[NUM_BUCKETS] = {0, 0, 0, 0};
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_counters);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(zeros), zeros);

        // Reset indirect draw commands (set instanceCount = 0)
        struct DrawCmd { uint32_t count, instanceCount, first, baseInstance; };
        // We'll set the vertex/index counts from CPU (known per mesh)
        // instanceCount gets written by compute shader

        glUseProgram(compute_shader);

        // Bind SSBOs
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, ssbo_units);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, ssbo_instances);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, ssbo_indirect);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, ssbo_counters);

        // Bind heightmap
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, heightmap_tex);
        glUniform1i(glGetUniformLocation(compute_shader, "u_heightmap"), 0);

        // Uniforms
        glUniform1ui(glGetUniformLocation(compute_shader, "u_count"), unit_count);
        glUniform1f(glGetUniformLocation(compute_shader, "u_dt"), dt);
        glUniform3fv(glGetUniformLocation(compute_shader, "u_cam_pos"), 1, glm::value_ptr(cam_pos));
        glUniform1f(glGetUniformLocation(compute_shader, "u_lod_dist"), lod_dist);
        glUniform1f(glGetUniformLocation(compute_shader, "u_terrain_size"), terrain_size);
        glUniform4fv(glGetUniformLocation(compute_shader, "u_frustum"), 6, (float*)frustum_planes);
        glUniform1f(glGetUniformLocation(compute_shader, "u_time"), game_time);

        // Dispatch
        uint32_t groups = (unit_count + 255) / 256;
        glDispatchCompute(groups, 1, 1);

        // Memory barrier so instance data is visible for draw
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);
    }

    // Read back positions from GPU to CPU (needed for AI target finding)
    // Only read the batch we need for combat AI
    void readback_positions(World& world, uint32_t start, uint32_t count) {
        if (start + count > world.entity_count) count = world.entity_count - start;
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_units);
        UnitGPU* mapped = (UnitGPU*)glMapBufferRange(GL_SHADER_STORAGE_BUFFER,
            start * sizeof(UnitGPU), count * sizeof(UnitGPU), GL_MAP_READ_BIT);
        if (mapped) {
            for (uint32_t i = 0; i < count; i++) {
                uint32_t idx = start + i;
                world.transforms.position[idx] = mapped[i].position;
            }
            glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
        }
    }

    // Get instance count per bucket (for CPU to know how many were drawn)
    void get_instance_counts(uint32_t out[4]) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_counters);
        uint32_t* mapped = (uint32_t*)glMapBufferRange(GL_SHADER_STORAGE_BUFFER,
            0, NUM_BUCKETS * sizeof(uint32_t), GL_MAP_READ_BIT);
        if (mapped) {
            for (int i = 0; i < 4; i++) out[i] = mapped[i];
            glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
        }
    }

    GLuint get_instance_ssbo() const { return ssbo_instances; }

    // Full GPU combat + movement dispatch
    void dispatch_combat_movement(uint32_t unit_count, float dt, glm::vec2 faction_center[2], uint32_t faction_alive[2], uint32_t frame) {
        if (!combat_gpu_ready || unit_count == 0) return;
        uint32_t groups = (unit_count + 255) / 256;
        uint32_t cell_groups = (22500 + 255) / 256;

        // --- GPU timer: measure true GPU time of the 4 sim dispatches ---
        if (gpu_timer == 0) glGenQueries(1, &gpu_timer);
        if (timer_pending) {
            GLint avail = 0;
            glGetQueryObjectiv(gpu_timer, GL_QUERY_RESULT_AVAILABLE, &avail);
            if (avail) {
                GLuint64 ns = 0; glGetQueryObjectui64v(gpu_timer, GL_QUERY_RESULT, &ns);
                last_gpu_ms = ns / 1.0e6; timer_pending = false;
            }
        }
        bool do_time = !timer_pending;
        if (do_time) glBeginQuery(GL_TIME_ELAPSED, gpu_timer);
        // 1: Clear spatial hash
        glUseProgram(spatial_shader);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, ssbo_units);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, ssbo_cell_counts);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, ssbo_cell_entries);
        glUniform1ui(glGetUniformLocation(spatial_shader, "u_count"), unit_count);
        glUniform1ui(glGetUniformLocation(spatial_shader, "u_phase"), 0);
        glDispatchCompute(cell_groups, 1, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

        // 2: Insert into spatial hash
        glUniform1ui(glGetUniformLocation(spatial_shader, "u_phase"), 1);
        glDispatchCompute(groups, 1, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

        // 3: Combat AI (decisions only - no damage dealing)
        glUseProgram(combat_shader);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, ssbo_units);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, ssbo_cell_counts);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, ssbo_cell_entries);
        glUniform1ui(glGetUniformLocation(combat_shader, "u_count"), unit_count);
        glUniform1f(glGetUniformLocation(combat_shader, "u_dt"), dt);
        glUniform2f(glGetUniformLocation(combat_shader, "u_faction_center[0]"), faction_center[0].x, faction_center[0].y);
        glUniform2f(glGetUniformLocation(combat_shader, "u_faction_center[1]"), faction_center[1].x, faction_center[1].y);
        glUniform1ui(glGetUniformLocation(combat_shader, "u_faction_alive[0]"), faction_alive[0]);
        glUniform1ui(glGetUniformLocation(combat_shader, "u_faction_alive[1]"), faction_alive[1]);
        glUniform1ui(glGetUniformLocation(combat_shader, "u_frame"), frame);
        glDispatchCompute(groups, 1, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

        // 4: Movement
        glUseProgram(movement_shader);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, ssbo_units);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, ssbo_cell_counts);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, ssbo_cell_entries);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, ssbo_move_cmds);
        glUniform1ui(glGetUniformLocation(movement_shader, "u_count"), unit_count);
        glUniform1f(glGetUniformLocation(movement_shader, "u_dt"), dt);
        glUniform1ui(glGetUniformLocation(movement_shader, "u_frame"), frame);
        glDispatchCompute(groups, 1, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
        if (do_time) { glEndQuery(GL_TIME_ELAPSED); timer_pending = true; }

        // Async-copy the just-computed unit buffer into the ring head, then fence
        // THAT copy. Readback will later map this buffer once its fence signals.
        if (rb_inited) {
            glBindBuffer(GL_COPY_READ_BUFFER, ssbo_units);
            glBindBuffer(GL_COPY_WRITE_BUFFER, rb_buf[rb_head]);
            glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0, unit_count * 64);
            rb_count_at[rb_head] = unit_count;
            if (rb_fence[rb_head]) glDeleteSync(rb_fence[rb_head]);
            rb_fence[rb_head] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            rb_head = (rb_head + 1) % RB_COUNT;
        }
        if (combat_fence) glDeleteSync(combat_fence);
        combat_fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    }

    // Upload full combat-format data to GPU
    void upload_combat_data(const World& world) {
        if (!combat_gpu_ready) return;
        struct UC { glm::vec2 pos, vel; float rot,hp,dmg,range,spd; uint32_t faction,type,state,target; float cd,max_hp,p2; };
        // PERF: reuse a persistent staging buffer instead of allocating 3.2MB
        // every frame (resize only grows; no per-frame construction churn).
        static std::vector<UC> d;
        if (d.size() < world.entity_count) d.resize(world.entity_count);
        for (uint32_t i = 0; i < world.entity_count; i++) {
            d[i].pos = world.transforms.position[i];
            d[i].vel = world.transforms.velocity[i];
            d[i].rot = world.transforms.rotation[i];
            d[i].hp = world.units.health[i];
            d[i].dmg = world.units.attack_damage[i];
            d[i].range = world.units.attack_range[i];
            d[i].spd = world.units.speed[i];
            d[i].faction = (uint32_t)world.units.faction[i];
            d[i].type = (uint32_t)world.units.type[i];
            d[i].state = world.alive[i] ? (uint32_t)world.units.state[i] : 3u;
            d[i].target = world.units.target[i];
            d[i].cd = world.units.attack_cooldown[i];
            // Per-unit terrain speed: biome base * slope factor (uphill slow,
            // downhill fast). Direction comes from current velocity; idle units
            // (no heading) fall back to biome-only via a zero move dir.
            float tmult = 0.7f;
            if (terrain_ptr) {
                glm::vec2 v = world.transforms.velocity[i];
                glm::vec2 mdir(0.0f);
                float vl = std::sqrt(v.x*v.x + v.y*v.y);
                if (vl > 1e-4f) mdir = v / vl;
                tmult = terrain_ptr->get_speed_mult(d[i].pos.x, d[i].pos.y, mdir);
            }
            d[i].max_hp = world.units.max_health[i]; d[i].p2 = tmult;
        }
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_units);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, world.entity_count * sizeof(UC), d.data());
    }

    // Readback from GPU to CPU (for rendering, selection, territory)
    uint32_t readback_offset = 0;
    static constexpr uint32_t READBACK_BATCH = 20000;

    void readback_combat(World& world) {
        if (!combat_gpu_ready) return;
        struct UC { glm::vec2 pos, vel; float rot,hp,dmg,range,spd; uint32_t faction,type,state,target; float cd,max_hp,p2; };

        // Combat is a per-frame CPU<->GPU round-trip (hp/state flow through the CPU
        // and are re-uploaded), so the readback MUST see THIS frame's GPU result
        // or damage is overwritten and nobody dies. Wait on the dispatch fence
        // then map the live buffer. (Black-screen safety comes from the live-unit
        // cap + GPU-reset detection, not from delaying this readback.)
        if (combat_fence) glClientWaitSync(combat_fence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
        glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT | GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT);
        uint32_t n = world.entity_count;
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_units);
        UC* d = (UC*)glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0,
            n * sizeof(UC), GL_MAP_READ_BIT);
        if (!d) return;

        for (uint32_t i = 0; i < n; i++) {
            if (!world.alive[i]) continue;
            // Ragdoll/Dead units are simulated on the CPU (tick_corpses): their
            // knockback fly + spin must NOT be overwritten by the stale GPU pos,
            // otherwise nuke/cannon blasts show no ragdoll launch effect.
            if (world.units.state[i] == UnitState::Ragdoll ||
                world.units.state[i] == UnitState::Dead) {
                continue;
            }
            world.transforms.position[i] = d[i].pos;
            world.transforms.velocity[i] = d[i].vel;
            world.transforms.rotation[i] = d[i].rot;
            if (world.units.state[i] != UnitState::Dead && world.units.state[i] != UnitState::Ragdoll) {
                world.units.state[i] = (UnitState)d[i].state;
            }
            world.units.target[i] = d[i].target;
            world.units.attack_cooldown[i] = d[i].cd;
            if (d[i].state == 4u) {
                world.units.health[i] = d[i].hp;
            }
        }
        glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
    }

    // Upload player move commands
    void upload_move_commands(const World& world) {
        if (!combat_gpu_ready) return;
        struct MC { float tx, ty; uint32_t has; float pad; };
        std::vector<MC> cmds(world.entity_count);
        for (uint32_t i = 0; i < world.entity_count; i++) {
            cmds[i].tx = world.commands.move_target[i].x;
            cmds[i].ty = world.commands.move_target[i].y;
            cmds[i].has = world.commands.has_move_command[i] ? 1 : 0;
            cmds[i].pad = 0;
        }
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_move_cmds);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, world.entity_count * sizeof(MC), cmds.data());
    }


    // Upload per-unit color+scale to GPU (binding 8) - only when units created/destroyed
    void upload_colors(const World& world) {
        if (!gpu_instancing_ready) return;
        struct CS { float r, g, b, scale; };
        std::vector<CS> colors(world.entity_count);
        for (uint32_t i = 0; i < world.entity_count; i++) {
            colors[i].r = world.renders.color[i].r;
            colors[i].g = world.renders.color[i].g;
            colors[i].b = world.renders.color[i].b;
            colors[i].scale = world.renders.scale[i];
        }
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_colors);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, world.entity_count * sizeof(CS), colors.data());
    }

    // GPU-side instance generation: builds instance buffers from combat UnitData
    // Call after dispatch_combat_movement() each frame
    void dispatch_instances(uint32_t unit_count, glm::vec3 cam_pos,
                           float lod_dist, float terrain_size, const glm::vec4 frustum_planes[6],
                           float game_time) {
        if (!gpu_instancing_ready) return;

        // Reset counters to 0
        uint32_t zeros[NUM_BUCKETS] = {0, 0, 0, 0};
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_counters);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(zeros), zeros);

        glUseProgram(compute_shader);

        // Bind SSBOs
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, ssbo_units);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, ssbo_instances);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, ssbo_indirect);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, ssbo_counters);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 8, ssbo_colors);

        // Bind heightmap
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, heightmap_tex);
        glUniform1i(glGetUniformLocation(compute_shader, "u_heightmap"), 0);

        // Uniforms
        glUniform1ui(glGetUniformLocation(compute_shader, "u_count"), unit_count);
        glUniform3fv(glGetUniformLocation(compute_shader, "u_cam_pos"), 1, glm::value_ptr(cam_pos));
        glUniform1f(glGetUniformLocation(compute_shader, "u_lod_dist"), lod_dist);
        glUniform1f(glGetUniformLocation(compute_shader, "u_terrain_size"), terrain_size);
        glUniform4fv(glGetUniformLocation(compute_shader, "u_frustum"), 6, (float*)frustum_planes);
        glUniform1f(glGetUniformLocation(compute_shader, "u_time"), game_time);

        // Dispatch
        uint32_t groups = (unit_count + 255) / 256;
        glDispatchCompute(groups, 1, 1);

        // Barrier so instance data is visible for draw
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);
    }

    void cleanup() {
        if (compute_shader) glDeleteProgram(compute_shader);
        if (combat_shader) glDeleteProgram(combat_shader);
        if (movement_shader) glDeleteProgram(movement_shader);
        if (spatial_shader) glDeleteProgram(spatial_shader);
        if (ssbo_units) glDeleteBuffers(1, &ssbo_units);
        if (ssbo_cell_counts) glDeleteBuffers(1, &ssbo_cell_counts);
        if (ssbo_cell_entries) glDeleteBuffers(1, &ssbo_cell_entries);
        if (ssbo_damage) glDeleteBuffers(1, &ssbo_damage);
        if (ssbo_move_cmds) glDeleteBuffers(1, &ssbo_move_cmds);
        if (ssbo_colors) glDeleteBuffers(1, &ssbo_colors);
        if (ssbo_instances) glDeleteBuffers(1, &ssbo_instances);
        if (ssbo_indirect) glDeleteBuffers(1, &ssbo_indirect);
        if (ssbo_counters) glDeleteBuffers(1, &ssbo_counters);
        if (heightmap_tex) glDeleteTextures(1, &heightmap_tex);
    }

public:
    void upload_heightmap(const Terrain& terrain) {
        // Convert terrain heights to a R32F texture
        std::vector<float> data(Terrain::GRID_SIZE * Terrain::GRID_SIZE);
        for (int z = 0; z < Terrain::GRID_SIZE; z++)
            for (int x = 0; x < Terrain::GRID_SIZE; x++)
                data[z * Terrain::GRID_SIZE + x] = terrain.heights(z,x);

        if (!heightmap_tex) glGenTextures(1, &heightmap_tex);
        glBindTexture(GL_TEXTURE_2D, heightmap_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, Terrain::GRID_SIZE, Terrain::GRID_SIZE,
                     0, GL_RED, GL_FLOAT, data.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    std::string load_file(const std::string& path) {
        std::ifstream f(path);
        if (!f.is_open()) return "";
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    static float encode_state(const World& world, uint32_t i) {
        float s = 0.0f;
        auto st = world.units.state[i];
        if (st == UnitState::Moving || st == UnitState::Retreating) s = 1.0f;
        else if (st == UnitState::Attacking) s = 2.0f;
        else if (st == UnitState::Dead) s = 3.0f;
        if (world.units.hit_timer[i] > 0) s += 4.0f;
        return s;
    }
};
