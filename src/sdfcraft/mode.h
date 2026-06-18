#pragma once
// =============================================================================
// SDFCraft - Mode driver (Phase A skeleton + Phase B core loop)
// -----------------------------------------------------------------------------
// Owns one survival/build session: the infinite world, the local player, the
// inventory and the GL chunk renderer. Streams chunks in/out around the player,
// processes dig/place against the raycast target, and draws the scene + a
// selection box. Input is fed in as a neutral struct so the entry point can map
// GLFW keys without this module depending on the windowing layer.
//
// This mode is fully isolated from the RTS: it never touches the ECS world,
// RtsNetEngine or the SDF terrain. Multiplayer (Phase B4/N) plugs in by routing
// dig/place through VoxelNetEngine instead of applying locally.
// =============================================================================
#include "world.h"
#include "player.h"
#include "inventory.h"
#include "items.h"
#include "crafting.h"
#include "chunk_renderer.h"
#include "world_ops.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <vector>
#include <array>
#include <algorithm>
#include <memory>

namespace sdfcraft {

struct FrameInput {
    float move_x = 0.0f;   // strafe [-1,1]
    float move_z = 0.0f;   // forward [-1,1]
    bool  jump = false;
    bool  crouch = false;  // / fly-down
    float look_dx = 0.0f;  // mouse delta (already scaled by sensitivity)
    float look_dy = 0.0f;
    bool  dig = false;       // left click held
    bool  place = false;     // right click (edge)
    bool  toggle_fly = false;
    int   hotbar_set = -1;   // 0..8 to set slot directly, -1 = none
    int   hotbar_scroll = 0; // wheel
};

class Mode {
public:
    World      world;
    Player     player;
    Inventory  inv;
    RecipeBook recipes;

    bool init(uint64_t seed, const std::string& shader_dir) {
        world = World(seed);
        // Replication seam: default to the local backend (single-player / host /
        // client prediction). Multiplayer swaps this for NetWorldOps without any
        // change to gameplay code below — every edit already flows through ops_.
        ops_ = std::make_unique<LocalWorldOps>(world);
        if (!renderer_.init(shader_dir)) return false;
        spawn_player();
        give_starter_items();
        return true;
    }

    // Multiplayer entry point (network phase): replace the local backend with a
    // server-authoritative + client-prediction backend. Gameplay is untouched.
    void useNetworkOps(bool is_server) {
        ops_ = std::make_unique<NetWorldOps>(world, is_server);
    }
    void shutdown() { renderer_.shutdown(); }

    void update(const FrameInput& in, float dt, int view_radius = 8) {
        // --- look ---
        player.yaw   += in.look_dx;
        player.pitch -= in.look_dy;
        if (player.pitch > 89.0f) player.pitch = 89.0f;
        if (player.pitch < -89.0f) player.pitch = -89.0f;

        if (in.toggle_fly) player.flying = !player.flying;
        if (in.hotbar_set >= 0) inv.selected = in.hotbar_set;
        if (in.hotbar_scroll) inv.scroll(in.hotbar_scroll);

        stream_chunks(view_radius);

        // Pump the replication backend (no-op locally; polls net + reconciles
        // when networked). Keeps gameplay identical across single/multiplayer.
        if (ops_) ops_->tick(dt);

        // --- physics ---
        glm::vec3 wish(in.move_x, 0, in.move_z);
        player.update(world, dt, wish, in.jump, in.crouch);

        // --- targeting ---
        last_hit_ = player.raycast(world, 6.0f);

        // --- dig (rate-limited by block hardness / tool) ---
        dig_cooldown_ -= dt;
        if (in.dig && last_hit_.hit && dig_cooldown_ <= 0.0f) {
            float interval = dig_interval();
            do_dig();
            dig_cooldown_ = interval;
        }
        if (!in.dig) dig_cooldown_ = 0.0f;

        // --- place (edge-triggered by caller) ---
        if (in.place && last_hit_.hit) do_place();
    }

    void render(int fb_w, int fb_h) {
        glm::mat4 view = glm::lookAt(player.eye(), player.eye() + player.forward(), glm::vec3(0,1,0));
        float aspect = fb_h > 0 ? (float)fb_w / (float)fb_h : 1.7778f;
        glm::mat4 proj = glm::perspective(glm::radians(70.0f), aspect, 0.05f, 1000.0f);

        glm::vec3 sky(0.55f, 0.72f, 0.92f);
        glClearColor(sky.r, sky.g, sky.b, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_CULL_FACE); glCullFace(GL_BACK); glFrontFace(GL_CCW);

        renderer_.sync(world);
        glm::vec3 sun = glm::normalize(glm::vec3(0.4f, 0.85f, 0.3f));
        float fog_end = (8 * CHUNK_SX) * 0.95f;
        renderer_.render(world, view, proj, player.eye(), sun, sky, fog_end * 0.55f, fog_end);

        if (last_hit_.hit)
            renderer_.render_selection(view, proj, glm::ivec3(last_hit_.bx, last_hit_.by, last_hit_.bz));
    }

    const RayHit& target() const { return last_hit_; }

private:
    ChunkRenderer renderer_;
    std::unique_ptr<WorldOps> ops_;   // replication seam (local or networked)
    RayHit last_hit_;
    float  dig_cooldown_ = 0.0f;
    float  dig_radius_ = 1.8f;   // smooth spherical carve radius (blocks), MassRTS-style

    void spawn_player() {
        int h = world.surface_height(0, 0);
        player.pos = glm::vec3(0.5f, (float)(h + 2), 0.5f);
    }

    void give_starter_items() {
        // A small starter kit so the build/craft loop is usable immediately.
        inv.add(block_item(BLOCK_LOG), 16, item_max_stack(block_item(BLOCK_LOG)));
        inv.add(block_item(BLOCK_COBBLE), 32, STACK_MAX);
        inv.add(ITEM_WOOD_PICKAXE, 1, 1);
        inv.add(block_item(BLOCK_TORCH), 16, STACK_MAX);
    }

    // Craft using a grid snapshot (row-major gw*gh). On success, consumes one of
    // each grid item and adds the result to the inventory. Returns true if crafted.
    bool try_craft(const ItemId* grid, int gw, int gh) {
        CraftResult res = recipes.match(grid, gw, gh);
        if (res.id == ITEM_NONE) return false;
        // consume one of each non-empty grid cell from the inventory
        for (int i = 0; i < gw*gh; i++) {
            if (grid[i] == ITEM_NONE) continue;
            consume_one(grid[i]);
        }
        inv.add(res.id, res.count, item_max_stack(res.id));
        return true;
    }

    void consume_one(ItemId id) {
        for (auto& s : inv.slots) {
            if (s.id == id && s.count > 0) { if (--s.count == 0) s.clear(); return; }
        }
    }

    // Load chunks within view_radius, unload well beyond it (hysteresis).
    // CRITICAL fix for the "walk a bit then freeze" stutter: chunk generation
    // (noise + trees + ores) is expensive, and crossing a chunk border used to
    // pull a whole ring of brand-new chunks into existence *in a single frame*,
    // stalling the main thread for hundreds of ms. We now apply a per-frame
    // GENERATION BUDGET: only the few nearest not-yet-loaded chunks are created
    // each frame, nearest first, and the rest stream in over subsequent frames.
    // Meshing is already bounded (ChunkRenderer::sync max_rebuild), so the two
    // together keep frame time flat while exploring the infinite world.
    void stream_chunks(int view_radius, int gen_budget = 4) {
        ChunkKey center = World::world_to_chunk((int)floorf(player.pos.x), (int)floorf(player.pos.z));

        // Gather missing chunks in the view disk, sorted by distance (nearest
        // first) so the ground under/around the player always fills in first.
        struct Pend { int dx, dz, d2; };
        std::vector<Pend> pending;
        for (int dz = -view_radius; dz <= view_radius; dz++)
        for (int dx = -view_radius; dx <= view_radius; dx++) {
            int d2 = dx*dx + dz*dz;
            if (d2 > view_radius*view_radius) continue;
            ChunkKey k{center.cx + dx, center.cz + dz};
            if (!world.chunk_loaded(k)) pending.push_back({dx, dz, d2});
        }
        std::sort(pending.begin(), pending.end(),
                  [](const Pend& a, const Pend& b){ return a.d2 < b.d2; });

        int made = 0;
        for (const Pend& p : pending) {
            if (made >= gen_budget) break;   // spread the rest over later frames
            world.get_chunk({center.cx + p.dx, center.cz + p.dz}, true);
            made++;
        }

        // Unload chunks well past the view radius (hysteresis avoids thrashing
        // at the boundary as the player paces back and forth).
        int unload = view_radius + 3;
        auto& cm = world.chunks();
        for (auto it = cm.begin(); it != cm.end();) {
            int dx = it->first.cx - center.cx, dz = it->first.cz - center.cz;
            if (dx*dx + dz*dz > unload*unload) it = cm.erase(it); // (Phase M: save dirty first)
            else ++it;
        }
    }

    void do_dig() {
        BlockId center = world.get_block(last_hit_.bx, last_hit_.by, last_hit_.bz);
        if (center == BLOCK_AIR || center == BLOCK_BEDROCK) return;

        // --- smooth spherical carve (MassRTS sdf_terrain style) ---
        // Modify the continuous SDF field with a smooth-min sphere centred on the
        // precise ray contact point. Marching Cubes then meshes a rounded bowl
        // (no stair-stepping), and carve_sphere reports which blocks flipped to
        // air so we can award drops with the usual tool-tier gate.
        const ItemDef& tool = item_def(inv.held().id);
        glm::vec3 hp = last_hit_.point;
        std::vector<std::array<int,4>> flips;
        // Route through the replication seam (local apply + prediction; the
        // network backend additionally sends/validates/broadcasts the edit).
        ops_->carveSphere(hp.x, hp.y, hp.z, dig_radius_, -1, &flips);

        for (auto& f : flips) {
            BlockId b = (BlockId)f[3];
            uint8_t need = block_min_tier(b);
            bool right_tool = (block_pref_tool(b) == ToolKind::None) ||
                              (tool.tool == block_pref_tool(b));
            if (need > 0 && (!right_tool || tool.tier < need)) continue; // no drop
            BlockId drop = (b == BLOCK_STONE) ? BLOCK_COBBLE
                         : (b == BLOCK_GRASS) ? BLOCK_DIRT
                         : b;
            inv.add(block_item(drop), 1, item_max_stack(block_item(drop)));
        }
    }

    // Dig cooldown scaled by tool match (faster with the right tool/tier).
    float dig_interval() {
        BlockId b = world.get_block(last_hit_.bx, last_hit_.by, last_hit_.bz);
        const ItemDef& tool = item_def(inv.held().id);
        float base = block_def(b).hardness;
        if (base <= 0.0f) return 0.12f;
        float mult = (tool.tool == block_pref_tool(b)) ? tool.dig_mult : 1.0f;
        float t = (base / mult) * 0.18f;
        return t < 0.08f ? 0.08f : (t > 1.2f ? 1.2f : t);
    }

    void do_place() {
        ItemStack& h = inv.held();
        if (h.empty() || !item_is_block(h.id)) return;
        int px = last_hit_.bx + last_hit_.nx;
        int py = last_hit_.by + last_hit_.ny;
        int pz = last_hit_.bz + last_hit_.nz;
        if (world.get_block(px, py, pz) != BLOCK_AIR) return;
        if (overlaps_player(px, py, pz)) return; // don't seal yourself in
        // Route through replication seam (local apply now; net send when online).
        if (ops_->setBlock(px, py, pz, item_block(h.id)))
            inv.consume_held();
    }

    bool overlaps_player(int bx, int by, int bz) {
        glm::vec3 p = player.pos;
        float minx = p.x - Player::HALF_W, maxx = p.x + Player::HALF_W;
        float miny = p.y, maxy = p.y + Player::HEIGHT;
        float minz = p.z - Player::HALF_W, maxz = p.z + Player::HALF_W;
        return (bx+1 > minx && bx < maxx) && (by+1 > miny && by < maxy) && (bz+1 > minz && bz < maxz);
    }
};

} // namespace sdfcraft
