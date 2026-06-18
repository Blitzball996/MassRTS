#pragma once
// =============================================================================
// SDFCraft - Infinite chunked block world
// -----------------------------------------------------------------------------
// A discrete block-voxel world (separate from the RTS SDF terrain). Chunks are
// CHUNK_SX*CHUNK_SY*CHUNK_SZ blocks, generated lazily from a 64-bit seed so the
// world is effectively infinite on X/Z. Only edits (player place/break) are
// authoritative state worth persisting; the natural layer is reproducible from
// the seed (matches the plan's "delta persistence").
//
// Networking: a Chunk's natural content is derivable from (seed, coord) on every
// peer, so the server only needs to ship the edit deltas. This file holds the
// generation + storage; net replication lives in the net layer.
// =============================================================================
#include "blocks.h"
#include <cstdint>
#include <vector>
#include <array>
#include <algorithm>
#include <unordered_map>
#include <cmath>
#include <glm/glm.hpp>

namespace sdfcraft {

static constexpr int CHUNK_SX = 16;   // blocks per chunk on X
static constexpr int CHUNK_SZ = 16;   // blocks per chunk on Z
static constexpr int CHUNK_SY = 128;  // world height (single vertical chunk)
static constexpr float BLOCK_SIZE = 1.0f;

struct ChunkKey {
    int32_t cx, cz;
    bool operator==(const ChunkKey& o) const { return cx == o.cx && cz == o.cz; }
};
struct ChunkKeyHash {
    size_t operator()(const ChunkKey& k) const {
        return ((size_t)(uint32_t)k.cx * 73856093u) ^ ((size_t)(uint32_t)k.cz * 19349663u);
    }
};

// --- deterministic value noise (seed-driven, no global RNG) ----------------
struct Noise {
    uint64_t seed;
    explicit Noise(uint64_t s) : seed(s) {}

    static uint64_t hash64(uint64_t x) {
        x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
        x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
        x ^= x >> 33; return x;
    }
    float rand2(int x, int z) const {
        uint64_t h = hash64(seed ^ (uint64_t)(uint32_t)x * 0x9E3779B1ULL
                                 ^ ((uint64_t)(uint32_t)z << 32));
        return (float)((h >> 11) & 0xFFFFFF) / (float)0xFFFFFF; // [0,1)
    }
    static float fade(float t) { return t*t*t*(t*(t*6-15)+10); }
    // smooth value noise sampled in world units
    float value(float x, float z, float freq) const {
        float fx = x * freq, fz = z * freq;
        int x0 = (int)floorf(fx), z0 = (int)floorf(fz);
        float tx = fade(fx - x0), tz = fade(fz - z0);
        float v00 = rand2(x0, z0),     v10 = rand2(x0+1, z0);
        float v01 = rand2(x0, z0+1),   v11 = rand2(x0+1, z0+1);
        float a = v00 + (v10 - v00) * tx;
        float b = v01 + (v11 - v01) * tx;
        return a + (b - a) * tz;
    }
    // fractal brownian motion (several octaves)
    float fbm(float x, float z, float freq, int octaves) const {
        float sum = 0, amp = 1, norm = 0;
        for (int i = 0; i < octaves; i++) {
            sum += value(x, z, freq) * amp;
            norm += amp; amp *= 0.5f; freq *= 2.0f;
        }
        return sum / norm;
    }
};

struct Chunk {
    ChunkKey key;
    std::vector<BlockId> blocks;   // size CHUNK_SX*CHUNK_SY*CHUNK_SZ
    // Continuous SDF layer — allocated on first carve/generation.
    // When non-empty, MCMesher uses this instead of the ±0.5 discrete field.
    // Values are signed distances in block units: negative=solid, positive=air.
    std::vector<float>   sdf;      // same index layout as blocks, or empty
    bool generated = false;
    bool dirty_mesh = true;        // needs remesh
    bool dirty_save = false;       // has unsaved player edits

    static inline int index(int lx, int ly, int lz) {
        return (ly * CHUNK_SZ + lz) * CHUNK_SX + lx;
    }
    BlockId get(int lx, int ly, int lz) const {
        if (ly < 0 || ly >= CHUNK_SY) return BLOCK_AIR;
        return blocks[index(lx, ly, lz)];
    }
    void set(int lx, int ly, int lz, BlockId b) {
        if (ly < 0 || ly >= CHUNK_SY) return;
        blocks[index(lx, ly, lz)] = b;
    }
    // Allocate / read / write the SDF layer.
    // Fill with the "not carved" sentinel (999) so untouched voxels fall through
    // to the analytic field. Filling with 0.0 was the "dig makes everything
    // vanish" bug: 0 reads as an exactly-on-surface value, so the moment the
    // layer was allocated the whole chunk collapsed to the isosurface.
    void ensure_sdf() {
        if (sdf.empty()) sdf.assign(CHUNK_SX * CHUNK_SY * CHUNK_SZ, 999.0f);
    }
    float get_sdf(int lx, int ly, int lz) const {
        if (sdf.empty() || ly < 0 || ly >= CHUNK_SY) return 999.0f; // sentinel: use blocks
        return sdf[index(lx, ly, lz)];
    }
    void set_sdf(int lx, int ly, int lz, float v) {
        if (ly < 0 || ly >= CHUNK_SY) return;
        ensure_sdf();
        sdf[index(lx, ly, lz)] = v;
    }
};

class World {
public:
    uint64_t seed = 1337;
    int sea_level = 62;

    explicit World(uint64_t s = 1337) : seed(s), noise_(s) {}

    // ---- coordinate helpers ----
    static int floordiv(int a, int b) { int q = a / b; if ((a % b != 0) && ((a < 0) != (b < 0))) q--; return q; }
    static int floormod(int a, int b) { int r = a % b; if (r != 0 && ((r < 0) != (b < 0))) r += b; return r; }

    static ChunkKey world_to_chunk(int wx, int wz) {
        return { floordiv(wx, CHUNK_SX), floordiv(wz, CHUNK_SZ) };
    }

    Chunk* get_chunk(ChunkKey k, bool create) {
        auto it = chunks_.find(k);
        if (it != chunks_.end()) return &it->second;
        if (!create) return nullptr;
        Chunk& c = chunks_[k];
        c.key = k;
        c.blocks.assign(CHUNK_SX * CHUNK_SY * CHUNK_SZ, BLOCK_AIR);
        generate(c);
        return &c;
    }

    bool chunk_loaded(ChunkKey k) const { return chunks_.find(k) != chunks_.end(); }

    // ---- block access in world coords ----
    BlockId get_block(int wx, int wy, int wz) {
        if (wy < 0 || wy >= CHUNK_SY) return BLOCK_AIR;
        ChunkKey k = world_to_chunk(wx, wz);
        Chunk* c = get_chunk(k, true);
        return c->get(floormod(wx, CHUNK_SX), wy, floormod(wz, CHUNK_SZ));
    }
    // returns true if the block actually changed
    bool set_block(int wx, int wy, int wz, BlockId b) {
        if (wy < 0 || wy >= CHUNK_SY) return false;
        ChunkKey k = world_to_chunk(wx, wz);
        Chunk* c = get_chunk(k, true);
        int lx = floormod(wx, CHUNK_SX), lz = floormod(wz, CHUNK_SZ);
        if (c->get(lx, wy, lz) == b) return false;
        c->set(lx, wy, lz, b);
        c->dirty_mesh = true;
        c->dirty_save = true;
        // neighbour chunk remesh if on a border (faces across the seam change)
        if (lx == 0)            mark_dirty({k.cx-1, k.cz});
        if (lx == CHUNK_SX - 1) mark_dirty({k.cx+1, k.cz});
        if (lz == 0)            mark_dirty({k.cx, k.cz-1});
        if (lz == CHUNK_SZ - 1) mark_dirty({k.cx, k.cz+1});
        return true;
    }

    void mark_dirty(ChunkKey k) {
        auto it = chunks_.find(k);
        if (it != chunks_.end()) it->second.dirty_mesh = true;
    }

    // ---- terrain height query (surface y, exclusive top) ----
    int surface_height(int wx, int wz) {
        return (int)surface_height_f((float)wx, (float)wz);
    }
    // Continuous (float) surface height — used by the analytic SDF so the
    // isosurface is smooth instead of snapping to integer block tops.
    float surface_height_f(float wx, float wz) {
        Noise& n = noise_;
        float base = n.fbm(wx, wz, 1.0f / 96.0f, 5);
        float mountain = n.fbm(wx + 4000.0f, wz - 4000.0f, 1.0f / 220.0f, 4);
        mountain = mountain * mountain;
        return sea_level - 6.0f + base * 26.0f + mountain * 48.0f;
    }

    // Pristine analytic SDF at a world point (continuous). Negative = solid,
    // positive = air. Normalised by the surface gradient so Marching Cubes
    // places clean triangles on slopes (same trick as MassRTS sdf_terrain).
    float analytic_field(float wx, float wy, float wz) {
        float h  = surface_height_f(wx, wz);
        float e  = 1.0f;
        float hx = surface_height_f(wx + e, wz) - surface_height_f(wx - e, wz);
        float hz = surface_height_f(wx, wz + e) - surface_height_f(wx, wz - e);
        float gx = hx / (2.0f * e), gz = hz / (2.0f * e);
        float inv = 1.0f / std::sqrt(1.0f + gx*gx + gz*gz);
        return (wy - h) * inv;   // distance above the surface (negative below)
    }

    // Continuous SDF used by the mesher. Reads the per-chunk float layer when a
    // voxel has been carved, otherwise falls back to the analytic field.
    float sdf_at(int wx, int wy, int wz) {
        if (wy < 0)         return -1.0f;        // solid floor
        if (wy >= CHUNK_SY) return  1.0f;        // air ceiling
        ChunkKey k = world_to_chunk(wx, wz);
        Chunk* c = get_chunk(k, true);
        int lx = floormod(wx, CHUNK_SX), lz = floormod(wz, CHUNK_SZ);
        float stored = c->get_sdf(lx, wy, lz);
        if (stored < 900.0f) return stored;      // carved/edited voxel
        // Untouched voxel: the terrain body IS the continuous analytic field.
        // We deliberately do NOT clamp it toward block occupancy — that was the
        // source of the stair-stepping and the render/raycast mismatch. The
        // field alone is smooth and matches what the mesher draws.
        return analytic_field((float)wx, (float)wy, (float)wz);
    }

    void prime_height_cache(int base_x, int base_z, int w, int d) {
        _hcache.base_x = base_x; _hcache.base_z = base_z;
        _hcache.stride = w;
        _hcache.h.resize(w * d);
        for (int iz = 0; iz < d; iz++)
        for (int ix = 0; ix < w; ix++)
            _hcache.h[iz * w + ix] = surface_height_f((float)(base_x + ix), (float)(base_z + iz));
    }

    // Fast analytic SDF that uses the pre-warmed height cache. Falls back to full computation.
    float analytic_field_cached(float wx, float wy, float wz) {
        float h  = _hcache.get((int)wx, (int)wz);
        if (h == 0.0f) return analytic_field(wx, wy, wz); // fallback (shouldn't happen)
        // gradient from cache (central differences, 1-unit step)
        float hxp = _hcache.get((int)wx+1, (int)wz);
        float hxm = _hcache.get((int)wx-1, (int)wz);
        float hzp = _hcache.get((int)wx, (int)wz+1);
        float hzm = _hcache.get((int)wx, (int)wz-1);
        float gx = (hxp - hxm) * 0.5f, gz = (hzp - hzm) * 0.5f;
        float inv = 1.0f / std::sqrt(1.0f + gx*gx + gz*gz);
        return (wy - h) * inv;
    }

    // sdf_at with cache (used by MCMesher during a build pass)
    float sdf_at_cached(int wx, int wy, int wz) {
        if (wy < 0)         return -1.0f;
        if (wy >= CHUNK_SY) return  1.0f;
        ChunkKey k = world_to_chunk(wx, wz);
        Chunk* c = get_chunk(k, true);
        int lx = floormod(wx, CHUNK_SX), lz = floormod(wz, CHUNK_SZ);
        float stored = c->get_sdf(lx, wy, lz);
        if (stored < 900.0f) return stored;
        return analytic_field_cached((float)wx, (float)wy, (float)wz);
    }

    // Trilinear-sampled continuous SDF (MassRTS sample_sdf). Sampling between
    // integer voxels is what makes the surface smooth instead of faceted, and
    // it lets us compute fine-grained normals. Reads sdf_at at the 8 surrounding
    // integer corners and blends.
    float sample_sdf(float wx, float wy, float wz) {
        int ix = (int)std::floor(wx), iy = (int)std::floor(wy), iz = (int)std::floor(wz);
        float fx = wx - ix, fy = wy - iy, fz = wz - iz;
        float c000 = sdf_at(ix,   iy,   iz),   c100 = sdf_at(ix+1, iy,   iz);
        float c010 = sdf_at(ix,   iy+1, iz),   c110 = sdf_at(ix+1, iy+1, iz);
        float c001 = sdf_at(ix,   iy,   iz+1), c101 = sdf_at(ix+1, iy,   iz+1);
        float c011 = sdf_at(ix,   iy+1, iz+1), c111 = sdf_at(ix+1, iy+1, iz+1);
        float c00 = c000*(1-fx) + c100*fx, c10 = c010*(1-fx) + c110*fx;
        float c01 = c001*(1-fx) + c101*fx, c11 = c011*(1-fx) + c111*fx;
        float c0 = c00*(1-fy) + c10*fy, c1 = c01*(1-fy) + c11*fy;
        return c0*(1-fz) + c1*fz;
    }

    // Fine (0.5-unit) SDF gradient → smooth normal, independent of voxel size.
    // This is the MassRTS sdf_gradient trick that removes the blocky shading.
    glm::vec3 sdf_normal(glm::vec3 p) {
        float e = 0.5f;
        float dx = sample_sdf(p.x+e, p.y, p.z) - sample_sdf(p.x-e, p.y, p.z);
        float dy = sample_sdf(p.x, p.y+e, p.z) - sample_sdf(p.x, p.y-e, p.z);
        float dz = sample_sdf(p.x, p.y, p.z+e) - sample_sdf(p.x, p.y, p.z-e);
        glm::vec3 g(dx, dy, dz);
        float L = glm::length(g);
        return L > 1e-6f ? g / L : glm::vec3(0,1,0);
    }

    // Spherical carve / fill (smooth, MassRTS-style). op<0 digs, op>0 fills.
    // Returns the list of (wx,wy,wz,oldblock) voxels whose block flipped, so the
    // caller can award drops. Smooth-min blends a rounded bowl into the field.
    void carve_sphere(float cx, float cy, float cz, float radius, int op,
                      std::vector<std::array<int,4>>* flips = nullptr) {
        float k = radius * 0.5f;             // smooth-blend width
        float reach = radius + k + 1.0f;
        int x0 = (int)std::floor(cx - reach), x1 = (int)std::ceil(cx + reach);
        int y0 = (int)std::floor(cy - reach), y1 = (int)std::ceil(cy + reach);
        int z0 = (int)std::floor(cz - reach), z1 = (int)std::ceil(cz + reach);
        for (int wy = y0; wy <= y1; wy++) {
            if (wy < 1 || wy >= CHUNK_SY) continue;   // keep bedrock + ceiling
            for (int wz = z0; wz <= z1; wz++)
            for (int wx = x0; wx <= x1; wx++) {
                float dx = (wx - cx), dy = (wy - cy), dz = (wz - cz);
                float dist = std::sqrt(dx*dx + dy*dy + dz*dz);
                if (dist > reach) continue;
                float sphere = dist - radius;
                float cur = sdf_at(wx, wy, wz);
                float nv = (op < 0) ? smax(cur, -sphere, k)   // dig: union with air
                                    : smin(cur,  sphere, k);  // fill: union with solid
                ChunkKey kk = world_to_chunk(wx, wz);
                Chunk* c = get_chunk(kk, true);
                int lx = floormod(wx, CHUNK_SX), lz = floormod(wz, CHUNK_SZ);
                bool was_solid = (cur < 0.0f);
                bool now_solid = (nv  < 0.0f);
                c->set_sdf(lx, wy, lz, nv);
                if (was_solid != now_solid) {
                    BlockId old = c->get(lx, wy, lz);
                    if (flips && was_solid && !now_solid && old != BLOCK_AIR && old != BLOCK_BEDROCK)
                        flips->push_back({wx, wy, wz, (int)old});
                    // keep the discrete block grid in sync for physics/raycast
                    c->set(lx, wy, lz, now_solid ? (old == BLOCK_AIR ? BLOCK_DIRT : old) : BLOCK_AIR);
                }
                c->dirty_mesh = true; c->dirty_save = true;
            }
        }
        // mark neighbouring chunks dirty along the touched border span
        for (int wz = z0; wz <= z1; wz++)
        for (int wx = x0; wx <= x1; wx++) mark_dirty(world_to_chunk(wx, wz));
    }

    static float smin(float a, float b, float k) {
        float h = std::max(k - std::fabs(a - b), 0.0f) / k;
        return std::min(a, b) - h*h*k*0.25f;
    }
    static float smax(float a, float b, float k) { return -smin(-a, -b, k); }

    std::unordered_map<ChunkKey, Chunk, ChunkKeyHash>& chunks() { return chunks_; }

private:
    Noise noise_;
    std::unordered_map<ChunkKey, Chunk, ChunkKeyHash> chunks_;

    // Cache for MC mesher: call prime_height_cache(base_x, base_z, SX+2, SZ+2)
    // before meshing to avoid recomputing surface_height_f per voxel column.
    // The cache stores h values for columns [base_x-1..base_x+SX+1] x [base_z-1..base_z+SZ+1].
    // cx0,cz0 are the base world coords passed to prime_height_cache.
    struct HeightCache {
        int base_x=0, base_z=0, stride=0;
        std::vector<float> h;
        float get(int wx, int wz) const {
            int lx = wx - base_x, lz = wz - base_z;
            if (lx < 0 || lz < 0 || lx >= stride || lz >= (int)h.size()/stride) return 0.0f;
            return h[lz * stride + lx];
        }
    };
    HeightCache _hcache;


    void generate(Chunk& c) {
        for (int lz = 0; lz < CHUNK_SZ; lz++)
        for (int lx = 0; lx < CHUNK_SX; lx++) {
            int wx = c.key.cx * CHUNK_SX + lx;
            int wz = c.key.cz * CHUNK_SZ + lz;
            int h = surface_height(wx, wz);
            if (h < 1) h = 1; if (h >= CHUNK_SY) h = CHUNK_SY - 1;
            for (int y = 0; y <= h; y++) {
                BlockId b;
                if (y == 0)                 b = BLOCK_BEDROCK;
                else if (y < h - 4)         b = BLOCK_STONE;
                else if (y < h)             b = (h <= sea_level + 1) ? BLOCK_SAND : BLOCK_DIRT;
                else /* y == h, surface */  b = surface_block(wx, wz, h);
                // ore pockets in stone
                if (b == BLOCK_STONE) b = maybe_ore(wx, y, wz);
                c.set(lx, y, lz, b);
            }
            // water fill up to sea level
            for (int y = h + 1; y <= sea_level; y++) c.set(lx, y, lz, BLOCK_WATER);
            // trees (sparse, deterministic per column)
            if (h > sea_level + 1) maybe_tree(c, lx, lz, wx, wz, h);
        }
        c.generated = true;
    }

    BlockId surface_block(int wx, int wz, int h) {
        if (h <= sea_level + 1) return BLOCK_SAND;
        if (h > sea_level + 38) return BLOCK_SNOW;
        return BLOCK_GRASS;
    }

    BlockId maybe_ore(int wx, int wy, int wz) {
        float r = noise_.rand2(wx * 31 + wy * 7, wz * 17 + wy * 3);
        if (wy < 16  && r > 0.992f) return BLOCK_DIAMOND_ORE;
        if (wy < 32  && r > 0.985f) return BLOCK_GOLD_ORE;
        if (wy < 48  && r > 0.975f) return BLOCK_IRON_ORE;
        if (r > 0.965f)             return BLOCK_COAL_ORE;
        return BLOCK_STONE;
    }

    void maybe_tree(Chunk& c, int lx, int lz, int wx, int wz, int h) {
        // keep trunks away from chunk borders so the (wider) canopy stays inside
        if (lx < 3 || lx > CHUNK_SX - 4 || lz < 3 || lz > CHUNK_SZ - 4) return;
        if (noise_.rand2(wx, wz) < 0.985f) return;

        // Classic full oak: tall trunk + a rounded multi-layer canopy that
        // actually fills in (no see-through hollow). Canopy is built bottom-up
        // with a radius that bulges in the middle and tapers to a tip, and we
        // fill every cell within the layer radius (distance test), so leaves are
        // solid rather than a thin ring.
        int trunk = 5 + (int)(noise_.rand2(wx + 99, wz - 99) * 3.0f); // 5..7
        for (int i = 1; i <= trunk; i++) c.set(lx, h + i, lz, BLOCK_LOG);
        int top = h + trunk;

        // Canopy spans from a couple blocks below the top up past it. Radius per
        // layer: widest in the lower-middle, shrinking to 1 at the crown.
        // layer offset dy -> radius
        const int   lo = -3, hi = 2;
        for (int dy = lo; dy <= hi; dy++) {
            int y = top + dy;
            if (y < 0 || y >= CHUNK_SY) continue;
            int r;
            if      (dy <= -2) r = 2;      // lower canopy: broad
            else if (dy <=  0) r = 2;      // mid: broadest
            else if (dy ==  1) r = 1;      // upper: narrowing
            else               r = 0;      // crown tip
            float rr = (r + 0.5f) * (r + 0.5f);
            for (int dx = -r; dx <= r; dx++)
            for (int dz = -r; dz <= r; dz++) {
                if (dx*dx + dz*dz > rr) continue;          // round the corners
                int x = lx + dx, z = lz + dz;
                if (x < 0 || x >= CHUNK_SX || z < 0 || z >= CHUNK_SZ) continue;
                // don't overwrite the trunk core (keeps the trunk visible)
                if (dx == 0 && dz == 0 && y <= top) continue;
                if (c.get(x, y, z) == BLOCK_AIR) c.set(x, y, z, BLOCK_LEAVES);
            }
        }
        // a single leaf cap on the very top for a pointed crown
        if (top + (hi) + 1 < CHUNK_SY && c.get(lx, top + hi + 1, lz) == BLOCK_AIR)
            c.set(lx, top + hi + 1, lz, BLOCK_LEAVES);
    }
};

} // namespace sdfcraft
