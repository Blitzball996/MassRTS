#pragma once
// =============================================================================
// SDFCraft - Authoritative simulation (server-side game state)
// -----------------------------------------------------------------------------
// THE single source of truth for everything that must agree across players:
//   * the World edits (carve/place) — applied here authoritatively
//   * mobs (spawn / AI / despawn) via EntityManager
//   * time-of-day + day count (drives the sun, sky colour and night spawns)
//   * per-player survival stats (health / hunger / air) and respawn
//
// Solo, listen-host and dedicated-server ALL run one ServerSim. There is no
// second "client simulation" of mobs or time — a client only renders the
// snapshots this produces. That is what keeps multiplayer consistent: one brain.
//
//   Solo      : ServerSim with a single local player, no sockets.
//   Host      : ServerSim + NetServer; local player is slot 0, remotes 1..N.
//   Dedicated : ServerSim + NetServer, no local player, headless 20Hz loop.
//
// Mob AI in ai.h targets ONE player position. For multiplayer we run population
// management around every player and, per mob, target the nearest player; melee
// damage is then routed to that nearest player's stats.
// =============================================================================
#include "world.h"
#include "player.h"
#include "ai.h"
#include "entity.h"
#include "items.h"
#include "inventory.h"
#include <glm/glm.hpp>
#include <cmath>
#include <vector>
#include <string>
#include <cstdint>
#include <algorithm>

namespace sdfcraft {

// One full day in real seconds (Minecraft is 1200s; we use a brisker 600s so a
// solo tester sees night within a couple of minutes).
static constexpr float DAY_LENGTH_SEC = 600.0f;

// Authoritative per-player state held by the server. The client mirrors its own
// copy from PlayerStats messages; it never decides its own health.
struct ServerPlayer {
    int       client_id = 0;       // 0 = host-local player; >0 = network slot
    bool      active = false;
    std::string name;
    glm::vec3 pos{0, 80, 0};
    float     yaw = 0, pitch = 0;
    bool      moving = false;       // walking this tick (from client input; drives anim)
    // survival stats (authoritative). We reuse Player's tick math by proxying
    // through a lightweight Player instance so the rules live in exactly one place.
    Player    avatar;              // holds health/hunger/air + survival_tick()
    bool      dead = false;
    float     respawn_timer = 0.0f;
    float     armor_points = 0.0f; // worn-armor defense (Mode pushes it from inv each tick)
};

class ServerSim {
public:
    // The sim operates on a World owned by the caller (Mode for solo/host, or a
    // standalone World for the dedicated server). Sharing one World is what lets
    // the host render/collide against the exact terrain it authoritatively edits.
    ServerSim(World& w, uint64_t seed)
        : world(w), mobs(seed ^ 0xA5A5A5A5u) {}

    World&        world;
    EntityManager mobs;
    float         time_of_day = 0.25f;   // 0..1; 0.25 = morning, 0.5 = noon, 0.75 = dusk
    uint32_t      day = 0;

    // ---- player slots -------------------------------------------------------
    // Returns the index into players_ for a client id, creating a slot if new.
    ServerPlayer& addPlayer(int client_id, const std::string& name) {
        for (auto& p : players_) if (p.active && p.client_id == client_id) return p;
        // reuse a free slot or append
        for (auto& p : players_) if (!p.active) { p = ServerPlayer{}; init_slot(p, client_id, name); return p; }
        players_.push_back(ServerPlayer{});
        init_slot(players_.back(), client_id, name);
        return players_.back();
    }
    void removePlayer(int client_id) {
        for (auto& p : players_) if (p.client_id == client_id) p.active = false;
    }
    ServerPlayer* player(int client_id) {
        for (auto& p : players_) if (p.active && p.client_id == client_id) return &p;
        return nullptr;
    }
    std::vector<ServerPlayer>& players() { return players_; }

    // ---- authoritative edits (called by host directly, or via EditIntent) ---
    bool carve(float x, float y, float z, float r, int op, BlockFlips* flips) {
        size_t before = flips ? flips->size() : 0;
        world.carve_sphere(x, y, z, r, op, flips);
        return !flips || flips->size() != before;
    }
    bool place(int x, int y, int z, BlockId b) { return world.set_block(x, y, z, b); }

    // ---- authoritative attack: a player swings at a mob -------------------
    // Returns the mob hit (or nullptr). On kill, the mob's drop is reported via
    // `out_drop` so the caller can credit the attacking player's inventory.
    Entity* attack(glm::vec3 eye, glm::vec3 dir, float reach, float dmg,
                   ItemId* out_drop, uint8_t* out_drop_n) {
        Entity* e = mobs.attack_ray(eye, dir, reach, dmg);
        if (e && !e->alive && out_drop) {
            const MobDef& d = e->def();
            *out_drop = d.drop;
            *out_drop_n = d.drop_max;
        }
        return e;
    }

    // ---- the authoritative step --------------------------------------------
    // Advances time, mobs (spawn + AI + damage to players), and each player's
    // survival stats. dt is real seconds.
    void tick(float dt) {
        ++frame_;
        // time of day
        time_of_day += dt / DAY_LENGTH_SEC;
        while (time_of_day >= 1.0f) { time_of_day -= 1.0f; day++; }
        bool night = is_night();
        mobs.is_night = night;

        // gather active player positions for AI targeting + spawn anchors
        active_positions_.clear();
        for (auto& p : players_) if (p.active && !p.dead) active_positions_.push_back(p.pos);

        // spawn/cull mobs around each player (bounded inside EntityManager)
        for (const glm::vec3& pp : active_positions_)
            mobs.manage_population(world, pp);

        // advance mobs; route melee damage to the nearest player to each mob
        step_mobs(dt);

        // soft separation so mobs (and players) don't stand inside each other.
        separate_entities();

        // survival stats per player
        for (auto& p : players_) {
            if (!p.active) continue;
            if (p.dead) {
                p.respawn_timer -= dt;     // client may also request Respawn early
                continue;
            }
            p.avatar.pos = p.pos;          // survival_tick reads eye() for drowning
            p.avatar.survival_tick(world, dt);
            if (p.avatar.dead) { p.dead = true; p.respawn_timer = 3.0f; }
        }
    }

    // Respawn a dead player at a safe surface spot near origin, stats restored.
    void respawn(int client_id) {
        ServerPlayer* p = player(client_id);
        if (!p) return;
        p->avatar = Player{};              // resets health=20/hunger=20/air=10
        p->dead = false;
        int h = world.surface_height(0, 0);
        p->pos = glm::vec3(0.5f, (float)(h + 2), 0.5f);
    }

    // ---- shared sun/sky helpers (server computes time; clients render it) ---
    bool is_night() const { return time_of_day < 0.23f || time_of_day > 0.76f; }

    // Sun direction for a given time-of-day. Noon = straight up-ish; it arcs E->W
    // and dips below the horizon at night (so lighting genuinely darkens).
    static glm::vec3 sun_from_time(float tod) {
        // map tod 0..1 to an angle: 0.25 sunrise(east), 0.5 noon(top), 0.75 sunset(west)
        float ang = (tod - 0.25f) * 6.2831853f;     // 0 at sunrise
        glm::vec3 s(std::cos(ang), std::sin(ang), 0.30f);
        return glm::normalize(s);
    }
    // Daylight factor 0 (deep night) .. 1 (full day) for ambient/sky tinting.
    static float daylight(float tod) {
        float h = std::sin((tod - 0.25f) * 6.2831853f);   // sun height -1..1
        return glm::clamp(h * 0.5f + 0.5f, 0.05f, 1.0f);
    }

private:
    std::vector<ServerPlayer> players_;
    std::vector<glm::vec3>    active_positions_;
    uint32_t                  frame_ = 0;   // tick counter for significance LOD

    // Significance LOD thresholds (XZ distance from the NEAREST player):
    //   < NEAR  -> tick every frame (full fidelity, you're watching them)
    //   < MID   -> tick every 4th frame
    //   else    -> tick every 8th frame
    // Far mobs bank skipped dt so a coarse tick still advances the right amount.
    static constexpr float LOD_NEAR2 = 24.0f * 24.0f;
    static constexpr float LOD_MID2  = 64.0f * 64.0f;

    void init_slot(ServerPlayer& p, int client_id, const std::string& name) {
        p.client_id = client_id;
        p.active = true;
        p.name = name.empty() ? ("player" + std::to_string(client_id)) : name;
        p.avatar = Player{};
        int h = world.surface_height(0, 0);
        p.pos = glm::vec3(0.5f, (float)(h + 2), 0.5f);
        p.avatar.pos = p.pos;
    }

    // Advance every mob one step. ai.h's EntityManager::update targets a single
    // player; for N players we re-implement the per-mob loop here so each mob
    // chases the NEAREST player and deals damage to that player's stats.
    void step_mobs(float dt) {
        if (active_positions_.empty()) {
            // no players: still age cooldowns + gravity so mobs don't freeze mid-air
            for (auto& e : mobs.entities) {
                if (!e.alive) continue;
                if (e.hurt_cooldown > 0)   e.hurt_cooldown   -= dt;
                if (e.attack_cooldown > 0) e.attack_cooldown -= dt;
                entity_physics(world, e, glm::vec3(0), false, dt);
                if (e.pos.y < -8) e.alive = false;
            }
            cull_dead();
            return;
        }

        for (auto& e : mobs.entities) {
            if (!e.alive) continue;

            // nearest player (cheap, every frame — needed for the LOD decision)
            int best = -1; float best_d2 = 1e30f;
            for (int i = 0; i < (int)active_positions_.size(); i++) {
                glm::vec3 to = active_positions_[i] - e.pos; to.y = 0;
                float d2 = glm::dot(to, to);
                if (d2 < best_d2) { best_d2 = d2; best = i; }
            }

            // --- significance LOD: decide whether this mob ticks this frame ---
            // Near = every frame, mid = every 4th, far = every 8th. Skipped mobs
            // bank their dt so the eventual tick advances the full elapsed time.
            // EXCEPTION: a mob that is airborne or was just hit must tick EVERY
            // frame regardless of distance — otherwise a knocked-back mob hangs in
            // the air for several frames before its coarse tick lets gravity pull
            // it down (the "hit mob floats" bug). Only ground-resting, unhurt mobs
            // are allowed to run at a coarse rate.
            bool must_tick = !e.on_ground || e.hurt_cooldown > 0.0f
                           || std::fabs(e.vel.y) > 0.5f;
            uint32_t interval = must_tick           ? 1u
                              : (best_d2 < LOD_NEAR2) ? 1u
                              : (best_d2 < LOD_MID2)  ? 4u : 8u;
            e.lod_accum += dt;
            if (interval > 1 && ((frame_ + e.lod_phase) % interval) != 0)
                continue;                       // not this mob's turn; stay banked
            float edt = e.lod_accum;            // advance by the banked elapsed time
            e.lod_accum = 0.0f;

            if (e.hurt_cooldown > 0)   e.hurt_cooldown   -= edt;
            if (e.attack_cooldown > 0) e.attack_cooldown -= edt;

            const MobDef& d = e.def();
            if (d.burns_in_sun && !mobs.is_night && sky_open(e.pos))
                e.hurt(2.0f * edt);

            glm::vec3 want(0); bool jump = false;
            float dealt = 0.0f;
            if (d.hostility == Hostility::Hostile || d.hostility == Hostility::Boss) {
                dealt = drive_hostile(e, active_positions_[best], want, jump, edt);
            } else {
                drive_passive(e, want, jump, edt);
            }
            // walking this tick? drives the walk animation (idle mobs hold still).
            e.render_moving = (want.x*want.x + want.z*want.z) > 0.01f;
            entity_physics(world, e, want, jump, edt);
            if (e.pos.y < -8) e.alive = false;

            // apply melee to the player this mob is next to
            if (dealt > 0.0f) damage_nearest_player(active_positions_[best], dealt);
        }
        cull_dead();
    }

    void cull_dead() {
        mobs.entities.erase(std::remove_if(mobs.entities.begin(), mobs.entities.end(),
            [](const Entity& e){ return !e.alive; }), mobs.entities.end());
    }

    // Soft cylindrical separation: nudge overlapping entities apart on the XZ
    // plane so mobs don't pile into one square and the player can't walk through
    // them. Authoritative (server/solo); players are pushed via their slot pos so
    // the push replicates like any other position change. One relaxation pass per
    // tick is enough — repeated ticks converge and it stays cheap.
    void separate_entities() {
        const float PLAYER_R = 0.30f;   // Player::HALF_W
        auto push = [](glm::vec3& a, glm::vec3& b, float ra, float rb,
                       bool move_a, bool move_b) {
            float dx = b.x - a.x, dz = b.z - a.z;
            float d2 = dx*dx + dz*dz;
            float min_d = ra + rb;
            if (d2 >= min_d*min_d) return;
            float d = std::sqrt(d2);
            glm::vec2 n = (d > 1e-4f) ? glm::vec2(dx/d, dz/d) : glm::vec2(1, 0);
            float overlap = (min_d - d);
            float ka = (move_a && move_b) ? 0.5f : (move_a ? 1.0f : 0.0f);
            float kb = (move_a && move_b) ? 0.5f : (move_b ? 1.0f : 0.0f);
            a.x -= n.x * overlap * ka; a.z -= n.y * overlap * ka;
            b.x += n.x * overlap * kb; b.z += n.y * overlap * kb;
        };
        // mob <-> mob
        auto& es = mobs.entities;
        for (size_t i = 0; i < es.size(); i++) {
            if (!es[i].alive) continue;
            for (size_t j = i+1; j < es.size(); j++) {
                if (!es[j].alive) continue;
                push(es[i].pos, es[j].pos, es[i].def().width, es[j].def().width, true, true);
            }
        }
        // mob <-> player (mob yields fully; player stays put here — the player's
        // own collider also pushes out client-side so it feels solid both ways)
        for (auto& p : players_) {
            if (!p.active || p.dead) continue;
            for (auto& e : es) {
                if (!e.alive) continue;
                push(p.pos, e.pos, PLAYER_R, e.def().width, false, true);
            }
        }
    }

    void damage_nearest_player(glm::vec3 mob_target_pos, float dmg) {
        ServerPlayer* best = nullptr; float best_d2 = 1e30f;
        for (auto& p : players_) {
            if (!p.active || p.dead) continue;
            glm::vec3 to = p.pos - mob_target_pos;
            float d2 = glm::dot(to, to);
            if (d2 < best_d2) { best_d2 = d2; best = &p; }
        }
        if (best) {
            // worn armor reduces incoming damage (~4% per point, capped at 80%).
            float reduce = glm::clamp(best->armor_points * 0.04f, 0.0f, 0.8f);
            best->avatar.hurt(dmg * (1.0f - reduce));
            if (best->avatar.dead) { best->dead = true; best->respawn_timer = 3.0f; }
        }
    }

    bool sky_open(glm::vec3 p) {
        int x=(int)floorf(p.x), z=(int)floorf(p.z), y=(int)floorf(p.y+1.5f);
        for (int yy=y+1; yy<CHUNK_SY; yy++)
            if (block_is_opaque(world.get_block_ro(x,yy,z))) return false;  // worker-safe
        return true;
    }

    // --- inlined mob behaviours (multi-player aware target passed in) --------
    void drive_passive(Entity& e, glm::vec3& want, bool& jump, float dt) {
        e.ai_timer -= dt;
        if (e.ai_timer <= 0.0f) {
            if (rng_.f01() < 0.35f) { e.wander_dir = glm::vec3(0); e.ai_timer = rng_.range(1,3); }
            else {
                float a = rng_.range(0, 6.2831853f);
                e.wander_dir = glm::vec3(cosf(a), 0, sinf(a));
                // face the wander heading using the player yaw convention
                e.yaw = glm::degrees(atan2f(e.wander_dir.x, -e.wander_dir.z));
                e.ai_timer = rng_.range(2, 5);
            }
        }
        move_toward(e, e.wander_dir, e.def().move_speed * 0.5f, want, jump);
    }

    float drive_hostile(Entity& e, glm::vec3 target, glm::vec3& want, bool& jump, float dt) {
        glm::vec3 to = target - e.pos; to.y = 0;
        float dist = glm::length(to);
        const MobDef& md = e.def();
        float dealt = 0.0f;
        if (dist < 32.0f && dist > 0.01f) {
            glm::vec3 dir = to / dist;
            // Same yaw convention as the player: forward_flat(yaw)=(sin,0,-cos).
            // So a heading `dir` maps to yaw = atan2(dir.x, -dir.z); the model
            // renderer's single -yaw rule then faces every entity correctly.
            e.yaw = glm::degrees(atan2f(dir.x, -dir.z));
            if (dist > 1.4f) move_toward(e, dir, md.move_speed, want, jump);
            else if (md.attack_dmg > 0.0f && e.attack_cooldown <= 0.0f) {
                dealt = md.attack_dmg; e.attack_cooldown = 1.0f;
            }
        } else {
            drive_passive(e, want, jump, dt);
        }
        return dealt;
    }

    void move_toward(Entity& e, glm::vec3 dir, float speed, glm::vec3& want, bool& jump) {
        if (glm::length(dir) < 1e-4f) { want = glm::vec3(0); return; }
        dir = glm::normalize(dir);
        want = dir * speed;
        // Mobs walk EXACTLY like the player: slopes/ledges are climbed by the
        // auto step-up in entity_move_axis, so we never set jump. (The player
        // doesn't hop to walk uphill, and neither should mobs.)
        (void)e; jump = false;
    }

    Rng rng_{0xC0FFEEu};
};

} // namespace sdfcraft
