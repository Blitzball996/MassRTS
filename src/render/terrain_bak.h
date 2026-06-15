#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <vector>
#include <cmath>

// Terrain types for biome coloring
enum class TerrainBiome : uint8_t { Grass = 0, Mountain = 1, Swamp = 2, Forest = 3, Trench = 4 };

class Terrain {
public:
    static constexpr int GRID_SIZE = 256; // higher res for better detail
    static constexpr float WORLD_SIZE = 3000.0f;
    static constexpr float MAX_HEIGHT = 60.0f;

    GLuint vao = 0, vbo = 0, ebo = 0;
    int index_count = 0;
    float heights[GRID_SIZE][GRID_SIZE];
    TerrainBiome biomes[GRID_SIZE][GRID_SIZE];

    void generate() {
        // Generate heightmap with biomes
        for (int z = 0; z < GRID_SIZE; z++) {
            for (int x = 0; x < GRID_SIZE; x++) {
                float wx = ((float)x / (GRID_SIZE - 1) - 0.5f) * WORLD_SIZE;
                float wz = ((float)z / (GRID_SIZE - 1) - 0.5f) * WORLD_SIZE;
                heights[z][x] = sample_height(wx, wz);
                biomes[z][x] = sample_biome(wx, wz, heights[z][x]);
            }
        }

        // Carve trenches (linear depressions)
        carve_trenches();

        // Build mesh
        std::vector<float> verts;
        std::vector<unsigned int> indices;

        for (int z = 0; z < GRID_SIZE; z++) {
            for (int x = 0; x < GRID_SIZE; x++) {
                float wx = ((float)x / (GRID_SIZE - 1) - 0.5f) * WORLD_SIZE;
                float wz = ((float)z / (GRID_SIZE - 1) - 0.5f) * WORLD_SIZE;
                float wy = heights[z][x];

                float hL = get_h(x-1,z), hR = get_h(x+1,z);
                float hD = get_h(x,z-1), hU = get_h(x,z+1);
                float step = WORLD_SIZE / (GRID_SIZE - 1);
                glm::vec3 normal = glm::normalize(glm::vec3(hL-hR, 2.0f*step, hD-hU));

                float u = (float)x / (GRID_SIZE - 1);
                float v = (float)z / (GRID_SIZE - 1);
                float biome_f = (float)biomes[z][x];

                // pos(3) + normal(3) + uv(2) + biome(1) = 9 floats
                verts.insert(verts.end(), {wx, wy, wz, normal.x, normal.y, normal.z, u, v, biome_f});
            }
        }

        for (int z = 0; z < GRID_SIZE - 1; z++) {
            for (int x = 0; x < GRID_SIZE - 1; x++) {
                unsigned int tl = z * GRID_SIZE + x;
                unsigned int tr = tl + 1;
                unsigned int bl = (z + 1) * GRID_SIZE + x;
                unsigned int br = bl + 1;
                indices.insert(indices.end(), {tl, bl, tr, tr, bl, br});
            }
        }

        index_count = (int)indices.size();

        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, verts.size()*sizeof(float), verts.data(), GL_STATIC_DRAW);

        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 9*sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 9*sizeof(float), (void*)(3*sizeof(float)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 9*sizeof(float), (void*)(6*sizeof(float)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, 9*sizeof(float), (void*)(8*sizeof(float)));
        glEnableVertexAttribArray(3);

        glGenBuffers(1, &ebo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size()*sizeof(unsigned int), indices.data(), GL_STATIC_DRAW);
        glBindVertexArray(0);
    }

    float get_height_at(float wx, float wz) const {
        float fx = (wx / WORLD_SIZE + 0.5f) * (GRID_SIZE - 1);
        float fz = (wz / WORLD_SIZE + 0.5f) * (GRID_SIZE - 1);
        int ix = (int)fx, iz = (int)fz;
        if (ix < 0) ix = 0; if (ix >= GRID_SIZE-1) ix = GRID_SIZE-2;
        if (iz < 0) iz = 0; if (iz >= GRID_SIZE-1) iz = GRID_SIZE-2;
        float tx = fx-ix, tz = fz-iz;
        float h00 = heights[iz][ix], h10 = heights[iz][ix+1];
        float h01 = heights[iz+1][ix], h11 = heights[iz+1][ix+1];
        return (h00*(1-tx)+h10*tx)*(1-tz) + (h01*(1-tx)+h11*tx)*tz;
    }

    TerrainBiome get_biome_at(float wx, float wz) const {
        float fx = (wx / WORLD_SIZE + 0.5f) * (GRID_SIZE - 1);
        float fz = (wz / WORLD_SIZE + 0.5f) * (GRID_SIZE - 1);
        int ix = glm::clamp((int)fx, 0, GRID_SIZE-1);
        int iz = glm::clamp((int)fz, 0, GRID_SIZE-1);
        return biomes[iz][ix];
    }

    // Speed multiplier based on terrain
    float get_speed_mult(float wx, float wz) const {
        TerrainBiome b = get_biome_at(wx, wz);
        switch (b) {
            case TerrainBiome::Swamp: return 0.5f;
            case TerrainBiome::Forest: return 0.7f;
            case TerrainBiome::Mountain: return 0.6f;
            case TerrainBiome::Trench: return 0.8f;
            default: return 1.0f;
        }
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
        return heights[z][x];
    }

    static float noise2d(float x, float y) {
        float ix = floor(x), iy = floor(y);
        float fx = x-ix, fy = y-iy;
        fx = fx*fx*(3-2*fx);
        fy = fy*fy*(3-2*fy);
        auto h = [](float a, float b){ return fract(sin(a*127.1f+b*311.7f)*43758.5453f); };
        float a=h(ix,iy), b=h(ix+1,iy), c=h(ix,iy+1), d=h(ix+1,iy+1);
        return (a*(1-fx)+b*fx)*(1-fy) + (c*(1-fx)+d*fx)*fy;
    }

    static float fract(float x) { return x - floor(x); }

    static float fbm(float x, float z, int octaves) {
        float val = 0, amp = 1.0f, freq = 1.0f, total = 0;
        for (int i = 0; i < octaves; i++) {
            val += noise2d(x*freq, z*freq) * amp;
            total += amp;
            amp *= 0.5f;
            freq *= 2.0f;
        }
        return val / total;
    }

    static float sample_height(float wx, float wz) {
        float h = 0;
        // Base rolling hills
        h += fbm(wx*0.001f, wz*0.001f, 4) * MAX_HEIGHT;

        // Mountain ridges (on edges)
        float edge_dist = glm::max(fabs(wx), fabs(wz));
        float mountain_factor = glm::clamp((edge_dist - 800.0f) / 400.0f, 0.0f, 1.0f);
        float ridge = fbm(wx*0.003f+5.7f, wz*0.003f+3.2f, 3);
        h += ridge * ridge * MAX_HEIGHT * 2.0f * mountain_factor;

        // Flatten center battle area gently
        float center_dist = sqrt(wx*wx + wz*wz);
        float flatten = 1.0f - glm::clamp(1.0f - center_dist/400.0f, 0.0f, 1.0f) * 0.7f;
        h *= flatten;

        // Swamp depressions (low areas near center-sides)
        float swamp_mask = glm::clamp(1.0f - fabs(wz)/600.0f, 0.0f, 1.0f) *
                           glm::clamp((fabs(wx) - 300.0f)/200.0f, 0.0f, 1.0f);
        swamp_mask *= glm::clamp(1.0f - fabs(wx)/900.0f, 0.0f, 1.0f);
        if (swamp_mask > 0.3f) h = glm::min(h, 2.0f);

        return h;
    }

    static TerrainBiome sample_biome(float wx, float wz, float height) {
        float center_dist = sqrt(wx*wx + wz*wz);

        // Mountains: high elevation on edges
        if (height > MAX_HEIGHT * 0.7f) return TerrainBiome::Mountain;

        // Swamp: low, wet areas on flanks
        float swamp_mask = glm::clamp(1.0f - fabs(wz)/600.0f, 0.0f, 1.0f) *
                           glm::clamp((fabs(wx) - 300.0f)/200.0f, 0.0f, 1.0f) *
                           glm::clamp(1.0f - fabs(wx)/900.0f, 0.0f, 1.0f);
        if (swamp_mask > 0.3f && height < 5.0f) return TerrainBiome::Swamp;

        // Forest: medium height patches
        float forest_noise = fbm(wx*0.004f+10.0f, wz*0.004f+20.0f, 2);
        if (forest_noise > 0.6f && height > 5.0f && height < MAX_HEIGHT*0.5f &&
            center_dist > 250.0f) return TerrainBiome::Forest;

        return TerrainBiome::Grass;
    }

    void carve_trenches() {
        // Trenches: horizontal lines at z ~ -150 and z ~ 150 (between armies)
        for (int z = 0; z < GRID_SIZE; z++) {
            for (int x = 0; x < GRID_SIZE; x++) {
                float wx = ((float)x/(GRID_SIZE-1)-0.5f)*WORLD_SIZE;
                float wz = ((float)z/(GRID_SIZE-1)-0.5f)*WORLD_SIZE;

                // Two trench lines
                float trench1 = fabs(wz - 100.0f);
                float trench2 = fabs(wz + 100.0f);
                float trench_dist = glm::min(trench1, trench2);

                // Only in center area
                if (fabs(wx) < 500.0f && trench_dist < 15.0f) {
                    float depth = (1.0f - trench_dist/15.0f) * 4.0f;
                    heights[z][x] -= depth;
                    biomes[z][x] = TerrainBiome::Trench;
                }
            }
        }
    }
};
