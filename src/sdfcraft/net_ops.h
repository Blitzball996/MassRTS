#pragma once
// =============================================================================
// SDFCraft - Edit replication transport (networking phase)
// -----------------------------------------------------------------------------
// Turns the WorldOps seam into a real client/server replication path WITHOUT
// coupling gameplay to sockets. The design mirrors the server-authoritative +
// client-prediction model in src/net/voxel/voxel_net_engine.h, but expressed in
// terms of the SDFCraft World so it is fully unit-testable offline.
//
// PIECES
//   * EditMsg        - a single replicated mutation (carve or place), POD, with
//                      a monotonically increasing sequence id + author id.
//   * encode/decode  - little-endian (de)serialization (same byte order as the
//                      shared net/core/byte_buffer transport).
//   * EditTransport  - abstract duplex channel: send bytes, poll received bytes.
//   * LoopbackTransport - in-process two-endpoint channel for tests/host+local.
//   * EditReplicator - applies inbound edits to a World and tags outbound ones
//                      with a sequence; dedups by (author,seq) so a broadcast
//                      that echoes back is not applied twice.
//
// This is transport-agnostic: a UDP/TCP backend only has to implement
// EditTransport; gameplay and World logic never change.
// =============================================================================
#include "world.h"
#include <cstdint>
#include <cstring>
#include <deque>
#include <vector>
#include <unordered_set>

namespace sdfcraft {

// ---- wire format ------------------------------------------------------------
enum class EditKind : uint8_t { Carve = 1, Place = 2 };

#pragma pack(push, 1)
struct EditMsg {
    EditKind kind;
    uint8_t  author;     // peer id that originated the edit
    uint16_t _pad = 0;
    uint32_t seq;        // per-author monotonic sequence
    // carve: (x,y,z,radius,material) ; place: (x,y,z) as ints in fx/fy/fz, block in material
    float    fx, fy, fz;
    float    radius;
    int32_t  material;   // carve op/material, or BlockId for place
};
#pragma pack(pop)

inline void encode_edit(const EditMsg& m, std::vector<uint8_t>& out) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&m);
    out.insert(out.end(), p, p + sizeof(EditMsg));
}
// Decode one EditMsg from buf at offset; returns bytes consumed (0 on short buf).
inline size_t decode_edit(const uint8_t* buf, size_t len, EditMsg& out) {
    if (len < sizeof(EditMsg)) return 0;
    std::memcpy(&out, buf, sizeof(EditMsg));
    return sizeof(EditMsg);
}

// ---- transport --------------------------------------------------------------
// A duplex byte channel. send() queues bytes toward the peer; poll() returns the
// next datagram received from the peer (empty vector when nothing pending).
class EditTransport {
public:
    virtual ~EditTransport() = default;
    virtual void send(const std::vector<uint8_t>& bytes) = 0;
    virtual bool poll(std::vector<uint8_t>& out) = 0;   // false when nothing queued
};

// In-process loopback: two endpoints cross-wired. Deterministic, zero sockets,
// so multiplayer logic is exercised in unit tests and in a host's own client.
class LoopbackTransport : public EditTransport {
public:
    // Connect this endpoint's outbox to `peer`'s inbox and vice-versa.
    static void pair(LoopbackTransport& a, LoopbackTransport& b) {
        a.peer_ = &b; b.peer_ = &a;
    }
    void send(const std::vector<uint8_t>& bytes) override {
        if (peer_) peer_->inbox_.push_back(bytes);
    }
    bool poll(std::vector<uint8_t>& out) override {
        if (inbox_.empty()) return false;
        out = std::move(inbox_.front());
        inbox_.pop_front();
        return true;
    }
private:
    LoopbackTransport* peer_ = nullptr;
    std::deque<std::vector<uint8_t>> inbox_;
};

// ---- replicator -------------------------------------------------------------
// Owns a World and an EditTransport. Local edits are applied immediately
// (prediction / authoritative apply) AND queued outbound tagged with a
// per-author sequence. Inbound edits are applied once, deduped by (author,seq).
class EditReplicator {
public:
    EditReplicator(World& w, EditTransport& t, uint8_t author)
        : world_(w), transport_(t), author_(author) {}

    // Originate a carve from this peer: apply locally + broadcast.
    bool carve(float x, float y, float z, float radius, int op, BlockFlips* flips) {
        bool changed = apply_carve(x, y, z, radius, op, flips);
        EditMsg m{}; m.kind = EditKind::Carve; m.author = author_; m.seq = ++out_seq_;
        m.fx = x; m.fy = y; m.fz = z; m.radius = radius; m.material = op;
        record(m);
        std::vector<uint8_t> b; encode_edit(m, b); transport_.send(b);
        return changed;
    }

    // Originate a block placement/removal from this peer: apply locally + broadcast.
    bool place(int x, int y, int z, BlockId block) {
        bool changed = world_.set_block(x, y, z, block);
        EditMsg m{}; m.kind = EditKind::Place; m.author = author_; m.seq = ++out_seq_;
        m.fx = (float)x; m.fy = (float)y; m.fz = (float)z; m.material = (int32_t)block;
        record(m);
        std::vector<uint8_t> b; encode_edit(m, b); transport_.send(b);
        return changed;
    }

    // Drain the transport, applying any not-yet-seen remote edits. Returns the
    // number of edits applied this call.
    int pump() {
        int applied = 0;
        std::vector<uint8_t> dg;
        while (transport_.poll(dg)) {
            size_t off = 0;
            EditMsg m{};
            while (size_t n = decode_edit(dg.data() + off, dg.size() - off, m)) {
                off += n;
                uint64_t k = key(m.author, m.seq);
                if (!seen_.insert(k).second) continue;   // already applied
                if (m.kind == EditKind::Carve)
                    apply_carve(m.fx, m.fy, m.fz, m.radius, m.material, nullptr);
                else
                    world_.set_block((int)m.fx, (int)m.fy, (int)m.fz, (BlockId)m.material);
                history_.push_back(m);                   // keep for late-join backfill
                applied++;
                if (off >= dg.size()) break;
            }
        }
        return applied;
    }

    uint32_t outboundSeq() const { return out_seq_; }

    // ---- late-join streaming sync -------------------------------------------
    // A peer that joins after edits have happened needs the full edit history to
    // reconstruct the carved/built world (chunks are generated deterministically;
    // only the player edits diverge). The host serializes its whole history into
    // one snapshot blob; the joiner decodes + applies it, deduped by (author,seq)
    // so any live edits that race the backfill are not double-applied.
    std::vector<uint8_t> snapshot() const {
        std::vector<uint8_t> out;
        uint32_t n = (uint32_t)history_.size();
        const uint8_t* np = reinterpret_cast<const uint8_t*>(&n);
        out.insert(out.end(), np, np + sizeof(n));
        for (const EditMsg& m : history_) encode_edit(m, out);
        return out;
    }

    // Apply a snapshot blob produced by snapshot(). Returns edits applied.
    int applySnapshot(const std::vector<uint8_t>& blob) {
        if (blob.size() < sizeof(uint32_t)) return 0;
        uint32_t n = 0; std::memcpy(&n, blob.data(), sizeof(n));
        size_t off = sizeof(n);
        int applied = 0;
        for (uint32_t i = 0; i < n; i++) {
            EditMsg m{};
            size_t consumed = decode_edit(blob.data() + off, blob.size() - off, m);
            if (!consumed) break;
            off += consumed;
            uint64_t k = key(m.author, m.seq);
            if (!seen_.insert(k).second) continue;       // already have it
            if (m.kind == EditKind::Carve)
                apply_carve(m.fx, m.fy, m.fz, m.radius, m.material, nullptr);
            else
                world_.set_block((int)m.fx, (int)m.fy, (int)m.fz, (BlockId)m.material);
            history_.push_back(m);
            applied++;
        }
        return applied;
    }

    size_t historySize() const { return history_.size(); }

private:
    static uint64_t key(uint8_t author, uint32_t seq) {
        return ((uint64_t)author << 32) | seq;
    }
    // Tag a locally-originated edit as seen and append it to the replay log.
    void record(const EditMsg& m) {
        seen_.insert(key(m.author, m.seq));
        history_.push_back(m);
    }
    bool apply_carve(float x, float y, float z, float r, int op, BlockFlips* flips) {
        size_t before = flips ? flips->size() : 0;
        world_.carve_sphere(x, y, z, r, op, flips);
        return flips ? (flips->size() != before) : true;
    }

    World&         world_;
    EditTransport& transport_;
    uint8_t        author_;
    uint32_t       out_seq_ = 0;
    std::unordered_set<uint64_t> seen_;
    std::vector<EditMsg>         history_;   // ordered replay log for late-join backfill
};

} // namespace sdfcraft
