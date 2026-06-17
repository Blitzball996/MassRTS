#pragma once
// =============================================================================
// SDFCraft - Item registry (tools, materials, food) on top of block items
// -----------------------------------------------------------------------------
// Block items use ids 1..BLOCK_COUNT-1 (see inventory.h). Non-block items start
// at ITEM_NONBLOCK and are defined here: crafting materials, tools (with a tier
// + dig category) and food. This mirrors MinecraftConsoles Item / DiggerItem /
// PickaxeItem / FoodItem without the per-class explosion.
// =============================================================================
#include "blocks.h"
#include "inventory.h"

namespace sdfcraft {

// Non-block item ids (contiguous from ITEM_NONBLOCK).
enum : ItemId {
    ITEM_STICK = ITEM_NONBLOCK,
    ITEM_COAL,
    ITEM_IRON_INGOT,
    ITEM_GOLD_INGOT,
    ITEM_DIAMOND,
    ITEM_APPLE,
    ITEM_BREAD,
    ITEM_PORKCHOP,
    ITEM_COOKED_PORKCHOP,
    // tools: (material x kind). Order matters for the table below.
    ITEM_WOOD_PICKAXE, ITEM_WOOD_AXE, ITEM_WOOD_SHOVEL, ITEM_WOOD_SWORD,
    ITEM_STONE_PICKAXE, ITEM_STONE_AXE, ITEM_STONE_SHOVEL, ITEM_STONE_SWORD,
    ITEM_IRON_PICKAXE, ITEM_IRON_AXE, ITEM_IRON_SHOVEL, ITEM_IRON_SWORD,
    ITEM_DIAMOND_PICKAXE, ITEM_DIAMOND_AXE, ITEM_DIAMOND_SHOVEL, ITEM_DIAMOND_SWORD,
    ITEM_COUNT
};

enum class ToolKind : uint8_t { None, Pickaxe, Axe, Shovel, Sword };

struct ItemDef {
    const char* name;
    uint8_t  max_stack;     // 64 for materials, 1 for tools
    ToolKind tool;          // None for non-tools
    uint8_t  tier;          // 0 none, 1 wood, 2 stone, 3 iron, 4 diamond
    float    dig_mult;      // mining-speed multiplier vs hand for matching blocks
    float    attack;        // melee damage (Phase E uses this)
    int      food;          // hunger restored (0 = not food)
};

inline const ItemDef& item_def(ItemId id) {
    static const ItemDef block_fallback{"block", STACK_MAX, ToolKind::None, 0, 1.0f, 1.0f, 0};
    if (item_is_block(id)) return block_fallback;

    static const ItemDef defs[ITEM_COUNT - ITEM_NONBLOCK] = {
        /* STICK         */ {"stick",        64, ToolKind::None,    0, 1.0f, 1.0f, 0},
        /* COAL          */ {"coal",         64, ToolKind::None,    0, 1.0f, 1.0f, 0},
        /* IRON_INGOT    */ {"iron_ingot",   64, ToolKind::None,    0, 1.0f, 1.0f, 0},
        /* GOLD_INGOT    */ {"gold_ingot",   64, ToolKind::None,    0, 1.0f, 1.0f, 0},
        /* DIAMOND       */ {"diamond",      64, ToolKind::None,    0, 1.0f, 1.0f, 0},
        /* APPLE         */ {"apple",        64, ToolKind::None,    0, 1.0f, 1.0f, 4},
        /* BREAD         */ {"bread",        64, ToolKind::None,    0, 1.0f, 1.0f, 5},
        /* PORKCHOP      */ {"porkchop",     64, ToolKind::None,    0, 1.0f, 1.0f, 3},
        /* COOKED_PORK   */ {"cooked_porkchop",64,ToolKind::None,   0, 1.0f, 1.0f, 8},
        /* WOOD_PICKAXE  */ {"wood_pickaxe",  1, ToolKind::Pickaxe, 1, 2.0f, 2.0f, 0},
        /* WOOD_AXE      */ {"wood_axe",      1, ToolKind::Axe,     1, 2.0f, 3.0f, 0},
        /* WOOD_SHOVEL   */ {"wood_shovel",   1, ToolKind::Shovel,  1, 2.0f, 1.5f, 0},
        /* WOOD_SWORD    */ {"wood_sword",    1, ToolKind::Sword,   1, 1.0f, 4.0f, 0},
        /* STONE_PICKAXE */ {"stone_pickaxe", 1, ToolKind::Pickaxe, 2, 4.0f, 3.0f, 0},
        /* STONE_AXE     */ {"stone_axe",     1, ToolKind::Axe,     2, 4.0f, 4.0f, 0},
        /* STONE_SHOVEL  */ {"stone_shovel",  1, ToolKind::Shovel,  2, 4.0f, 2.5f, 0},
        /* STONE_SWORD   */ {"stone_sword",   1, ToolKind::Sword,   2, 1.0f, 5.0f, 0},
        /* IRON_PICKAXE  */ {"iron_pickaxe",  1, ToolKind::Pickaxe, 3, 6.0f, 4.0f, 0},
        /* IRON_AXE      */ {"iron_axe",      1, ToolKind::Axe,     3, 6.0f, 5.0f, 0},
        /* IRON_SHOVEL   */ {"iron_shovel",   1, ToolKind::Shovel,  3, 6.0f, 3.5f, 0},
        /* IRON_SWORD    */ {"iron_sword",    1, ToolKind::Sword,   3, 1.0f, 6.0f, 0},
        /* DIAMOND_PICK  */ {"diamond_pickaxe",1,ToolKind::Pickaxe, 4, 8.0f, 5.0f, 0},
        /* DIAMOND_AXE   */ {"diamond_axe",   1, ToolKind::Axe,     4, 8.0f, 6.0f, 0},
        /* DIAMOND_SHOVEL*/ {"diamond_shovel",1, ToolKind::Shovel,  4, 8.0f, 4.5f, 0},
        /* DIAMOND_SWORD */ {"diamond_sword", 1, ToolKind::Sword,   4, 1.0f, 7.0f, 0},
    };
    if (id < ITEM_NONBLOCK || id >= ITEM_COUNT) return block_fallback;
    return defs[id - ITEM_NONBLOCK];
}

inline uint8_t item_max_stack(ItemId id) { return item_def(id).max_stack; }

// Which tool kind mines a given block fastest (for dig-speed + correct drops).
inline ToolKind block_pref_tool(BlockId b) {
    switch (b) {
        case BLOCK_STONE: case BLOCK_COBBLE: case BLOCK_COAL_ORE:
        case BLOCK_IRON_ORE: case BLOCK_GOLD_ORE: case BLOCK_DIAMOND_ORE:
        case BLOCK_FURNACE:
            return ToolKind::Pickaxe;
        case BLOCK_LOG: case BLOCK_PLANK: case BLOCK_CRAFTING_TABLE:
            return ToolKind::Axe;
        case BLOCK_DIRT: case BLOCK_GRASS: case BLOCK_SAND:
        case BLOCK_GRAVEL: case BLOCK_SNOW:
            return ToolKind::Shovel;
        default: return ToolKind::None;
    }
}

// Minimum tier required to get a drop (ores need stone+ etc). 0 = always drops.
inline uint8_t block_min_tier(BlockId b) {
    switch (b) {
        case BLOCK_STONE: case BLOCK_COBBLE: case BLOCK_COAL_ORE: return 1; // wood+
        case BLOCK_IRON_ORE:                                     return 2; // stone+
        case BLOCK_GOLD_ORE: case BLOCK_DIAMOND_ORE:             return 3; // iron+
        default: return 0;
    }
}

} // namespace sdfcraft
