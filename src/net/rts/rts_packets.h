#pragma once
// =============================================================================
// RTS Mode Packets - lockstep deterministic command synchronization
// -----------------------------------------------------------------------------
// ID range: 1-99 (reserved for RTS)
//
// RTS sync model (see NETWORK_ARCHITECTURE.md + ROLLBACK_NETCODE.md):
//   * Clients send Commands to server
//   * Server broadcasts CommandBatch to all clients
//   * All clients execute commands on same tick (lockstep barrier)
//   * Periodic SyncCheck to detect desync
//   * Terrain carves are commands (deterministic replay)
//
// Deterministic requirements:
//   * Fixed-point math for critical paths (positions, combat)
//   * Same command order on all clients
//   * No floating-point in wire format (use fixed-point i32)
// =============================================================================
#include "../core/packet.h"
#include <vector>
#include <glm/glm.hpp>

namespace net {
namespace rts {

// ---- RTS packet IDs --------------------------------------------------------
enum RtsPacketId {
    RTS_CONNECT        = 1,
    RTS_ACCEPT         = 2,
    RTS_COMMAND        = 3,   // client → server: command(s) for future tick
    RTS_COMMAND_BATCH  = 4,   // server → clients: broadcast all commands for tick T
    RTS_SYNC_CHECK     = 5,   // periodic checksum to detect desync
    RTS_TERRAIN_CARVE  = 6,   // SDF dig/fill command
    RTS_PLAYER_JOINED  = 7,
    RTS_PLAYER_LEFT    = 8,
};

// ---- Command types ---------------------------------------------------------
enum class CmdType : uint8_t {
    Move = 0,
    AttackMove = 1,
    Stop = 2,
    BuildStructure = 3,
    Rally = 4,
    TerrainCarve = 5,  // dig/fill
};

// ---- Core RTS command structure --------------------------------------------
// Fixed-point positions (1 unit = 1/256 world unit) for determinism across platforms
struct RtsCommand {
    uint32_t tick;          // execution tick
    uint8_t  player_id;
    uint8_t  faction;
    CmdType  type;
    uint8_t  _pad;
    
    // Fixed-point target position (xy in xz plane, y ignored for most commands)
    int32_t  target_x_fp;   // fixed16.16
    int32_t  target_z_fp;
    
    uint32_t param_a;       // build type / carve op / etc
    uint32_t param_b;       // radius / count / etc
    uint32_t unit_start;    // affected unit id range
    uint32_t unit_end;

    void write(ByteBuffer& buf) const {
        buf.write_u32(tick);
        buf.write_u8(player_id);
        buf.write_u8(faction);
        buf.write_u8((uint8_t)type);
        buf.write_u8(_pad);
        buf.write_i32(target_x_fp);
        buf.write_i32(target_z_fp);
        buf.write_u32(param_a);
        buf.write_u32(param_b);
        buf.write_u32(unit_start);
        buf.write_u32(unit_end);
    }

    void read(ByteBuffer& buf) {
        tick = buf.read_u32();
        player_id = buf.read_u8();
        faction = buf.read_u8();
        type = (CmdType)buf.read_u8();
        _pad = buf.read_u8();
        target_x_fp = buf.read_i32();
        target_z_fp = buf.read_i32();
        param_a = buf.read_u32();
        param_b = buf.read_u32();
        unit_start = buf.read_u32();
        unit_end = buf.read_u32();
    }

    // Helper: convert from float world pos (editor/input) to fixed-point wire format
    static int32_t toFixed(float f) { return (int32_t)(f * 65536.0f); }
    static float fromFixed(int32_t fp) { return (float)fp / 65536.0f; }
};

// =============================================================================
// RtsCommandPacket - client sends command(s) to server
// =============================================================================
class RtsCommandPacket : public Packet {
public:
    std::vector<RtsCommand> commands;

    int getId() const override { return RTS_COMMAND; }
    int estimatedSize() const override { 
        return 4 + (int)commands.size() * 44;  // varint count + commands
    }

    void write(ByteBuffer& buf) const override {
        buf.write_varint((int32_t)commands.size());
        for (auto& c : commands) c.write(buf);
    }

    void read(ByteBuffer& buf) override {
        int count = buf.read_varint();
        if (count < 0 || count > 1000) throw std::runtime_error("cmd count invalid");
        commands.resize(count);
        for (auto& c : commands) c.read(buf);
    }

    void handle(PacketHandler& h) override { (void)h; }
};

// =============================================================================
// RtsCommandBatchPacket - server broadcasts all commands for tick T
// =============================================================================
class RtsCommandBatchPacket : public Packet {
public:
    uint32_t tick;
    std::vector<RtsCommand> commands;

    int getId() const override { return RTS_COMMAND_BATCH; }
    int estimatedSize() const override { 
        return 8 + (int)commands.size() * 44;
    }

    void write(ByteBuffer& buf) const override {
        buf.write_u32(tick);
        buf.write_varint((int32_t)commands.size());
        for (auto& c : commands) c.write(buf);
    }

    void read(ByteBuffer& buf) override {
        tick = buf.read_u32();
        int count = buf.read_varint();
        if (count < 0 || count > 1000) throw std::runtime_error("batch count invalid");
        commands.resize(count);
        for (auto& c : commands) c.read(buf);
    }

    void handle(PacketHandler& h) override { (void)h; }
};

// =============================================================================
// RtsSyncCheckPacket - periodic checksum to detect desync
// =============================================================================
class RtsSyncCheckPacket : public Packet {
public:
    uint32_t tick;
    uint32_t checksum;  // FNV-1a hash of all unit positions/hp (fixed-point)

    int getId() const override { return RTS_SYNC_CHECK; }
    int estimatedSize() const override { return 8; }

    void write(ByteBuffer& buf) const override {
        buf.write_u32(tick);
        buf.write_u32(checksum);
    }

    void read(ByteBuffer& buf) override {
        tick = buf.read_u32();
        checksum = buf.read_u32();
    }

    void handle(PacketHandler& h) override { (void)h; }
};

// =============================================================================
// RtsConnectPacket - client requests to join game
// =============================================================================
class RtsConnectPacket : public Packet {
public:
    std::string player_name;
    uint32_t version = 1;  // protocol version

    int getId() const override { return RTS_CONNECT; }
    int estimatedSize() const override { return 4 + (int)player_name.size(); }

    void write(ByteBuffer& buf) const override {
        buf.write_u32(version);
        buf.write_string(player_name);
    }

    void read(ByteBuffer& buf) override {
        version = buf.read_u32();
        player_name = buf.read_string(32);
    }

    void handle(PacketHandler& h) override { (void)h; }
};

// =============================================================================
// RtsAcceptPacket - server accepts client, assigns id/faction/seed
// =============================================================================
class RtsAcceptPacket : public Packet {
public:
    uint8_t  assigned_player_id;
    uint8_t  assigned_faction;
    uint32_t start_tick;
    uint64_t terrain_seed;  // deterministic terrain generation seed

    int getId() const override { return RTS_ACCEPT; }
    int estimatedSize() const override { return 14; }

    void write(ByteBuffer& buf) const override {
        buf.write_u8(assigned_player_id);
        buf.write_u8(assigned_faction);
        buf.write_u32(start_tick);
        buf.write_u64(terrain_seed);
    }

    void read(ByteBuffer& buf) override {
        assigned_player_id = buf.read_u8();
        assigned_faction = buf.read_u8();
        start_tick = buf.read_u32();
        terrain_seed = buf.read_u64();
    }

    void handle(PacketHandler& h) override { (void)h; }
};

// =============================================================================
// Packet registration (auto-registers when this header is included)
// =============================================================================
inline void registerRtsPackets() {
    Packet::registerPacket(RTS_COMMAND, false, true, false,
        []() { return std::make_unique<RtsCommandPacket>(); });
    Packet::registerPacket(RTS_COMMAND_BATCH, true, false, true,
        []() { return std::make_unique<RtsCommandBatchPacket>(); });
    Packet::registerPacket(RTS_SYNC_CHECK, true, true, false,
        []() { return std::make_unique<RtsSyncCheckPacket>(); });
    Packet::registerPacket(RTS_CONNECT, false, true, false,
        []() { return std::make_unique<RtsConnectPacket>(); });
    Packet::registerPacket(RTS_ACCEPT, true, false, false,
        []() { return std::make_unique<RtsAcceptPacket>(); });
}

} // namespace rts
} // namespace net
