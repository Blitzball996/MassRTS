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
        Noise& n = noise_;
        float base = n.fbm((float)wx, (float)wz, 1.0f / 96.0f, 5);  // 0..1 rolling
        float mountain = n.fbm((float)wx + 4000.0f, (float)wz - 4000.0f, 1.0f / 220.0f, 4);
        mountain = mountain * mountain;                              // sharpen peaks
        float h = sea_level - 6.0f + base * 26.0f + mountain * 48.0f;
        return (int)h;
    }

    std::unordered_map<ChunkKey, Chunk, ChunkKeyHash>& chunks() { return chunks_; }

private:
    Noise noise_;
    std::unordered_map<ChunkKey, Chunk, ChunkKeyHash> chunks_;

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
        // keep trunks away from chunk borders so canopy stays inside this chunk
        if (lx < 2 || lx > CHUNK_SX - 3 || lz < 2 || lz > CHUNK_SZ - 3) return;
        if (noise_.rand2(wx, wz) < 0.985f) return;
        int trunk = 4 + (int)(noise_.rand2(wx + 99, wz - 99) * 2.0f);
        for (int i = 1; i <= trunk; i++) c.set(lx, h + i, lz, BLOCK_LOG);
        int top = h + trunk;
        for (int dy = -1; dy <= 1; dy++)
        for (int dx = -2; dx <= 2; dx++)
        for (int dz = -2; dz <= 2; dz++) {
            if (abs(dx) == 2 && abs(dz) == 2) continue;
            int x = lx + dx, z = lz + dz, y = top + dy;
            if (x < 0 || x >= CHUNK_SX || z < 0 || z >= CHUNK_SZ) continue;
            if (c.get(x, y, z) == BLOCK_AIR) c.set(x, y, z, BLOCK_LEAVES);
        }
        c.set(lx, top + 1, lz, BLOCK_LEAVES);
    }
};

} // namespace sdfcraft
