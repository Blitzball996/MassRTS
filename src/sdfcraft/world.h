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
#include <set>
#include <algorithm>
#include <unordered_map>
#include <cmath>
#include <glm/glm.hpp>

namespace sdfcraft {

static constexpr int CHUNK_SX = 16;   // blocks per chunk on X
static constexpr int CHUNK_SZ = 16;   // blocks per chunk on Z
static constexpr int CHUNK_SY = 128;  // world height (single vertical chunk)
static constexpr float BLOCK_SIZE = 1.0f;

// {x,y,z, oldBlockId} for each block a carve/edit flipped (shared by world_ops
// and the net replicator; defined here so neither needs to include the other).
using BlockFlips = std::vector<std::array<int,4>>;

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

    // Height lookup that is IDENTICAL whether or not the column is in the warm
    // cache: in-range -> cached value, out-of-range -> recompute with the exact
    // same surface_height_f() the cache was filled from. Using one formula
    // everywhere is what keeps the isosurface continuous across chunk seams.
    // (The previous version fell back to analytic_field(), a DIFFERENT float-coord
    // formula, so the cached cells and the fallback cells disagreed at the cache
    // boundary and tore the mesh — even in shallow layers.)
    float cached_h(int wx, int wz) {
        float h = _hcache.get(wx, wz);
        if (h != HeightCache::MISS) return h;
        return surface_height_f((float)wx, (float)wz);
    }

    // Fast analytic SDF that uses the pre-warmed height cache. Internally and at
    // every chunk boundary it samples the SAME int-snapped height field, so the
    // surface is fully continuous and matches what the mesher draws.
    float analytic_field_cached(float wx, float wy, float wz) {
        int ix = (int)std::floor(wx), iz = (int)std::floor(wz);
        float h   = cached_h(ix,   iz);
        float hxp = cached_h(ix+1, iz);
        float hxm = cached_h(ix-1, iz);
        float hzp = cached_h(ix,   iz+1);
        float hzm = cached_h(ix,   iz-1);
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

    // === Tree physics: check if LOG/LEAVES are floating, trigger gravity collapse ===
    // When a tree block is broken, check connected tree blocks above for support.
    // If unsupported (no path to ground), they fall and drop as items.
    void check_tree_collapse(int wx, int wy, int wz, std::vector<std::array<int,4>>* drops = nullptr) {
        // Flood-fill upward from the break point to find all connected LOG/LEAVES
        std::vector<std::array<int,3>> to_check;
        std::set<std::array<int,3>> visited;
        std::vector<std::array<int,3>> floating;
        
        // Start from blocks above the broken one
        for (int dy = 1; dy <= 16; dy++) {
            int y = wy + dy;
            if (y >= CHUNK_SY) break;
            BlockId b = get_block(wx, y, wz);
            if (b == BLOCK_LOG || b == BLOCK_LEAVES) {
                to_check.push_back({wx, y, wz});
                break;
            }
        }
        
        // BFS to find connected tree blocks
        while (!to_check.empty()) {
            auto [x, y, z] = to_check.back();
            to_check.pop_back();
            
            if (visited.count({x, y, z})) continue;
            visited.insert({x, y, z});
            
            BlockId b = get_block(x, y, z);
            if (b != BLOCK_LOG && b != BLOCK_LEAVES) continue;
            
            floating.push_back({x, y, z});
            
            // Check 6 neighbors (3x3x3 for branches)
            for (int dx = -1; dx <= 1; dx++)
            for (int dy = -1; dy <= 1; dy++)
            for (int dz = -1; dz <= 1; dz++) {
                if (dx == 0 && dy == 0 && dz == 0) continue;
                int nx = x + dx, ny = y + dy, nz = z + dz;
                if (ny < 0 || ny >= CHUNK_SY) continue;
                if (!visited.count({nx, ny, nz})) {
                    BlockId nb = get_block(nx, ny, nz);
                    if (nb == BLOCK_LOG || nb == BLOCK_LEAVES) {
                        to_check.push_back({nx, ny, nz});
                    }
                }
            }
        }
        
        // Check if any floating block has ground support
        bool has_support = false;
        for (auto [x, y, z] : floating) {
            // Check if this block connects to ground (non-tree solid block below)
            for (int cy = y - 1; cy >= 0; cy--) {
                BlockId below = get_block(x, cy, z);
                if (below == BLOCK_LOG || below == BLOCK_LEAVES) continue;
                if (below != BLOCK_AIR && below != BLOCK_WATER) {
                    has_support = true;
                    break;
                }
                break; // hit air/water, no support this column
            }
            if (has_support) break;
        }
        
        // If no support, remove all floating blocks and drop them
        if (!has_support && !floating.empty()) {
            for (auto [x, y, z] : floating) {
                BlockId b = get_block(x, y, z);
                set_block(x, y, z, BLOCK_AIR);
                if (drops) drops->push_back({x, y, z, (int)b});
            }
        }
    }

    // === Advanced terrain sculpting tools (MassRTS-style) ===
    
    // Raise terrain: push surface up smoothly
    void raise_terrain(float cx, float cy, float cz, float radius, float strength) {
        float reach = radius + 2.0f;
        int x0 = (int)std::floor(cx - reach), x1 = (int)std::ceil(cx + reach);
        int y0 = (int)std::floor(cy - reach), y1 = (int)std::ceil(cy + reach);
        int z0 = (int)std::floor(cz - reach), z1 = (int)std::ceil(cz + reach);
        
        for (int wy = y0; wy <= y1; wy++) {
            if (wy < 1 || wy >= CHUNK_SY - 1) continue;
            for (int wz = z0; wz <= z1; wz++)
            for (int wx = x0; wx <= x1; wx++) {
                float dx = (wx - cx), dy = (wy - cy), dz = (wz - cz);
                float dist = std::sqrt(dx*dx + dy*dy + dz*dz);
                if (dist > radius) continue;
                
                // Smooth falloff (cosine bell)
                float falloff = 0.5f * (1.0f + std::cos(3.14159265f * (dist / radius)));
                float offset = -strength * falloff; // negative = push into solid
                
                ChunkKey kk = world_to_chunk(wx, wz);
                Chunk* c = get_chunk(kk, true);
                int lx = floormod(wx, CHUNK_SX), lz = floormod(wz, CHUNK_SZ);
                float cur = c->get_sdf(lx, wy, lz);
                c->set_sdf(lx, wy, lz, cur + offset);
                c->dirty_mesh = true; c->dirty_save = true;
            }
        }
        for (int wz = z0; wz <= z1; wz++)
        for (int wx = x0; wx <= x1; wx++) mark_dirty(world_to_chunk(wx, wz));
    }
    
    // Smooth terrain: average nearby SDF values (reduces marching cubes artifacts)
    void smooth_terrain(float cx, float cy, float cz, float radius, float strength) {
        float reach = radius + 1.0f;
        int x0 = (int)std::floor(cx - reach), x1 = (int)std::ceil(cx + reach);
        int y0 = (int)std::floor(cy - reach), y1 = (int)std::ceil(cy + reach);
        int z0 = (int)std::floor(cz - reach), z1 = (int)std::ceil(cz + reach);
        
        // Collect current SDF values first (avoid feedback)
        struct Entry { int wx, wy, wz; float avg; };
        std::vector<Entry> updates;
        
        for (int wy = y0; wy <= y1; wy++) {
            if (wy < 1 || wy >= CHUNK_SY - 1) continue;
            for (int wz = z0; wz <= z1; wz++)
            for (int wx = x0; wx <= x1; wx++) {
                float dx = (wx - cx), dy = (wy - cy), dz = (wz - cz);
                float dist = std::sqrt(dx*dx + dy*dy + dz*dz);
                if (dist > radius) continue;
                
                // Average 26-neighbors (3x3x3 kernel) for better smoothing
                float sum = 0.0f;
                int cnt = 0;
                for (int ddy = -1; ddy <= 1; ddy++)
                for (int ddz = -1; ddz <= 1; ddz++)
                for (int ddx = -1; ddx <= 1; ddx++) {
                    sum += sdf_at(wx+ddx, wy+ddy, wz+ddz);
                    cnt++;
                }
                float avg = sum / (float)cnt;
                
                float falloff = 0.5f * (1.0f + std::cos(3.14159265f * (dist / radius)));
                updates.push_back({wx, wy, wz, avg * falloff * strength + sdf_at(wx,wy,wz) * (1.0f - falloff * strength)});
            }
        }
        
        // Apply updates
        for (auto& e : updates) {
            ChunkKey kk = world_to_chunk(e.wx, e.wz);
            Chunk* c = get_chunk(kk, true);
            int lx = floormod(e.wx, CHUNK_SX), lz = floormod(e.wz, CHUNK_SZ);
            c->set_sdf(lx, e.wy, lz, e.avg);
            c->dirty_mesh = true; c->dirty_save = true;
        }
        
        for (int wz = z0; wz <= z1; wz++)
        for (int wx = x0; wx <= x1; wx++) mark_dirty(world_to_chunk(wx, wz));
    }
    
    // Flatten terrain: bring surface to target height
    void flatten_terrain(float cx, float cy, float cz, float radius, float target_y, float strength) {
        float reach = radius + 1.0f;
        int x0 = (int)std::floor(cx - reach), x1 = (int)std::ceil(cx + reach);
        int y0 = (int)std::floor(cy - reach), y1 = (int)std::ceil(cy + reach);
        int z0 = (int)std::floor(cz - reach), z1 = (int)std::ceil(cz + reach);
        
        for (int wy = y0; wy <= y1; wy++) {
            if (wy < 1 || wy >= CHUNK_SY - 1) continue;
            for (int wz = z0; wz <= z1; wz++)
            for (int wx = x0; wx <= x1; wx++) {
                float dx = (wx - cx), dz = (wz - cz);
                float dist_xz = std::sqrt(dx*dx + dz*dz);
                if (dist_xz > radius) continue;
                
                float falloff = 0.5f * (1.0f + std::cos(3.14159265f * (dist_xz / radius)));
                float target_sdf = (float)wy - target_y; // positive above target, negative below
                
                ChunkKey kk = world_to_chunk(wx, wz);
                Chunk* c = get_chunk(kk, true);
                int lx = floormod(wx, CHUNK_SX), lz = floormod(wz, CHUNK_SZ);
                float cur = c->get_sdf(lx, wy, lz);
                float blend = cur + (target_sdf - cur) * falloff * strength;
                c->set_sdf(lx, wy, lz, blend);
                c->dirty_mesh = true; c->dirty_save = true;
            }
        }
        
        for (int wz = z0; wz <= z1; wz++)
        for (int wx = x0; wx <= x1; wx++) mark_dirty(world_to_chunk(wx, wz));
    }
    
    // Boxify terrain: turn smooth SDF into sharp cubic forms (for dungeon rooms)
    void boxify_terrain(float cx, float cy, float cz, float radius, float strength) {
        float reach = radius + 1.0f;
        int x0 = (int)std::floor(cx - reach), x1 = (int)std::ceil(cx + reach);
        int y0 = (int)std::floor(cy - reach), y1 = (int)std::ceil(cy + reach);
        int z0 = (int)std::floor(cz - reach), z1 = (int)std::ceil(cz + reach);
        
        for (int wy = y0; wy <= y1; wy++) {
            if (wy < 1 || wy >= CHUNK_SY - 1) continue;
            for (int wz = z0; wz <= z1; wz++)
            for (int wx = x0; wx <= x1; wx++) {
                float dx = (wx - cx), dy = (wy - cy), dz = (wz - cz);
                float dist = std::sqrt(dx*dx + dy*dy + dz*dz);
                if (dist > radius) continue;
                
                ChunkKey kk = world_to_chunk(wx, wz);
                Chunk* c = get_chunk(kk, true);
                int lx = floormod(wx, CHUNK_SX), lz = floormod(wz, CHUNK_SZ);
                float cur = c->get_sdf(lx, wy, lz);
                
                // Snap to nearest voxel center: quantize SDF to sharp grid
                float snapped = (cur > 0.0f) ? std::ceil(cur) : std::floor(cur);
                float falloff = 0.5f * (1.0f + std::cos(3.14159265f * (dist / radius)));
                float blend = cur + (snapped - cur) * falloff * strength;
                
                c->set_sdf(lx, wy, lz, blend);
                c->dirty_mesh = true; c->dirty_save = true;
            }
        }
        
        for (int wz = z0; wz <= z1; wz++)
        for (int wx = x0; wx <= x1; wx++) mark_dirty(world_to_chunk(wx, wz));
    }

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
        static constexpr float MISS = -1e30f;   // out-of-range sentinel
        float get(int wx, int wz) const {
            int lx = wx - base_x, lz = wz - base_z;
            if (lx < 0 || lz < 0 || lx >= stride || lz >= (int)h.size()/stride) return MISS;
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
                // Geological depth layers (more realistic underground)
                else if (y < 6)             b = BLOCK_STONE;  // deep stone
                else if (y < h - 8)         b = BLOCK_STONE;  // stone layer
                else if (y < h - 3)         b = (h <= sea_level + 1) ? BLOCK_SAND : BLOCK_DIRT;  // subsoil
                else if (y < h)             b = (h <= sea_level + 1) ? BLOCK_SAND : BLOCK_DIRT;  // soil
                else /* y == h, surface */  b = surface_block(wx, wz, h);
                // ore pockets in deep stone
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
        // Mostly grass world (Minecraft-style green plains)
        if (h <= sea_level) return BLOCK_SAND;  // underwater/beach only
        if (h > sea_level + 42) return BLOCK_SNOW;  // high mountain peaks only
        
        // Everything else is grass (green world)
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
        if (lx < 6 || lx > CHUNK_SX - 7 || lz < 6 || lz > CHUNK_SZ - 7) return;
        // Snowy peaks above this height stay bare (treeline).
        if (h > sea_level + 36) return;
        float density = noise_.rand2(wx, wz);
        if (density < 0.965f) return;   // forests (a touch denser than before)

        // Per-tree random streams (deterministic from world position).
        float r1 = noise_.rand2(wx + 99, wz - 99);
        float r2 = noise_.rand2(wx - 51, wz + 71);
        float r3 = noise_.rand2(wx + 17, wz + 233);

        // Pick a species: pine on cold/high ground, bushy oak elsewhere, with an
        // occasional tall "ancient" variant for skyline variety.
        bool cold = (h > sea_level + 26);
        if (cold || r3 > 0.82f) build_pine(c, lx, lz, h, r1, r2);
        else                    build_oak(c, lx, lz, h, r1, r2, r3 > 0.6f);
    }

    // Rounded broadleaf oak: tapering trunk + a full 3D ellipsoid canopy. The
    // canopy is sampled as a solid spheroid (no stacked-disk gaps) with a small
    // per-voxel noise so the silhouette breaks up naturally instead of reading
    // as a stepped blob.
    // Voxel-style realistic oak: thick trunk (2x2 base), branches, detailed canopy
    void build_oak(Chunk& c, int lx, int lz, int h, float r1, float r2, bool tall) {
        int trunk_height = (tall ? 8 : 5) + (int)(r1 * 4.0f);     // 5..12
        
        // === Thick trunk (2x2 at base, tapering to 1x1) ===
        int taper_point = trunk_height / 2;
        for (int i = 1; i <= trunk_height; i++) {
            if (i <= taper_point) {
                // Thick base (2x2)
                set_local(c, lx,   h + i, lz,   BLOCK_LOG);
                set_local(c, lx+1, h + i, lz,   BLOCK_LOG);
                set_local(c, lx,   h + i, lz+1, BLOCK_LOG);
                set_local(c, lx+1, h + i, lz+1, BLOCK_LOG);
            } else {
                // Thin top (1x1)
                set_local(c, lx, h + i, lz, BLOCK_LOG);
            }
        }
        
        int top = h + trunk_height;
        
        // === Add branches (horizontal LOG extensions) ===
        int branch_start = top - 4;
        if (branch_start < h + 3) branch_start = h + 3;
        
        // 4 main branches in cardinal directions
        for (int b = 0; b < 4; b++) {
            int branch_y = branch_start + (int)(r1 * 3.0f) + b;
            if (branch_y > top - 1) branch_y = top - 1;
            
            int branch_len = 2 + (int)(r2 * 2.0f);  // 2..4 blocks
            int dx = 0, dz = 0;
            if (b == 0) dx = 1;       // +X
            else if (b == 1) dx = -1; // -X
            else if (b == 2) dz = 1;  // +Z
            else dz = -1;             // -Z
            
            for (int i = 1; i <= branch_len; i++) {
                set_local(c, lx + dx * i, branch_y, lz + dz * i, BLOCK_LOG);
                // slight upward tilt
                if (i == branch_len) set_local(c, lx + dx * i, branch_y + 1, lz + dz * i, BLOCK_LOG);
            }
        }
        
        // === Canopy (ellipsoid leaves around branches) ===
        float leanx = (r2 > 0.66f) ? 1.0f : (r2 < 0.33f ? -1.0f : 0.0f);
        float rx = (tall ? 5.0f : 4.0f) + r2 * 1.0f;  // wider canopy
        float ry = (tall ? 4.0f : 3.5f) + r1 * 0.7f;
        int   cy = top - 1;
        int   ir = (int)std::ceil(std::max(rx, ry)) + 1;
        
        for (int dy = -ir; dy <= ir; dy++)
        for (int dx = -ir; dx <= ir; dx++)
        for (int dz = -ir; dz <= ir; dz++) {
            int y = cy + dy;
            if (y < 0 || y >= CHUNK_SY) continue;
            float lean = leanx * ((float)(y - h) / (float)std::max(1, trunk_height)) * 1.2f;
            int x = lx + dx + (int)std::lround(lean), z = lz + dz;
            if (x < 0 || x >= CHUNK_SX || z < 0 || z >= CHUNK_SZ) continue;
            
            float fx = (float)dx / rx, fy = (float)dy / ry, fz = (float)dz / rx;
            float d = fx*fx + fy*fy + fz*fz;
            float n = noise_.rand2((x*131 + y*17), (z*57 + y*91)) * 0.24f;
            
            if (d > 1.0f + n - 0.08f) continue;
            if (c.get(x, y, z) == BLOCK_AIR) c.set(x, y, z, BLOCK_LEAVES);
        }
    }

    // Conifer pine: thicker trunk (2x2 base), dense stacked cone
    void build_pine(Chunk& c, int lx, int lz, int h, float r1, float r2) {
        int trunk_height = 8 + (int)(r1 * 6.0f);  // 8..14 tall
        
        // === Thick trunk (2x2 at base, tapering to 1x1) ===
        int taper_point = trunk_height / 2;
        for (int i = 1; i <= trunk_height; i++) {
            if (i <= taper_point) {
                // Thick base (2x2)
                set_local(c, lx,   h + i, lz,   BLOCK_LOG);
                set_local(c, lx+1, h + i, lz,   BLOCK_LOG);
                set_local(c, lx,   h + i, lz+1, BLOCK_LOG);
                set_local(c, lx+1, h + i, lz+1, BLOCK_LOG);
            } else {
                // Thin top (1x1)
                set_local(c, lx, h + i, lz, BLOCK_LOG);
            }
        }
        
        // === Conical leaf layers ===
        int base = h + 2 + (int)(r2 * 2.0f);  // first ring height
        int topY = h + trunk_height;
        float maxR = 3.8f;  // slightly wider
        
        for (int y = base; y <= topY; y++) {
            float frac = (float)(y - base) / (float)std::max(1, topY - base);
            float rad = (1.0f - frac) * maxR + 0.4f;  // smooth taper
            float rr = rad * rad;
            int ir = (int)std::ceil(rad);
            
            for (int dx = -ir; dx <= ir; dx++)
            for (int dz = -ir; dz <= ir; dz++) {
                float dd = (float)(dx*dx + dz*dz);
                float n = noise_.rand2((lx+dx)*71 + y*13, (lz+dz)*29 + y*53) * 0.6f;
                if (dd > rr + n) continue;
                
                int x = lx + dx, z = lz + dz;
                if (x < 0 || x >= CHUNK_SX || z < 0 || z >= CHUNK_SZ) continue;
                // Don't block the 2x2 trunk core
                if ((dx == 0 || dx == 1) && (dz == 0 || dz == 1) && y <= h + taper_point) continue;
                
                if (c.get(x, y, z) == BLOCK_AIR) c.set(x, y, z, BLOCK_LEAVES);
            }
        }
        
        // Pointed crown
        if (topY + 1 < CHUNK_SY) set_local(c, lx, topY + 1, lz, BLOCK_LEAVES);
        if (topY     < CHUNK_SY) set_local(c, lx, topY,     lz, BLOCK_LEAVES);
    }

    // Set a block by chunk-local coords with bounds guard.
    static void set_local(Chunk& c, int lx, int ly, int lz, BlockId b) {
        if (lx < 0 || lx >= CHUNK_SX || lz < 0 || lz >= CHUNK_SZ) return;
        if (ly < 0 || ly >= CHUNK_SY) return;
        c.set(lx, ly, lz, b);
    }
};

} // namespace sdfcraft
