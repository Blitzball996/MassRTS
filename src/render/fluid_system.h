#pragma once
// ============================================================================
// FLUID SYSTEM — height-field SHALLOW WATER via the virtual-pipe model
// (Mei et al. 2007, "Fast Hydraulic Erosion Simulation"). Implemented as a
// deterministic CPU solver on a 2D grid + GPU water-surface rendering.
//
// Why pipe-model SWE (not SPH/MPM particles): a 2D height grid with local
// update rules is cheap, rock-stable, and — crucially — deterministic and
// fixed-point-friendly, so multiplayer only needs to sync the trigger EVENTS
// (terrain edits, splash points), never the full field. At 256x256 = 65k cells
// a few passes per tick is trivial next to the 190k-unit sim.
//
// Each cell stores:
//   bed_height  b  — terrain elevation (sampled from Terrain, re-synced on edits)
//   water_height h — water column above the bed; surface = b + h
//   flux         f — outflow to the 4 neighbors (L,R,T,B) as a vec4
//   velocity     v — 2D, derived from net flux (drives foam / flow direction)
//
// Per tick, three passes:
//   1) flux:   each pipe accelerates by the surface-height difference (static
//              pressure), then ALL outflow is clamped so a cell can never lose
//              more water than it holds (the CFL-style stability guarantee).
//   2) height: h += dt * (inflow - outflow) / cell_area.
//   3) velocity: from net horizontal flux (for foam + advecting debris).
//
// Terrain destruction coupling: SDF carving rewrites bed_height in the affected
// cells; next tick the water responds on its own — craters fill into ponds,
// breached walls flood, sustained sources carve rivers.
// ============================================================================
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <vector>
#include <string>
#include <cmath>
#include "terrain.h"

class FluidSystem {
public:
    static constexpr int   GRID = 256;
    static constexpr float WORLD_SIZE = 6000.0f;            // matches Terrain
    static constexpr float CELL = WORLD_SIZE / (GRID - 1);
    static constexpr float GRAVITY = 9.81f;
    static constexpr float PIPE_AREA = CELL * CELL;         // virtual pipe cross-section
    static constexpr float MIN_WATER = 0.02f;               // below this a cell reads as dry

    // Host-side state (authoritative — solver runs on CPU for determinism).
    std::vector<float>     water_height;   // h
    std::vector<float>     bed_height;     // b (cached from terrain)
    std::vector<glm::vec4> flux;           // outflow L,R,T,B
    std::vector<glm::vec2> velocity;       // derived horizontal velocity

    const Terrain* terrain = nullptr;
    bool   enabled = false;   // skip all work until water exists
    double accum = 0.0;       // fixed-timestep accumulator
    static constexpr float FIXED_DT = 1.0f / 60.0f;

    // GPU render resources.
    GLuint vao = 0, vbo_pos = 0, vbo_dyn = 0, ebo = 0;
    GLuint shader = 0;
    int    index_count = 0;
    std::vector<float> dyn;   // per-vertex dynamic data (surfaceY, depth, vx, vz)

    inline size_t idx(int x, int z) const { return (size_t)z * GRID + x; }

    void init(const Terrain* t, GLuint water_shader) {
        terrain = t;
        shader = water_shader;
        size_t n = (size_t)GRID * GRID;
        water_height.assign(n, 0.0f);
        bed_height.assign(n, 0.0f);
        flux.assign(n, glm::vec4(0.0f));
        velocity.assign(n, glm::vec2(0.0f));
        refresh_bed();
        build_mesh();
        enabled = false;
    }

    void refresh_bed() {
        if (!terrain) return;
        for (int z = 0; z < GRID; ++z)
            for (int x = 0; x < GRID; ++x) {
                float wx = ((float)x / (GRID - 1) - 0.5f) * WORLD_SIZE;
                float wz = ((float)z / (GRID - 1) - 0.5f) * WORLD_SIZE;
                bed_height[idx(x, z)] = terrain->get_height_at(wx, wz);
            }
    }

    inline int clampi(int v, int lo, int hi) const { return v < lo ? lo : (v > hi ? hi : v); }

    // Re-sync only cells overlapping a world disc (after SDF carving). This is
    // the terrain-destruction coupling: bed changes -> water reacts next tick.
    void refresh_bed_region(float wx, float wz, float radius) {
        if (!terrain) return;
        int cx = (int)((wx / WORLD_SIZE + 0.5f) * (GRID - 1));
        int cz = (int)((wz / WORLD_SIZE + 0.5f) * (GRID - 1));
        int r = (int)(radius / CELL) + 2;
        for (int dz = -r; dz <= r; ++dz)
            for (int dx = -r; dx <= r; ++dx) {
                int gx = cx + dx, gz = cz + dz;
                if (gx < 0 || gx >= GRID || gz < 0 || gz >= GRID) continue;
                float worldx = ((float)gx / (GRID - 1) - 0.5f) * WORLD_SIZE;
                float worldz = ((float)gz / (GRID - 1) - 0.5f) * WORLD_SIZE;
                bed_height[idx(gx, gz)] = terrain->get_height_at(worldx, worldz);
            }
    }

    inline int cell_index(float wx, float wz) const {
        int gx = clampi((int)((wx / WORLD_SIZE + 0.5f) * (GRID - 1)), 0, GRID-1);
        int gz = clampi((int)((wz / WORLD_SIZE + 0.5f) * (GRID - 1)), 0, GRID-1);
        return gz * GRID + gx;
    }

    // Fill water up to a target world Y over a radius (lakes/rivers seeding).
    void add_water_pool(float wx, float wz, float radius, float target_y) {
        int r = (int)(radius / CELL) + 1;
        int cx = (int)((wx / WORLD_SIZE + 0.5f) * (GRID - 1));
        int cz = (int)((wz / WORLD_SIZE + 0.5f) * (GRID - 1));
        for (int dz = -r; dz <= r; ++dz)
            for (int dx = -r; dx <= r; ++dx) {
                int gx = cx + dx, gz = cz + dz;
                if (gx < 0 || gx >= GRID || gz < 0 || gz >= GRID) continue;
                if ((float)(dx*dx + dz*dz) * CELL * CELL > radius * radius) continue;
                size_t i = idx(gx, gz);
                float want = target_y - bed_height[i];
                if (want > water_height[i]) water_height[i] = want;
                if (water_height[i] < 0.0f) water_height[i] = 0.0f;
            }
        enabled = true;
    }

    // Continuous source/sink (rate>0 spring, rate<0 drain), meters/sec.
    void add_source(float wx, float wz, float rate) {
        size_t i = (size_t)cell_index(wx, wz);
        water_height[i] = glm::max(0.0f, water_height[i] + rate * FIXED_DT);
        if (rate > 0) enabled = true;
    }

    // Splash impulse at an explosion point (raises the column locally).
    void add_splash(float wx, float wz, float amount) {
        int cx = (int)((wx / WORLD_SIZE + 0.5f) * (GRID - 1));
        int cz = (int)((wz / WORLD_SIZE + 0.5f) * (GRID - 1));
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx) {
                int gx = clampi(cx+dx,0,GRID-1), gz = clampi(cz+dz,0,GRID-1);
                water_height[idx(gx,gz)] += amount * ((dx==0&&dz==0) ? 1.0f : 0.4f);
            }
        enabled = true;
    }

    float surface_at(float wx, float wz) const {
        size_t i = (size_t)cell_index(wx, wz);
        return bed_height[i] + water_height[i];
    }
    float depth_at(float wx, float wz) const {
        return water_height[(size_t)cell_index(wx, wz)];
    }

    // Flood every cell whose bed sits below sea_y up to sea_y. Use this once at
    // battle start to fill natural basins (lakes/rivers). offset is relative to
    // the terrain's minimum elevation so it scales with any map.
    void seed_sea_level(float offset_above_min) {
        float lo = 1e9f;
        for (float b : bed_height) lo = glm::min(lo, b);
        float sea = lo + offset_above_min;
        bool any = false;
        for (int z = 0; z < GRID; ++z)
            for (int x = 0; x < GRID; ++x) {
                size_t i = idx(x, z);
                if (bed_height[i] < sea) { water_height[i] = sea - bed_height[i]; any = true; }
            }
        if (any) { enabled = true; upload_dynamic(); }
    }

    // One pipe-model SWE tick (fixed dt). See header comment for the 3 passes.
    void step(float dt) {
        const float lf = GRAVITY * PIPE_AREA / CELL; // flux acceleration factor
        // --- Pass 1: flux update with CFL clamp ---
        for (int z = 0; z < GRID; ++z) {
            for (int x = 0; x < GRID; ++x) {
                size_t i = idx(x, z);
                float hi = water_height[i];
                float surf = bed_height[i] + hi;
                glm::vec4& f = flux[i];
                // neighbor surface heights (wall boundary = same surface -> no flow)
                float sL = (x>0)      ? bed_height[idx(x-1,z)] + water_height[idx(x-1,z)] : surf;
                float sR = (x<GRID-1) ? bed_height[idx(x+1,z)] + water_height[idx(x+1,z)] : surf;
                float sT = (z<GRID-1) ? bed_height[idx(x,z+1)] + water_height[idx(x,z+1)] : surf;
                float sB = (z>0)      ? bed_height[idx(x,z-1)] + water_height[idx(x,z-1)] : surf;
                f.x = glm::max(0.0f, f.x + dt * lf * (surf - sL));
                f.y = glm::max(0.0f, f.y + dt * lf * (surf - sR));
                f.z = glm::max(0.0f, f.z + dt * lf * (surf - sT));
                f.w = glm::max(0.0f, f.w + dt * lf * (surf - sB));
                // CFL clamp: never drain more than the cell holds this step.
                float out = f.x + f.y + f.z + f.w;
                if (out > 0.0f) {
                    float avail = hi * PIPE_AREA / dt;
                    float K = glm::min(1.0f, avail / out);
                    f *= K;
                }
            }
        }
        // --- Pass 2: height integration from net flux ---
        for (int z = 0; z < GRID; ++z) {
            for (int x = 0; x < GRID; ++x) {
                size_t i = idx(x, z);
                float in = 0.0f;
                if (x>0)      in += flux[idx(x-1,z)].y; // neighbor's R -> our L
                if (x<GRID-1) in += flux[idx(x+1,z)].x;
                if (z<GRID-1) in += flux[idx(x,z+1)].w;
                if (z>0)      in += flux[idx(x,z-1)].z;
                glm::vec4 f = flux[i];
                float out = f.x + f.y + f.z + f.w;
                float dV = dt * (in - out);
                water_height[i] = glm::max(0.0f, water_height[i] + dV / PIPE_AREA);
            }
        }
        // --- Pass 3: derive velocity from net horizontal flux (foam/advection) ---
        for (int z = 0; z < GRID; ++z) {
            for (int x = 0; x < GRID; ++x) {
                size_t i = idx(x, z);
                glm::vec4 f = flux[i];
                float inL = (x>0)      ? flux[idx(x-1,z)].y : 0.0f;
                float inR = (x<GRID-1) ? flux[idx(x+1,z)].x : 0.0f;
                float inT = (z<GRID-1) ? flux[idx(x,z+1)].w : 0.0f;
                float inB = (z>0)      ? flux[idx(x,z-1)].z : 0.0f;
                float wx = 0.5f * ((inL - f.x) + (f.y - inR));
                float wz = 0.5f * ((inB - f.w) + (f.z - inT));
                float h = glm::max(water_height[i], 0.05f);
                velocity[i] = glm::vec2(wx, wz) / (h * CELL);
            }
        }
    }

    // Fixed-timestep advance so the sim is deterministic regardless of frame rate.
    void update(float dt) {
        if (!enabled) return;
        accum += dt;
        int steps = 0;
        while (accum >= FIXED_DT && steps < 4) { step(FIXED_DT); accum -= FIXED_DT; steps++; }
        if (accum > FIXED_DT) accum = 0.0; // avoid spiral of death on hitches
        upload_dynamic();
    }

    // Build the static XZ grid (positions) + index buffer once. Per-frame we
    // only stream the dynamic attributes (surface Y, depth, velocity).
    void build_mesh() {
        std::vector<float> pos; pos.reserve((size_t)GRID*GRID*2);
        for (int z = 0; z < GRID; ++z)
            for (int x = 0; x < GRID; ++x) {
                pos.push_back(((float)x/(GRID-1) - 0.5f) * WORLD_SIZE);
                pos.push_back(((float)z/(GRID-1) - 0.5f) * WORLD_SIZE);
            }
        std::vector<unsigned int> ind; ind.reserve((size_t)(GRID-1)*(GRID-1)*6);
        for (int z = 0; z < GRID-1; ++z)
            for (int x = 0; x < GRID-1; ++x) {
                unsigned int a = z*GRID + x, b = a+1, c = a+GRID, d = c+1;
                ind.push_back(a); ind.push_back(c); ind.push_back(b);
                ind.push_back(b); ind.push_back(c); ind.push_back(d);
            }
        index_count = (int)ind.size();
        dyn.assign((size_t)GRID*GRID*4, 0.0f);

        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glGenBuffers(1, &vbo_pos);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_pos);
        glBufferData(GL_ARRAY_BUFFER, pos.size()*sizeof(float), pos.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        glEnableVertexAttribArray(0);
        glGenBuffers(1, &vbo_dyn);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_dyn);
        glBufferData(GL_ARRAY_BUFFER, dyn.size()*sizeof(float), nullptr, GL_DYNAMIC_DRAW);
        // location 1: surfaceY, location 2: depth, location 3: velocity.xy
        glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)0);                 glEnableVertexAttribArray(1);
        glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)(1*sizeof(float))); glEnableVertexAttribArray(2);
        glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)(2*sizeof(float))); glEnableVertexAttribArray(3);
        glGenBuffers(1, &ebo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, ind.size()*sizeof(unsigned int), ind.data(), GL_STATIC_DRAW);
        glBindVertexArray(0);
    }

    void upload_dynamic() {
        if (!vbo_dyn) return;
        for (int z = 0; z < GRID; ++z)
            for (int x = 0; x < GRID; ++x) {
                size_t i = idx(x, z);
                size_t o = i * 4;
                dyn[o+0] = bed_height[i] + water_height[i];
                dyn[o+1] = water_height[i];
                dyn[o+2] = velocity[i].x;
                dyn[o+3] = velocity[i].y;
            }
        glBindBuffer(GL_ARRAY_BUFFER, vbo_dyn);
        glBufferSubData(GL_ARRAY_BUFFER, 0, dyn.size()*sizeof(float), dyn.data());
    }

    void render(const glm::mat4& view, const glm::mat4& proj, glm::vec3 cam_pos, float time) {
        if (!enabled || !shader || index_count == 0) return;
        glUseProgram(shader);
        glUniformMatrix4fv(glGetUniformLocation(shader,"u_view"),1,GL_FALSE,&view[0][0]);
        glUniformMatrix4fv(glGetUniformLocation(shader,"u_proj"),1,GL_FALSE,&proj[0][0]);
        glUniform3f(glGetUniformLocation(shader,"u_cam_pos"), cam_pos.x, cam_pos.y, cam_pos.z);
        glUniform1f(glGetUniformLocation(shader,"u_time"), time);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE); // transparent surface: test depth, don't write
        glBindVertexArray(vao);
        glDrawElements(GL_TRIANGLES, index_count, GL_UNSIGNED_INT, nullptr);
        glBindVertexArray(0);
        glDepthMask(GL_TRUE);
    }

    void cleanup() {
        if (vao) glDeleteVertexArrays(1, &vao);
        if (vbo_pos) glDeleteBuffers(1, &vbo_pos);
        if (vbo_dyn) glDeleteBuffers(1, &vbo_dyn);
        if (ebo) glDeleteBuffers(1, &ebo);
    }
};
