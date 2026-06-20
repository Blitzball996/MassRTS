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
#include "hud_renderer.h"
#include "sky_renderer.h"
#include "world_ops.h"
#include "planet.h"
#include "planet_mesh.h"
#include "planet_renderer.h"
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
    bool  request_fly = false;   // dedicated key: take off / enter fly mode
    bool  request_walk = false;  // dedicated key: land / enter walk mode
    bool  fly_boost = false;  // hold to fly fast (planet-scale travel)
    bool  toggle_planet = false;  // G: globe overview view
    int   hotbar_set = -1;   // 0..8 to set slot directly, -1 = none
    
    // Terrain sculpting mode keys (1-5 = modes, R/T = radius/strength)
    bool key_1 = false, key_2 = false, key_3 = false, key_4 = false, key_5 = false;
    bool key_r_down = false, key_r_up = false;
    bool key_t_down = false, key_t_up = false;
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
        hud_ready_ = hud_.init();   // 2D overlay; non-fatal if it fails
        // Gradient sky dome (atmospheric blue + drifting clouds + sun). This was
        // dead code before — the header was included but never instantiated, so
        // the background was a flat clear colour. Non-fatal if shaders missing.
        sky_ready_ = sky_.init(shader_dir);
        // Planet overview renderer (same game: fly up high + press G to see the
        // whole globe this local world sits on). Non-fatal if it fails.
        planet_ready_ = planet_rend_.init();
        planet_.height = [](const dvec3& dir)->double {
            double n = std::sin(dir.x*8.0)*std::cos(dir.y*6.0)*std::sin(dir.z*7.0);
            n += 0.5*std::sin(dir.x*23.0+1.0)*std::sin(dir.z*19.0);
            return n * 3000.0;
        };
        spawn_player();
        give_starter_items();
        return true;
    }

    // Multiplayer entry point (network phase): replace the local backend with a
    // server-authoritative + client-prediction backend. Gameplay is untouched.
    void useNetworkOps(bool is_server) {
        ops_ = std::make_unique<NetWorldOps>(world, is_server);
    }
    void shutdown() { renderer_.shutdown(); if (sky_ready_) sky_.shutdown(); }

    void update(const FrameInput& in, float dt, int view_radius = 24) {
        view_radius_ = view_radius;
        time_ += dt;   // drives sky cloud drift
        // --- look ---
        player.yaw   += in.look_dx;
        player.pitch -= in.look_dy;
        if (player.pitch > 89.0f) player.pitch = 89.0f;
        if (player.pitch < -89.0f) player.pitch = -89.0f;

        if (in.toggle_fly) player.flying = !player.flying;
        if (in.request_fly)  player.flying = true;   // dedicated take-off key
        if (in.request_walk) player.flying = false;  // dedicated land key
        if (in.toggle_planet && planet_ready_) planet_view_ = !planet_view_;
        if (in.hotbar_set >= 0) inv.selected = in.hotbar_set;
        if (in.hotbar_scroll) inv.scroll(in.hotbar_scroll);

        stream_chunks(view_radius, 16);   // wider view + higher budget fills faster

        // Pump the replication backend (no-op locally; polls net + reconciles
        // when networked). Keeps gameplay identical across single/multiplayer.
        if (ops_) ops_->tick(dt);

        // --- physics ---
        glm::vec3 wish(in.move_x, 0, in.move_z);
        player.update(world, dt, wish, in.jump, in.crouch,
                      in.fly_boost ? 12.0f : 1.0f);
        player.update_camera_smoothing(dt);

        // --- targeting ---
        last_hit_ = player.raycast(world, 6.0f);

        // --- Sculpting mode & tool switching (1-5 keys, R/T for radius/strength) ---
        if (in.key_1) sculpt_mode_ = SculptMode::DIG;
        if (in.key_2) sculpt_mode_ = SculptMode::RAISE;
        if (in.key_3) sculpt_mode_ = SculptMode::SMOOTH;
        if (in.key_4) sculpt_mode_ = SculptMode::FLATTEN;
        if (in.key_5) sculpt_mode_ = SculptMode::BOXIFY;
        if (in.key_r_down) dig_radius_ = std::min(dig_radius_ + 0.3f, 8.0f);  // increase radius
        if (in.key_r_up) dig_radius_ = std::max(dig_radius_ - 0.3f, 0.5f);   // decrease radius
        if (in.key_t_down) sculpt_strength_ = std::min(sculpt_strength_ + 0.1f, 2.0f);
        if (in.key_t_up) sculpt_strength_ = std::max(sculpt_strength_ - 0.1f, 0.1f);

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
        glm::vec3 eye = player.render_eye();
        glm::mat4 view = glm::lookAt(eye, eye + player.forward(), glm::vec3(0,1,0));
        float aspect = fb_h > 0 ? (float)fb_w / (float)fb_h : 1.7778f;
        // Far plane scales with how far we can actually see: on the ground it's
        // ~1km, but climbing toward orbit we push it out to tens of km so the
        // horizon and the planet below stay visible instead of being clipped.
        float alt = player.pos.y;
        float far_plane = 2500.0f + std::max(0.0f, alt) * 40.0f;
        if (far_plane > 200000.0f) far_plane = 200000.0f;
        glm::mat4 proj = glm::perspective(glm::radians(70.0f), aspect, 0.05f, far_plane);

        // Sky darkens toward deep blue / black as you climb toward orbit, so the
        // clear colour matches the thinning fog for a seamless ground->space fade.
        float sky_t = glm::clamp((std::max(0.0f, alt) - 80.0f) / 4000.0f, 0.0f, 1.0f);
        glm::vec3 sky = glm::mix(glm::vec3(0.55f, 0.72f, 0.92f),
                                 glm::vec3(0.02f, 0.03f, 0.08f), sky_t);
        glClearColor(sky.r, sky.g, sky.b, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_CULL_FACE); glCullFace(GL_BACK); glFrontFace(GL_CCW);

        glm::vec3 sun = glm::normalize(glm::vec3(0.4f, 0.85f, 0.3f));

        // --- Gradient sky dome (drawn first, fills the background) -----------
        // A real procedural sky (blue gradient + sun + drifting clouds) instead
        // of the old flat clear colour. It draws at the far plane so any terrain
        // or the planet backdrop composites on top of it. Skip it at high
        // altitude where the clear-colour space fade takes over.
        if (sky_ready_ && sky_t < 0.85f)
            sky_.render(view, proj, sun, time_);

        if (planet_view_ && planet_ready_) {
            // Pull the camera way back so the whole globe fits in frame.
            // Mesh is built camera-relative (double precision), so the eye
            // sits at the origin and the planet centre is in front of it.
            double R = planet_.cfg.radius_m;
            dvec3  cam_pos(0.0, 0.0, R * 3.0);
            planet_.update_lod(cam_pos);
            std::vector<PlanetVertex> verts;
            planet_.build(cam_pos, verts);
            planet_rend_.upload(verts);

            glm::vec3 center_rel = glm::vec3(-cam_pos.x, -cam_pos.y, -cam_pos.z);
            glm::mat4 pview = glm::lookAt(glm::vec3(0.0f), center_rel, glm::vec3(0,1,0));
            glm::mat4 pproj = glm::perspective(glm::radians(45.0f), aspect,
                                               (float)(R * 0.01),
                                               (float)(R * 10.0));
            planet_rend_.render(pview, pproj, sun, glm::vec3(0,1,0));
            return;
        }

        // --- Seamless planet backdrop --------------------------------------
        // The flat local block world sits on the surface of the globe. We place
        // the planet centre R metres straight below local sea level and draw the
        // LOD globe as a far backdrop FIRST, then clear depth and draw the local
        // terrain on top. Low down you just see a faint curved horizon; climbing
        // smoothly reveals real planetary curvature and the limb against space —
        // no fog wall, a continuous ground->sky->orbit gradient.
        if (planet_ready_) {
            render_planet_backdrop(eye, view, aspect, alt);
            glClear(GL_DEPTH_BUFFER_BIT);   // local terrain always on top
        }

        // Meshing is now memory-bound and sub-millisecond per chunk, so we can
        // afford a larger time budget to resolve a freshly-streamed region fast
        // without reintroducing the old multi-chunk frame spikes.
        renderer_.sync(world, player.eye(), 6.0);
        // Fog tuned to the loaded view distance, and pushed WAY back as you climb
        // so flying up doesn't bury you in haze. At altitude the fog all but
        // disappears, giving the seamless ground->sky->planet gradient.
        float view_dist = (float)view_radius_ * CHUNK_SX;
        float climb = std::max(0.0f, alt - 80.0f);
        float fog_end = view_dist * 0.95f + climb * 30.0f;
        float fog_start = fog_end * 0.70f + climb * 20.0f;
        // Sky/fog colour lightens and the fog thins with altitude so the
        // transition to the deep-blue upper atmosphere reads smoothly.
        float t_alt = glm::clamp(climb / 4000.0f, 0.0f, 1.0f);
        glm::vec3 fog_color = glm::mix(sky, glm::vec3(0.02f, 0.03f, 0.08f), t_alt);
        renderer_.render(world, view, proj, eye, sun, fog_color, fog_start, fog_end);

        if (last_hit_.hit)
            renderer_.render_selection(view, proj, glm::ivec3(last_hit_.bx, last_hit_.by, last_hit_.bz));

        // 2D HUD overlay last, on top of everything.
        if (hud_ready_)
            hud_.draw(fb_w, fb_h, player, inv);
    }

    const RayHit& target() const { return last_hit_; }

private:
    // Draw the LOD globe as a backdrop behind the local terrain. The planet
    // centre sits R metres below local sea level so the local ground rests on
    // the sphere's surface; as the player climbs, more of the curve/limb shows.
    void render_planet_backdrop(const glm::vec3& eye, const glm::mat4& view,
                                float aspect, float alt) {
        double R = planet_.cfg.radius_m;
        // Camera position in planet space: straight up from the surface by the
        // player's altitude above sea level (local x/z map to a tiny tangent
        // offset that's negligible against an Earth-sized radius, so we keep the
        // camera on the local "up" axis for a stable, jitter-free backdrop).
        double sea = (double)world.sea_level;
        double h_above = (double)alt - sea;            // metres above sea level
        if (h_above < 1.0) h_above = 1.0;
        dvec3 cam_pos(0.0, R + h_above, 0.0);          // +Y is local up

        planet_.update_lod(cam_pos);
        std::vector<PlanetVertex> verts;
        planet_.build(cam_pos, verts);
        planet_rend_.upload(verts);

        // Reuse the player's orientation but with the planet's own projection so
        // the horizon lines up. View rotation only (eye at origin = floating
        // origin); the mesh is already camera-relative.
        glm::mat4 rot = glm::mat4(glm::mat3(view));
        // Far plane must reach the limb: distance to horizon ~ sqrt(2*R*h).
        float horizon = (float)std::sqrt(2.0 * R * h_above) + (float)R * 0.05f;
        glm::mat4 pproj = glm::perspective(glm::radians(70.0f), aspect,
                                           std::max(1.0f, (float)h_above * 0.5f),
                                           horizon * 1.2f);
        glm::vec3 sun = glm::normalize(glm::vec3(0.4f, 0.85f, 0.3f));
        glDepthMask(GL_FALSE);
        planet_rend_.render(rot, pproj, sun, glm::vec3(0,1,0));
        glDepthMask(GL_TRUE);
    }


    ChunkRenderer  renderer_;
    HudRenderer    hud_;
    bool           hud_ready_ = false;
    SkyRenderer    sky_;
    bool           sky_ready_ = false;
    float          time_ = 0.0f;        // seconds, drives sky cloud drift
    PlanetMesh     planet_;
    PlanetRenderer planet_rend_;
    bool           planet_view_ = false;   // G toggles globe overview
    bool           planet_ready_ = false;
    int            view_radius_ = 8;   // last view radius (for fog/far scaling)
    std::unique_ptr<WorldOps> ops_;   // replication seam (local or networked)
    RayHit last_hit_;
    float  dig_cooldown_ = 0.0f;
    float  dig_radius_ = 1.8f;   // smooth spherical carve radius (blocks), MassRTS-style
    
    // Terrain sculpting mode (MassRTS-style tools)
    enum class SculptMode { DIG, RAISE, SMOOTH, FLATTEN, BOXIFY } sculpt_mode_ = SculptMode::DIG;
    float sculpt_strength_ = 1.0f;

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
    void stream_chunks(int view_radius, int gen_budget = 6) {
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

        const ItemDef& tool = item_def(inv.held().id);

        // --- object blocks (trees / planks / placed blocks) are NOT part of the
        // smooth SDF terrain field, so carve_sphere can't touch them. Mine them
        // as discrete blocks: remove the hit block and drop it. This is what
        // makes trees choppable (and any built structure breakable). ---
        if (!block_is_terrain(center)) {
            uint8_t need = block_min_tier(center);
            bool right_tool = (block_pref_tool(center) == ToolKind::None) ||
                              (tool.tool == block_pref_tool(center));
            ops_->setBlock(last_hit_.bx, last_hit_.by, last_hit_.bz, BLOCK_AIR);
            if (!(need > 0 && (!right_tool || tool.tier < need)))
                inv.add(block_item(center), 1, item_max_stack(block_item(center)));
            
            // === Tree physics: check if breaking this block causes tree to collapse ===
            if (center == BLOCK_LOG || center == BLOCK_LEAVES) {
                std::vector<std::array<int,4>> drops;
                world.check_tree_collapse(last_hit_.bx, last_hit_.by, last_hit_.bz, &drops);
                // Award dropped items from collapsed tree blocks
                for (auto& d : drops) {
                    BlockId dropped = (BlockId)d[3];
                    inv.add(block_item(dropped), 1, item_max_stack(block_item(dropped)));
                }
            }
            return;
        }

        // --- terrain sculpting with multiple modes (MassRTS-style tools) ---
        glm::vec3 hp = last_hit_.point;
        
        switch (sculpt_mode_) {
            case SculptMode::DIG: {
                // Original dig: smooth spherical carve
                std::vector<std::array<int,4>> flips;
                ops_->carveSphere(hp.x, hp.y, hp.z, dig_radius_, -1, &flips);
                for (auto& f : flips) {
                    BlockId b = (BlockId)f[3];
                    uint8_t need = block_min_tier(b);
                    bool right_tool = (block_pref_tool(b) == ToolKind::None) ||
                                      (tool.tool == block_pref_tool(b));
                    if (need > 0 && (!right_tool || tool.tier < need)) continue;
                    BlockId drop = (b == BLOCK_STONE) ? BLOCK_COBBLE
                                 : (b == BLOCK_GRASS) ? BLOCK_DIRT : b;
                    inv.add(block_item(drop), 1, item_max_stack(block_item(drop)));
                }
                // Auto-smooth after deep digging to prevent marching cubes artifacts
                if (hp.y < 30.0f && !flips.empty()) {
                    world.smooth_terrain(hp.x, hp.y, hp.z, dig_radius_ * 1.1f, 0.3f);
                }
                break;
            }
            case SculptMode::RAISE: {
                // Push terrain up (mountain building)
                world.raise_terrain(hp.x, hp.y, hp.z, dig_radius_ * 1.5f, sculpt_strength_ * 0.3f);
                break;
            }
            case SculptMode::SMOOTH: {
                // Smooth out marching cubes artifacts
                world.smooth_terrain(hp.x, hp.y, hp.z, dig_radius_ * 1.2f, sculpt_strength_ * 0.5f);
                break;
            }
            case SculptMode::FLATTEN: {
                // Flatten to hit point's Y level (platforms)
                world.flatten_terrain(hp.x, hp.y, hp.z, dig_radius_ * 1.5f, hp.y, sculpt_strength_ * 0.35f);
                break;
            }
            case SculptMode::BOXIFY: {
                // Turn smooth cave into cubic room (dungeons)
                world.boxify_terrain(hp.x, hp.y, hp.z, dig_radius_, sculpt_strength_ * 0.7f);
                break;
            }
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
        
        // Allow placing on terrain surface (embed into ground like Minecraft)
        int px = last_hit_.bx + last_hit_.nx;
        int py = last_hit_.by + last_hit_.ny;
        int pz = last_hit_.bz + last_hit_.nz;
        
        // If target is terrain (not air/water), place ON the hit block instead of adjacent
        BlockId target = world.get_block(px, py, pz);
        if (block_is_terrain(target)) {
            // Place on the terrain surface (embed into ground)
            px = last_hit_.bx;
            py = last_hit_.by;
            pz = last_hit_.bz;
        } else if (target != BLOCK_AIR) {
            return; // can't place if space occupied by non-terrain block
        }
        
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
