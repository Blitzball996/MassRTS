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
            // Natural terrain is drawn by the smooth Marching-Cubes surface;
            // skip it here so we don't z-fight that surface with cubes. Only
            // "object" blocks (logs/leaves/planks/glass/placed blocks/water)
            // are emitted as discrete cubes — that keeps trees crisply blocky.
            if (block_is_terrain(b)) continue;
            int wx = base_x + lx, wy = ly, wz = base_z + lz;
            const BlockDef& def = block_def(b);
            std::vector<float>& dst = (def.opaque) ? out.opaque : out.transparent;

            // For each of 6 directions, draw face if neighbour is non-opaque
            // (and not the same liquid, to avoid internal water faces). A face
            // bordering terrain is still drawn (terrain is the smooth surface,
            // not a cube neighbour), so tree trunks sitting in the ground keep
            // their sides.
            static const int dirs[6][3] = {
                {+1,0,0},{-1,0,0},{0,+1,0},{0,-1,0},{0,0,+1},{0,0,-1}
            };
            for (int d = 0; d < 6; d++) {
                int nx = wx + dirs[d][0], ny = wy + dirs[d][1], nz = wz + dirs[d][2];
                BlockId nb = neighbor(world, c, lx + dirs[d][0], ly + dirs[d][1], lz + dirs[d][2], nx, ny, nz);
                bool draw;
                if (def.liquid)            draw = (nb == BLOCK_AIR); // water: only exposed
                else if (block_is_terrain(nb)) draw = true;          // expose against smooth ground
                else                       draw = !block_is_opaque(nb) && nb != b;
                if (!draw) continue;
                glm::vec3 col = (dirs[d][1] > 0) ? def.top_color : def.color;
                // simple directional shade so cubes read as 3D
                float shade = face_shade(d);
                // Cube-meshed blocks are all "special" object materials (wood,
                // leaves, water, glass, placed blocks). Encode v_mat as 200+code
                // so the chunk shader takes its discrete-material path — values
                // below 100 are reserved for the MC terrain's earthy DEPTH value.
                emit_face(dst, glm::vec3((float)wx, (float)wy, (float)wz), d,
                          col * shade, 200.0f + block_material(b));
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

    static void push_vert(std::vector<float>& v, glm::vec3 p, glm::vec3 n, glm::vec3 col, float mat) {
        v.push_back(p.x); v.push_back(p.y); v.push_back(p.z);
        v.push_back(n.x); v.push_back(n.y); v.push_back(n.z);
        v.push_back(col.r); v.push_back(col.g); v.push_back(col.b);
        v.push_back(mat);
    }

    // Emit one cube face (2 triangles) at integer block origin `o`.
    static void emit_face(std::vector<float>& v, glm::vec3 o, int dir, glm::vec3 col, float mat) {
        // 8 cube corners (block occupies [o, o+1])
        glm::vec3 p000 = o + glm::vec3(0,0,0), p100 = o + glm::vec3(1,0,0);
        glm::vec3 p010 = o + glm::vec3(0,1,0), p110 = o + glm::vec3(1,1,0);
        glm::vec3 p001 = o + glm::vec3(0,0,1), p101 = o + glm::vec3(1,0,1);
        glm::vec3 p011 = o + glm::vec3(0,1,1), p111 = o + glm::vec3(1,1,1);
        // CCW winding (counter-clockwise when viewed from outside) to match GL_CCW front face.
        // Each face is defined as two triangles with outward normal.
        glm::vec3 n, a, b, c, d;
        switch (dir) {
            case 0: n={1,0,0};  a=p100;b=p110;c=p111;d=p101; break; // +X: reverse order
            case 1: n={-1,0,0}; a=p000;b=p001;c=p011;d=p010; break; // -X: reverse order
            case 2: n={0,1,0};  a=p010;b=p011;c=p111;d=p110; break; // +Y: reverse order
            case 3: n={0,-1,0}; a=p000;b=p100;c=p101;d=p001; break; // -Y: reverse order
            case 4: n={0,0,1};  a=p001;b=p101;c=p111;d=p011; break; // +Z: reverse order
            default:n={0,0,-1}; a=p100;b=p000;c=p010;d=p110; break; // -Z: reverse order
        }
        push_vert(v, a, n, col, mat); push_vert(v, b, n, col, mat); push_vert(v, c, n, col, mat);
        push_vert(v, a, n, col, mat); push_vert(v, c, n, col, mat); push_vert(v, d, n, col, mat);
    }
};

} // namespace sdfcraft
