#pragma once
// =============================================================================
// SDFCraft - Block registry (ported concept from MinecraftConsoles Tile.*)
// Discrete block-voxel layer used by the survival/build mode. Fully isolated
// from the RTS SDF terrain. Each block id maps to material properties used by
// meshing (color/face tint), gameplay (hardness, solidity) and item drops.
// =============================================================================
#include <cstdint>
#include <glm/glm.hpp>

namespace sdfcraft {

using BlockId = uint8_t;

// Core block ids. Keep AIR == 0 so a freshly-zeroed chunk is empty.
enum : BlockId {
    BLOCK_AIR = 0,
    BLOCK_STONE,
    BLOCK_DIRT,
    BLOCK_GRASS,
    BLOCK_SAND,
    BLOCK_WATER,
    BLOCK_LOG,
    BLOCK_LEAVES,
    BLOCK_PLANK,
    BLOCK_COBBLE,
    BLOCK_GLASS,
    BLOCK_COAL_ORE,
    BLOCK_IRON_ORE,
    BLOCK_GOLD_ORE,
    BLOCK_DIAMOND_ORE,
    BLOCK_BEDROCK,
    BLOCK_SNOW,
    BLOCK_GRAVEL,
    BLOCK_CRAFTING_TABLE,
    BLOCK_FURNACE,
    BLOCK_TORCH,
    BLOCK_COUNT
};

struct BlockDef {
    const char* name;
    glm::vec3   color;       // base albedo used by the simple shader
    glm::vec3   top_color;   // top-face tint (grass etc.); == color if same
    float       hardness;    // seconds-scale dig time base (<0 = unbreakable)
    bool        solid;       // blocks movement / occludes faces
    bool        opaque;      // hides neighbour faces (false for glass/leaves/water)
    bool        liquid;      // water-like (no collision, special render)
};

// Indexed by BlockId. Colors are placeholder palette tints; a real texture
// atlas (Phase A4) replaces these later but the gameplay props stay.
inline const BlockDef& block_def(BlockId id) {
    static const BlockDef defs[BLOCK_COUNT] = {
        /* AIR      */ {"air",      {0,0,0},            {0,0,0},            0.0f,  false, false, false},
        /* STONE    */ {"stone",    {0.50f,0.50f,0.52f},{0.50f,0.50f,0.52f},1.5f,  true,  true,  false},
        /* DIRT     */ {"dirt",     {0.40f,0.28f,0.18f},{0.40f,0.28f,0.18f},0.5f,  true,  true,  false},
        /* GRASS    */ {"grass",    {0.40f,0.28f,0.18f},{0.32f,0.55f,0.24f},0.6f,  true,  true,  false},
        /* SAND     */ {"sand",     {0.80f,0.74f,0.50f},{0.80f,0.74f,0.50f},0.5f,  true,  true,  false},
        /* WATER    */ {"water",    {0.15f,0.35f,0.65f},{0.15f,0.35f,0.65f},-1.0f, false, false, true },
        /* LOG      */ {"log",      {0.42f,0.30f,0.16f},{0.55f,0.42f,0.24f},2.0f,  true,  true,  false},
        /* LEAVES   */ {"leaves",   {0.22f,0.42f,0.18f},{0.22f,0.42f,0.18f},0.2f,  true,  false, false},
        /* PLANK    */ {"plank",    {0.62f,0.46f,0.28f},{0.62f,0.46f,0.28f},2.0f,  true,  true,  false},
        /* COBBLE   */ {"cobble",   {0.42f,0.42f,0.44f},{0.42f,0.42f,0.44f},2.0f,  true,  true,  false},
        /* GLASS    */ {"glass",    {0.75f,0.85f,0.92f},{0.75f,0.85f,0.92f},0.3f,  true,  false, false},
        /* COAL_ORE */ {"coal_ore", {0.30f,0.30f,0.32f},{0.30f,0.30f,0.32f},3.0f,  true,  true,  false},
        /* IRON_ORE */ {"iron_ore", {0.58f,0.50f,0.44f},{0.58f,0.50f,0.44f},3.0f,  true,  true,  false},
        /* GOLD_ORE */ {"gold_ore", {0.66f,0.58f,0.36f},{0.66f,0.58f,0.36f},3.0f,  true,  true,  false},
        /* DIAMOND  */ {"diamond_ore",{0.40f,0.72f,0.74f},{0.40f,0.72f,0.74f},3.5f,true,  true,  false},
        /* BEDROCK  */ {"bedrock",  {0.18f,0.18f,0.20f},{0.18f,0.18f,0.20f},-1.0f, true,  true,  false},
        /* SNOW     */ {"snow",     {0.92f,0.94f,0.98f},{0.92f,0.94f,0.98f},0.3f,  true,  true,  false},
        /* GRAVEL   */ {"gravel",   {0.46f,0.44f,0.43f},{0.46f,0.44f,0.43f},0.7f,  true,  true,  false},
        /* CRAFTING */ {"crafting_table",{0.55f,0.40f,0.22f},{0.50f,0.36f,0.20f},2.0f,true,true,false},
        /* FURNACE  */ {"furnace",  {0.38f,0.38f,0.40f},{0.40f,0.40f,0.42f},3.5f,  true,  true,  false},
        /* TORCH    */ {"torch",    {0.95f,0.80f,0.35f},{0.95f,0.80f,0.35f},0.0f,  false, false, false},
    };
    if (id >= BLOCK_COUNT) id = BLOCK_AIR;
    return defs[id];
}

inline bool block_is_air(BlockId id)    { return id == BLOCK_AIR; }
inline bool block_is_solid(BlockId id)  { return block_def(id).solid; }
inline bool block_is_opaque(BlockId id) { return block_def(id).opaque; }
inline bool block_is_liquid(BlockId id) { return block_def(id).liquid; }

} // namespace sdfcraft
