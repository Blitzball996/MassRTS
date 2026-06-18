#pragma once
// =============================================================================
// SDFCraft - Planet mesh builder (Phase P1 visualisation)
// -----------------------------------------------------------------------------
// Walks the cube-sphere quadtree, refreshes LOD against the camera, and emits a
// triangle mesh of the (displaced) sphere surface in FLOATING-ORIGIN space so a
// real 6371 km planet renders without float jitter. Pure logic + glm; the GL
// upload lives in the renderer. Each visible leaf becomes a small grid patch so
// the surface is smooth and can carry height displacement later.
// =============================================================================
#include "planet.h"
#include <glm/glm.hpp>
#include <vector>
#include <functional>

namespace sdfcraft {

struct PlanetVertex {
    glm::vec3 pos;     // floating-origin (camera-relative) metres
    glm::vec3 normal;  // surface normal (unit)
    float     height;  // normalised terrain height [-1,1] for shading
};

// Optional height function: given a unit-sphere direction, return displacement
// in metres (mountains/oceans). Default = sphere (flat). Kept double for scale.
using HeightFn = std::function<double(const dvec3& dir)>;

class PlanetMesh {
public:
    PlanetConfig cfg;
    LodPolicy    pol;
    HeightFn     height = [](const dvec3&){ return 0.0; };

    PlanetMesh() { reset_roots(); }
    ~PlanetMesh() { for (auto* r : roots_) { free_quad(r); delete r; } }

    // Refresh LOD selection against the camera (double-precision world metres).
    void update_lod(const dvec3& cam) {
        for (auto* r : roots_) refine(*r, cam);
    }

    // Build a triangle mesh of all current leaves, camera-relative.
    // GRID = subdivisions per leaf patch edge (>=1).
    void build(const dvec3& cam, std::vector<PlanetVertex>& out, int GRID = 8) {
        out.clear();
        for (auto* r : roots_) emit(*r, cam, out, GRID);
    }

    // PLACEHOLDER_METHODS
    // (public API complete: update_lod + build)

private:
    std::array<QuadNode*,6> roots_{};

    void reset_roots() {
        for (int f=0; f<6; f++)
            roots_[f] = new QuadNode{ (CubeFace)f, 0, -1,-1, 1,1 };
    }

    // PLACEHOLDER_PRIVATE

    // Recursively subdivide/merge to match the camera distance (hysteresis).
    void refine(QuadNode& n, const dvec3& cam) {
        if (!n.has_children) {
            if (should_subdivide(cfg, n, cam, pol)) split_quad(n);
        } else {
            if (should_merge(cfg, n, cam, pol)) { free_quad(&n); return; }
        }
        if (n.has_children)
            for (auto* c : n.child) refine(*c, cam);
    }

    // Displaced surface position (double) for face param (u,v).
    dvec3 surface_pos(CubeFace f, double u, double v) const {
        dvec3 dir = cube_to_unit_sphere(f, u, v);
        double r = cfg.radius_m + height(dir);
        return dir * r;
    }

    // Emit a GRID x GRID patch for a leaf node (or recurse into children).
    void emit(QuadNode& n, const dvec3& cam,
              std::vector<PlanetVertex>& out, int GRID) {
        if (n.has_children) { for (auto* c : n.child) emit(*c, cam, out, GRID); return; }

        double du = (n.u1 - n.u0) / GRID, dv = (n.v1 - n.v0) / GRID;
        // build the grid of positions/normals, then two triangles per cell
        auto vert = [&](int i, int j) -> PlanetVertex {
            double u = n.u0 + du*i, v = n.v0 + dv*j;
            dvec3 w = surface_pos(n.face, u, v);
            PlanetVertex pv;
            pv.pos    = to_render_space(w, cam);
            pv.normal = glm::vec3(glm::normalize(w));   // sphere normal (approx)
            pv.height = (float)((glm::length(w) - cfg.radius_m) /
                                (cfg.max_height));       // normalised
            return pv;
        };
        for (int j=0; j<GRID; j++)
        for (int i=0; i<GRID; i++) {
            PlanetVertex a=vert(i,j), b=vert(i+1,j), c=vert(i+1,j+1), d=vert(i,j+1);
            out.push_back(a); out.push_back(b); out.push_back(c);
            out.push_back(a); out.push_back(c); out.push_back(d);
        }
    }
};

} // namespace sdfcraft
