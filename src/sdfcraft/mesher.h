#pragma once
// =============================================================================
// SDFCraft - Chunk mesher
// -----------------------------------------------------------------------------
// Builds a render mesh for a chunk by emitting only the faces that border a
// non-opaque neighbour (standard hidden-face culling). Cross-chunk neighbour
// lookups go through World so seams between chunks are culled correctly.
// Output is a flat vertex buffer: pos(3) + normal(3) + color(3) per vertex,
// 6 vertices per visible quad. Liquids/non-opaque blocks get a separate list
// so the renderer can draw them after opaque geometry with blending.
// =============================================================================
#include "world.h"
#include <vector>
#include <glm/glm.hpp>

namespace sdfcraft {

struct ChunkMesh {
    std::vector<float> opaque;       // solid blocks
    std::vector<float> transparent;  // water / glass / leaves
    void clear() { opaque.clear(); transparent.clear(); }
    bool empty() const { return opaque.empty() && transparent.empty(); }
};

class Mesher {
public:
    // Face normals and the 4 corner offsets (CCW) for each of the 6 cube faces.
    static void build(World& world, Chunk& c, ChunkMesh& out) {
        out.clear();
        int base_x = c.key.cx * CHUNK_SX;
        int base_z = c.key.cz * CHUNK_SZ;

        for (int ly = 0; ly < CHUNK_SY; ly++)
        for (int lz = 0; lz < CHUNK_SZ; lz++)
        for (int lx = 0; lx < CHUNK_SX; lx++) {
            BlockId b = c.get(lx, ly, lz);
            if (b == BLOCK_AIR) continue;
            int wx = base_x + lx, wy = ly, wz = base_z + lz;
            const BlockDef& def = block_def(b);
            std::vector<float>& dst = (def.opaque) ? out.opaque : out.transparent;

            // For each of 6 directions, draw face if neighbour is non-opaque
            // (and not the same liquid, to avoid internal water faces).
            static const int dirs[6][3] = {
                {+1,0,0},{-1,0,0},{0,+1,0},{0,-1,0},{0,0,+1},{0,0,-1}
            };
            for (int d = 0; d < 6; d++) {
                int nx = wx + dirs[d][0], ny = wy + dirs[d][1], nz = wz + dirs[d][2];
                BlockId nb = neighbor(world, c, lx + dirs[d][0], ly + dirs[d][1], lz + dirs[d][2], nx, ny, nz);
                bool draw;
                if (def.liquid) draw = (nb == BLOCK_AIR); // water: only top/exposed
                else            draw = !block_is_opaque(nb) && nb != b;
                if (!draw) continue;
                glm::vec3 col = (dirs[d][1] > 0) ? def.top_color : def.color;
                // simple directional shade so cubes read as 3D
                float shade = face_shade(d);
                emit_face(dst, glm::vec3((float)wx, (float)wy, (float)wz), d, col * shade);
            }
        }
    }

private:
    static float face_shade(int d) {
        switch (d) {
            case 2: return 1.00f;  // +Y top brightest
            case 3: return 0.55f;  // -Y bottom darkest
            case 0: case 1: return 0.80f; // X sides
            default: return 0.68f; // Z sides
        }
    }

    static BlockId neighbor(World& world, Chunk& c, int lx, int ly, int lz, int wx, int wy, int wz) {
        if (ly < 0 || ly >= CHUNK_SY) return BLOCK_AIR;
        if (lx >= 0 && lx < CHUNK_SX && lz >= 0 && lz < CHUNK_SZ)
            return c.get(lx, ly, lz);
        return world.get_block(wx, wy, wz); // cross-chunk
    }

    static void push_vert(std::vector<float>& v, glm::vec3 p, glm::vec3 n, glm::vec3 col) {
        v.push_back(p.x); v.push_back(p.y); v.push_back(p.z);
        v.push_back(n.x); v.push_back(n.y); v.push_back(n.z);
        v.push_back(col.r); v.push_back(col.g); v.push_back(col.b);
    }

    // Emit one cube face (2 triangles) at integer block origin `o`.
    static void emit_face(std::vector<float>& v, glm::vec3 o, int d, glm::vec3 col) {
        // 8 cube corners (block occupies [o, o+1])
        glm::vec3 p000 = o + glm::vec3(0,0,0), p100 = o + glm::vec3(1,0,0);
        glm::vec3 p010 = o + glm::vec3(0,1,0), p110 = o + glm::vec3(1,1,0);
        glm::vec3 p001 = o + glm::vec3(0,0,1), p101 = o + glm::vec3(1,0,1);
        glm::vec3 p011 = o + glm::vec3(0,1,1), p111 = o + glm::vec3(1,1,1);
        glm::vec3 n, a, b2, c2, dd;
        switch (d) {
            case 0: n={1,0,0};  a=p100;b2=p101;c2=p111;dd=p110; break; // +X
            case 1: n={-1,0,0}; a=p001;b2=p000;c2=p010;dd=p011; break; // -X
            case 2: n={0,1,0};  a=p010;b2=p110;c2=p111;dd=p011; break; // +Y
            case 3: n={0,-1,0}; a=p001;b2=p101;c2=p100;dd=p000; break; // -Y
            case 4: n={0,0,1};  a=p101;b2=p001;c2=p011;dd=p111; break; // +Z
            default:n={0,0,-1}; a=p000;b2=p100;c2=p110;dd=p010; break; // -Z
        }
        push_vert(v, a, n, col); push_vert(v, b2, n, col); push_vert(v, c2, n, col);
        push_vert(v, a, n, col); push_vert(v, c2, n, col); push_vert(v, dd, n, col);
    }
};

} // namespace sdfcraft
