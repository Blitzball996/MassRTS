#pragma once
// =============================================================================
// SDFCraft - Crafting + smelting recipes
// -----------------------------------------------------------------------------
// Shaped + shapeless recipes over a 2x2 (inventory) or 3x3 (workbench) grid,
// plus furnace smelting. Mirrors MinecraftConsoles Recipes / ShapedRecipy /
// ShapelessRecipy / FurnaceRecipes but as plain data tables. The matcher is
// position-independent for shapeless and offset-tolerant for shaped (the
// pattern can sit anywhere in the grid).
// =============================================================================
#include "items.h"
#include <vector>
#include <array>
#include <string>
#include <algorithm>

namespace sdfcraft {

struct CraftResult { ItemId id = ITEM_NONE; uint8_t count = 0; };

struct Recipe {
    bool shapeless = false;
    int  w = 0, h = 0;                 // shaped pattern size
    std::array<ItemId, 9> pattern{};   // shaped: row-major w*h (0 = empty)
    std::vector<ItemId> ingredients;   // shapeless: multiset of required items
    CraftResult result;
};

class RecipeBook {
public:
    RecipeBook() { build(); }

    // grid: row-major up to 3x3 (gw x gh). Returns the crafted result or empty.
    CraftResult match(const ItemId* grid, int gw, int gh) const {
        for (const auto& r : recipes_) {
            if (r.shapeless) { if (match_shapeless(r, grid, gw, gh)) return r.result; }
            else             { if (match_shaped(r, grid, gw, gh))    return r.result; }
        }
        return {};
    }

    CraftResult smelt(ItemId in) const {
        for (auto& s : smelting_) if (s.first == in) return s.second;
        return {};
    }
    bool is_fuel(ItemId id, int& burn_ticks) const {
        switch (id) {
            case ITEM_COAL:                 burn_ticks = 1600; return true; // 8 items
            case block_item(BLOCK_LOG):     burn_ticks = 300;  return true;
            case block_item(BLOCK_PLANK):   burn_ticks = 300;  return true;
            case ITEM_STICK:                burn_ticks = 100;  return true;
            default: burn_ticks = 0; return false;
        }
    }

    const std::vector<Recipe>& all() const { return recipes_; }

private:
    std::vector<Recipe> recipes_;
    std::vector<std::pair<ItemId, CraftResult>> smelting_;

    static Recipe shapeless(std::vector<ItemId> ing, ItemId out, uint8_t n) {
        Recipe r; r.shapeless = true; r.ingredients = std::move(ing);
        r.result = {out, n}; return r;
    }
    static Recipe shaped(int w, int h, std::vector<ItemId> pat, ItemId out, uint8_t n) {
        Recipe r; r.w = w; r.h = h;
        for (size_t i = 0; i < pat.size() && i < 9; i++) r.pattern[i] = pat[i];
        r.result = {out, n}; return r;
    }

    void build() {
        const ItemId P = block_item(BLOCK_PLANK);
        const ItemId L = block_item(BLOCK_LOG);
        const ItemId C = block_item(BLOCK_COBBLE);
        const ItemId S = ITEM_STICK;
        const ItemId I = ITEM_IRON_INGOT;
        const ItemId D = ITEM_DIAMOND;
        const ItemId X = ITEM_NONE;

        // --- basics ---
        recipes_.push_back(shapeless({L}, P, 4));                 // log -> 4 planks
        recipes_.push_back(shaped(1,2,{P,P}, S, 4));              // 2 planks -> 4 sticks
        recipes_.push_back(shaped(2,2,{P,P,P,P}, block_item(BLOCK_CRAFTING_TABLE), 1));
        recipes_.push_back(shaped(3,3,{C,C,C, C,X,C, C,C,C}, block_item(BLOCK_FURNACE), 1));
        recipes_.push_back(shaped(1,2,{block_item(BLOCK_COAL_ORE),S}, block_item(BLOCK_TORCH), 4));
        recipes_.push_back(shaped(1,2,{ITEM_COAL,S}, block_item(BLOCK_TORCH), 4));

        // --- tools (pickaxe/axe/shovel/sword) for each material ---
        add_tool_set(P, S, ITEM_WOOD_PICKAXE, ITEM_WOOD_AXE, ITEM_WOOD_SHOVEL, ITEM_WOOD_SWORD);
        add_tool_set(C, S, ITEM_STONE_PICKAXE, ITEM_STONE_AXE, ITEM_STONE_SHOVEL, ITEM_STONE_SWORD);
        add_tool_set(I, S, ITEM_IRON_PICKAXE, ITEM_IRON_AXE, ITEM_IRON_SHOVEL, ITEM_IRON_SWORD);
        add_tool_set(D, S, ITEM_DIAMOND_PICKAXE, ITEM_DIAMOND_AXE, ITEM_DIAMOND_SHOVEL, ITEM_DIAMOND_SWORD);

        // --- food (Phase E adds wheat->bread once farming exists) ---

        // --- smelting (furnace) ---
        smelting_.push_back({block_item(BLOCK_IRON_ORE),    {ITEM_IRON_INGOT, 1}});
        smelting_.push_back({block_item(BLOCK_GOLD_ORE),    {ITEM_GOLD_INGOT, 1}});
        smelting_.push_back({block_item(BLOCK_DIAMOND_ORE), {ITEM_DIAMOND, 1}});
        smelting_.push_back({block_item(BLOCK_COAL_ORE),    {ITEM_COAL, 1}});
        smelting_.push_back({block_item(BLOCK_SAND),        {block_item(BLOCK_GLASS), 1}});
        smelting_.push_back({block_item(BLOCK_COBBLE),      {block_item(BLOCK_STONE), 1}});
        smelting_.push_back({ITEM_PORKCHOP,                 {ITEM_COOKED_PORKCHOP, 1}});
    }

    void add_tool_set(ItemId mat, ItemId stick, ItemId pick, ItemId axe, ItemId shovel, ItemId sword) {
        const ItemId X = ITEM_NONE;
        recipes_.push_back(shaped(3,3,{mat,mat,mat, X,stick,X, X,stick,X}, pick, 1));
        recipes_.push_back(shaped(2,3,{mat,mat, mat,stick, X,stick}, axe, 1));
        recipes_.push_back(shaped(1,3,{mat,stick,stick}, shovel, 1));
        recipes_.push_back(shaped(1,3,{mat,mat,stick}, sword, 1));
    }

    // --- matchers ---
    static bool match_shapeless(const Recipe& r, const ItemId* grid, int gw, int gh) {
        std::vector<ItemId> have;
        for (int i = 0; i < gw*gh; i++) if (grid[i] != ITEM_NONE) have.push_back(grid[i]);
        if (have.size() != r.ingredients.size()) return false;
        std::vector<ItemId> need = r.ingredients;
        for (ItemId h : have) {
            auto it = std::find(need.begin(), need.end(), h);
            if (it == need.end()) return false;
            need.erase(it);
        }
        return need.empty();
    }

    // Shaped: find the recipe's bounding box anywhere in the grid; everything
    // outside must be empty, inside must match exactly.
    static bool match_shaped(const Recipe& r, const ItemId* grid, int gw, int gh) {
        if (r.w > gw || r.h > gh) return false;
        for (int oy = 0; oy + r.h <= gh; oy++)
        for (int ox = 0; ox + r.w <= gw; ox++) {
            bool ok = true;
            for (int y = 0; y < gh && ok; y++)
            for (int x = 0; x < gw && ok; x++) {
                ItemId g = grid[y*gw + x];
                bool inside = (x >= ox && x < ox + r.w && y >= oy && y < oy + r.h);
                ItemId want = inside ? r.pattern[(y-oy)*r.w + (x-ox)] : ITEM_NONE;
                if (g != want) ok = false;
            }
            if (ok) return true;
        }
        return false;
    }
};

} // namespace sdfcraft
