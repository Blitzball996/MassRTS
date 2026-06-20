// =============================================================================
// SDFCraft - GAMEPLAY VALIDATOR
// -----------------------------------------------------------------------------
// Unlike selftest.cpp (which only checks data logic and happily passed while
// the game still showed stray polygons + camera judder), this harness drives
// the *real* render/camera path:
//   1. Actually walks the player up a slope frame-by-frame and measures the
//      rendered camera-Y jitter (vertical acceleration spikes => visible judder).
//   2. Builds real MCMesher output after digging and flags geometry defects the
//      eye perceives as "weird polygons": zero-area triangles, NaN/zero normals,
//      and floating triangles disconnected from the surface (stray polys).
//   3. Validates tree geometry the same way.
//
// Exit code != 0 on any failure, so CI / build scripts can gate on it.
// =============================================================================
#include "sdfcraft/world.h"
#include "sdfcraft/player.h"
#include "sdfcraft/mc_mesher.h"
#include "sdfcraft/mesher.h"
#include <glm/glm.hpp>
#include <cstdio>
#include <cmath>
#include <vector>
#include <cstdint>
#include <unordered_set>
#include <algorithm>

using namespace sdfcraft;

static int g_fail = 0;
#define FAIL(...) do { printf("  [FAIL] " __VA_ARGS__); printf("\n"); g_fail++; } while(0)
#define OKAY(...) do { printf("  [ ok ] " __VA_ARGS__); printf("\n"); } while(0)

// Packed vertex stride in ChunkMesh buffers: pos(3) nrm(3) col(3) mat(1).
static const int VSTRIDE = 10;

struct Tri { glm::vec3 p[3], n[3]; };

static std::vector<Tri> unpack(const std::vector<float>& buf) {
    std::vector<Tri> out;
    size_t verts = buf.size() / VSTRIDE;
    for (size_t t = 0; t + 2 < verts; t += 3) {
        Tri tri;
        for (int k = 0; k < 3; k++) {
            const float* f = &buf[(t + k) * VSTRIDE];
            tri.p[k] = glm::vec3(f[0], f[1], f[2]);
            tri.n[k] = glm::vec3(f[3], f[4], f[5]);
        }
        out.push_back(tri);
    }
    return out;
}
// --- geometry defect scan: returns counts of perceptible mesh problems ------
struct MeshReport {
    int tris = 0;
    int degenerate = 0;   // zero-area / collapsed triangles
    int bad_normal = 0;   // NaN, inf, or zero-length vertex normals
    int stray = 0;        // triangles whose all 3 verts are isolated from the bulk
    float max_edge = 0.f;
};

static MeshReport scan_mesh(const std::vector<float>& buf) {
    MeshReport r;
    auto tris = unpack(buf);
    r.tris = (int)tris.size();

    // Stray detection by triangle CENTROID binning. A connected isosurface packs
    // many triangle centroids into every 1-unit neighbourhood; a genuinely
    // floating ("stray") polygon sits alone with no other centroids around it.
    auto ckey = [](glm::vec3 c) -> int64_t {
        int64_t x = (int64_t)std::floor(c.x);
        int64_t y = (int64_t)std::floor(c.y);
        int64_t z = (int64_t)std::floor(c.z);
        return (x & 0x1FFFFF) | ((y & 0x1FFFFF) << 21) | ((z & 0x1FFFFF) << 42);
    };
    std::unordered_set<int64_t> cells;
    std::vector<glm::vec3> cen(tris.size());
    for (size_t i = 0; i < tris.size(); i++) {
        cen[i] = (tris[i].p[0] + tris[i].p[1] + tris[i].p[2]) / 3.0f;
        cells.insert(ckey(cen[i]));
    }
    auto neigh_cells = [&](glm::vec3 c) -> int {
        int64_t x = (int64_t)std::floor(c.x), y = (int64_t)std::floor(c.y), z = (int64_t)std::floor(c.z);
        int n = 0;
        for (int dx=-1; dx<=1; dx++) for (int dy=-1; dy<=1; dy++) for (int dz=-1; dz<=1; dz++) {
            int64_t k = ((x+dx)&0x1FFFFF) | (((y+dy)&0x1FFFFF)<<21) | (((z+dz)&0x1FFFFF)<<42);
            if (cells.count(k)) n++;
        }
        return n;
    };

    for (size_t i = 0; i < tris.size(); i++) {
        auto& t = tris[i];
        glm::vec3 e1 = t.p[1] - t.p[0], e2 = t.p[2] - t.p[0];
        float area = glm::length(glm::cross(e1, e2)) * 0.5f;
        if (area < 1e-7f) r.degenerate++;
        for (int k = 0; k < 3; k++) {
            float len = glm::length(t.n[k]);
            if (!std::isfinite(len) || len < 1e-4f ||
                !std::isfinite(t.p[k].x+t.p[k].y+t.p[k].z)) { r.bad_normal++; break; }
        }
        for (int k = 0; k < 3; k++)
            r.max_edge = std::max(r.max_edge, glm::length(t.p[(k+1)%3] - t.p[k]));
        // stray: this centroid's 3x3x3 cell neighbourhood holds no other geometry.
        if (neigh_cells(cen[i]) <= 1) r.stray++;
    }
    return r;
}
// =========================================================================
// TEST 1 - CAMERA: walk up a slope and measure rendered eye-Y jitter.
// Drives player.update() every frame (real physics) then reads render_eye()
// after update_camera_smoothing(), exactly like mode.h does in-game.
// Jitter = max frame-to-frame change in vertical *velocity* of the camera.
// A smooth follow keeps this tiny; raw voxel-snapped ground produces spikes.
// =========================================================================
static void test_camera_slope() {
    printf("[TEST] camera slope-walk jitter\n");
    World w(2024);
    Player p;
    // Find a spot and a forward direction that climbs. Start on the surface.
    int sx = 4, sz = 4;
    float sh = (float)w.surface_height(sx, sz);
    p.pos = glm::vec3(sx + 0.5f, sh + 1.0f, sz + 0.5f);
    p.smooth_y_ = p.pos.y;
    // Pick the compass heading that climbs fastest, so we truly walk UP a slope.
    float best_yaw = 0.0f, best_gain = -1e9f;
    for (int deg = 0; deg < 360; deg += 15) {
        float yr = glm::radians((float)deg);
        glm::vec3 dir(sinf(yr), 0, -cosf(yr));
        int ax = (int)floorf(p.pos.x + dir.x * 6.0f);
        int az = (int)floorf(p.pos.z + dir.z * 6.0f);
        float gain = (float)w.surface_height(ax, az) - sh;
        if (gain > best_gain) { best_gain = gain; best_yaw = (float)deg; }
    }
    p.yaw = best_yaw;            // walk toward the steepest available climb
    p.flying = false;

    const float dt = 1.0f / 60.0f;
    float prev_y = p.render_eye().y;
    float prev_vy = 0.0f;
    float max_jerk = 0.0f, max_step = 0.0f;
    int climbed = 0;
    float start_ground = sh;

    for (int frame = 0; frame < 600; frame++) {       // 10 simulated seconds
        p.update(w, dt, glm::vec3(0, 0, 1), false, false);  // hold forward
        p.update_camera_smoothing(dt);
        float y = p.render_eye().y;
        float vy = (y - prev_y) / dt;
        float jerk = std::fabs(vy - prev_vy);
        // ignore the big intentional snaps (jumps/falls > 2 blocks handled by snap)
        if (std::fabs(y - prev_y) < 1.5f) {
            max_jerk = std::max(max_jerk, jerk);
            max_step = std::max(max_step, std::fabs(y - prev_y));
        }
        prev_y = y; prev_vy = vy;
        int g = w.surface_height((int)floorf(p.pos.x), (int)floorf(p.pos.z));
        if (g > start_ground + 1) climbed++;
    }

    printf("  heading=%.0fdeg expected-gain=%.1f  climbed-frames=%d  max eye step=%.4f/frame  max jerk=%.3f\n",
           p.yaw, best_gain, climbed, max_step, max_jerk);
    if (climbed == 0 && best_gain > 1.0f)
        FAIL("player never gained height despite a %.1f-block climb ahead (stuck?)", best_gain);
    else OKAY("player traversed terrain (%d uphill frames)", climbed);
    // Smoothed camera should never teleport >0.5 block in one frame while walking,
    // and the vertical jerk should stay well bounded (no visible judder).
    if (max_step > 0.5f) FAIL("camera eye Y jumps %.3f in a single frame (judder)", max_step);
    else OKAY("per-frame eye step bounded (<=0.5 block)");
    if (max_jerk > 8.0f) FAIL("camera vertical jerk spike %.2f (visible shake on slope)", max_jerk);
    else OKAY("vertical jerk bounded (smooth slope follow)");
}
// =========================================================================
// TEST 2 - DIG: carve into the surface, mesh the affected chunks via the REAL
// MCMesher, and scan for the artifacts that look like "weird polygons".
// =========================================================================
static void test_dig_geometry() {
    printf("[TEST] dig produces clean geometry (no stray polys)\n");
    World w(1337);
    float sy = (float)w.surface_height(0, 0);
    // Overlapping carves like a player swinging a tool repeatedly.
    w.carve_sphere(0, sy,        0, 2.0f, -1);
    w.carve_sphere(2, sy - 1,    1, 1.5f, -1);
    w.carve_sphere(-1, sy - 2,  -1, 2.5f, -1);
    w.carve_sphere(1, sy + 1,    0, 1.0f, -1);

    MeshReport agg;
    // mesh every chunk that could touch the carve (3x3 around origin chunk)
    for (int cx = -1; cx <= 1; cx++)
    for (int cz = -1; cz <= 1; cz++) {
        Chunk* c = w.get_chunk({cx, cz}, true);
        if (!c) continue;
        ChunkMesh m;
        MCMesher::build(w, *c, m);
        MeshReport r = scan_mesh(m.opaque);
        agg.tris       += r.tris;
        agg.degenerate += r.degenerate;
        agg.bad_normal += r.bad_normal;
        agg.stray      += r.stray;
        agg.max_edge    = std::max(agg.max_edge, r.max_edge);
    }
    printf("  tris=%d  degenerate=%d  bad_normal=%d  stray=%d  max_edge=%.3f\n",
           agg.tris, agg.degenerate, agg.bad_normal, agg.stray, agg.max_edge);
    if (agg.tris == 0)         FAIL("carve produced no geometry at all");
    else OKAY("carve produced %d triangles", agg.tris);
    if (agg.degenerate > 0)    FAIL("%d zero-area triangles (slivers)", agg.degenerate);
    else OKAY("no degenerate triangles");
    if (agg.bad_normal > 0)    FAIL("%d triangles with NaN/zero normals", agg.bad_normal);
    else OKAY("all normals finite & non-zero");
    if (agg.stray > 0)         FAIL("%d floating/stray polygons disconnected from surface", agg.stray);
    else OKAY("no stray polygons");
    if (agg.max_edge > 3.0f)   FAIL("max edge %.2f too long (spike across the hole)", agg.max_edge);
    else OKAY("edge lengths bounded (%.2f)", agg.max_edge);
}

// =========================================================================
// TEST 3 - TREES: mesh a chunk known to contain a tree and verify the trunk +
// leaves produce well-formed geometry (no degenerate / NaN polys).
// =========================================================================
static void test_tree_geometry() {
    printf("[TEST] tree geometry well-formed\n");
    World w(7);
    int found = 0, bad = 0, total = 0, leaf_tris = 0;
    long leaf_blocks = 0, log_blocks = 0;
    for (int cx = -6; cx <= 6; cx++)
    for (int cz = -6; cz <= 6; cz++) {
        Chunk* c = w.get_chunk({cx, cz}, true);
        if (!c) continue;
        for (int ly = 0; ly < CHUNK_SY; ly++)
        for (int lz = 0; lz < CHUNK_SZ; lz++)
        for (int lx = 0; lx < CHUNK_SX; lx++) {
            BlockId b = c->get(lx, ly, lz);
            if (b == BLOCK_LEAVES) leaf_blocks++;
            else if (b == BLOCK_LOG) log_blocks++;
        }
        ChunkMesh cube;
        Mesher::build(w, *c, cube);
        MeshReport ro = scan_mesh(cube.opaque);
        MeshReport rt = scan_mesh(cube.transparent);
        int tt = ro.tris + rt.tris;
        if (tt > 0) {
            found++; total += tt;
            bad += ro.degenerate + ro.bad_normal + rt.degenerate + rt.bad_normal;
            leaf_tris += rt.tris;
        }
    }
    printf("  chunks with cube geometry=%d  total tris=%d  log-blocks=%ld leaf-blocks=%ld  leaf tris=%d  malformed=%d\n",
           found, total, log_blocks, leaf_blocks, leaf_tris, bad);
    if (leaf_blocks > 0 && leaf_tris == 0)
        FAIL("%ld leaf blocks exist but produced 0 transparent triangles (trees render broken)", leaf_blocks);
    else if (leaf_blocks > 0)
        OKAY("leaves meshed (%d transparent tris from %ld leaf blocks)", leaf_tris, leaf_blocks);
    if (found == 0) FAIL("no chunk geometry generated");
    else OKAY("generated geometry across %d chunks", found);
    if (bad > 0)    FAIL("%d malformed tree/ground triangles", bad);
    else OKAY("no malformed triangles in sampled chunks");
}

int main() {
    printf("=== SDFCraft GAMEPLAY VALIDATOR (real render/camera path) ===\n");
    test_camera_slope();
    test_dig_geometry();
    test_tree_geometry();
    printf("=============================================================\n");
    if (g_fail) { printf("VALIDATION FAILED: %d problem(s) detected.\n", g_fail); return 1; }
    printf("ALL GAMEPLAY CHECKS PASSED.\n");
    return 0;
}



