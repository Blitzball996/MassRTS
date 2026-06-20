#pragma once
// =============================================================================
// Procedural Terrain Height - Multi-octave FBM noise from 3DWorld
// -----------------------------------------------------------------------------
// Extracted and adapted from 3DWorld/src/mesh_gen.cpp (GPL v3).
// Provides realistic mountain/hill generation for planet surfaces.
// Independent of 3DWorld's engine - only needs GLM.
// =============================================================================
#include <glm/glm.hpp>
#include <glm/gtc/noise.hpp>
#include <cmath>

namespace terrain {

// Terrain shape modes (affects how noise octaves are combined)
enum Shape {
    SHAPE_LINEAR = 0,  // Standard FBM - smooth rolling hills
    SHAPE_BILLOWY = 1, // Cloud-like bumps - rounded hills
    SHAPE_RIDGED = 2   // Sharp mountain ridges
};

// Generate multi-octave fractal noise at (x,y)
// Based on 3DWorld's gen_noise() - lacunarity 1.92, gain 0.5, ~6 octaves
inline float fbm_noise(float x, float y, Shape shape = SHAPE_LINEAR, bool use_simplex = true) {
    float zval = 0.0f, mag = 1.0f, freq = 1.0f;
    const float lacunarity = 1.92f, gain = 0.5f;
    const int octaves = 6;
    
    for (int i = 0; i < octaves; ++i) {
        glm::vec2 pos(freq * x, freq * y);
        float noise = use_simplex ? glm::simplex(pos) : glm::perlin(pos);
        
        // Shape modification
        switch (shape) {
        case SHAPE_LINEAR:  break; // Standard
        case SHAPE_BILLOWY: noise = std::abs(noise) - 0.40f; break;
        case SHAPE_RIDGED:  noise = 0.45f - std::abs(noise); break;
        }
        
        zval += mag * noise;
        mag  *= gain;
        freq *= lacunarity;
    }
    return zval;
}

// Domain warping: perturb coordinates before sampling noise
// Creates more organic, flowing terrain features (3DWorld's MGEN_DWARP)
inline float fbm_domain_warp(float x, float y, Shape shape = SHAPE_LINEAR) {
    const float scale = 0.2f;
    
    // First warp
    float dx1 = fbm_noise(x + 0.0f, y + 0.0f, shape);
    float dy1 = fbm_noise(x + 5.2f, y + 1.3f, shape);
    
    // Second warp (compound)
    float dx2 = fbm_noise(x + scale*dx1 + 1.7f, y + scale*dy1 + 9.2f, shape);
    float dy2 = fbm_noise(x + scale*dx1 + 8.3f, y + scale*dy1 + 2.8f, shape);
    
    // Final sample at warped position
    return fbm_noise(x + scale*dx2, y + scale*dy2, shape);
}

// High-level terrain generator: returns height in meters
// coord: normalized sphere position (e.g., planet.height receives dvec3 dir)
// amplitude: max height variation (e.g., 5000.0 for ±5km mountains)
// frequency: terrain feature density (higher = more detail, smaller features)
inline double generate_height(const glm::dvec3& coord, double amplitude = 5000.0, 
                              double frequency = 3.0, Shape shape = SHAPE_RIDGED,
                              bool use_domain_warp = true) {
    // Convert 3D sphere coords to 2D noise space (avoid poles clustering)
    float x = static_cast<float>(coord.x * frequency);
    float y = static_cast<float>(coord.z * frequency); // use xz plane
    
    float noise_val = use_domain_warp ? fbm_domain_warp(x, y, shape) 
                                       : fbm_noise(x, y, shape);
    
    return noise_val * amplitude;
}

// Multi-biome: blend different noise patterns by latitude or other criteria
// Example: RIDGED at equator (mountains), LINEAR at poles (plains)
inline double generate_biome_height(const glm::dvec3& coord, double amplitude = 5000.0) {
    double lat = std::abs(coord.y); // latitude proxy (0=equator, 1=pole)
    
    // Mountains at low latitudes, smoother at high latitudes
    Shape shape = (lat < 0.5) ? SHAPE_RIDGED : SHAPE_LINEAR;
    double freq = (lat < 0.5) ? 4.0 : 2.0; // more detail in mountains
    
    return generate_height(coord, amplitude, freq, shape, true);
}

} // namespace terrain
