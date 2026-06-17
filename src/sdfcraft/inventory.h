#pragma once
// =============================================================================
// SDFCraft - Items, item stacks, hotbar + inventory
// -----------------------------------------------------------------------------
// Minimal item model for the Phase B loop: every placeable block has a matching
// item; dug blocks drop into the inventory; the selected hotbar slot decides
// what gets placed. Crafting (Phase E) and tools (Phase D) extend ItemId later.
// =============================================================================
#include "blocks.h"
#include <array>
#include <cstdint>

namespace sdfcraft {

using ItemId = uint16_t;

// Item ids 1..BLOCK_COUNT-1 mirror block ids (block items). Ids >= ITEM_NONBLOCK
// are reserved for tools / materials added in later phases.
static constexpr ItemId ITEM_NONE = 0;
static constexpr ItemId ITEM_NONBLOCK = 256;

inline constexpr bool item_is_block(ItemId id) { return id != ITEM_NONE && id < ITEM_NONBLOCK; }
inline constexpr BlockId item_block(ItemId id) { return item_is_block(id) ? (BlockId)id : BLOCK_AIR; }
inline constexpr ItemId block_item(BlockId b)  { return (ItemId)b; }

struct ItemStack {
    ItemId  id    = ITEM_NONE;
    uint8_t count = 0;
    bool empty() const { return id == ITEM_NONE || count == 0; }
    void clear() { id = ITEM_NONE; count = 0; }
};

static constexpr int HOTBAR_SLOTS = 9;
static constexpr int INV_ROWS = 3;
static constexpr int INV_COLS = 9;
static constexpr int INV_SLOTS = HOTBAR_SLOTS + INV_ROWS * INV_COLS; // 9 + 27 = 36
static constexpr uint8_t STACK_MAX = 64;

class Inventory {
public:
    std::array<ItemStack, INV_SLOTS> slots{};
    int selected = 0;  // active hotbar slot [0, HOTBAR_SLOTS)

    ItemStack& held() { return slots[selected]; }

    // Add items; returns leftover count that didn't fit. `max` is the per-item
    // stack limit (caller passes item_max_stack(id) to avoid a circular include).
    uint8_t add(ItemId id, uint8_t n, uint8_t max = STACK_MAX) {
        if (id == ITEM_NONE || max == 0) return 0;
        // fill existing stacks first
        for (auto& s : slots) {
            if (n == 0) break;
            if (s.id == id && s.count < max) {
                uint8_t room = (uint8_t)(max - s.count);
                uint8_t take = n < room ? n : room;
                s.count += take; n -= take;
            }
        }
        // then empty slots
        for (auto& s : slots) {
            if (n == 0) break;
            if (s.empty()) {
                s.id = id;
                uint8_t take = n < max ? n : max;
                s.count = take; n -= take;
            }
        }
        return n;
    }

    // Consume one of the held item (after placing). Returns true if consumed.
    bool consume_held() {
        ItemStack& s = held();
        if (s.empty()) return false;
        if (--s.count == 0) s.clear();
        return true;
    }

    void scroll(int dir) {
        selected = (selected + dir) % HOTBAR_SLOTS;
        if (selected < 0) selected += HOTBAR_SLOTS;
    }
};

} // namespace sdfcraft
