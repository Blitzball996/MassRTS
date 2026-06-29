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
#include "inventory_screen.h"
#include "mc_model.h"
#include "sky_renderer.h"
#include "world_ops.h"
#include "server_sim.h"
#include "game_server.h"
#include "net_session.h"
#include "net_protocol.h"
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
#include <string>
#include <unordered_map>

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
    bool  attack = false;    // left click (edge) — melee a mob if one is targeted
    bool  eat = false;       // consume held food (edge)
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
    // Inventory screen (E to toggle). While open, look/move are frozen and the
    // cursor is free; clicks drive the GUI instead of dig/place.
    bool  inv_toggle = false;     // edge: open/close inventory
    // craft_toggle removed: workbench 3×3 via right-click block (future)
    bool  mouse_click = false;    // edge: left click (GUI pick/place)
    bool  mouse_right = false;    // edge: right click (GUI half/one)
    float mouse_x = 0, mouse_y = 0;  // cursor pixel pos (GUI mode)
};

// Remote player as seen by a client (for rendering other people).
// Snapshots arrive at ~10Hz; the extra fields drive client-side render
// interpolation so other players glide between snapshots instead of teleporting.
struct RemotePlayer {
    uint8_t id = 0;
    glm::vec3 pos{0,0,0};       // latest authoritative position (snapshot target)
    float yaw = 0, pitch = 0;
    std::string name;
    // --- client-side render smoothing (Task 1, purely local) ---
    glm::vec3 prev_pos{0,0,0};  // rendered position at the last snapshot boundary
    glm::vec3 target_pos{0,0,0};// position to interpolate toward (latest snapshot)
    glm::vec3 render_pos{0,0,0};// interpolated position used for drawing
    float prev_yaw = 0, target_yaw = 0, render_yaw = 0;
    float lerp_t = 1.0f;        // 0..1 progress prev->target; 1 = settled
    bool  seeded = false;       // first snapshot snaps in place (no interp)
    bool  moving = false;       // authoritative walk state (drives leg/arm anim)
};

// How this Mode instance participates in the session.
enum class Role {
    Solo,    // local ServerSim, no sockets
    Host,    // local ServerSim + GameServer (listen server), local player is slot 0
    Client   // no sim; mirrors the server's authoritative snapshots
};

class Mode {
public:
    World      world;
    Player     player;
    Inventory  inv;
    RecipeBook recipes;

    // Single-player / host init (runs the authoritative sim locally).
    bool init(uint64_t seed, const std::string& shader_dir) {
        return init_role(Role::Solo, seed, shader_dir, "", SDFCRAFT_DEFAULT_PORT);
    }

    // Start as a listen-host: same as solo but also serves remote clients.
    bool initHost(uint64_t seed, const std::string& shader_dir, uint16_t port) {
        return init_role(Role::Host, seed, shader_dir, "", port);
    }

    // Connect to a server as a client: world is driven by snapshots, not a sim.
    bool initClient(const std::string& ip, uint16_t port, const std::string& shader_dir) {
        return init_role(Role::Client, 0, shader_dir, ip, port);
    }

    bool init_role(Role role, uint64_t seed, const std::string& shader_dir,
                   const std::string& ip, uint16_t port) {
        role_ = role;

        if (role_ == Role::Client) {
            net_init_ = std::make_unique<NetInit>();
            client_ = std::make_unique<NetClient>();
            if (!client_->connect(ip, port)) {
                std::cerr << "[sdfcraft] connect to " << ip << ":" << port << " failed\n";
                return false;
            }
            client_->send(enc_hello("player"));
            // World seed arrives in Welcome; start with a provisional world so
            // chunk streaming/rendering works while we wait one frame for it.
            world = World(1337);
            ops_ = std::make_unique<LocalWorldOps>(world);   // client-side prediction
        } else {
            // Solo and Host both run the authoritative simulation locally,
            // operating on THIS Mode's `world` so what we render/collide is
            // exactly what the sim authoritatively edits.
            world = World(seed);
            ops_ = std::make_unique<LocalWorldOps>(world);
            if (role_ == Role::Host) {
                net_init_ = std::make_unique<NetInit>();
                server_ = std::make_unique<GameServer>(world, seed, port, /*local host player*/ true);
                if (!server_->ok()) {
                    std::cerr << "[sdfcraft] host listen on port " << port << " failed\n";
                    return false;
                }
            } else {
                solo_sim_ = std::make_unique<ServerSim>(world, seed);
                solo_sim_->addPlayer(0, "player");
            }
        }

        // Renderers (all roles render).
        if (!renderer_.init(shader_dir)) return false;
        hud_ready_  = hud_.init();
        inv_scr_ready_ = inv_screen_.init();
        mob_ready_  = mobs_rend_.init();
        sky_ready_  = sky_.init(shader_dir);
        planet_ready_ = planet_ready_ = planet_rend_.init();
        planet_.height = [](const dvec3& dir)->double {
            double n = std::sin(dir.x*8.0)*std::cos(dir.y*6.0)*std::sin(dir.z*7.0);
            n += 0.5*std::sin(dir.x*23.0+1.0)*std::sin(dir.z*19.0);
            return n * 3000.0;
        };

        spawn_player();
        give_starter_items();
        return true;
    }

    // Accessor: the authoritative sim for solo/host (null on a pure client).
    ServerSim* sim() {
        if (role_ == Role::Host && server_) return &server_->sim();
        if (role_ == Role::Solo && solo_sim_) return solo_sim_.get();
        return nullptr;
    }

    // True while any full-screen GUI (inventory) is open — the entry
    // point frees the cursor and stops mouse-look so the player can click.
    bool invOpen() const { return inv_open_; }

    void shutdown() {
        renderer_.shutdown();
        if (mob_ready_) mobs_rend_.shutdown();
        if (sky_ready_) sky_.shutdown();
        if (inv_scr_ready_) inv_screen_.destroy();
    }

    void update(const FrameInput& in, float dt, int view_radius = 24) {
        view_radius_ = view_radius;
        time_ += dt;   // drives sky cloud drift

        // --- network receive (client mirrors authoritative state) ---
        if (role_ == Role::Client) client_receive();

        // --- inventory screen: toggle + while-open input capture ---
        if (in.inv_toggle) {
            inv_open_ = !inv_open_;
            if (!inv_open_ && inv_scr_ready_) inv_screen_.returnAll(inv);  // don't lose dragged items
        }
        if (inv_open_) {
            // GUI owns the mouse: route clicks to slots, freeze look/world/physics.
            last_cursor_x_ = in.mouse_x; last_cursor_y_ = in.mouse_y;
            if (inv_scr_ready_ && (in.mouse_click || in.mouse_right))
                inv_screen_.click(inv, recipes, last_fbw_, last_fbh_, in.mouse_x, in.mouse_y, in.mouse_right);
            advance_sim(in, dt);
            return;
        }

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

        // --- death lock: a dead player can't move/act, only respawn ---
        if (player.dead) {
            if (in.jump || in.place || in.attack) request_respawn();
            player.update_camera_smoothing(dt);
            advance_sim(in, dt);
            return;
        }

        // --- physics ---
        glm::vec3 wish(in.move_x, 0, in.move_z);
        player.update(world, dt, wish, in.jump, in.crouch,
                      in.fly_boost ? 12.0f : 1.0f);
        push_out_of_mobs();   // mobs are solid: don't let the player stand inside one
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

        // --- melee attack (edge): swing at a targeted mob before falling back to dig ---
        if (in.attack) do_attack();

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

        // --- eat held food (edge) ---
        if (in.eat) do_eat();

        // --- advance authoritative sim (solo/host) + sync local player state ---
        advance_sim(in, dt);
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

        // Day/night: the sun direction + sky brightness come from the
        // authoritative time-of-day (driven by the sim; mirrored on clients).
        float tod = time_of_day();
        float day_f = ServerSim::daylight(tod);   // 0 night .. 1 day
        glm::vec3 sun_dir = ServerSim::sun_from_time(tod);

        // Sky darkens toward deep blue / black as you climb toward orbit, so the
        // clear colour matches the thinning fog for a seamless ground->space fade.
        float sky_t = glm::clamp((std::max(0.0f, alt) - 80.0f) / 4000.0f, 0.0f, 1.0f);
        // Daytime sky blue dims to a dark night blue by daylight factor.
        glm::vec3 day_sky   = glm::vec3(0.55f, 0.72f, 0.92f);
        glm::vec3 night_sky = glm::vec3(0.03f, 0.04f, 0.10f);
        glm::vec3 ground_sky = glm::mix(night_sky, day_sky, day_f);
        glm::vec3 sky = glm::mix(ground_sky, glm::vec3(0.02f, 0.03f, 0.08f), sky_t);
        glClearColor(sky.r, sky.g, sky.b, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_CULL_FACE); glCullFace(GL_BACK); glFrontFace(GL_CCW);

        glm::vec3 sun = sun_dir;

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

        // --- mobs + other players (MC textured models) ---
        if (mob_ready_) {
            const std::vector<Entity>& draw_mobs = mob_list();
            mobs_rend_.render(draw_mobs, view, proj, sun, time_);
            // other players drawn as the Steve model (clients see each other;
            // host sees its connected clients). Faces travel dir, walks when moving.
            if (!remote_render_.empty())
                mobs_rend_.renderPlayers(remote_render_, view, proj, sun, time_);
        }

        if (last_hit_.hit)
            renderer_.render_selection(view, proj, glm::ivec3(last_hit_.bx, last_hit_.by, last_hit_.bz));

        // 2D HUD overlay last, on top of everything.
        if (hud_ready_)
            hud_.draw(fb_w, fb_h, player, inv);

        // remember fb size for GUI hit-testing this frame
        last_fbw_ = fb_w; last_fbh_ = fb_h;
        // inventory screen draws on top of the HUD when open
        if (inv_open_ && inv_scr_ready_)
            inv_screen_.draw(inv, recipes, fb_w, fb_h, last_cursor_x_, last_cursor_y_);
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
    InventoryScreen inv_screen_;
    bool           inv_scr_ready_ = false;
    bool           inv_open_ = false;
    int            last_fbw_ = 1600, last_fbh_ = 900;   // last framebuffer size (for GUI hit-test)
    float          last_cursor_x_ = 0, last_cursor_y_ = 0;
    McModelRenderer mobs_rend_;
    bool           mob_ready_ = false;
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

    // --- session role + networking ---
    Role role_ = Role::Solo;
    std::unique_ptr<NetInit>    net_init_;
    std::unique_ptr<GameServer> server_;    // Host only
    std::unique_ptr<ServerSim>  solo_sim_;  // Solo only
    std::unique_ptr<NetClient>  client_;    // Client only
    uint8_t my_id_ = 0;                     // assigned player id (client)
    float   move_send_timer_ = 0.0f;        // throttle PlayerMove sends
    // client mirror of authoritative state:
    float   client_tod_ = 0.25f;            // time-of-day from TimeSync
    std::vector<Entity> client_mobs_;       // mobs reconstructed from MobSnapshot (interpolated for render)
    std::unordered_map<int, RemotePlayer> remotes_;   // other players (id -> state)
    std::vector<PlayerRender> remote_render_;   // other players, drawn as Steve

    // --- client-side snapshot interpolation (Task 1, render smoothing only) ---
    // Snapshots arrive at ~10Hz; we glide entities between them at framerate.
    static constexpr float SNAPSHOT_INTERVAL = 0.10f;   // matches server 10Hz broadcast
    struct MobInterp {
        Entity   ent;            // last snapshot's full data (kind/health/yaw/id)
        glm::vec3 prev_pos{0,0,0};
        glm::vec3 target_pos{0,0,0};
        float     prev_yaw = 0, target_yaw = 0;
        float     lerp_t = 1.0f; // 0..1 progress; reset to 0 on each new snapshot
        bool      seeded = false;// first appearance snaps in place
        bool      live = false;  // present in the most recent snapshot
    };
    std::unordered_map<uint32_t, MobInterp> mob_interp_;   // keyed by mob id

    // Terrain sculpting mode (MassRTS-style tools)
    enum class SculptMode { DIG, RAISE, SMOOTH, FLATTEN, BOXIFY } sculpt_mode_ = SculptMode::DIG;
    float sculpt_strength_ = 1.0f;

    // --- role-aware helpers -------------------------------------------------
    // Authoritative time-of-day: from the local sim (solo/host) or the last
    // TimeSync (client). Drives the sun + sky in render().
    float time_of_day() {
        if (ServerSim* s = sim()) return s->time_of_day;
        return client_tod_;
    }

    // Mobs to draw: the sim's live list (solo/host) or the client's mirror.
    const std::vector<Entity>& mob_list() {
        if (ServerSim* s = sim()) return s->mobs.entities;
        return client_mobs_;
    }

    // Advance the authoritative sim (solo/host) and pull the local player's
    // stats back out of it; on a client, push our move and apply mirrored stats.
    void advance_sim(const FrameInput& in, float dt) {
        // Am I walking this tick? (drives my avatar's walk anim for everyone else.)
        bool moving = (in.move_x != 0.0f || in.move_z != 0.0f) && !player.flying;
        if (role_ == Role::Client) {
            // send our predicted position to the server (throttled ~20 Hz)
            move_send_timer_ -= dt;
            if (move_send_timer_ <= 0.0f && client_ && client_->connected()) {
                move_send_timer_ = 0.05f;
                NetPlayerState s{ my_id_, player.pos.x, player.pos.y, player.pos.z,
                                  player.yaw, player.pitch, moving ? (uint8_t)1 : (uint8_t)0 };
                client_->send(enc_move(s));
            }
            advance_interp(dt);        // smooth remotes + mobs toward latest snapshot
            rebuild_remote_render();   // builds boxes from interpolated render_pos
            return;
        }

        ServerSim* s = sim();
        if (!s) return;
        // keep the host/solo local player's authoritative position in sync so
        // mobs target where we actually are and survival drowning reads our eye.
        if (ServerPlayer* sp = s->player(0)) {
            sp->pos = player.pos; sp->yaw = player.yaw; sp->pitch = player.pitch;
            sp->moving = moving;
            sp->armor_points = total_armor(inv);   // worn armor → authoritative defense
            sp->avatar.air = player.air;  // carry air so drowning is continuous
        }

        if (role_ == Role::Host) server_->update(dt);
        else                     s->tick(dt);

        // pull authoritative survival stats back into the rendered player
        if (ServerPlayer* sp = s->player(0)) {
            // survival_tick lives on the avatar; mirror its results to `player`
            player.health     = sp->avatar.health;
            player.max_health = sp->avatar.max_health;
            player.hunger     = sp->avatar.hunger;
            player.air        = sp->avatar.air;
            player.dead       = sp->dead;
            if (sp->avatar.hurt_flash > player.hurt_flash) player.hurt_flash = sp->avatar.hurt_flash;
        }
        rebuild_remote_render();
    }

    // Receive + apply authoritative snapshots on a client.
    void client_receive() {
        if (!client_) return;
        for (auto& bytes : client_->poll()) {
            ByteReader r(bytes.data(), bytes.size());
            switch (r.type()) {
                case MsgType::Welcome: {
                    uint8_t id; uint64_t seed; float tod; uint32_t day;
                    if (r.get(id) && r.get(seed) && r.get(tod) && r.get(day)) {
                        my_id_ = id; client_tod_ = tod;
                        // rebuild the world on the authoritative seed so our
                        // procedurally-generated terrain matches the server's.
                        world = World(seed);
                        ops_ = std::make_unique<LocalWorldOps>(world);
                        spawn_player();
                    }
                    break;
                }
                case MsgType::Edit: {
                    NetEdit e; if (!r.get(e)) break;
                    if (e.author == my_id_) break;     // already predicted locally
                    if (e.kind == 1) world.carve_sphere(e.x, e.y, e.z, e.radius, e.material, nullptr);
                    else             world.set_block((int)e.x, (int)e.y, (int)e.z, (BlockId)e.material);
                    break;
                }
                case MsgType::Roster: {
                    NetPlayerState s; std::string name;
                    if (r.get(s) && r.get_str(name)) {
                        if (s.id == my_id_) break;
                        RemotePlayer& rp = remotes_[s.id];
                        glm::vec3 np{s.x, s.y, s.z};
                        if (!rp.seeded) {
                            // first time we see this player: snap, don't interpolate
                            rp.render_pos = np; rp.render_yaw = s.yaw;
                            rp.prev_pos = np;   rp.prev_yaw = s.yaw;
                            rp.seeded = true;
                        } else {
                            // start a fresh interp leg from wherever we're rendering now
                            rp.prev_pos = rp.render_pos; rp.prev_yaw = rp.render_yaw;
                        }
                        rp.target_pos = np; rp.target_yaw = s.yaw;
                        rp.lerp_t = 0.0f;
                        rp.id = s.id; rp.pos = np; rp.yaw = s.yaw; rp.pitch = s.pitch;
                        rp.moving = (s.moving != 0);
                        rp.name = name;
                    }
                    break;
                }
                case MsgType::RemovePlayer: {
                    uint8_t id; if (r.get(id)) remotes_.erase(id);
                    break;
                }
                case MsgType::TimeSync: {
                    float tod; uint32_t day; if (r.get(tod) && r.get(day)) client_tod_ = tod;
                    break;
                }
                case MsgType::MobSnapshot: {
                    std::vector<NetMob> snap;
                    if (dec_mob_snapshot(r, snap)) {
                        // Mark everything stale, then refresh from this snapshot so
                        // mobs missing from it can be dropped (they died/despawned).
                        for (auto& kv : mob_interp_) kv.second.live = false;
                        for (auto& m : snap) {
                            MobInterp& mi = mob_interp_[m.id];
                            Entity e; e.id = m.id; e.kind = (MobKind)m.kind;
                            e.pos = {m.x, m.y, m.z}; e.yaw = m.yaw; e.health = m.health;
                            e.alive = true;
                            e.render_moving = (m.moving != 0);
                            // replay the authoritative hit flash so clients see
                            // mobs flash red when struck (server sets the hurt bit).
                            if (m.hurt) e.hurt_cooldown = 0.4f;
                            glm::vec3 np = e.pos;
                            if (!mi.seeded) {
                                // newly appeared: render at reported pos, no interp
                                mi.prev_pos = np; mi.prev_yaw = m.yaw;
                                mi.seeded = true;
                            } else {
                                // start a new interp leg from current rendered spot
                                mi.prev_pos = mi.ent.pos; mi.prev_yaw = mi.ent.yaw;
                            }
                            mi.target_pos = np; mi.target_yaw = m.yaw;
                            mi.lerp_t = 0.0f;
                            mi.live = true;
                            // keep kind/health/id current; pos is overwritten by interp
                            mi.ent = e;
                        }
                        // drop mobs that vanished from this snapshot
                        for (auto it = mob_interp_.begin(); it != mob_interp_.end();) {
                            if (!it->second.live) it = mob_interp_.erase(it);
                            else ++it;
                        }
                    }
                    break;
                }
                case MsgType::PlayerStats: {
                    float hp, maxhp, hunger, air; uint8_t dead;
                    if (r.get(hp)&&r.get(maxhp)&&r.get(hunger)&&r.get(air)&&r.get(dead)) {
                        player.health = hp; player.max_health = maxhp;
                        player.hunger = hunger; player.air = air; player.dead = (dead != 0);
                    }
                    break;
                }
                default: break;
            }
        }
    }

    // Shortest-angle lerp between two degrees values (handles 350->10 wrap).
    static float lerp_angle_deg(float a, float b, float t) {
        float d = std::fmod(b - a + 540.0f, 360.0f) - 180.0f;   // -180..180
        return a + d * t;
    }

    // Advance client-side render interpolation each frame (Task 1). Pure render
    // smoothing: nothing here touches the wire. Remote players and mobs glide
    // from their previous rendered spot toward the latest snapshot over one
    // snapshot interval (~0.1s); clamped at 1 so they settle if a snapshot is late.
    void advance_interp(float dt) {
        float step = dt / SNAPSHOT_INTERVAL;
        // remote players
        for (auto& kv : remotes_) {
            RemotePlayer& rp = kv.second;
            rp.lerp_t = std::min(1.0f, rp.lerp_t + step);
            rp.render_pos = glm::mix(rp.prev_pos, rp.target_pos, rp.lerp_t);
            rp.render_yaw = lerp_angle_deg(rp.prev_yaw, rp.target_yaw, rp.lerp_t);
        }
        // mobs: produce the interpolated client_mobs_ list for rendering
        client_mobs_.clear();
        client_mobs_.reserve(mob_interp_.size());
        for (auto& kv : mob_interp_) {
            MobInterp& mi = kv.second;
            mi.lerp_t = std::min(1.0f, mi.lerp_t + step);
            Entity e = mi.ent;   // carries id/kind/health/alive
            e.pos = glm::mix(mi.prev_pos, mi.target_pos, mi.lerp_t);
            e.yaw = lerp_angle_deg(mi.prev_yaw, mi.target_yaw, mi.lerp_t);
            mi.ent.pos = e.pos;  // remember current rendered spot for the next leg
            mi.ent.yaw = e.yaw;
            // fade the hit flash locally so it blinks once instead of staying red
            if (mi.ent.hurt_cooldown > 0.0f) mi.ent.hurt_cooldown = std::max(0.0f, mi.ent.hurt_cooldown - dt);
            client_mobs_.push_back(e);
        }
    }

    // Push the local player out of any mob it overlaps (XZ cylinder), so mobs
    // feel solid client-side too. The server also separates authoritatively; this
    // is the immediate local feedback (and the only push in solo). Vertical is
    // left to gravity/step so you can still stand on top if you land on one.
    void push_out_of_mobs() {
        const float PR = Player::HALF_W;
        for (const Entity& e : mob_list()) {
            if (!e.alive) continue;
            // vertical overlap check (cylinders, not infinite): skip if clearly above/below
            float mob_h = e.def().height;
            if (player.pos.y > e.pos.y + mob_h || player.pos.y + Player::HEIGHT < e.pos.y) continue;
            float rr = PR + e.def().width;
            float dx = player.pos.x - e.pos.x, dz = player.pos.z - e.pos.z;
            float d2 = dx*dx + dz*dz;
            if (d2 >= rr*rr || d2 < 1e-8f) continue;
            float d = std::sqrt(d2);
            float push = (rr - d);
            player.pos.x += (dx/d) * push;
            player.pos.z += (dz/d) * push;
        }
    }

    // interpolated pos + yaw + authoritative walk state so they face their
    // travel direction and only animate legs when actually moving.
    void rebuild_remote_render() {
        remote_render_.clear();
        if (role_ == Role::Client) {
            for (auto& kv : remotes_) {
                const RemotePlayer& rp = kv.second;
                remote_render_.push_back({ rp.render_pos, rp.render_yaw, rp.pitch, rp.moving });
            }
        } else if (ServerSim* s = sim()) {
            for (auto& sp : s->players())
                if (sp.active && sp.client_id != 0)
                    remote_render_.push_back({ sp.pos, sp.yaw, sp.pitch, sp.moving });
        }
    }

    // Edit routing: solo applies locally; host applies + broadcasts; client
    // predicts locally AND sends an intent for the server to confirm.
    bool route_carve(float x, float y, float z, float r, int op, BlockFlips* flips) {
        if (role_ == Role::Host) { server_->hostCarve(x, y, z, r, op, flips); return true; }
        if (role_ == Role::Client) {
            world.carve_sphere(x, y, z, r, op, flips);     // prediction
            if (client_) client_->send(enc_edit(MsgType::EditIntent,
                NetEdit{1, my_id_, x, y, z, r, op}));
            return true;
        }
        return ops_->carveSphere(x, y, z, r, op, flips);   // solo
    }
    bool route_place(int x, int y, int z, BlockId b) {
        if (role_ == Role::Host) { server_->hostPlace(x, y, z, b); return true; }
        if (role_ == Role::Client) {
            bool ok = world.set_block(x, y, z, b);
            if (client_) client_->send(enc_edit(MsgType::EditIntent,
                NetEdit{2, my_id_, (float)x, (float)y, (float)z, 0.0f, (int32_t)b}));
            return ok;
        }
        return ops_->setBlock(x, y, z, b);                 // solo
    }

    void request_respawn() {
        player.dead = false;
        if (role_ == Role::Client) { if (client_) client_->send(enc_respawn()); }
        else if (ServerSim* s = sim()) { s->respawn(0); }
        spawn_player();
        player.health = player.max_health; player.hunger = 20.0f; player.air = 10.0f;
    }

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
            ToolKind pref = block_pref_tool(center);
            bool right_tool = (pref == ToolKind::None) || (tool.tool == pref);
            route_place(last_hit_.bx, last_hit_.by, last_hit_.bz, BLOCK_AIR);
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
                route_carve(hp.x, hp.y, hp.z, dig_radius_, -1, &flips);
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
                // Trees rooted in the just-removed terrain are now unsupported.
                // The terrain carve path never ran tree physics, so digging under
                // a tree left its logs/leaves hovering — the floating wood/leaf
                // scrap mistaken for a "weird polygon" in dug holes. Scan every
                // column in the carve footprint (+1 ring), flood up from the dig
                // level and collapse any now-unsupported tree. Re-collapsing an
                // already-dropped tree is a no-op, so duplicate columns are fine.
                {
                    std::vector<std::array<int,4>> tdrops;
                    int R = (int)std::ceil(dig_radius_) + 1;
                    int cxp = (int)std::floor(hp.x), cyp = (int)std::floor(hp.y), czp = (int)std::floor(hp.z);
                    for (int dx = -R; dx <= R; dx++)
                    for (int dz = -R; dz <= R; dz++)
                        world.check_tree_collapse(cxp + dx, cyp, czp + dz, &tdrops);
                    for (auto& d : tdrops)
                        inv.add(block_item((BlockId)d[3]), 1, item_max_stack(block_item((BlockId)d[3])));
                }
                // NOTE: the old auto-smooth_terrain() call was REMOVED. It box-
                // blurred the SDF field after every deep dig; repeated digging at
                // one spot averaged the "air above / solid below" boundary into a
                // near-flat plateau, which Marching Cubes then meshed as a big
                // flat GREY SHEET hanging in the hole. The smin/smax carve already
                // produces a smooth rounded bowl, so the extra blur only hurt.
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
        if (route_place(px, py, pz, item_block(h.id)))
            inv.consume_held();
    }

    // --- melee: swing the held item at the mob under the crosshair ----------
    // Damage is resolved authoritatively: solo/host hit the sim's EntityManager
    // directly; a client sends an AttackIntent and lets the server decide. The
    // hit only "counts" against a mob actually pierced by the look ray.
    void do_attack() {
        glm::vec3 eye = player.eye(), dir = player.forward();
        float dmg = item_def(inv.held().id).attack;
        if (role_ == Role::Client) {
            if (client_) client_->send(enc_attack(0, eye.x, eye.y, eye.z, dir.x, dir.y, dir.z));
            return;
        }
        ServerSim* s = sim();
        if (!s) return;
        ItemId drop = ITEM_NONE; uint8_t dn = 0;
        Entity* e = s->attack(eye, dir, 4.0f, dmg, &drop, &dn);
        if (e && !e->alive && drop != ITEM_NONE && dn > 0)
            inv.add(drop, dn, item_max_stack(drop));   // credit the kill's loot
    }

    // --- eat: consume the held food, restoring hunger ------------------------
    void do_eat() {
        ItemStack& h = inv.held();
        if (h.empty()) return;
        int food = item_def(h.id).food;
        if (food <= 0) return;
        if (role_ == Role::Client) {
            if (client_) client_->send(enc_eat());
            inv.consume_held();                 // optimistic
            return;
        }
        if (ServerSim* s = sim()) {
            if (ServerPlayer* sp = s->player(0)) sp->avatar.eat(food);
        }
        player.eat(food);                       // immediate feedback
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
