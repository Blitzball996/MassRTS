#pragma once
// ============================================================================
// FLUID SYSTEM  (Plan A scaffold: GPU Shallow-Water Equations on a height grid)
//
// Status: PREP / SKELETON. This sets up the data layout, host API, and the
// GPU-compute dispatch points so the SWE solver and water-surface rendering can
// be filled in incrementally. It deliberately does NOT yet solve the equations
// every frame -- update() is a no-op until the compute shaders are wired.
//
// Design (see PROJECT_PROGRESS.md sec 4):
//   - 2D height-field water column `h` over the terrain bed `b` (bed = terrain
//     height). Total surface = b + h. Solve SWE for (h, hu, hv) on the GPU.
//   - Terrain edits (carve/raise) update the bed; water redistributes naturally
//     next step. Explosions inject an impulse into `h` (a splash pulse).
//   - Deterministic: same inputs -> same result, so multiplayer syncs only the
//     trigger EVENTS (explosion point, terrain delta), never the full field.
// ============================================================================
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <vector>
#include <string>
#include "terrain.h"

class FluidSystem {
public:
    // Simulation grid resolution. Coarser than the terrain heightmap on purpose
    // (SWE stays stable + cheap); it samples the terrain bed by bilinear lookup.
    static constexpr int   GRID = 256;
    static constexpr float WORLD_SIZE = 6000.0f;            // matches Terrain
    static constexpr float CELL = WORLD_SIZE / (GRID - 1);

    // Host-side mirror of the water column (meters of water above the bed).
    // Kept for spawning, debug readback, and CPU-side queries; the authoritative
    // state lives in GPU buffers once the solver is online.
    std::vector<float> water_height;   // h
    std::vector<float> bed_height;     // b (cached from terrain)

    const Terrain* terrain = nullptr;
    bool gpu_ready = false;

    // GPU state buffers (ping-pong for the explicit SWE integrator).
    GLuint ssbo_state[2] = {0, 0};     // packed (h, hu, hv, _) per cell
    GLuint ssbo_bed = 0;               // bed height per cell
    GLuint swe_program = 0;            // compute shader (filled in later)
    int    cur = 0;                    // which ssbo_state is current

    // ---- Host API (stable; solver fills in behind these) -------------------

    void init(const Terrain* t) {
        terrain = t;
        water_height.assign((size_t)GRID * GRID, 0.0f);
        bed_height.assign((size_t)GRID * GRID, 0.0f);
        refresh_bed();
        // GPU buffer allocation + compute shader load happen here once the SWE
        // kernel is authored. Left unset so the rest of the engine can link
        // against FluidSystem today without a solver.
        gpu_ready = false;
    }

    // Cache the terrain bed elevation into bed_height (call after terrain edits).
    void refresh_bed() {
        if (!terrain) return;
        for (int z = 0; z < GRID; ++z)
            for (int x = 0; x < GRID; ++x) {
                float wx = ((float)x / (GRID - 1) - 0.5f) * WORLD_SIZE;
                float wz = ((float)z / (GRID - 1) - 0.5f) * WORLD_SIZE;
                bed_height[(size_t)z * GRID + x] = terrain->get_height_at(wx, wz);
            }
    }

    // World (x,z) -> grid cell (clamped). Shared by spawn/splash/query.
    inline int cell_index(float wx, float wz) const {
        int gx = (int)glm::clamp((wx / WORLD_SIZE + 0.5f) * (GRID - 1), 0.0f, (float)(GRID - 1));
        int gz = (int)glm::clamp((wz / WORLD_SIZE + 0.5f) * (GRID - 1), 0.0f, (float)(GRID - 1));
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
                size_t i = (size_t)gz * GRID + gx;
                water_height[i] = glm::max(0.0f, target_y - bed_height[i]);
            }
    }

    // Inject a splash impulse at an explosion point (raises the column locally).
    void add_splash(float wx, float wz, float amount) {
        size_t i = (size_t)cell_index(wx, wz);
        water_height[i] += amount;
    }

    // Advance the SWE solver. No-op until the compute kernel is wired; kept so
    // callers can integrate the per-frame call site now.
    void update(float /*dt*/) {
        if (!gpu_ready) return;
        // TODO: ping-pong dispatch swe_program (flux + height integration),
        // apply bed coupling, then cur ^= 1.
    }

    void cleanup() {
        if (ssbo_state[0]) glDeleteBuffers(2, ssbo_state);
        if (ssbo_bed)      glDeleteBuffers(1, &ssbo_bed);
        if (swe_program)   glDeleteProgram(swe_program);
    }
};
