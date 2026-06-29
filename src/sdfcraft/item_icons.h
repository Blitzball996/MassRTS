#pragma once
// =============================================================================
// SDFCraft - ItemId -> sprite-atlas icon mapping (hotbar / inventory icons)
// -----------------------------------------------------------------------------
// Maps every ItemId to a 16x16 cell in one of two classic MC 1.2 sprite sheets:
//   assets/textures/gui/terrain.png (256x256) - BLOCK textures
//   assets/textures/gui/items.png   (256x256) - tools / food / materials
//
// Block items (id < ITEM_NONBLOCK) map via their BlockId into terrain.png;
// non-block items (tools/food/materials) map via ItemId into items.png. This
// lets the HUD draw the *real* recognizable icon for any held item instead of
// the flat block-colour swatch (which only ever covered block items).
//
// Atlas layout: 16x16 grid of 16px cells, (col,row) 0-indexed from the TOP-LEFT.
// UV rect = (col*16/256, row*16/256) .. ((col+1)*16/256, (row+1)*16/256).
//
// icon_for(id) returns ok=false for ITEM_NONE or any unmapped id, so the caller
// can fall back to the colour swatch.
// =============================================================================
#include "items.h"   // ItemId/BlockId, item_is_block/item_block, BLOCK_*/ITEM_*
#include <glm/glm.hpp>
#include <vector>

namespace sdfcraft {

// Source sheet for an icon.
enum : int { ICON_ATLAS_TERRAIN = 0, ICON_ATLAS_ITEMS = 1 };

struct IconRef {
    int   atlas;            // 0 = terrain.png, 1 = items.png
    float u0, v0, u1, v1;   // UV rect into the 256x256 sheet ([0,1], top-left origin)
    bool  ok;               // false => no icon (caller uses the colour swatch)
};

// 16px cell on a 256x256 (16x16) atlas -> UV rect. (col,row) 0-indexed top-left.
inline IconRef icon_cell(int atlas, int col, int row) {
    const float c = 16.0f / 256.0f;
    return IconRef{ atlas, col * c, row * c, (col + 1) * c, (row + 1) * c, true };
}

inline IconRef icon_none() { return IconRef{ 0, 0, 0, 0, 0, false }; }

// Block (BlockId) -> terrain.png cell. Classic MC 1.2 atlas positions.
inline IconRef icon_for_block(BlockId b) {
    switch (b) {
        case BLOCK_STONE:          return icon_cell(ICON_ATLAS_TERRAIN, 1, 0);
        case BLOCK_DIRT:           return icon_cell(ICON_ATLAS_TERRAIN, 2, 0);
        case BLOCK_GRASS:          return icon_cell(ICON_ATLAS_TERRAIN, 3, 0); // grass *side* reads as grass
        case BLOCK_COBBLE:         return icon_cell(ICON_ATLAS_TERRAIN, 0, 1);
        case BLOCK_PLANK:          return icon_cell(ICON_ATLAS_TERRAIN, 4, 0);
        case BLOCK_SAND:           return icon_cell(ICON_ATLAS_TERRAIN, 2, 1);
        case BLOCK_GRAVEL:         return icon_cell(ICON_ATLAS_TERRAIN, 3, 1);
        case BLOCK_LOG:            return icon_cell(ICON_ATLAS_TERRAIN, 4, 1); // log side
        case BLOCK_LEAVES:         return icon_cell(ICON_ATLAS_TERRAIN, 4, 3);
        case BLOCK_GLASS:          return icon_cell(ICON_ATLAS_TERRAIN, 1, 3);
        case BLOCK_COAL_ORE:       return icon_cell(ICON_ATLAS_TERRAIN, 2, 2);
        case BLOCK_IRON_ORE:       return icon_cell(ICON_ATLAS_TERRAIN, 1, 2);
        case BLOCK_GOLD_ORE:       return icon_cell(ICON_ATLAS_TERRAIN, 0, 2);
        case BLOCK_DIAMOND_ORE:    return icon_cell(ICON_ATLAS_TERRAIN, 2, 3);
        case BLOCK_SNOW:           return icon_cell(ICON_ATLAS_TERRAIN, 2, 4);
        case BLOCK_BEDROCK:        return icon_cell(ICON_ATLAS_TERRAIN, 1, 1);
        case BLOCK_CRAFTING_TABLE: return icon_cell(ICON_ATLAS_TERRAIN, 11, 2); // table top
        case BLOCK_FURNACE:        return icon_cell(ICON_ATLAS_TERRAIN, 12, 2); // furnace front
        case BLOCK_TORCH:          return icon_cell(ICON_ATLAS_TERRAIN, 0, 5);
        case BLOCK_WATER:          return icon_cell(ICON_ATLAS_TERRAIN, 15, 13);
        default:                   return icon_none();
    }
}

// Non-block item (ItemId) -> items.png cell. Classic MC 1.2 atlas positions.
inline IconRef icon_for_item(ItemId id) {
    switch (id) {
        // materials / food
        case ITEM_STICK:           return icon_cell(ICON_ATLAS_ITEMS, 5, 3);
        case ITEM_COAL:            return icon_cell(ICON_ATLAS_ITEMS, 7, 0);
        case ITEM_IRON_INGOT:      return icon_cell(ICON_ATLAS_ITEMS, 7, 1);
        case ITEM_GOLD_INGOT:      return icon_cell(ICON_ATLAS_ITEMS, 7, 2);
        case ITEM_DIAMOND:         return icon_cell(ICON_ATLAS_ITEMS, 7, 3);
        case ITEM_APPLE:           return icon_cell(ICON_ATLAS_ITEMS, 10, 0);
        case ITEM_BREAD:           return icon_cell(ICON_ATLAS_ITEMS, 9, 2);
        case ITEM_PORKCHOP:        return icon_cell(ICON_ATLAS_ITEMS, 7, 5);
        case ITEM_COOKED_PORKCHOP: return icon_cell(ICON_ATLAS_ITEMS, 8, 5);
        // pickaxes (row 6)
        case ITEM_WOOD_PICKAXE:    return icon_cell(ICON_ATLAS_ITEMS, 0, 6);
        case ITEM_STONE_PICKAXE:   return icon_cell(ICON_ATLAS_ITEMS, 1, 6);
        case ITEM_IRON_PICKAXE:    return icon_cell(ICON_ATLAS_ITEMS, 2, 6);
        case ITEM_DIAMOND_PICKAXE: return icon_cell(ICON_ATLAS_ITEMS, 3, 6);
        // swords (row 4)
        case ITEM_WOOD_SWORD:      return icon_cell(ICON_ATLAS_ITEMS, 0, 4);
        case ITEM_STONE_SWORD:     return icon_cell(ICON_ATLAS_ITEMS, 1, 4);
        case ITEM_IRON_SWORD:      return icon_cell(ICON_ATLAS_ITEMS, 2, 4);
        case ITEM_DIAMOND_SWORD:   return icon_cell(ICON_ATLAS_ITEMS, 3, 4);
        // axes (row 7)
        case ITEM_WOOD_AXE:        return icon_cell(ICON_ATLAS_ITEMS, 0, 7);
        case ITEM_STONE_AXE:       return icon_cell(ICON_ATLAS_ITEMS, 1, 7);
        case ITEM_IRON_AXE:        return icon_cell(ICON_ATLAS_ITEMS, 2, 7);
        case ITEM_DIAMOND_AXE:     return icon_cell(ICON_ATLAS_ITEMS, 3, 7);
        // shovels (row 5)
        case ITEM_WOOD_SHOVEL:     return icon_cell(ICON_ATLAS_ITEMS, 0, 5);
        case ITEM_STONE_SHOVEL:    return icon_cell(ICON_ATLAS_ITEMS, 1, 5);
        case ITEM_IRON_SHOVEL:     return icon_cell(ICON_ATLAS_ITEMS, 2, 5);
        case ITEM_DIAMOND_SHOVEL:  return icon_cell(ICON_ATLAS_ITEMS, 3, 5);
        // armor (items.png cols: helmet=0 chest=1 legs=2 boots=3; rows: leather=0
        // iron=2 diamond=3 — the classic MC 1.2 armor-icon block).
        case ITEM_LEATHER_HELMET:  return icon_cell(ICON_ATLAS_ITEMS, 0, 0);
        case ITEM_LEATHER_CHEST:   return icon_cell(ICON_ATLAS_ITEMS, 1, 0);
        case ITEM_LEATHER_LEGS:    return icon_cell(ICON_ATLAS_ITEMS, 2, 0);
        case ITEM_LEATHER_BOOTS:   return icon_cell(ICON_ATLAS_ITEMS, 3, 0);
        case ITEM_IRON_HELMET:     return icon_cell(ICON_ATLAS_ITEMS, 0, 2);
        case ITEM_IRON_CHEST:      return icon_cell(ICON_ATLAS_ITEMS, 1, 2);
        case ITEM_IRON_LEGS:       return icon_cell(ICON_ATLAS_ITEMS, 2, 2);
        case ITEM_IRON_BOOTS:      return icon_cell(ICON_ATLAS_ITEMS, 3, 2);
        case ITEM_DIAMOND_HELMET:  return icon_cell(ICON_ATLAS_ITEMS, 0, 3);
        case ITEM_DIAMOND_CHEST:   return icon_cell(ICON_ATLAS_ITEMS, 1, 3);
        case ITEM_DIAMOND_LEGS:    return icon_cell(ICON_ATLAS_ITEMS, 2, 3);
        case ITEM_DIAMOND_BOOTS:   return icon_cell(ICON_ATLAS_ITEMS, 3, 3);
        case ITEM_LEATHER:         return icon_cell(ICON_ATLAS_ITEMS, 8, 1);  // leather material
        default:                   return icon_none();
    }
}

// Resolve any ItemId to an icon. Block items -> terrain.png via BlockId;
// non-block items -> items.png via ItemId. ok=false for ITEM_NONE/unmapped.
inline IconRef icon_for(ItemId id) {
    if (id == ITEM_NONE) return icon_none();
    if (item_is_block(id)) return icon_for_block(item_block(id));
    return icon_for_item(id);
}

// =============================================================================
// Isometric block cube icons (Minecraft inventory style)
// -----------------------------------------------------------------------------
// Block items render as a 3-face iso cube (top/left/right) instead of a flat
// face. Per-block we need a TOP cell and a SIDE cell (most blocks use the same
// texture all around; grass/log differ). block_face(b, face) returns the cell
// for face 0=top, 1=side, falling back to icon_for_block for both.
// =============================================================================

// Block (BlockId) + face -> terrain.png cell. face: 0=top, 1=side.
inline IconRef block_face(BlockId b, int face) {
    const bool top = (face == 0);
    switch (b) {
        // grass: green top, grass-side ring around the sides
        case BLOCK_GRASS:  return top ? icon_cell(ICON_ATLAS_TERRAIN, 0, 0)   // grass top
                                      : icon_cell(ICON_ATLAS_TERRAIN, 3, 0);  // grass side
        // log: ring end on top, bark on the sides
        case BLOCK_LOG:    return top ? icon_cell(ICON_ATLAS_TERRAIN, 5, 1)   // log top (rings)
                                      : icon_cell(ICON_ATLAS_TERRAIN, 4, 1);  // log side (bark)
        // grass-like snow cover: snow on top, dirt+snow side reads fine as the snow cell
        default:           return icon_for_block(b);                          // same all faces
    }
}

// One textured triangle vertex in DESTINATION PIXEL space (top-left origin),
// carrying its UV (already in [0,1]) and a per-face shade tint. The caller
// converts (x,y) px -> its own NDC vertex format and draws with terrain.png.
struct IsoTri { float x, y, u, v; glm::vec4 tint; };

// Append one quad (4 corners, CCW-ish) as two triangles into `out`. Corners
// p0..p3 map to uv c0..c3 respectively.
inline void iso_push_quad(std::vector<IsoTri>& out,
                          glm::vec2 p0, glm::vec2 p1, glm::vec2 p2, glm::vec2 p3,
                          glm::vec2 t0, glm::vec2 t1, glm::vec2 t2, glm::vec2 t3,
                          glm::vec4 tint) {
    out.push_back({p0.x, p0.y, t0.x, t0.y, tint});
    out.push_back({p1.x, p1.y, t1.x, t1.y, tint});
    out.push_back({p2.x, p2.y, t2.x, t2.y, tint});
    out.push_back({p0.x, p0.y, t0.x, t0.y, tint});
    out.push_back({p2.x, p2.y, t2.x, t2.y, tint});
    out.push_back({p3.x, p3.y, t3.x, t3.y, tint});
}

// Emit a Minecraft-style isometric cube into a (x,y,w,h) pixel box. Three faces:
//   top   = rhombus (full brightness)
//   left  = parallelogram (~80% brightness)
//   right = parallelogram (~60% brightness)
// Classic 2:1 dimetric. Corner points (cx = x + w/2):
//   top:    (cx,y) (x+w,y+0.25h) (cx,y+0.5h) (x,y+0.25h)
//   left:   (x,y+0.25h) (cx,y+0.5h) (cx,y+h) (x,y+0.75h)
//   right:  (cx,y+0.5h) (x+w,y+0.25h) (x+w,y+0.75h) (cx,y+h)
// topUV/sideUV are the terrain.png face rects; UVs are mapped axis-aligned to
// each face (a slight skew on the parallelograms still reads as a cube).
inline void emit_iso_cube(std::vector<IsoTri>& out, float x, float y, float w, float h,
                          IconRef topUV, IconRef sideUV) {
    const float cx = x + w * 0.5f;
    const float y0 = y, y25 = y + h * 0.25f, y50 = y + h * 0.5f,
                y75 = y + h * 0.75f, y100 = y + h;
    const float xl = x, xr = x + w;

    const glm::vec2 tT(topUV.u0, topUV.v0),  tTr(topUV.u1, topUV.v0),
                    tBr(topUV.u1, topUV.v1), tB(topUV.u0, topUV.v1);
    const glm::vec2 sT(sideUV.u0, sideUV.v0),  sTr(sideUV.u1, sideUV.v0),
                    sBr(sideUV.u1, sideUV.v1), sB(sideUV.u0, sideUV.v1);

    const glm::vec4 shadeTop(1, 1, 1, 1), shadeLeft(0.8f, 0.8f, 0.8f, 1), shadeRight(0.6f, 0.6f, 0.6f, 1);

    // TOP rhombus: T(top) R(right) B(bottom) L(left) -> uv tl tr br bl
    iso_push_quad(out,
        {cx, y0}, {xr, y25}, {cx, y50}, {xl, y25},
        tT, tTr, tBr, tB, shadeTop);

    // LEFT face: top-left,(down-right to center),bottom-center,bottom-left
    //   corners: (xl,y25) (cx,y50) (cx,y100) (xl,y75)  -> uv tl tr br bl
    iso_push_quad(out,
        {xl, y25}, {cx, y50}, {cx, y100}, {xl, y75},
        sT, sTr, sBr, sB, shadeLeft);

    // RIGHT face: (cx,y50) (xr,y25) (xr,y75) (cx,y100) -> uv tl tr br bl
    iso_push_quad(out,
        {cx, y50}, {xr, y25}, {xr, y75}, {cx, y100},
        sT, sTr, sBr, sB, shadeRight);
}

} // namespace sdfcraft
