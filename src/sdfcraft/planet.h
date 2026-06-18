#pragma once
// =============================================================================
// SDFCraft - Planet foundation: cube-sphere + double-precision + octree LOD
// -----------------------------------------------------------------------------
// Phase P1/P3 groundwork (see docs/SDF_MINECRAFT_PORT_PLAN.md §1.4). This header
// is pure math (no OpenGL, no gameplay) so it can be unit-tested in isolation.
//
// WHY THIS EXISTS
//   The flat world uses single-precision world coords. At Earth scale (radius
//   6,371 km) a 32-bit float has ~0.5 m resolution near 6e6 — the surface would
//   visibly jitter. So planetary positions are kept in DOUBLE precision, and the
//   renderer works in a FLOATING-ORIGIN frame (camera-relative) where floats are
//   tiny and exact again.
//
// PIECES
//   * PlanetConfig   - radius etc. (Earth by default).
//   * cube-sphere    - map the 6 faces of a cube onto the sphere; each face is
//                      the root of one octree (quadtree on the surface, but we
//                      keep full octree for depth/voxels below the surface).
//   * OctreeNode     - axis-aligned node in cube-face parameter space [-1,1]^2 x
//                      depth; distance-driven subdivide/merge with hysteresis.
//   * floating origin - rebase doubles to camera-relative floats for rendering.
// =============================================================================
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cstdint>
#include <vector>
#include <cmath>
#include <array>

namespace sdfcraft {

// ---- double-precision vec3 (glm::dvec3 alias for clarity) ------------------
using dvec3 = glm::dvec3;

struct PlanetConfig {
    double radius_m   = 6371000.0;   // Earth mean radius (metres)
    double max_height = 9000.0;      // tallest terrain above sea level (Everest-ish)
    double min_depth  = 11000.0;     // deepest carve below sea level (Mariana-ish)
    int    max_lod    = 24;          // octree depth: 6371km / 2^24 ≈ 0.38 m leaf
};

// Which of the 6 cube faces. Order fixed for serialization/networking.
enum CubeFace : uint8_t { FACE_PX=0, FACE_NX, FACE_PY, FACE_NY, FACE_PZ, FACE_NZ };

// PLACEHOLDER_CUBESPHERE

// --- cube-sphere mapping --------------------------------------------------
// Map a face + 2D parameter (u,v in [-1,1]) to a point on the UNIT sphere.
// Uses the standard "tangent-adjusted" warp that equalises the area of cells
// far better than naive normalisation (less distortion at face centres/edges).
inline dvec3 cube_to_unit_sphere(CubeFace f, double u, double v) {
    // tangent warp (Cobe-style approximation): spreads samples evenly
    auto warp = [](double a){ return std::tan(a * 0.7853981633974483); }; // a*pi/4
    double x = warp(u), y = warp(v);
    dvec3 p;
    switch (f) {
        case FACE_PX: p = dvec3( 1.0,    y,   -x); break;
        case FACE_NX: p = dvec3(-1.0,    y,    x); break;
        case FACE_PY: p = dvec3(   x,  1.0,   -y); break;
        case FACE_NY: p = dvec3(   x, -1.0,    y); break;
        case FACE_PZ: p = dvec3(   x,    y,  1.0); break;
        case FACE_NZ: p = dvec3(  -x,    y, -1.0); break;
    }
    return glm::normalize(p);
}

// Surface point in metres (sphere radius), before terrain displacement.
inline dvec3 cube_to_planet(const PlanetConfig& cfg, CubeFace f, double u, double v) {
    return cube_to_unit_sphere(f, u, v) * cfg.radius_m;
}

// Inverse: given a unit direction, find which face + (u,v) it projects to.
inline void unit_sphere_to_cube(const dvec3& dir, CubeFace& f, double& u, double& v) {
    dvec3 a = glm::abs(dir);
    auto unwarp = [](double t){ return std::atan(t) / 0.7853981633974483; };
    if (a.x >= a.y && a.x >= a.z) {
        if (dir.x > 0) { f = FACE_PX; u = unwarp(-dir.z/dir.x); v = unwarp( dir.y/dir.x); }
        else           { f = FACE_NX; u = unwarp( dir.z/-dir.x); v = unwarp( dir.y/-dir.x); }
    } else if (a.y >= a.x && a.y >= a.z) {
        if (dir.y > 0) { f = FACE_PY; u = unwarp( dir.x/dir.y); v = unwarp(-dir.z/dir.y); }
        else           { f = FACE_NY; u = unwarp( dir.x/-dir.y); v = unwarp( dir.z/-dir.y); }
    } else {
        if (dir.z > 0) { f = FACE_PZ; u = unwarp( dir.x/dir.z); v = unwarp( dir.y/dir.z); }
        else           { f = FACE_NZ; u = unwarp(-dir.x/-dir.z); v = unwarp( dir.y/-dir.z); }
    }
}

// --- floating origin ------------------------------------------------------
// Convert a double world position to a single-precision, camera-relative one
// for rendering. Near the camera the result is small => float-exact (no jitter
// even though the absolute position is millions of metres from planet centre).
inline glm::vec3 to_render_space(const dvec3& world, const dvec3& cam) {
    return glm::vec3(world - cam);
}

// PLACEHOLDER_OCTREE

// --- cube-face quadtree node (the surface LOD structure) ------------------
// On a planet surface the relevant subdivision is 2D per face (a quadtree);
// vertical/voxel depth is handled separately by the per-leaf voxel chunk. Each
// node covers a square [u0,u1]x[v0,v1] in face parameter space at a given LOD
// level (0 = whole face). Distance to the camera decides subdivide vs merge.
struct QuadNode {
    CubeFace face;
    int      level;        // 0 = root (whole face)
    double   u0, v0, u1, v1;
    QuadNode* child[4] = {nullptr,nullptr,nullptr,nullptr};
    bool      has_children = false;

    dvec3 center_dir() const {
        return cube_to_unit_sphere(face, 0.5*(u0+u1), 0.5*(v0+v1));
    }
    // approximate world-space edge length (metres) of this node on the sphere
    double edge_m(const PlanetConfig& cfg) const {
        dvec3 a = cube_to_planet(cfg, face, u0, v0);
        dvec3 b = cube_to_planet(cfg, face, u1, v0);
        return glm::length(b - a);
    }
};

// LOD policy: subdivide while the node is "large" relative to its distance.
// k is the quality factor (higher = more detail / more nodes). Hysteresis: we
// subdivide at k but only merge back at k*merge_slack to avoid popping on the
// boundary as the camera moves back and forth.
struct LodPolicy {
    double k = 2.5;            // subdivide when edge_m > k * distance? (see test)
    double merge_slack = 1.6;  // merge only when clearly too detailed
    int    max_level = 24;
};

// Returns true if `node` should have children given camera position `cam` (m).
inline bool should_subdivide(const PlanetConfig& cfg, const QuadNode& n,
                             const dvec3& cam, const LodPolicy& p) {
    if (n.level >= p.max_level) return false;
    dvec3 surf = n.center_dir() * cfg.radius_m;
    double dist = glm::length(cam - surf);
    if (dist < 1.0) dist = 1.0;
    double edge = n.edge_m(cfg);
    // subdivide while the node still subtends a large angle (edge/dist big)
    return (edge / dist) > (1.0 / p.k);
}
inline bool should_merge(const PlanetConfig& cfg, const QuadNode& n,
                         const dvec3& cam, const LodPolicy& p) {
    dvec3 surf = n.center_dir() * cfg.radius_m;
    double dist = glm::length(cam - surf);
    if (dist < 1.0) dist = 1.0;
    double edge = n.edge_m(cfg);
    return (edge / dist) < (1.0 / (p.k * p.merge_slack));
}

// Split a node into its 4 children (caller owns memory; freed by free_quad).
inline void split_quad(QuadNode& n) {
    double um = 0.5*(n.u0+n.u1), vm = 0.5*(n.v0+n.v1);
    const double uu[4][4] = {
        {n.u0,n.v0,um,vm}, {um,n.v0,n.u1,vm},
        {n.u0,vm,um,n.v1}, {um,vm,n.u1,n.v1}
    };
    for (int i=0;i<4;i++) {
        n.child[i] = new QuadNode{ n.face, n.level+1,
            uu[i][0], uu[i][1], uu[i][2], uu[i][3] };
    }
    n.has_children = true;
}
inline void free_quad(QuadNode* n) {
    if (!n) return;
    for (auto* c : n->child) free_quad(c);
    for (auto*& c : n->child) c = nullptr;
    n->has_children = false;
}

} // namespace sdfcraft
