#pragma once
// =============================================================================
// SDFCraft - Marching Cubes mesher (replaces blocky face mesher for solid terrain)
// -----------------------------------------------------------------------------
// Builds a smooth isosurface from the block occupancy field.
// SDF value at each corner: -0.5 if solid block, +0.5 if air.
// The isosurface (isovalue = 0) falls exactly at block boundaries, but
// neighbour trilinear interpolation gives smooth normals + rounded edges.
//
// Vertex format: 10 floats (px py pz  nx ny nz  r g b  mat) — same as cube mesher.
// Only emits opaque geometry into ChunkMesh::opaque; non-opaque blocks
// (water/glass/leaves) stay in the cube mesher's transparent pass.
// =============================================================================
#include "world.h"
#include "blocks.h"
#include "mesher.h"   // for ChunkMesh
#include <glm/glm.hpp>
#include <cmath>
#include <array>

namespace sdfcraft {

// ---------------------------------------------------------------------------
// Marching-Cubes tables (standard Lorensen-Cline / Paul Bourke, all 256 rows)
// ---------------------------------------------------------------------------
static const int MC_EDGE_TABLE[256] = {
0x0  ,0x109,0x203,0x30a,0x406,0x50f,0x605,0x70c,
0x80c,0x905,0xa0f,0xb06,0xc0a,0xd03,0xe09,0xf00,
0x190,0x99 ,0x393,0x29a,0x596,0x49f,0x795,0x69c,
0x99c,0x895,0xb9f,0xa96,0xd9a,0xc93,0xf99,0xe90,
0x230,0x339,0x33 ,0x13a,0x636,0x73f,0x435,0x53c,
0xa3c,0xb35,0x83f,0x936,0xe3a,0xf33,0xc39,0xd30,
0x3a0,0x2a9,0x1a3,0xaa ,0x7a6,0x6af,0x5a5,0x4ac,
0xbac,0xaa5,0x9af,0x8a6,0xfaa,0xea3,0xda9,0xca0,
0x460,0x569,0x663,0x76a,0x66 ,0x16f,0x265,0x36c,
0xc6c,0xd65,0xe6f,0xf66,0x86a,0x963,0xa69,0xb60,
0x5f0,0x4f9,0x7f3,0x6fa,0x1f6,0xff ,0x3f5,0x2fc,
0xdfc,0xcf5,0xfff,0xef6,0x9fa,0x8f3,0xbf9,0xaf0,
0x650,0x759,0x453,0x55a,0x256,0x35f,0x55 ,0x15c,
0xe5c,0xf55,0xc5f,0xd56,0xa5a,0xb53,0x859,0x950,
0x7c0,0x6c9,0x5c3,0x4ca,0x3c6,0x2cf,0x1c5,0xcc ,
0xfcc,0xec5,0xdcf,0xcc6,0xbca,0xac3,0x9c9,0x8c0,
0x8c0,0x9c9,0xac3,0xbca,0xcc6,0xdcf,0xec5,0xfcc,
0xcc ,0x1c5,0x2cf,0x3c6,0x4ca,0x5c3,0x6c9,0x7c0,
0x950,0x859,0xb53,0xa5a,0xd56,0xc5f,0xf55,0xe5c,
0x15c,0x55 ,0x35f,0x256,0x55a,0x453,0x759,0x650,
0xaf0,0xbf9,0x8f3,0x9fa,0xef6,0xfff,0xcf5,0xdfc,
0x2fc,0x3f5,0xff ,0x1f6,0x6fa,0x7f3,0x4f9,0x5f0,
0xb60,0xa69,0x963,0x86a,0xf66,0xe6f,0xd65,0xc6c,
0x36c,0x265,0x16f,0x66 ,0x76a,0x663,0x569,0x460,
0xca0,0xda9,0xea3,0xfaa,0x8a6,0x9af,0xaa5,0xbac,
0x4ac,0x5a5,0x6af,0x7a6,0xaa ,0x1a3,0x2a9,0x3a0,
0xd30,0xc39,0xf33,0xe3a,0x936,0x83f,0xb35,0xa3c,
0x53c,0x435,0x73f,0x636,0x13a,0x33 ,0x339,0x230,
0xe90,0xf99,0xc93,0xd9a,0xa96,0xb9f,0x895,0x99c,
0x69c,0x795,0x49f,0x596,0x29a,0x393,0x99 ,0x190,
0xf00,0xe09,0xd03,0xc0a,0xb06,0xa0f,0x905,0x80c,
0x70c,0x605,0x50f,0x406,0x30a,0x203,0x109,0x0
};

// Triangle table: 256 entries, each up to 5 triangles (15 ints), -1 terminated.
static const int MC_TRI_TABLE[256][16] = {
{-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{0,8,3,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{0,1,9,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{1,8,3,9,8,1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{1,2,10,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{0,8,3,1,2,10,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{9,2,10,0,2,9,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{2,8,3,2,10,8,10,9,8,-1,-1,-1,-1,-1,-1,-1},
{3,11,2,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{0,11,2,8,11,0,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{1,9,0,2,3,11,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{1,11,2,1,9,11,9,8,11,-1,-1,-1,-1,-1,-1,-1},
{3,10,1,11,10,3,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{0,10,1,0,8,10,8,11,10,-1,-1,-1,-1,-1,-1,-1},
{3,9,0,3,11,9,11,10,9,-1,-1,-1,-1,-1,-1,-1},
{9,8,10,10,8,11,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{4,7,8,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{4,3,0,7,3,4,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{0,1,9,8,4,7,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{4,1,9,4,7,1,7,3,1,-1,-1,-1,-1,-1,-1,-1},
{1,2,10,8,4,7,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{3,4,7,3,0,4,1,2,10,-1,-1,-1,-1,-1,-1,-1},
{9,2,10,9,0,2,8,4,7,-1,-1,-1,-1,-1,-1,-1},
{2,10,9,2,9,7,2,7,3,7,9,4,-1,-1,-1,-1},
{8,4,7,3,11,2,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{11,4,7,11,2,4,2,0,4,-1,-1,-1,-1,-1,-1,-1},
{9,0,1,8,4,7,2,3,11,-1,-1,-1,-1,-1,-1,-1},
{4,7,11,9,4,11,9,11,2,9,2,1,-1,-1,-1,-1},
{3,10,1,3,11,10,7,8,4,-1,-1,-1,-1,-1,-1,-1},
{1,11,10,1,4,11,1,0,4,7,11,4,-1,-1,-1,-1},
{4,7,8,9,0,11,9,11,10,11,0,3,-1,-1,-1,-1},
{4,7,11,4,11,9,9,11,10,-1,-1,-1,-1,-1,-1,-1},
{9,5,4,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{9,5,4,0,8,3,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{0,5,4,1,5,0,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{8,5,4,8,3,5,3,1,5,-1,-1,-1,-1,-1,-1,-1},
{1,2,10,9,5,4,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{3,0,8,1,2,10,4,9,5,-1,-1,-1,-1,-1,-1,-1},
{5,2,10,5,4,2,4,0,2,-1,-1,-1,-1,-1,-1,-1},
{2,10,5,3,2,5,3,5,4,3,4,8,-1,-1,-1,-1},
{9,5,4,2,3,11,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{0,11,2,0,8,11,4,9,5,-1,-1,-1,-1,-1,-1,-1},
{0,5,4,0,1,5,2,3,11,-1,-1,-1,-1,-1,-1,-1},
{2,1,5,2,5,8,2,8,11,4,8,5,-1,-1,-1,-1},
{10,3,11,10,1,3,9,5,4,-1,-1,-1,-1,-1,-1,-1},
{4,9,5,0,8,1,8,10,1,8,11,10,-1,-1,-1,-1},
{5,4,0,5,0,11,5,11,10,11,0,3,-1,-1,-1,-1},
{5,4,8,5,8,10,10,8,11,-1,-1,-1,-1,-1,-1,-1},
{9,7,8,5,7,9,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{9,3,0,9,5,3,5,7,3,-1,-1,-1,-1,-1,-1,-1},
{0,7,8,0,1,7,1,5,7,-1,-1,-1,-1,-1,-1,-1},
{1,5,3,3,5,7,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{9,7,8,9,5,7,10,1,2,-1,-1,-1,-1,-1,-1,-1},
{10,1,2,9,5,0,5,3,0,5,7,3,-1,-1,-1,-1},
{8,0,2,8,2,5,8,5,7,10,5,2,-1,-1,-1,-1},
{2,10,5,2,5,3,3,5,7,-1,-1,-1,-1,-1,-1,-1},
{7,9,5,7,8,9,3,11,2,-1,-1,-1,-1,-1,-1,-1},
{9,5,7,9,7,2,9,2,0,2,7,11,-1,-1,-1,-1},
{2,3,11,0,1,8,1,7,8,1,5,7,-1,-1,-1,-1},
{11,2,1,11,1,7,7,1,5,-1,-1,-1,-1,-1,-1,-1},
{9,5,8,8,5,7,10,1,3,10,3,11,-1,-1,-1,-1},
{5,7,0,5,0,9,7,11,0,1,0,10,11,10,0,-1},
{11,10,0,11,0,3,10,5,0,8,0,7,5,7,0,-1},
{11,10,5,7,11,5,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{10,6,5,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{0,8,3,5,10,6,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{9,0,1,5,10,6,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{1,8,3,1,9,8,5,10,6,-1,-1,-1,-1,-1,-1,-1},
{1,6,5,2,6,1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{1,6,5,1,2,6,3,0,8,-1,-1,-1,-1,-1,-1,-1},
{9,6,5,9,0,6,0,2,6,-1,-1,-1,-1,-1,-1,-1},
{5,9,8,5,8,2,5,2,6,3,2,8,-1,-1,-1,-1},
{2,3,11,10,6,5,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{11,0,8,11,2,0,10,6,5,-1,-1,-1,-1,-1,-1,-1},
{0,1,9,2,3,11,5,10,6,-1,-1,-1,-1,-1,-1,-1},
{5,10,6,1,9,2,9,11,2,9,8,11,-1,-1,-1,-1},
{6,3,11,6,5,3,5,1,3,-1,-1,-1,-1,-1,-1,-1},
{0,8,11,0,11,5,0,5,1,5,11,6,-1,-1,-1,-1},
{3,11,6,0,3,6,0,6,5,0,5,9,-1,-1,-1,-1},
{6,5,9,6,9,11,11,9,8,-1,-1,-1,-1,-1,-1,-1},
{5,10,6,4,7,8,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{4,3,0,4,7,3,6,5,10,-1,-1,-1,-1,-1,-1,-1},
{1,9,0,5,10,6,8,4,7,-1,-1,-1,-1,-1,-1,-1},
{10,6,5,1,9,7,1,7,3,7,9,4,-1,-1,-1,-1},
{6,1,2,6,5,1,4,7,8,-1,-1,-1,-1,-1,-1,-1},
{1,2,5,5,2,6,3,0,4,3,4,7,-1,-1,-1,-1},
{8,4,7,9,0,5,0,6,5,0,2,6,-1,-1,-1,-1},
{7,3,9,7,9,4,3,2,9,5,9,6,2,6,9,-1},
{3,11,2,7,8,4,10,6,5,-1,-1,-1,-1,-1,-1,-1},
{5,10,6,4,7,2,4,2,0,2,7,11,-1,-1,-1,-1},
{0,1,9,4,7,8,2,3,11,5,10,6,-1,-1,-1,-1},
{9,2,1,9,11,2,9,4,11,7,11,4,5,10,6,-1},
{8,4,7,3,11,5,3,5,1,5,11,6,-1,-1,-1,-1},
{5,1,11,5,11,6,1,0,11,7,11,4,0,4,11,-1},
{0,5,9,0,6,5,0,3,6,11,6,3,8,4,7,-1},
{6,5,9,6,9,11,4,7,9,7,11,9,-1,-1,-1,-1},
{10,4,9,6,4,10,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{4,10,6,4,9,10,0,8,3,-1,-1,-1,-1,-1,-1,-1},
{10,0,1,10,6,0,6,4,0,-1,-1,-1,-1,-1,-1,-1},
{8,3,1,8,1,6,8,6,4,6,1,10,-1,-1,-1,-1},
{1,4,9,1,2,4,2,6,4,-1,-1,-1,-1,-1,-1,-1},
{3,0,8,1,2,9,2,4,9,2,6,4,-1,-1,-1,-1},
{0,2,4,4,2,6,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{8,3,2,8,2,4,4,2,6,-1,-1,-1,-1,-1,-1,-1},
{10,4,9,10,6,4,11,2,3,-1,-1,-1,-1,-1,-1,-1},
{0,8,2,2,8,11,4,9,10,4,10,6,-1,-1,-1,-1},
{3,11,2,0,1,6,0,6,4,6,1,10,-1,-1,-1,-1},
{6,4,1,6,1,10,4,8,1,2,1,11,8,11,1,-1},
{9,6,4,9,3,6,9,1,3,11,6,3,-1,-1,-1,-1},
{8,11,1,8,1,0,11,6,1,9,1,4,6,4,1,-1},
{3,11,6,3,6,0,0,6,4,-1,-1,-1,-1,-1,-1,-1},
{6,4,8,11,6,8,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{7,10,6,7,8,10,8,9,10,-1,-1,-1,-1,-1,-1,-1},
{0,7,3,0,10,7,0,9,10,6,7,10,-1,-1,-1,-1},
{10,6,7,1,10,7,1,7,8,1,8,0,-1,-1,-1,-1},
{10,6,7,10,7,1,1,7,3,-1,-1,-1,-1,-1,-1,-1},
{1,2,6,1,6,8,1,8,9,8,6,7,-1,-1,-1,-1},
{2,6,9,2,9,1,6,7,9,0,9,3,7,3,9,-1},
{7,8,0,7,0,6,6,0,2,-1,-1,-1,-1,-1,-1,-1},
{7,3,2,6,7,2,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{2,3,11,10,6,8,10,8,9,8,6,7,-1,-1,-1,-1},
{2,0,7,2,7,11,0,9,7,6,7,10,9,10,7,-1},
{1,8,0,1,7,8,1,10,7,6,7,10,2,3,11,-1},
{11,2,1,11,1,7,10,6,1,6,7,1,-1,-1,-1,-1},
{8,9,6,8,6,7,9,1,6,11,6,3,1,3,6,-1},
{0,9,1,11,6,7,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{7,8,0,7,0,6,3,11,0,11,6,0,-1,-1,-1,-1},
{7,11,6,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{7,6,11,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{3,0,8,11,7,6,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{0,1,9,11,7,6,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{8,1,9,8,3,1,11,7,6,-1,-1,-1,-1,-1,-1,-1},
{10,1,2,6,11,7,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{1,2,10,3,0,8,6,11,7,-1,-1,-1,-1,-1,-1,-1},
{2,9,0,2,10,9,6,11,7,-1,-1,-1,-1,-1,-1,-1},
{6,11,7,2,10,3,10,8,3,10,9,8,-1,-1,-1,-1},
{7,2,3,6,2,7,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{7,0,8,7,6,0,6,2,0,-1,-1,-1,-1,-1,-1,-1},
{2,7,6,2,3,7,0,1,9,-1,-1,-1,-1,-1,-1,-1},
{1,6,2,1,8,6,1,9,8,8,7,6,-1,-1,-1,-1},
{10,7,6,10,1,7,1,3,7,-1,-1,-1,-1,-1,-1,-1},
{10,7,6,1,7,10,1,8,7,1,0,8,-1,-1,-1,-1},
{0,3,7,0,7,10,0,10,9,6,10,7,-1,-1,-1,-1},
{7,6,10,7,10,8,8,10,9,-1,-1,-1,-1,-1,-1,-1},
{6,8,4,11,8,6,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{3,6,11,3,0,6,0,4,6,-1,-1,-1,-1,-1,-1,-1},
{8,6,11,8,4,6,9,0,1,-1,-1,-1,-1,-1,-1,-1},
{9,4,6,9,6,3,9,3,1,11,3,6,-1,-1,-1,-1},
{6,8,4,6,11,8,2,10,1,-1,-1,-1,-1,-1,-1,-1},
{1,2,10,3,0,11,0,6,11,0,4,6,-1,-1,-1,-1},
{4,11,8,4,6,11,0,2,9,2,10,9,-1,-1,-1,-1},
{10,9,3,10,3,2,9,4,3,11,3,6,4,6,3,-1},
{8,2,3,8,4,2,4,6,2,-1,-1,-1,-1,-1,-1,-1},
{0,4,2,4,6,2,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{1,9,0,2,3,4,2,4,6,4,3,8,-1,-1,-1,-1},
{1,9,4,1,4,2,2,4,6,-1,-1,-1,-1,-1,-1,-1},
{8,1,3,8,6,1,8,4,6,6,10,1,-1,-1,-1,-1},
{10,1,0,10,0,6,6,0,4,-1,-1,-1,-1,-1,-1,-1},
{4,6,3,4,3,8,6,10,3,0,3,9,10,9,3,-1},
{10,9,4,6,10,4,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{4,9,5,7,6,11,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{0,8,3,4,9,5,11,7,6,-1,-1,-1,-1,-1,-1,-1},
{5,0,1,5,4,0,7,6,11,-1,-1,-1,-1,-1,-1,-1},
{11,7,6,8,3,4,3,5,4,3,1,5,-1,-1,-1,-1},
{9,5,4,10,1,2,7,6,11,-1,-1,-1,-1,-1,-1,-1},
{6,11,7,1,2,10,0,8,3,4,9,5,-1,-1,-1,-1},
{7,6,11,5,4,10,4,2,10,4,0,2,-1,-1,-1,-1},
{3,4,8,3,5,4,3,2,5,10,5,2,11,7,6,-1},
{7,2,3,7,6,2,5,4,9,-1,-1,-1,-1,-1,-1,-1},
{9,5,4,0,8,6,0,6,2,6,8,7,-1,-1,-1,-1},
{3,6,2,3,7,6,1,5,0,5,4,0,-1,-1,-1,-1},
{6,2,8,6,8,7,2,1,8,4,8,5,1,5,8,-1},
{9,5,4,10,1,6,1,7,6,1,3,7,-1,-1,-1,-1},
{1,6,10,1,7,6,1,0,7,8,7,0,9,5,4,-1},
{4,0,10,4,10,5,0,3,10,6,10,7,3,7,10,-1},
{7,6,10,7,10,8,5,4,10,4,8,10,-1,-1,-1,-1},
{6,9,5,6,11,9,11,8,9,-1,-1,-1,-1,-1,-1,-1},
{3,6,11,0,6,3,0,5,6,0,9,5,-1,-1,-1,-1},
{0,11,8,0,5,11,0,1,5,5,6,11,-1,-1,-1,-1},
{6,11,3,6,3,5,5,3,1,-1,-1,-1,-1,-1,-1,-1},
{1,2,10,9,5,11,9,11,8,11,5,6,-1,-1,-1,-1},
{0,11,3,0,6,11,0,9,6,5,6,9,1,2,10,-1},
{11,8,5,11,5,6,8,0,5,10,5,2,0,2,5,-1},
{6,11,3,6,3,5,2,10,3,10,5,3,-1,-1,-1,-1},
{5,8,9,5,2,8,5,6,2,3,8,2,-1,-1,-1,-1},
{9,5,6,9,6,0,0,6,2,-1,-1,-1,-1,-1,-1,-1},
{1,5,8,1,8,0,5,6,8,3,8,2,6,2,8,-1},
{1,5,6,2,1,6,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{1,3,6,1,6,10,3,8,6,5,6,9,8,9,6,-1},
{10,1,0,10,0,6,9,5,0,5,6,0,-1,-1,-1,-1},
{0,3,8,5,6,10,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{10,5,6,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{11,5,10,7,5,11,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{11,5,10,11,7,5,8,3,0,-1,-1,-1,-1,-1,-1,-1},
{5,11,7,5,10,11,1,9,0,-1,-1,-1,-1,-1,-1,-1},
{10,7,5,10,11,7,9,8,1,8,3,1,-1,-1,-1,-1},
{11,1,2,11,7,1,7,5,1,-1,-1,-1,-1,-1,-1,-1},
{0,8,3,1,2,7,1,7,5,7,2,11,-1,-1,-1,-1},
{9,7,5,9,2,7,9,0,2,2,11,7,-1,-1,-1,-1},
{7,5,2,7,2,11,5,9,2,3,2,8,9,8,2,-1},
{2,5,10,2,3,5,3,7,5,-1,-1,-1,-1,-1,-1,-1},
{8,2,0,8,5,2,8,7,5,10,2,5,-1,-1,-1,-1},
{9,0,1,5,10,3,5,3,7,3,10,2,-1,-1,-1,-1},
{9,8,2,9,2,1,8,7,2,10,2,5,7,5,2,-1},
{1,3,5,3,7,5,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{0,8,7,0,7,1,1,7,5,-1,-1,-1,-1,-1,-1,-1},
{9,0,3,9,3,5,5,3,7,-1,-1,-1,-1,-1,-1,-1},
{9,8,7,5,9,7,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{5,8,4,5,10,8,10,11,8,-1,-1,-1,-1,-1,-1,-1},
{5,0,4,5,11,0,5,10,11,11,3,0,-1,-1,-1,-1},
{0,1,9,8,4,10,8,10,11,10,4,5,-1,-1,-1,-1},
{10,11,4,10,4,5,11,3,4,9,4,1,3,1,4,-1},
{2,5,1,2,8,5,2,11,8,4,5,8,-1,-1,-1,-1},
{0,4,11,0,11,3,4,5,11,2,11,1,5,1,11,-1},
{0,2,5,0,5,9,2,11,5,4,5,8,11,8,5,-1},
{9,4,5,2,11,3,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{2,5,10,3,5,2,3,4,5,3,8,4,-1,-1,-1,-1},
{5,10,2,5,2,4,4,2,0,-1,-1,-1,-1,-1,-1,-1},
{3,10,2,3,5,10,3,8,5,4,5,8,0,1,9,-1},
{5,10,2,5,2,4,1,9,2,9,4,2,-1,-1,-1,-1},
{8,4,5,8,5,3,3,5,1,-1,-1,-1,-1,-1,-1,-1},
{0,4,5,1,0,5,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{8,4,5,8,5,3,9,0,5,0,3,5,-1,-1,-1,-1},
{9,4,5,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{4,11,7,4,9,11,9,10,11,-1,-1,-1,-1,-1,-1,-1},
{0,8,3,4,9,7,9,11,7,9,10,11,-1,-1,-1,-1},
{1,10,11,1,11,4,1,4,0,7,4,11,-1,-1,-1,-1},
{3,1,4,3,4,8,1,10,4,7,4,11,10,11,4,-1},
{4,11,7,9,11,4,9,2,11,9,1,2,-1,-1,-1,-1},
{9,7,4,9,11,7,9,1,11,2,11,1,0,8,3,-1},
{11,7,4,11,4,2,2,4,0,-1,-1,-1,-1,-1,-1,-1},
{11,7,4,11,4,2,8,3,4,3,2,4,-1,-1,-1,-1},
{2,9,10,2,7,9,2,3,7,7,4,9,-1,-1,-1,-1},
{9,10,7,9,7,4,10,2,7,8,7,0,2,0,7,-1},
{3,7,10,3,10,2,7,4,10,1,10,0,4,0,10,-1},
{1,10,2,8,7,4,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{4,9,1,4,1,7,7,1,3,-1,-1,-1,-1,-1,-1,-1},
{4,9,1,4,1,7,0,8,1,8,7,1,-1,-1,-1,-1},
{4,0,3,7,4,3,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{4,8,7,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{9,10,8,10,11,8,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{3,0,9,3,9,11,11,9,10,-1,-1,-1,-1,-1,-1,-1},
{0,1,10,0,10,8,8,10,11,-1,-1,-1,-1,-1,-1,-1},
{3,1,10,11,3,10,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{1,2,11,1,11,9,9,11,8,-1,-1,-1,-1,-1,-1,-1},
{3,0,9,3,9,11,1,2,9,2,11,9,-1,-1,-1,-1},
{0,2,11,8,0,11,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{3,2,11,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{2,3,8,2,8,10,10,8,9,-1,-1,-1,-1,-1,-1,-1},
{9,10,2,0,9,2,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{2,3,8,2,8,10,0,1,8,1,10,8,-1,-1,-1,-1},
{1,10,2,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{1,3,8,9,1,8,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{0,9,1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{0,3,8,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
{-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1}
};

// Standard cube corner layout (matches the tables above).
// Corner i position offset (x,y,z) within the cell:
static const int MC_CORNER[8][3] = {
    {0,0,0},{1,0,0},{1,0,1},{0,0,1},
    {0,1,0},{1,1,0},{1,1,1},{0,1,1}
};
// Each edge connects two corners:
static const int MC_EDGE_CORNERS[12][2] = {
    {0,1},{1,2},{2,3},{3,0},
    {4,5},{5,6},{6,7},{7,4},
    {0,4},{1,5},{2,6},{3,7}
};

class MCMesher {
public:
    // Continuous SDF sample: negative inside solid, positive in air. Reads the
    // world's analytic + carved float field so the isosurface is smooth (no
    // ±0.5 stair-stepping). Leaves/foliage are excluded from this surface — they
    // are handled by the separate transparent pass.
    static float sample(World& w, int wx, int wy, int wz) {
        return w.sdf_at_cached(wx, wy, wz);
    }

    // Albedo + material class of the nearest solid corner (for vertex color).
    static glm::vec3 solid_color(World& w, int wx, int wy, int wz, bool top, float& mat_out) {
        if (wy < 0 || wy >= CHUNK_SY) wy = glm::clamp(wy, 0, CHUNK_SY - 1);
        BlockId b = w.get_block(wx, wy, wz);
        if (b == BLOCK_AIR || !block_def(b).opaque) {
            // fall back to the nearest solid below — the surface usually sits on
            // it. Search a couple of voxels so a slight surface/grid mismatch on
            // a freshly-dug wall doesn't fall through to the neutral default
            // (which used to paint bright tan rims around every dug hole).
            for (int dy = 1; dy <= 3; dy++) {
                BlockId d = w.get_block(wx, glm::max(wy - dy, 0), wz);
                if (d != BLOCK_AIR && block_def(d).opaque) { b = d; break; }
            }
        }
        if (b == BLOCK_AIR) { mat_out = (float)MAT_ROCK; return glm::vec3(0.32f, 0.30f, 0.28f); }
        const BlockDef& def = block_def(b);
        mat_out = block_material(b);
        return top ? def.top_color : def.color;
    }

    static void build(World& world, Chunk& c, ChunkMesh& out) {
        const int base_x = c.key.cx * CHUNK_SX;
        const int base_z = c.key.cz * CHUNK_SZ;

        // Prime the height cache wide enough for the gradient ring below.
        world.prime_height_cache(base_x - 2, base_z - 2, CHUNK_SX + 6, CHUNK_SZ + 6);

        // --- Dense local SDF field (the real optimisation) -------------------
        // The old mesher recomputed the continuous field per *vertex* for both
        // the isosurface AND a 6x8-sample normal — hundreds of 5-octave noise
        // evals per vertex. We now sample the field ONCE into a local buffer that
        // covers the chunk plus a 1-voxel gradient ring, then march + shade from
        // memory (O(1) reads, zero noise in the inner loops). This alone turns a
        // ~20-40ms chunk build into well under a millisecond.
        const int wx0 = base_x - 2, wz0 = base_z - 2, wy0 = -2;
        const int NX = CHUNK_SX + 6, NZ = CHUNK_SZ + 6, NY = CHUNK_SY + 4;
        static thread_local std::vector<float> field;
        field.resize((size_t)NX * NY * NZ);
        auto bidx = [=](int ix, int iy, int iz) { return ((size_t)iy * NZ + iz) * NX + ix; };
        for (int iz = 0; iz < NZ; iz++)
        for (int ix = 0; ix < NX; ix++) {
            int wx = wx0 + ix, wz = wz0 + iz;
            for (int iy = 0; iy < NY; iy++) {
                // Store the RAW continuous field — exactly like MassRTS_GPU's
                // sdf_terrain. The analytic field is already gradient-normalised
                // (~unit slope near the surface) and carved cells are smin/smax
                // blended near zero, so both share a scale and Marching Cubes
                // places clean crossings without any banding. The old tanh/clamp
                // "fix" was a misdiagnosis: it flattened the gradient far from the
                // surface, which corrupted the corner-difference normals and
                // produced the faceted bright polygons. No banding here.
                field[bidx(ix, iy, iz)] = world.sdf_at_cached(wx, wy0 + iy, wz);
            }
        }
        // Trilinear sample of the local field at fractional world coords. This is
        // MassRTS_GPU's sample_sdf: sampling the field BETWEEN voxels (not just at
        // integer corners) is what lets us read a smooth gradient exactly where
        // each triangle vertex sits, instead of lerping coarse corner normals.
        auto Fs = [&](float wx, float wy, float wz) -> float {
            float fx = wx - wx0, fy = wy - wy0, fz = wz - wz0;
            if (fx < 0) fx = 0; else if (fx > NX - 1.001f) fx = NX - 1.001f;
            if (fy < 0) fy = 0; else if (fy > NY - 1.001f) fy = NY - 1.001f;
            if (fz < 0) fz = 0; else if (fz > NZ - 1.001f) fz = NZ - 1.001f;
            int ix = (int)fx, iy = (int)fy, iz = (int)fz;
            float tx = fx - ix, ty = fy - iy, tz = fz - iz;
            float c000=field[bidx(ix,iy,iz)],     c100=field[bidx(ix+1,iy,iz)];
            float c010=field[bidx(ix,iy+1,iz)],   c110=field[bidx(ix+1,iy+1,iz)];
            float c001=field[bidx(ix,iy,iz+1)],   c101=field[bidx(ix+1,iy,iz+1)];
            float c011=field[bidx(ix,iy+1,iz+1)], c111=field[bidx(ix+1,iy+1,iz+1)];
            float c00=c000*(1-tx)+c100*tx, c10=c010*(1-tx)+c110*tx;
            float c01=c001*(1-tx)+c101*tx, c11=c011*(1-tx)+c111*tx;
            float c0=c00*(1-ty)+c10*ty, c1=c01*(1-ty)+c11*ty;
            return c0*(1-tz)+c1*tz;
        };
        // Integer-corner field read for the cube classification.
        auto F = [&](int wx, int wy, int wz) -> float {
            return Fs((float)wx, (float)wy, (float)wz);
        };
        // Per-vertex normal: central difference of the trilinear field at the
        // EXACT vertex position with a fine 0.5-block step (MassRTS sdf_gradient).
        // Evaluating the gradient where the vertex actually sits — rather than at
        // the two integer endpoints and lerping — is the single change that makes
        // dug walls shade as a smooth rounded surface instead of faceted polygons.
        auto Ng = [&](glm::vec3 p) -> glm::vec3 {
            const float e = 0.5f;
            float dx = Fs(p.x+e, p.y, p.z) - Fs(p.x-e, p.y, p.z);
            float dy = Fs(p.x, p.y+e, p.z) - Fs(p.x, p.y-e, p.z);
            float dz = Fs(p.x, p.y, p.z+e) - Fs(p.x, p.y, p.z-e);
            glm::vec3 g(dx, dy, dz);
            float L = glm::length(g);
            return L > 1e-6f ? g / L : glm::vec3(0, 1, 0);
        };

        for (int ly = -1; ly < CHUNK_SY; ly++)
        for (int lz = 0; lz <= CHUNK_SZ; lz++)
        for (int lx = 0; lx <= CHUNK_SX; lx++) {
            float val[8];
            glm::vec3 pos[8];
            int cubeindex = 0;
            for (int i = 0; i < 8; i++) {
                int gx = base_x + lx + MC_CORNER[i][0];
                int gy = ly + MC_CORNER[i][1];
                int gz = base_z + lz + MC_CORNER[i][2];
                val[i] = F(gx, gy, gz);
                pos[i] = glm::vec3((float)gx, (float)gy, (float)gz);
                if (val[i] < 0.0f) cubeindex |= (1 << i);
            }
            int edges = MC_EDGE_TABLE[cubeindex];
            if (edges == 0) continue;

            glm::vec3 vert[12];
            for (int e = 0; e < 12; e++) {
                if (!(edges & (1 << e))) continue;
                int a = MC_EDGE_CORNERS[e][0], b = MC_EDGE_CORNERS[e][1];
                float da = val[a], db = val[b];
                // Clamp t to the edge. Carved cells store smin/smax-blended field
                // values whose magnitudes no longer vary linearly along an edge,
                // so the raw root (0-da)/(db-da) can land far outside [0,1] and
                // fling the vertex outside its cell — that is the stray stretched
                // polygon seen around a fresh dig. Clamping keeps every vertex on
                // its own edge so the surface stays watertight.
                float denom = db - da;
                float t = (std::fabs(denom) < 1e-6f) ? 0.5f : (0.0f - da) / denom;
                t = (t < 0.0f) ? 0.0f : (t > 1.0f ? 1.0f : t);
                vert[e] = pos[a] + t * (pos[b] - pos[a]);
            }

            const int* tri = MC_TRI_TABLE[cubeindex];
            for (int i = 0; tri[i] != -1; i += 3) {
                glm::vec3 p0 = vert[tri[i]], p1 = vert[tri[i+1]], p2 = vert[tri[i+2]];
                emit_tri(world, out.opaque,
                         p0, p1, p2,
                         Ng(p0), Ng(p1), Ng(p2));
            }
        }
    }

private:
    static void emit_tri(World& w, std::vector<float>& dst,
                         glm::vec3 p0, glm::vec3 p1, glm::vec3 p2,
                         glm::vec3 n0, glm::vec3 n1, glm::vec3 n2) {
        // Drop degenerate / near-zero-area triangles (iso-crossing landing on a
        // cube corner). The floating "billboard" quads in a dug hole are killed
        // at the source instead — carve_sphere no longer lifts solid cells just
        // outside the dig sphere to air, so those thin rim shelves never form.
        glm::vec3 fn = glm::cross(p1 - p0, p2 - p0);
        float area2 = glm::length(fn);          // = 2 * triangle area
        if (area2 < 1e-4f) return;              // degenerate / near-zero-area
        // Repair any non-finite / zero interpolated normal with the face normal
        // so we never emit NaN-shaded geometry.
        fn /= area2;
        auto fix = [&](glm::vec3 n) {
            float l = glm::length(n);
            return (std::isfinite(l) && l > 1e-4f) ? n / l : fn;
        };
        n0 = fix(n0); n1 = fix(n1); n2 = fix(n2);

        // Winding correction. The MC triangle/edge tables emit inconsistent
        // winding for the complex cube configs (overhangs, caves, thin dug
        // walls), which forced us to render terrain with GL_CULL_FACE OFF — so
        // BOTH sides of every thin wall drew, and a deep dig looked like folded
        // paper / book pages. The SDF-gradient vertex normals are reliably
        // OUTWARD, so if the face normal points the other way the triangle is
        // wound backwards: swap two corners to flip it. With every triangle wound
        // outward, backface culling can be turned back on and the folds vanish.
        glm::vec3 avgN = n0 + n1 + n2;
        float al = glm::length(avgN);
        avgN = (al > 1e-4f) ? avgN / al : fn;
        if (glm::dot(fn, avgN) < 0.0f) {
            glm::vec3 tp = p1; p1 = p2; p2 = tp;
            glm::vec3 tn = n1; n1 = n2; n2 = tn;
            fn = -fn;
        }

        // === Material: smooth DEPTH for earthy terrain, snapped CODE for the
        //     special block materials ===========================================
        // The "weird polygons" came from interpolating a MATERIAL CODE across a
        // triangle (grass=1 .. ore=8 sweeps through every palette). But grass /
        // dirt / rock are really just one continuous earthy surface ordered by
        // DEPTH below the natural ground — and depth is a smooth geometric value
        // that is perfectly safe to interpolate per-vertex. So:
        //   * all-earthy triangle  -> per-vertex v_mat = depth-below-surface.
        //     The shader blends grass->dirt->rock by that depth, giving a SMOOTH
        //     (no sawtooth, no garish) transition.
        //   * any special block (ore/wood/leaves/water/sand/snow/gravel) on the
        //     triangle -> snap the WHOLE triangle to 200+code so it never
        //     interpolates into the earthy range or between two palettes.
        int   m0, m1, m2;  float d0, d1, d2;  glm::vec3 c0, c1, c2;
        classify_vertex(w, p0, n0, m0, d0, c0);
        classify_vertex(w, p1, n1, m1, d1, c1);
        classify_vertex(w, p2, n2, m2, d2, c2);
        auto earthy = [](int m){ return m == MAT_GRASS || m == MAT_DIRT || m == MAT_ROCK; };
        if (earthy(m0) && earthy(m1) && earthy(m2)) {
            push_vert_uniform(dst, p0, n0, c0, d0);
            push_vert_uniform(dst, p1, n1, c1, d1);
            push_vert_uniform(dst, p2, n2, c2, d2);
        } else {
            glm::vec3 ctr = (p0 + p1 + p2) * (1.0f / 3.0f);
            glm::vec3 cn  = n0 + n1 + n2;
            float cnl = glm::length(cn);
            cn = (cnl > 1e-4f) ? cn / cnl : fn;
            int mc; float dd; glm::vec3 cc;
            classify_vertex(w, ctr, cn, mc, dd, cc);
            float code = 200.0f + (float)mc;     // >=200 => special, snapped
            push_vert_uniform(dst, p0, n0, cc, code);
            push_vert_uniform(dst, p1, n1, cc, code);
            push_vert_uniform(dst, p2, n2, cc, code);
        }
    }

    // Classify one surface point: its block material code, its depth below the
    // natural surface (>=0), and an albedo. Natural top uses the generator's
    // biome block; dug/steep points sample the block just inside the surface.
    static void classify_vertex(World& w, glm::vec3 p, glm::vec3 n,
                                int& mat, float& depth, glm::vec3& col) {
        float surf = w.surface_height_f(p.x, p.z);
        depth = surf - p.y;
        if (depth < 0.0f) depth = 0.0f;
        if (depth > 60.0f) depth = 60.0f;        // keep clear of the 200 sentinel
        float mf = 0.0f;
        if (p.y > surf - 1.5f) {
            BlockId sb = w.surface_block_at((int)std::floor(p.x), (int)std::floor(p.z));
            col = block_def(sb).top_color;
            mf  = block_material(sb);
        } else {
            bool top = n.y > 0.5f;
            glm::vec3 inside = p - n * 0.5f;
            col = solid_color(w, (int)std::floor(inside.x), (int)std::floor(inside.y),
                              (int)std::floor(inside.z), top, mf);
        }
        mat = (int)(mf + 0.5f);
    }

    // Push one vertex with an explicit colour + packed material/depth value.
    static void push_vert_uniform(std::vector<float>& dst, glm::vec3 p, glm::vec3 n,
                                  glm::vec3 col, float mat) {
        dst.push_back(p.x); dst.push_back(p.y); dst.push_back(p.z);
        dst.push_back(n.x); dst.push_back(n.y); dst.push_back(n.z);
        dst.push_back(col.r); dst.push_back(col.g); dst.push_back(col.b);
        dst.push_back(mat);
    }
};

} // namespace sdfcraft

