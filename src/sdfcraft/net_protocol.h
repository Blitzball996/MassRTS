#pragma once
// =============================================================================
// SDFCraft - Multiplayer wire protocol (Phase B3/N)
// -----------------------------------------------------------------------------
// One tagged, length-framed message stream shared by host, client and dedicated
// server. The session layer (net_session.h) handles framing/sockets; this file
// only defines the message TYPES and their (de)serialization. Gameplay never
// touches sockets — it produces/consumes these PODs.
//
// AUTHORITY MODEL (server-authoritative + client prediction)
//   client -> server : Hello, EditIntent, PlayerMove, AttackIntent, EatIntent, Respawn
//   server -> client : Welcome, Edit (confirmed/broadcast), Roster, MobSnapshot,
//                       TimeSync, PlayerStats, RemovePlayer
//
// All integers little-endian (x86/ARM-LE only, same assumption as net_ops.h).
// Each message is: [uint8 type][payload...]; the session layer prefixes the
// whole thing with a uint32 length so reads are self-delimiting.
// =============================================================================
#include "entity.h"
#include "items.h"
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>

namespace sdfcraft {

// --- message type tags -------------------------------------------------------
enum class MsgType : uint8_t {
    // client -> server
    Hello        = 1,   // join request (proto version, desired name)
    EditIntent   = 2,   // carve/place request (server validates + broadcasts)
    PlayerMove   = 3,   // client's predicted position/orientation
    AttackIntent = 4,   // melee swing at a mob (server resolves damage)
    EatIntent    = 5,   // consume held food
    Respawn      = 6,   // request respawn after death

    // server -> client
    Welcome      = 64,  // assigned player id + world seed + initial time
    Edit         = 65,  // authoritative carve/place to apply (echo or remote)
    Roster       = 66,  // one remote player's id/name/position (repeated)
    RemovePlayer = 67,  // a player left
    MobSnapshot  = 68,  // full live-mob list near a player (id/kind/pos/yaw/hp)
    TimeSync     = 69,  // authoritative time-of-day + day count
    PlayerStats  = 70,  // this client's authoritative health/hunger/air + dead
};

static constexpr uint16_t SDFCRAFT_PROTO_VERSION = 1;
static constexpr uint16_t SDFCRAFT_DEFAULT_PORT  = 55001;

// --- little-endian POD cursor (write/read) -----------------------------------
struct ByteWriter {
    std::vector<uint8_t> buf;
    explicit ByteWriter(MsgType t) { buf.push_back((uint8_t)t); }
    template <class T> void put(const T& v) {
        static_assert(std::is_trivially_copyable<T>::value, "POD only");
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
        buf.insert(buf.end(), p, p + sizeof(T));
    }
    void put_str(const std::string& s) {
        uint16_t n = (uint16_t)std::min<size_t>(s.size(), 255);
        put(n); buf.insert(buf.end(), s.begin(), s.begin() + n);
    }
};

struct ByteReader {
    const uint8_t* p; size_t n; size_t off = 1;   // off=1 skips the type byte
    ByteReader(const uint8_t* d, size_t len) : p(d), n(len) {}
    MsgType type() const { return n ? (MsgType)p[0] : (MsgType)0; }
    bool ok() const { return off <= n; }
    template <class T> bool get(T& v) {
        if (off + sizeof(T) > n) return false;
        std::memcpy(&v, p + off, sizeof(T)); off += sizeof(T); return true;
    }
    bool get_str(std::string& s) {
        uint16_t len; if (!get(len)) return false;
        if (off + len > n) return false;
        s.assign((const char*)p + off, len); off += len; return true;
    }
};

// --- payload structs (kept flat; serialized field-by-field) ------------------
// Network protocol kinds mirror the EditKind in net_ops.h but travel here.
struct NetEdit {
    uint8_t  kind;     // 1=carve, 2=place  (matches EditKind)
    uint8_t  author;   // originating player id
    float    x, y, z;
    float    radius;   // carve radius (place: unused)
    int32_t  material; // carve op / placed BlockId
};

struct NetPlayerState {
    uint8_t  id;
    float    x, y, z;
    float    yaw, pitch;
    uint8_t  moving;   // 1 = walking this tick (drives remote walk animation)
};

struct NetMob {
    uint32_t id;
    uint8_t  kind;     // MobKind
    float    x, y, z;
    float    yaw;
    float    health;
    uint8_t  moving;   // 1 = walking this tick (drives walk animation)
    uint8_t  hurt;     // 1 = took damage very recently (drives red hit flash)
};

// --- encoders (server + client) ----------------------------------------------
inline std::vector<uint8_t> enc_hello(const std::string& name) {
    ByteWriter w(MsgType::Hello); w.put(SDFCRAFT_PROTO_VERSION); w.put_str(name); return w.buf;
}
inline std::vector<uint8_t> enc_welcome(uint8_t id, uint64_t seed, float tod, uint32_t day) {
    ByteWriter w(MsgType::Welcome); w.put(id); w.put(seed); w.put(tod); w.put(day); return w.buf;
}
inline std::vector<uint8_t> enc_edit(MsgType t, const NetEdit& e) {
    ByteWriter w(t); w.put(e); return w.buf;     // t = EditIntent or Edit
}
inline std::vector<uint8_t> enc_move(const NetPlayerState& s) {
    ByteWriter w(MsgType::PlayerMove); w.put(s); return w.buf;
}
inline std::vector<uint8_t> enc_roster(const NetPlayerState& s, const std::string& name) {
    ByteWriter w(MsgType::Roster); w.put(s); w.put_str(name); return w.buf;
}
inline std::vector<uint8_t> enc_remove_player(uint8_t id) {
    ByteWriter w(MsgType::RemovePlayer); w.put(id); return w.buf;
}
inline std::vector<uint8_t> enc_time(float tod, uint32_t day) {
    ByteWriter w(MsgType::TimeSync); w.put(tod); w.put(day); return w.buf;
}
inline std::vector<uint8_t> enc_stats(float hp, float maxhp, float hunger, float air, uint8_t dead) {
    ByteWriter w(MsgType::PlayerStats); w.put(hp); w.put(maxhp); w.put(hunger); w.put(air); w.put(dead); return w.buf;
}
inline std::vector<uint8_t> enc_attack(uint32_t mob_id, float ex, float ey, float ez,
                                       float dx, float dy, float dz) {
    ByteWriter w(MsgType::AttackIntent);
    w.put(mob_id); w.put(ex); w.put(ey); w.put(ez); w.put(dx); w.put(dy); w.put(dz);
    return w.buf;
}
inline std::vector<uint8_t> enc_eat() { ByteWriter w(MsgType::EatIntent); return w.buf; }
inline std::vector<uint8_t> enc_respawn() { ByteWriter w(MsgType::Respawn); return w.buf; }

// MobSnapshot: [uint16 count][NetMob...]
inline std::vector<uint8_t> enc_mob_snapshot(const std::vector<NetMob>& mobs) {
    ByteWriter w(MsgType::MobSnapshot);
    uint16_t n = (uint16_t)std::min<size_t>(mobs.size(), 2000);
    w.put(n);
    for (uint16_t i = 0; i < n; i++) w.put(mobs[i]);
    return w.buf;
}
inline bool dec_mob_snapshot(ByteReader& r, std::vector<NetMob>& out) {
    uint16_t n; if (!r.get(n)) return false;
    out.clear(); out.reserve(n);
    for (uint16_t i = 0; i < n; i++) { NetMob m; if (!r.get(m)) return false; out.push_back(m); }
    return true;
}

} // namespace sdfcraft
