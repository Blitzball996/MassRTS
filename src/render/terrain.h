#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <vector>
#include <cmath>
#include <random>

enum class TerrainBiome : uint8_t { Grass=0, Mountain=1, Swamp=2, Forest=3, Trench=4, River=5, Sand=6 };

class Terrain {
public:
    static constexpr int GRID_SIZE = 512;
    static constexpr float WORLD_SIZE = 6000.0f;
    static constexpr float MAX_HEIGHT = 80.0f;
    static constexpr float CELL_SIZE = WORLD_SIZE / (GRID_SIZE - 1);

    // Map preset parameters (modifiable before generate)
    float height_scale = 1.0f;
    float flatten_radius = 350.0f;
    float mountain_edge = 700.0f;
    bool has_river = true;
    int num_trenches = 3;
    float actual_world_size = WORLD_SIZE;
    GLuint vao = 0, vbo = 0, ebo = 0;
    int index_count = 0;
    // Heap-allocated to avoid stack overflow (512*512*sizeof(float)=~1MB each)
    float* heights_flat = nullptr;
    TerrainBiome* biomes_flat = nullptr;
    glm::vec3* normals_flat = nullptr;
    uint32_t seed = 0;

    Terrain() {
        heights_flat = new float[GRID_SIZE * GRID_SIZE]();
        biomes_flat = new TerrainBiome[GRID_SIZE * GRID_SIZE]();
        normals_flat = new glm::vec3[GRID_SIZE * GRID_SIZE]();
    }
    ~Terrain() {
        delete[] heights_flat;
        delete[] biomes_flat;
        delete[] normals_flat;
    }
    // Access helpers
    float& heights(int z, int x) { return heights_flat[z * GRID_SIZE + x]; }
    float heights(int z, int x) const { return heights_flat[z * GRID_SIZE + x]; }
    TerrainBiome& biomes(int z, int x) { return biomes_flat[z * GRID_SIZE + x]; }
    TerrainBiome biomes(int z, int x) const { return biomes_flat[z * GRID_SIZE + x]; }
    glm::vec3& normals(int z, int x) { return normals_flat[z * GRID_SIZE + x]; }
    glm::vec3 normals(int z, int x) const { return normals_flat[z * GRID_SIZE + x]; }

    void generate() {
        generate_with_seed((uint32_t)time(nullptr));
    }

    void apply_preset(int preset_idx) {
        if (preset_idx < 0 || preset_idx >= 6) return;
        // Map presets defined inline
        struct MP { float hs,fr,me; bool river; int trench; };
        static const MP presets[] = {
            {0.3f, 500.0f, 1200.0f, false, 0},
            {0.5f, 300.0f, 900.0f, true, 2},
            {1.2f, 200.0f, 500.0f, false, 4},
            {0.15f, 800.0f, 1400.0f, false, 0},
            {0.6f, 250.0f, 800.0f, true, 1},
            {0.4f, 150.0f, 1000.0f, false, 6},
        };
        height_scale = presets[preset_idx].hs;
        flatten_radius = presets[preset_idx].fr;
        mountain_edge = presets[preset_idx].me;
        has_river = presets[preset_idx].river;
        num_trenches = presets[preset_idx].trench;
    }

    void generate_with_seed(uint32_t s) {
        seed = s;
        std::mt19937 rng(seed);
        float offset_x = std::uniform_real_distribution<float>(0, 1000)(rng);
        float offset_z = std::uniform_real_distribution<float>(0, 1000)(rng);

        // Generate heightmap
        for (int z = 0; z < GRID_SIZE; z++) {
            for (int x = 0; x < GRID_SIZE; x++) {
                float wx = ((float)x / (GRID_SIZE-1) - 0.5f) * WORLD_SIZE;
                float wz = ((float)z / (GRID_SIZE-1) - 0.5f) * WORLD_SIZE;
                heights(z,x) = sample_height(wx + offset_x, wz + offset_z, wx, wz);
            }
        }

        // Carve trenches (preset count) BEFORE the smoothing pass so their
        // walls get rounded along with the rest of the terrain.
        carve_trenches(rng);

        // Apply height_scale from preset
        for (int z = 0; z < GRID_SIZE; z++)
            for (int x = 0; x < GRID_SIZE; x++)
                heights(z,x) *= height_scale;

        // Smoothing pass (5x) - eliminates jagged edges. Extra passes tame the
        // steepest ridged-noise peaks so the 3-unit-voxel Marching Cubes mesh
        // stops shattering into spikes on near-vertical cliffs.
        for (int pass = 0; pass < 5; pass++) {
            for (int z = 1; z < GRID_SIZE-1; z++) {
                for (int x = 1; x < GRID_SIZE-1; x++) {
                    float avg = (heights(z-1,x)+heights(z+1,x)+heights(z,x-1)+heights(z,x+1))*0.25f;
                    heights(z,x) = glm::mix(heights(z,x), avg, 0.4f);
                }
            }
        }

        // Carve the river LAST, as a smooth distance-to-spline channel. Running
        // it AFTER the smoothing pass means smoothing can never re-sharpen or
        // wash out the banks: the cross-section is C1-continuous by construction,
        // so the riverbank stays smooth at any heightfield/voxel resolution
        // (root-cause fix for the stair-stepped "folds" along the water edge).
        if (has_river) carve_river(rng);

        // Calculate normals
        for (int z = 0; z < GRID_SIZE; z++) {
            for (int x = 0; x < GRID_SIZE; x++) {
                float hL = get_h(x-1,z), hR = get_h(x+1,z);
                float hD = get_h(x,z-1), hU = get_h(x,z+1);
                normals(z,x) = glm::normalize(glm::vec3(hL-hR, 2.0f*CELL_SIZE, hD-hU));
            }
        }

        // Assign biomes
        for (int z = 0; z < GRID_SIZE; z++) {
            for (int x = 0; x < GRID_SIZE; x++) {
                float wx = ((float)x/(GRID_SIZE-1)-0.5f)*WORLD_SIZE;
                float wz = ((float)z/(GRID_SIZE-1)-0.5f)*WORLD_SIZE;
                if (biomes(z,x) != TerrainBiome::River && biomes(z,x) != TerrainBiome::Trench)
                    biomes(z,x) = sample_biome(wx+offset_x, wz+offset_z, wx, wz, heights(z,x), normals(z,x));
            }
        }

        // CPU vertex assembly only. The GL upload (upload_mesh_gl) is deferred
        // so generate_with_seed can run on a background loading thread without
        // touching the OpenGL context (which would freeze the app).
        build_mesh_cpu();
    }

    float get_height_at(float wx, float wz) const {
        float fx = (wx / WORLD_SIZE + 0.5f) * (GRID_SIZE-1);
        float fz = (wz / WORLD_SIZE + 0.5f) * (GRID_SIZE-1);
        int ix = (int)fx, iz = (int)fz;
        if (ix < 0) ix = 0; if (ix >= GRID_SIZE-1) ix = GRID_SIZE-2;
        if (iz < 0) iz = 0; if (iz >= GRID_SIZE-1) iz = GRID_SIZE-2;
        float tx = fx-ix, tz = fz-iz;
        float h00=heights(iz,ix), h10=heights(iz,ix+1);
        float h01=heights(iz+1,ix), h11=heights(iz+1,ix+1);
        return (h00*(1-tx)+h10*tx)*(1-tz) + (h01*(1-tx)+h11*tx)*tz;
    }

    glm::vec3 get_normal_at(float wx, float wz) const {
        float fx = (wx / WORLD_SIZE + 0.5f) * (GRID_SIZE-1);
        float fz = (wz / WORLD_SIZE + 0.5f) * (GRID_SIZE-1);
        int ix = glm::clamp((int)fx, 0, GRID_SIZE-1);
        int iz = glm::clamp((int)fz, 0, GRID_SIZE-1);
        return normals(iz,ix);
    }

    TerrainBiome get_biome_at(float wx, float wz) const {
        float fx = (wx / WORLD_SIZE + 0.5f) * (GRID_SIZE-1);
        float fz = (wz / WORLD_SIZE + 0.5f) * (GRID_SIZE-1);
        int ix = glm::clamp((int)fx, 0, GRID_SIZE-1);
        int iz = glm::clamp((int)fz, 0, GRID_SIZE-1);
        return biomes(iz,ix);
    }

    // Slope-based speed: uphill slow, downhill fast
    float get_speed_mult(float wx, float wz, glm::vec2 move_dir) const {
        TerrainBiome b = get_biome_at(wx, wz);
        float base = 1.0f;
        switch(b) {
            case TerrainBiome::Swamp: base = 0.45f; break;
            case TerrainBiome::Forest: base = 0.7f; break;
            case TerrainBiome::Mountain: base = 0.55f; break;
            case TerrainBiome::Trench: base = 0.8f; break;
            case TerrainBiome::River: base = 0.35f; break; // swimming
            case TerrainBiome::Sand: base = 0.8f; break;
            default: base = 1.0f; break;
        }

        // Slope factor: project move direction onto terrain normal XZ.
        glm::vec3 n = get_normal_at(wx, wz);
        float slope_dot = -(n.x * move_dir.x + n.z * move_dir.y); // positive = uphill
        // Uphill is now a real tactical cost: climbing a steep face drops to
        // ~35% speed, while charging downhill gives up to +30%. The stronger
        // multiplier (1.5) plus a lower floor makes holding the high ground
        // meaningful instead of a tiny nudge.
        float slope_factor = 1.0f - slope_dot * 1.5f;
        slope_factor = glm::clamp(slope_factor, 0.35f, 1.3f);

        return base * slope_factor;
    }

    bool is_water(float wx, float wz) const {
        return get_biome_at(wx, wz) == TerrainBiome::River;
    }

    void render() {
        glBindVertexArray(vao);
        glDrawElements(GL_TRIANGLES, index_count, GL_UNSIGNED_INT, 0);
        glBindVertexArray(0);
    }

    void cleanup() {
        glDeleteVertexArrays(1, &vao);
        glDeleteBuffers(1, &vbo);
        glDeleteBuffers(1, &ebo);
    }

private:
    float get_h(int x, int z) const {
        x = glm::clamp(x, 0, GRID_SIZE-1);
        z = glm::clamp(z, 0, GRID_SIZE-1);
        return heights(z,x);
    }

    // === Noise functions ===
    static float hash2(float x, float y) { return fract(sin(x*127.1f+y*311.7f)*43758.5453f); }
    static float fract(float x) { return x - floor(x); }

    static float value_noise(float x, float y) {
        float ix = floor(x), iy = floor(y);
        float fx = x-ix, fy = y-iy;
        fx = fx*fx*(3-2*fx); fy = fy*fy*(3-2*fy);
        float a=hash2(ix,iy), b=hash2(ix+1,iy), c=hash2(ix,iy+1), d=hash2(ix+1,iy+1);
        return (a*(1-fx)+b*fx)*(1-fy) + (c*(1-fx)+d*fx)*fy;
    }

    static float perlin_noise(float x, float y) {
        float ix=floor(x), iy=floor(y), fx=x-ix, fy=y-iy;
        auto grad = [](float h, float dx, float dy) {
            int i = (int)(h*16.0f) & 3;
            float u = (i<2)?dx:dy, v = (i<2)?dy:dx;
            return ((i&1)?-u:u) + ((i&2)?-v:v);
        };
        float sx=fx*fx*(3-2*fx), sy=fy*fy*(3-2*fy);
        float n00=grad(hash2(ix,iy),fx,fy), n10=grad(hash2(ix+1,iy),fx-1,fy);
        float n01=grad(hash2(ix,iy+1),fx,fy-1), n11=grad(hash2(ix+1,iy+1),fx-1,fy-1);
        return ((n00*(1-sx)+n10*sx)*(1-sy) + (n01*(1-sx)+n11*sx)*sy) * 0.5f + 0.5f;
    }

    static float fbm(float x, float y, int oct) {
        float v=0, a=1.0f, f=1.0f, t=0;
        for (int i=0; i<oct; i++) { v+=perlin_noise(x*f,y*f)*a; t+=a; a*=0.5f; f*=2.0f; }
        return v/t;
    }

    static float ridged(float x, float y, int oct) {
        float v=0, a=1.0f, f=1.0f, t=0;
        for (int i=0; i<oct; i++) {
            float n = 1.0f - fabs(perlin_noise(x*f,y*f)*2.0f - 1.0f);
            v += n*n*a; t+=a; a*=0.5f; f*=2.1f;
        }
        return v/t;
    }

    static float sample_height(float nx, float nz, float wx, float wz) {
        float h = 0;
        // Base terrain: broad rolling hills
        h += fbm(nx*0.0008f, nz*0.0008f, 6) * MAX_HEIGHT * 0.55f;
        // Medium detail: knolls and dips
        h += fbm(nx*0.003f+3.7f, nz*0.003f+1.2f, 4) * MAX_HEIGHT * 0.30f;
        // Fine relief: undulating ground so it never reads as a flat plane
        h += fbm(nx*0.011f+9.3f, nz*0.011f+4.6f, 3) * MAX_HEIGHT * 0.10f;

        // Mountains on edges (ridged noise) - taller, starts a bit closer
        float edge = glm::max(fabs(wx), fabs(wz));
        float mtn_mask = glm::clamp((edge - 650.0f) / 450.0f, 0.0f, 1.0f);
        h += ridged(nx*0.002f+7.0f, nz*0.002f+5.0f, 5) * MAX_HEIGHT * 1.8f * mtn_mask;
        // Foothills leading up to the mountains
        float foot = glm::clamp((edge - 450.0f) / 300.0f, 0.0f, 1.0f);
        h += ridged(nx*0.005f+2.0f, nz*0.005f+8.0f, 3) * MAX_HEIGHT * 0.35f * foot;

        // Keep the central battlefield gently rolling (not dead flat, not steep):
        // blend toward a soft local mean instead of crushing all relief to zero.
        float center_dist = sqrt(wx*wx + wz*wz);
        float flatten = glm::clamp(1.0f - center_dist/320.0f, 0.0f, 1.0f) * 0.45f;
        float gentle = fbm(nx*0.0015f+5.5f, nz*0.0015f+2.2f, 4) * MAX_HEIGHT * 0.18f;
        h = h * (1.0f - flatten) + gentle * flatten;

        h += 2.0f; // minimum floor above 0

        return h;
    }

    static TerrainBiome sample_biome(float nx, float nz, float wx, float wz, float height, glm::vec3 normal) {
        float slope = 1.0f - normal.y;
        if (height > MAX_HEIGHT * 0.6f && slope > 0.15f) return TerrainBiome::Mountain;
        if (height > MAX_HEIGHT * 0.7f) return TerrainBiome::Mountain;

        // Swamp in low flat areas. Jitter the thresholds with noise so the
        // swamp boundary is organic rather than a hard grid-aligned step (the
        // hard cutoff produced the jagged "folds" at the swamp edge).
        float sjit = (fbm(nx*0.06f+7.0f, nz*0.06f+11.0f, 4) - 0.5f);
        float h_thr = 8.0f + sjit * 4.0f;
        float s_thr = 0.10f + sjit * 0.05f;
        if (height < h_thr && slope < s_thr) return TerrainBiome::Swamp;

        // Forest patches (noise-driven)
        float fnoise = fbm(nx*0.003f+20.0f, nz*0.003f+30.0f, 3);
        float center_dist = sqrt(wx*wx + wz*wz);
        if (fnoise > 0.58f && height > 8.0f && height < MAX_HEIGHT*0.45f && center_dist > 200.0f)
            return TerrainBiome::Forest;

        // Sandy areas near rivers (will be set after river carving)
        if (height < 5.0f) return TerrainBiome::Sand;

        return TerrainBiome::Grass;
    }

    void carve_river(std::mt19937& rng) {
        // Smooth river carved by DISTANCE TO A SPLINE CENTRELINE (kkk.txt fix).
        // The centreline is a 1-D fbm (sum of sines) that wanders organically;
        // the cross-section is one continuous smoothstep profile of distance-to-
        // centre: deep bed -> rising banks -> existing ground, all C1-continuous.
        // Because the cut is a continuous FUNCTION of position (not a per-cell
        // "is this cell water?" boolean), the banks never quantise into grid
        // stair-steps no matter how coarse the heightfield / voxel grid is.
        float river_z = std::uniform_real_distribution<float>(-150, 150)(rng);
        float ph0 = std::uniform_real_distribution<float>(0, 6.28f)(rng);
        float ph1 = std::uniform_real_distribution<float>(0, 6.28f)(rng);
        float ph2 = std::uniform_real_distribution<float>(0, 6.28f)(rng);
        float ph3 = std::uniform_real_distribution<float>(0, 6.28f)(rng);
        float amp = std::uniform_real_distribution<float>(90, 170)(rng);

        const float WATER_Y   = 2.5f;   // channel-top / nominal water level
        const float BED_DROP  = 3.0f;   // depth of the deepest bed below WATER_Y
        const float BANK_SPAN = 55.0f;  // wide, smooth bank transition distance

        for (int x = 0; x < GRID_SIZE; x++) {
            float wx = ((float)x/(GRID_SIZE-1)-0.5f)*WORLD_SIZE;
            // fbm-style meander: large slow bends + medium kinks + small wiggle.
            float meander =
                  sin(wx*0.0018f + ph0) * amp
                + sin(wx*0.0041f + ph1) * amp*0.45f
                + sin(wx*0.0093f + ph2) * amp*0.22f
                + sin(wx*0.0200f + ph3) * amp*0.10f;
            float river_center_z = river_z + meander;
            // Half-width of the flat bed; breathes along the course.
            float half_width = 22.0f
                + sin(wx*0.006f + ph2)*8.0f
                + sin(wx*0.017f + ph1)*4.0f;

            for (int z = 0; z < GRID_SIZE; z++) {
                float wz = ((float)z/(GRID_SIZE-1)-0.5f)*WORLD_SIZE;
                // Continuous organic edge wobble (smooth noise, never grid-snapped)
                // so the shoreline meanders instead of tracing the centreline.
                float wobble = (fbm(wx*0.05f, wz*0.05f, 4) - 0.5f) * 14.0f
                             + (fbm(wx*0.18f, wz*0.18f, 3) - 0.5f) * 5.0f;
                float d = fabs(wz - river_center_z) + wobble; // distance to centre

                // Bed profile: deepest at the centre, easing up to WATER_Y by the
                // edge of the flat bed.
                float bedT = glm::smoothstep(0.0f, half_width, d); // 0 centre -> 1 bed edge
                float bed  = WATER_Y - BED_DROP * (1.0f - bedT);

                // Bank blend: 0 inside the channel, 1 once BANK_SPAN past the bed
                // edge. Eases the carved bed back into the existing ground over a
                // wide, smooth band (this is what kills the angular folds).
                float bank   = glm::smoothstep(half_width, half_width + BANK_SPAN, d);
                float carved = glm::mix(bed, heights(z,x), bank);

                // A river only ever cuts DOWN; where the ground already sits below
                // the carved profile, leave it (the crossover is where
                // carved==ground, so the surface stays smooth there too).
                if (carved < heights(z,x)) heights(z,x) = carved;

                // Biome is for shading / unit speed / water queries ONLY. The mesh
                // is shaped by the smooth height profile above, NOT by this flag,
                // so a per-cell biome boundary no longer drives the visible edge.
                if (d < half_width) biomes(z,x) = TerrainBiome::River;
                else if (bank < 0.5f && biomes(z,x) == TerrainBiome::Grass)
                    biomes(z,x) = TerrainBiome::Sand;
            }
        }
    }

    void carve_trenches(std::mt19937& rng) {
        int nt = num_trenches > 0 ? num_trenches : std::uniform_int_distribution<int>(2, 4)(rng);
        for (int t = 0; t < nt; t++) {
            float tz = std::uniform_real_distribution<float>(-300, 300)(rng);
            float tx_start = std::uniform_real_distribution<float>(-400, -100)(rng);
            float tx_end = std::uniform_real_distribution<float>(100, 400)(rng);
            float width = 12.0f;
            float depth = 3.5f;

            for (int z = 0; z < GRID_SIZE; z++) {
                for (int x = 0; x < GRID_SIZE; x++) {
                    float wx = ((float)x/(GRID_SIZE-1)-0.5f)*WORLD_SIZE;
                    float wz = ((float)z/(GRID_SIZE-1)-0.5f)*WORLD_SIZE;
                    if (wx < tx_start || wx > tx_end) continue;
                    float zig = sin(wx*0.02f) * 20.0f;
                    float dist = fabs(wz - (tz + zig));
                    if (dist < width) {
                        float d = (1.0f - dist/width) * depth;
                        heights(z,x) -= d;
                        biomes(z,x) = TerrainBiome::Trench;
                    }
                }
            }
        }
    }


public:
    // ====================================================================
    // SCULPT SYSTEM - smooth, money-driven terrain editing.
    // Brushes modify the heightmap with a smooth radial falloff, recompute
    // normals locally, then re-upload ONLY the touched vertex rows via
    // glBufferSubData (no full rebuild) so editing is real-time.
    // ====================================================================
    enum class Brush { Raise, Dig, Smooth, Flatten };

    // Apply a brush centered at world (wx,wz). `radius` in world units,
    // `strength` in world height units (per call). Returns the number of
    // vertices changed (caller uses it to charge money).
    int sculpt(float wx, float wz, float radius, float strength, Brush brush) {
        if (!heights_flat || !normals_flat) return 0;
        // brush center in grid space
        float gcx = (wx / WORLD_SIZE + 0.5f) * (GRID_SIZE - 1);
        float gcz = (wz / WORLD_SIZE + 0.5f) * (GRID_SIZE - 1);
        float grad = radius / CELL_SIZE;
        int x0 = (int)floorf(gcx - grad) - 1, x1 = (int)ceilf(gcx + grad) + 1;
        int z0 = (int)floorf(gcz - grad) - 1, z1 = (int)ceilf(gcz + grad) + 1;
        if (x0 < 0) x0 = 0; if (z0 < 0) z0 = 0;
        if (x1 > GRID_SIZE-1) x1 = GRID_SIZE-1;
        if (z1 > GRID_SIZE-1) z1 = GRID_SIZE-1;
        if (x0 > x1 || z0 > z1) return 0;

        // For Flatten we target the height at the brush center.
        float target = get_height_at(wx, wz);
        int changed = 0;

        for (int z = z0; z <= z1; z++) {
            for (int x = x0; x <= x1; x++) {
                float dx = (x - gcx) * CELL_SIZE;
                float dz = (z - gcz) * CELL_SIZE;
                float d = sqrtf(dx*dx + dz*dz);
                if (d > radius) continue;
                // smooth falloff: 1 at center -> 0 at edge (cosine bell)
                float f = 0.5f * (1.0f + cosf(3.14159265f * (d / radius)));
                float& h = heights(z, x);
                switch (brush) {
                    case Brush::Raise:   h += strength * f; break;
                    case Brush::Dig:     h -= strength * f; break;
                    case Brush::Flatten: h += (target - h) * f * 0.5f; break;
                    case Brush::Smooth: {
                        float avg = h, cnt = 1.0f;
                        if (x>0){avg+=heights(z,x-1);cnt++;}
                        if (x<GRID_SIZE-1){avg+=heights(z,x+1);cnt++;}
                        if (z>0){avg+=heights(z-1,x);cnt++;}
                        if (z<GRID_SIZE-1){avg+=heights(z+1,x);cnt++;}
                        avg/=cnt; h += (avg - h) * f; break;
                    }
                }
                if (h < -60.0f) h = -60.0f;
                if (h > MAX_HEIGHT*2.2f) h = MAX_HEIGHT*2.2f;
                changed++;
            }
        }
        // Recompute normals over a 1-cell-expanded region (normals use neighbors)
        int nx0 = x0>0?x0-1:0, nz0 = z0>0?z0-1:0;
        int nx1 = x1<GRID_SIZE-1?x1+1:x1, nz1 = z1<GRID_SIZE-1?z1+1:z1;
        for (int z = nz0; z <= nz1; z++)
            for (int x = nx0; x <= nx1; x++) {
                float hL = heights(z, x>0?x-1:x);
                float hR = heights(z, x<GRID_SIZE-1?x+1:x);
                float hD = heights(z>0?z-1:z, x);
                float hU = heights(z<GRID_SIZE-1?z+1:z, x);
                normals(z,x) = glm::normalize(glm::vec3(hL-hR, 2.0f*CELL_SIZE, hD-hU));
            }
        reupload_rows(nz0, nz1);
        return changed;
    }

    // Re-upload vertex rows [z0..z1] to the GPU (pos.y, normal, height_norm change).
    void reupload_rows(int z0, int z1) {
        if (!vbo) return;
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        std::vector<float> row(GRID_SIZE * 10);
        for (int z = z0; z <= z1; z++) {
            for (int x = 0; x < GRID_SIZE; x++) {
                float wx = ((float)x/(GRID_SIZE-1)-0.5f)*WORLD_SIZE;
                float wz = ((float)z/(GRID_SIZE-1)-0.5f)*WORLD_SIZE;
                glm::vec3 n = normals(z,x);
                float u=(float)x/(GRID_SIZE-1), v=(float)z/(GRID_SIZE-1);
                float* o = &row[x*10];
                o[0]=wx; o[1]=heights(z,x); o[2]=wz;
                o[3]=n.x; o[4]=n.y; o[5]=n.z;
                o[6]=u; o[7]=v; o[8]=(float)(uint8_t)biomes(z,x); o[9]=heights(z,x)/MAX_HEIGHT;
            }
            GLintptr off = (GLintptr)z * GRID_SIZE * 10 * sizeof(float);
            glBufferSubData(GL_ARRAY_BUFFER, off, GRID_SIZE*10*sizeof(float), row.data());
        }
    }

    // CPU vertex/index assembly. Safe on a worker thread (no GL). Stores into
    // member vectors so the GL upload can happen later on the main thread.
    std::vector<float> mesh_verts;
    std::vector<unsigned int> mesh_indices;

    void build_mesh_cpu() {
        mesh_verts.clear();
        mesh_verts.reserve(GRID_SIZE * GRID_SIZE * 10);
        mesh_indices.clear();
        mesh_indices.reserve((GRID_SIZE-1)*(GRID_SIZE-1)*6);

        for (int z = 0; z < GRID_SIZE; z++) {
            for (int x = 0; x < GRID_SIZE; x++) {
                float wx = ((float)x/(GRID_SIZE-1)-0.5f)*WORLD_SIZE;
                float wz = ((float)z/(GRID_SIZE-1)-0.5f)*WORLD_SIZE;
                glm::vec3 n = normals(z,x);
                float u = (float)x/(GRID_SIZE-1);
                float v = (float)z/(GRID_SIZE-1);
                float biome_f = (float)biomes(z,x);
                mesh_verts.insert(mesh_verts.end(), {wx, heights(z,x), wz, n.x,n.y,n.z, u,v, biome_f, heights(z,x)/MAX_HEIGHT});
            }
        }
        for (int z = 0; z < GRID_SIZE-1; z++) {
            for (int x = 0; x < GRID_SIZE-1; x++) {
                unsigned int tl = z*GRID_SIZE+x, tr=tl+1;
                unsigned int bl = (z+1)*GRID_SIZE+x, br=bl+1;
                mesh_indices.insert(mesh_indices.end(), {tl,bl,tr, tr,bl,br});
            }
        }
        index_count = (int)mesh_indices.size();
    }

    // GL upload of the CPU-built mesh. MUST run on the main thread.
    void upload_mesh_gl() {
        if (ebo) { glDeleteBuffers(1, &ebo); ebo = 0; }
        if (vbo) { glDeleteBuffers(1, &vbo); vbo = 0; }
        if (vao) { glDeleteVertexArrays(1, &vao); vao = 0; }

        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, mesh_verts.size()*sizeof(float), mesh_verts.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 10*sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 10*sizeof(float), (void*)(3*sizeof(float)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 10*sizeof(float), (void*)(6*sizeof(float)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, 10*sizeof(float), (void*)(8*sizeof(float)));
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, 10*sizeof(float), (void*)(9*sizeof(float)));
        glEnableVertexAttribArray(4);

        glGenBuffers(1, &ebo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, mesh_indices.size()*sizeof(unsigned int), mesh_indices.data(), GL_STATIC_DRAW);
        glBindVertexArray(0);
    }

    void build_mesh() {
        build_mesh_cpu();
        upload_mesh_gl();
    }
};

                                                                                                                                                                                                                                                                                                                               
