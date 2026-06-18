#pragma once
// =============================================================================
// SDFCraft - WorldOps: the replication seam for all authoritative edits
// -----------------------------------------------------------------------------
// "Network replication from day one": every mutation of the shared world
// (carving terrain, placing/removing blocks) goes through this interface
// INSTEAD of touching World directly from gameplay code. That single choke
// point is what makes single-player and multiplayer the same code path:
//
//   * LocalWorldOps  - applies edits immediately to the local World. Used in
//                      single-player AND, on a client, as the *prediction* that
//                      runs before the server confirms.
//   * NetWorldOps    - (multiplayer) routes the edit through VoxelNetEngine:
//                      client predicts locally + sends intent; server validates
//                      (anti-cheat), applies authoritatively, and broadcasts.
//
// Gameplay (Mode::do_dig / do_place) never branches on "are we networked?" —
// it just calls ops().carveSphere(...) / ops().setBlock(...). Swapping the
// backend switches between offline and server-authoritative replication.
//
// Design mirrors the established server-authoritative + client-prediction model
// already sketched in src/net/voxel/voxel_net_engine.h.
// =============================================================================
#include "world.h"
#include <vector>
#include <array>
#include <cstdint>

namespace sdfcraft {

// Result of an edit: which blocks flipped (for drops / FX). Always reported by
// the LOCAL application so prediction feels instant; the network backend may
// later reconcile (rare rollback) without changing this immediate feedback.
using BlockFlips = std::vector<std::array<int,4>>; // {x,y,z, oldBlockId}

// Abstract replication seam. Both backends implement it; gameplay holds a
// reference and never knows which one it has.
class WorldOps {
public:
    virtual ~WorldOps() = default;

    // Smooth spherical carve (MassRTS SDF style). `flips` receives the blocks
    // that turned to air so the caller can award drops. Returns true if anything
    // changed locally (prediction succeeded).
    virtual bool carveSphere(float wx, float wy, float wz, float radius,
                             int material, BlockFlips* flips) = 0;

    // Place a single block. Returns true if applied locally.
    virtual bool setBlock(int x, int y, int z, BlockId block) = 0;

    // Per-frame pump: local backend is a no-op; network backend polls sockets,
    // applies confirmed/broadcast edits, and reconciles mispredictions.
    virtual void tick(float /*dt*/) {}

    // True when edits are authoritative-immediately (offline/host). On a pure
    // client this is still true for *prediction*; reconciliation handles the
    // rare server disagreement.
    virtual bool isAuthoritative() const { return true; }
};

// PLACEHOLDER_BACKENDS

// -----------------------------------------------------------------------------
// LocalWorldOps - single-player / host / client-side prediction.
// Applies straight to the local World. This is the default backend so the game
// is fully playable offline, while gameplay code is already written against the
// replication interface.
// -----------------------------------------------------------------------------
class LocalWorldOps : public WorldOps {
public:
    explicit LocalWorldOps(World& w) : world_(w) {}

    bool carveSphere(float wx, float wy, float wz, float radius,
                     int material, BlockFlips* flips) override {
        size_t before = flips ? flips->size() : 0;
        world_.carve_sphere(wx, wy, wz, radius, material, flips);
        return !flips || flips->size() != before;
    }

    bool setBlock(int x, int y, int z, BlockId block) override {
        return world_.set_block(x, y, z, block);
    }

    bool isAuthoritative() const override { return true; }

private:
    World& world_;
};

// -----------------------------------------------------------------------------
// NetWorldOps - multiplayer (server-authoritative + client prediction).
// SCAFFOLD: wired to compile and to define the exact seam VoxelNetEngine plugs
// into. The flow (already specified in voxel_net_engine.h) is:
//   client edit -> predict locally (LocalWorldOps) + send intent packet
//   server      -> validate (anti-cheat) -> apply -> broadcast delta
//   client tick -> apply confirmed broadcasts; rollback on rejection
// The packet plumbing is filled in during network phase B4/N; gameplay code
// does NOT change when that happens — only this backend does.
// -----------------------------------------------------------------------------
class NetWorldOps : public WorldOps {
public:
    // `local` performs immediate client-side prediction; `is_server` flips
    // whether this peer applies authoritatively (host) or predicts (client).
    NetWorldOps(World& w, bool is_server)
        : world_(w), local_(w), is_server_(is_server) {}

    bool carveSphere(float wx, float wy, float wz, float radius,
                     int material, BlockFlips* flips) override {
        // Predict immediately for responsiveness (client) / apply (server).
        bool changed = local_.carveSphere(wx, wy, wz, radius, material, flips);
        // TODO(net B4): if client -> queue VoxelPlayerActionPacket(dig) w/ seq
        //               if server -> serverAddEdit + serverBroadcastEdit
        return changed;
    }

    bool setBlock(int x, int y, int z, BlockId block) override {
        bool changed = local_.setBlock(x, y, z, block);
        // TODO(net B4): send/broadcast place edit as above.
        return changed;
    }

    void tick(float /*dt*/) override {
        // TODO(net B4): poll socket; apply confirmed/broadcast edits to world_;
        // reconcile any rejected predictions (rollback + replay).
    }

    bool isAuthoritative() const override { return is_server_; }

private:
    World&        world_;
    LocalWorldOps local_;     // prediction path reused verbatim
    bool          is_server_;
};

} // namespace sdfcraft

