#pragma once
// =============================================================================
// SDFCraft - Game server: binds ServerSim to a NetServer (Phase N)
// -----------------------------------------------------------------------------
// Wraps the authoritative ServerSim with the multi-client transport. Used by:
//   * the listen-HOST  (host runs this in-process; its own player is slot 0)
//   * the DEDICATED server (runDedicatedServer below: headless 20Hz loop)
//
// Responsibilities each tick:
//   1. accept new clients -> add a ServerPlayer, send Welcome + current roster
//   2. ingest client messages (EditIntent / PlayerMove / Attack / Eat / Respawn)
//      — validated minimally, applied to ServerSim, and broadcast as needed
//   3. advance ServerSim (mobs / time / survival)
//   4. broadcast world snapshots: edits already broadcast on apply; periodically
//      push Roster (player positions), MobSnapshot, TimeSync, and per-client
//      PlayerStats.
//
// Edits are echoed to ALL clients (including the originator) so every peer
// converges on the same authoritative result; clients dedupe their own
// prediction by author+kind position (best-effort — terrain carve is idempotent).
// =============================================================================
#include "server_sim.h"
#include "net_session.h"
#include "net_protocol.h"
#include "inventory.h"
#include "items.h"
#include <glm/glm.hpp>
#include <unordered_map>
#include <vector>

namespace sdfcraft {

class GameServer {
public:
    GameServer(World& world, uint64_t seed, uint16_t port, bool with_local_host_player)
        : sim_(world, seed), seed_(seed) {
        ok_ = net_.start(port);
        if (with_local_host_player) {
            // host's own player occupies slot 0 (no socket).
            sim_.addPlayer(0, "host");
            host_inv_.add(ITEM_WOOD_SWORD, 1, 1);
            host_inv_.add(ITEM_BREAD, 8, STACK_MAX);
        }
    }

    bool ok() const { return ok_; }
    ServerSim& sim() { return sim_; }
    uint64_t seed() const { return seed_; }

    // ---- host-local helpers (slot 0): the host plays through these so its
    // edits/stats follow the exact same authoritative path as a remote client.
    ServerPlayer* hostPlayer() { return sim_.player(0); }

    // Apply + broadcast a host-originated edit (carve).
    void hostCarve(float x, float y, float z, float r, int op, BlockFlips* flips) {
        sim_.carve(x, y, z, r, op, flips);
        NetEdit e{1, 0, x, y, z, r, op};
        log_edit(e);
        net_.broadcast(enc_edit(MsgType::Edit, e));
    }
    void hostPlace(int x, int y, int z, BlockId b) {
        sim_.place(x, y, z, b);
        NetEdit e{2, 0, (float)x, (float)y, (float)z, 0.0f, (int32_t)b};
        log_edit(e);
        net_.broadcast(enc_edit(MsgType::Edit, e));
    }

    // ---- the per-frame server step -----------------------------------------
    void update(float dt) {
        // 1. accept new clients
        for (int id : net_.acceptNew()) {
            ServerPlayer& p = sim_.addPlayer(id, "");
            net_.sendTo(id, enc_welcome((uint8_t)id, seed_, sim_.time_of_day, sim_.day));
            // send the new client the current roster of everyone else
            for (auto& other : sim_.players()) {
                if (!other.active || other.client_id == id) continue;
                NetPlayerState s{ (uint8_t)other.client_id, other.pos.x, other.pos.y,
                                  other.pos.z, other.yaw, other.pitch, other.moving ? (uint8_t)1 : (uint8_t)0 };
                net_.sendTo(id, enc_roster(s, other.name));
            }
            // DELTA SYNC backfill: replay the authoritative edit log so a late
            // joiner sees every carve/place made before it connected. The client
            // rebuilds its World from the seed on Welcome (above, ordered before
            // these on the same TCP stream), then applies these Edits on top —
            // natural terrain + replayed edits == the exact authoritative world.
            // The client's Edit handler skips edits where author==my_id; historical
            // authors never equal a fresh joiner's id, so all backfill applies.
            for (const NetEdit& e : edit_log_)
                net_.sendTo(id, enc_edit(MsgType::Edit, e));
            (void)p;
        }

        // 2. ingest client messages
        for (InMsg& in : net_.poll()) ingest(in);

        // 3. handle disconnects
        for (int id : net_.takeDisconnects()) {
            sim_.removePlayer(id);
            net_.broadcast(enc_remove_player((uint8_t)id));
        }

        // 4. advance authoritative sim
        sim_.tick(dt);

        // 5. periodic broadcasts (rate-limited)
        bcast_timer_ -= dt;
        if (bcast_timer_ <= 0.0f) {
            bcast_timer_ = 0.10f;   // 10 Hz snapshots
            broadcast_world_state();
        }
    }

private:
    ServerSim   sim_;
    NetServer   net_;
    uint64_t    seed_;
    bool        ok_ = false;
    float       bcast_timer_ = 0.0f;
    Inventory   host_inv_;     // host's own inventory (slot 0)
    // per-client inventory for awarding mob drops over the wire (kept minimal)
    std::unordered_map<int, Inventory> client_inv_;

    // --- DELTA SYNC edit log (Task 2) ----------------------------------------
    // Ordered record of every authoritative carve/place. Replayed to a client
    // when it joins so late-joiners (and clients streaming in chunks edited while
    // they were away) reconstruct the exact world: natural terrain from the seed
    // + this replay. We do FULL-LOG replay (no spatial/AOI filter yet) — simple
    // and correct; per-chunk AOI delta streaming is the documented follow-up.
    //
    // CAP: bounded to the last EDIT_LOG_CAP edits to keep memory/backfill
    // bandwidth sane. KNOWN LIMITATION: once the cap is exceeded the oldest edits
    // are dropped from the log, so a very-late joiner would miss edits older than
    // the cap (the natural terrain shows through there). Acceptable for now;
    // persistent per-chunk delta storage is the proper long-term fix.
    static constexpr size_t EDIT_LOG_CAP = 4096;
    std::vector<NetEdit> edit_log_;

    void log_edit(const NetEdit& e) {
        edit_log_.push_back(e);
        if (edit_log_.size() > EDIT_LOG_CAP)
            edit_log_.erase(edit_log_.begin(),
                            edit_log_.begin() + (edit_log_.size() - EDIT_LOG_CAP));
    }

    void ingest(InMsg& in) {
        ByteReader r(in.bytes.data(), in.bytes.size());
        switch (r.type()) {
            case MsgType::Hello: {
                uint16_t ver; std::string name;
                r.get(ver); r.get_str(name);
                ServerPlayer* p = sim_.player(in.client_id);
                if (p) { p->name = name.empty() ? p->name : name;
                         net_.broadcastExcept(in.client_id,
                            enc_roster({(uint8_t)p->client_id,p->pos.x,p->pos.y,p->pos.z,p->yaw,p->pitch, p->moving?(uint8_t)1:(uint8_t)0}, p->name)); }
                break;
            }
            case MsgType::EditIntent: {
                NetEdit e; if (!r.get(e)) break;
                // (anti-cheat hook: validate reach/permission here later)
                if (e.kind == 1) sim_.carve(e.x, e.y, e.z, e.radius, e.material, nullptr);
                else             sim_.place((int)e.x, (int)e.y, (int)e.z, (BlockId)e.material);
                e.author = (uint8_t)in.client_id;
                log_edit(e);                                  // record for late-join backfill
                net_.broadcast(enc_edit(MsgType::Edit, e));   // echo to all incl. sender
                break;
            }
            case MsgType::PlayerMove: {
                NetPlayerState s; if (!r.get(s)) break;
                ServerPlayer* p = sim_.player(in.client_id);
                if (p) { p->pos = {s.x, s.y, s.z}; p->yaw = s.yaw; p->pitch = s.pitch;
                         p->moving = (s.moving != 0); }
                break;
            }
            case MsgType::AttackIntent: {
                uint32_t mob_id; float ex,ey,ez,dx,dy,dz;
                if (!(r.get(mob_id)&&r.get(ex)&&r.get(ey)&&r.get(ez)&&r.get(dx)&&r.get(dy)&&r.get(dz))) break;
                ItemId drop=ITEM_NONE; uint8_t dn=0;
                sim_.attack({ex,ey,ez}, {dx,dy,dz}, 4.0f, 4.0f, &drop, &dn);
                // (mob drop crediting over the wire is a follow-up; the kill itself
                //  is authoritative and will vanish from the next MobSnapshot.)
                (void)drop; (void)dn;
                break;
            }
            case MsgType::EatIntent: {
                // server has no per-client inventory contents yet; the client
                // consumes its own item and we just heal hunger authoritatively.
                ServerPlayer* p = sim_.player(in.client_id);
                if (p && !p->dead) p->avatar.eat(4);
                break;
            }
            case MsgType::Respawn: {
                sim_.respawn(in.client_id);
                break;
            }
            default: break;
        }
    }

    void broadcast_world_state() {
        // time
        net_.broadcast(enc_time(sim_.time_of_day, sim_.day));

        // roster (everyone's position) — small, fine to send each snapshot
        for (auto& p : sim_.players()) {
            if (!p.active) continue;
            NetPlayerState s{ (uint8_t)p.client_id, p.pos.x, p.pos.y, p.pos.z, p.yaw, p.pitch, p.moving?(uint8_t)1:(uint8_t)0 };
            net_.broadcastExcept(p.client_id, enc_roster(s, p.name));
        }

        // mobs near each client (here: full list; AOI culling is a Phase N follow-up)
        std::vector<NetMob> snap;
        snap.reserve(sim_.mobs.entities.size());
        for (auto& e : sim_.mobs.entities) {
            if (!e.alive) continue;
            snap.push_back({ e.id, (uint8_t)e.kind, e.pos.x, e.pos.y, e.pos.z, e.yaw, e.health,
                             e.render_moving?(uint8_t)1:(uint8_t)0,
                             e.hurt_cooldown > 0.0f ? (uint8_t)1 : (uint8_t)0 });
        }
        net_.broadcast(enc_mob_snapshot(snap));

        // per-client authoritative stats
        for (auto& p : sim_.players()) {
            if (!p.active || p.client_id == 0) continue;   // slot 0 = host, reads sim directly
            const Player& a = p.avatar;
            net_.sendTo(p.client_id,
                enc_stats(a.health, a.max_health, a.hunger, a.air, p.dead ? 1 : 0));
        }
    }
};

// -----------------------------------------------------------------------------
// Dedicated server: headless authoritative loop. No GL, no window, no local
// player. Runs ServerSim at a fixed 20 Hz tick until killed. Same code path as
// the host, minus rendering and the slot-0 player.
// -----------------------------------------------------------------------------
int runDedicatedServer(uint64_t seed, uint16_t port);

} // namespace sdfcraft
