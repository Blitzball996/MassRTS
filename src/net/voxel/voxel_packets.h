#pragma once
// =============================================================================
// Voxel Sandbox Mode Packets - server-authoritative world sync
// -----------------------------------------------------------------------------
// ID range: 100-199 (reserved for voxel mode)
//
// Architecture ported from MinecraftConsoles (see DUAL_MODE_NETWORK.md):
//   * Server authoritative: server validates all edits, broadcasts to clients
//   * Client prediction: dig/place instantly locally, server confirms/rejects
//   * Chunk streaming: only send visible chunks within view distance
//   * Entity delta packing: 4-byte position updates (MoveEntityPacketSmall style)
//   * Dirty field sync: only changed entity data fields (SynchedEntityData style)
//   * Inventory server-authoritative: prevent item duplication exploits
//
// World state model:
//   Base layer (procedural)     = SDF terrain from seed (zero bandwidth)
//   Delta layer (player edits)  = voxel changes (synced & persisted)
//   Entities                    = players, mobs (position + data sync)
// =============================================================================
#include "../core/packet.h"
#include "../core/fixed_point.h"
#include <vector>
#include <glm/glm.hpp>

namespace net {
namespace voxel {

// ---- Voxel packet IDs ------------------------------------------------------
enum VoxelPacketId {
    VOXEL_CONNECT           = 100,
    VOXEL_ACCEPT            = 101,
    VOXEL_CHUNK_VISIBILITY  = 102,  // server tells client chunk enter/leave view
    VOXEL_CHUNK_DATA        = 103,  // chunk edits (delta layer only)
    VOXEL_EDIT_BATCH        = 104,  // multiple voxel changes (dig/place)
    VOXEL_PLAYER_ACTION     = 105,  // client: dig/place/interact
    VOXEL_PLAYER_MOVE       = 106,  // 4-byte delta (MC MoveEntityPacketSmall style)
    VOXEL_PLAYER_TELEPORT   = 107,  // full position (when delta overflows)
    VOXEL_ENTITY_DATA       = 108,  // dirty entity fields (hp/state/equipment)
    VOXEL_INVENTORY_SLOT    = 109,  // single inventory slot change
    VOXEL_INVENTORY_FULL    = 110,  // full inventory (on join)
};

// ---- Voxel edit (single block change) --------------------------------------
struct VoxelEdit {
    int64_t voxel_key;      // packed (x:21 | y:21 | z:21) global voxel coord
    uint8_t block_type;     // 0=air (dig), 1..255=block types
    uint8_t flags;          // bit0=player_placed, bit1=natural

    static int64_t pack(int32_t x, int32_t y, int32_t z) {
        return ((int64_t)(x & 0x1FFFFF) << 42) |
               ((int64_t)(y & 0x1FFFFF) << 21) |
               ((int64_t)(z & 0x1FFFFF));
    }
    static void unpack(int64_t key, int32_t& x, int32_t& y, int32_t& z) {
        x = (int32_t)((key >> 42) & 0x1FFFFF); if (x & 0x100000) x |= 0xFFE00000;
        y = (int32_t)((key >> 21) & 0x1FFFFF); if (y & 0x100000) y |= 0xFFE00000;
        z = (int32_t)(key & 0x1FFFFF);         if (z & 0x100000) z |= 0xFFE00000;
    }

    void write(ByteBuffer& buf) const {
        buf.write_i64(voxel_key);
        buf.write_u8(block_type);
        buf.write_u8(flags);
    }
    void read(ByteBuffer& buf) {
        voxel_key = buf.read_i64();
        block_type = buf.read_u8();
        flags = buf.read_u8();
    }
};

// =============================================================================
// VoxelConnectPacket - client requests to join voxel world
// =============================================================================
class VoxelConnectPacket : public Packet {
public:
    std::string player_name;
    uint32_t version = 1;

    int getId() const override { return VOXEL_CONNECT; }
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
// VoxelAcceptPacket - server accepts, assigns id + world seed
// =============================================================================
class VoxelAcceptPacket : public Packet {
public:
    uint32_t player_entity_id;
    uint64_t world_seed;
    float spawn_x, spawn_y, spawn_z;

    int getId() const override { return VOXEL_ACCEPT; }
    int estimatedSize() const override { return 24; }

    void write(ByteBuffer& buf) const override {
        buf.write_u32(player_entity_id);
        buf.write_u64(world_seed);
        buf.write_f32(spawn_x);
        buf.write_f32(spawn_y);
        buf.write_f32(spawn_z);
    }
    void read(ByteBuffer& buf) override {
        player_entity_id = buf.read_u32();
        world_seed = buf.read_u64();
        spawn_x = buf.read_f32();
        spawn_y = buf.read_f32();
        spawn_z = buf.read_f32();
    }
    void handle(PacketHandler& h) override { (void)h; }
};

// =============================================================================
// VoxelChunkVisibilityPacket - notify client chunk enter/leave view
// =============================================================================
class VoxelChunkVisibilityPacket : public Packet {
public:
    int32_t cx, cy, cz;  // chunk coords
    bool visible;        // true=load, false=unload

    int getId() const override { return VOXEL_CHUNK_VISIBILITY; }
    int estimatedSize() const override { return 13; }

    void write(ByteBuffer& buf) const override {
        buf.write_i32(cx);
        buf.write_i32(cy);
        buf.write_i32(cz);
        buf.write_u8(visible ? 1 : 0);
    }
    void read(ByteBuffer& buf) override {
        cx = buf.read_i32();
        cy = buf.read_i32();
        cz = buf.read_i32();
        visible = buf.read_u8() != 0;
    }
    void handle(PacketHandler& h) override { (void)h; }
};

// =============================================================================
// VoxelChunkDataPacket - chunk delta layer (only player edits, not base terrain)
// =============================================================================
class VoxelChunkDataPacket : public Packet {
public:
    int32_t cx, cy, cz;
    std::vector<VoxelEdit> edits;  // empty if chunk unmodified (pure seed)
    bool shouldDelay = true;       // goes to slow queue (large data)

    int getId() const override { return VOXEL_CHUNK_DATA; }
    int estimatedSize() const override { 
        return 16 + (int)edits.size() * 10; 
    }

    void write(ByteBuffer& buf) const override {
        buf.write_i32(cx);
        buf.write_i32(cy);
        buf.write_i32(cz);
        buf.write_varint((int32_t)edits.size());
        for (auto& e : edits) e.write(buf);
    }
    void read(ByteBuffer& buf) override {
        cx = buf.read_i32();
        cy = buf.read_i32();
        cz = buf.read_i32();
        int count = buf.read_varint();
        if (count < 0 || count > 100000) throw std::runtime_error("edit count");
        edits.resize(count);
        for (auto& e : edits) e.read(buf);
    }
    void handle(PacketHandler& h) override { (void)h; }
};

// =============================================================================
// VoxelEditBatchPacket - batched dig/place (MC ChunkTilesUpdatePacket style)
// =============================================================================
class VoxelEditBatchPacket : public Packet {
public:
    int32_t cx, cz;  // chunk
    std::vector<uint16_t> positions;  // chunk-local offsets
    std::vector<uint8_t> block_types;
    uint8_t count;

    int getId() const override { return VOXEL_EDIT_BATCH; }
    int estimatedSize() const override { return 9 + count * 3; }

    void write(ByteBuffer& buf) const override {
        buf.write_i32(cx);
        buf.write_i32(cz);
        buf.write_u8(count);
        for (int i = 0; i < count; i++) {
            buf.write_u16(positions[i]);
            buf.write_u8(block_types[i]);
        }
    }
    void read(ByteBuffer& buf) override {
        cx = buf.read_i32();
        cz = buf.read_i32();
        count = buf.read_u8();
        positions.resize(count);
        block_types.resize(count);
        for (int i = 0; i < count; i++) {
            positions[i] = buf.read_u16();
            block_types[i] = buf.read_u8();
        }
    }
    void handle(PacketHandler& h) override { (void)h; }
};

// =============================================================================
// VoxelPlayerActionPacket - client digs/places block
// =============================================================================
class VoxelPlayerActionPacket : public Packet {
public:
    enum Action : uint8_t {
        START_DIG = 0,
        FINISH_DIG = 1,
        PLACE_BLOCK = 2,
        CANCEL = 3
    };
    uint32_t player_id;
    Action action;
    int64_t voxel_key;
    uint8_t face;  // direction (for placement orientation)

    int getId() const override { return VOXEL_PLAYER_ACTION; }
    int estimatedSize() const override { return 14; }

    void write(ByteBuffer& buf) const override {
        buf.write_u32(player_id);
        buf.write_u8((uint8_t)action);
        buf.write_i64(voxel_key);
        buf.write_u8(face);
    }
    void read(ByteBuffer& buf) override {
        player_id = buf.read_u32();
        action = (Action)buf.read_u8();
        voxel_key = buf.read_i64();
        face = buf.read_u8();
    }
    void handle(PacketHandler& h) override { (void)h; }
};

// =============================================================================
// VoxelPlayerMovePacket - 4-byte delta movement (MC MoveEntityPacketSmall)
// =============================================================================
class VoxelPlayerMovePacket : public Packet {
public:
    uint16_t id_and_yrot;  // id(11 bits) | yrot(5 bits)
    int16_t xyz_delta;     // x(5) | y(6) | z(5) packed deltas in fixed-point
    
    // Helper: pack from float deltas (client prediction)
    static VoxelPlayerMovePacket fromDelta(uint32_t id, float dx, float dy, float dz, float yaw) {
        VoxelPlayerMovePacket p;
        // Quantize deltas to char range (-128..127 in 1/32 units)
        int8_t x = (int8_t)(dx * 32.0f);
        int8_t y = (int8_t)(dy * 32.0f);
        int8_t z = (int8_t)(dz * 32.0f);
        uint8_t yr = (uint8_t)((yaw / 360.0f) * 32.0f) & 0x1F;
        
        p.id_and_yrot = (uint16_t)((id & 0x7FF) | (yr << 11));
        p.xyz_delta = (int16_t)(((x & 0x1F) << 11) | ((y & 0x3F) << 5) | (z & 0x1F));
        return p;
    }

    int getId() const override { return VOXEL_PLAYER_MOVE; }
    int estimatedSize() const override { return 4; }

    void write(ByteBuffer& buf) const override {
        buf.write_u16(id_and_yrot);
        buf.write_i16(xyz_delta);
    }
    void read(ByteBuffer& buf) override {
        id_and_yrot = buf.read_u16();
        xyz_delta = buf.read_i16();
    }
    void handle(PacketHandler& h) override { (void)h; }

    // Packet invalidation: old moves superseded by new ones (bandwidth opt)
    bool canBeInvalidated() const override { return true; }
    bool isInvalidatedBy(const Packet& other) const override {
        if (other.getId() != VOXEL_PLAYER_MOVE) return false;
        auto& o = (const VoxelPlayerMovePacket&)other;
        return (id_and_yrot & 0x7FF) == (o.id_and_yrot & 0x7FF);  // same player id
    }
};

// =============================================================================
// VoxelPlayerTeleportPacket - full position (when delta overflows)
// =============================================================================
class VoxelPlayerTeleportPacket : public Packet {
public:
    uint32_t entity_id;
    double x, y, z;
    float yaw, pitch;

    int getId() const override { return VOXEL_PLAYER_TELEPORT; }
    int estimatedSize() const override { return 36; }

    void write(ByteBuffer& buf) const override {
        buf.write_u32(entity_id);
        buf.write_f64(x);
        buf.write_f64(y);
        buf.write_f64(z);
        buf.write_f32(yaw);
        buf.write_f32(pitch);
    }
    void read(ByteBuffer& buf) override {
        entity_id = buf.read_u32();
        x = buf.read_f64();
        y = buf.read_f64();
        z = buf.read_f64();
        yaw = buf.read_f32();
        pitch = buf.read_f32();
    }
    void handle(PacketHandler& h) override { (void)h; }
};

// =============================================================================
// VoxelEntityDataPacket - dirty entity fields (MC SynchedEntityData)
// =============================================================================
class VoxelEntityDataPacket : public Packet {
public:
    uint32_t entity_id;
    struct Field {
        uint8_t type_and_id;  // type(3 bits) | id(5 bits)
        union {
            uint8_t  u8_val;
            int16_t  i16_val;
            int32_t  i32_val;
            float    f32_val;
        };
        void write(ByteBuffer& buf) const {
            buf.write_u8(type_and_id);
            uint8_t type = (type_and_id >> 5) & 0x7;
            switch (type) {
                case 0: buf.write_u8(u8_val); break;
                case 1: buf.write_i16(i16_val); break;
                case 2: buf.write_i32(i32_val); break;
                case 3: buf.write_f32(f32_val); break;
            }
        }
        void read(ByteBuffer& buf) {
            type_and_id = buf.read_u8();
            uint8_t type = (type_and_id >> 5) & 0x7;
            switch (type) {
                case 0: u8_val = buf.read_u8(); break;
                case 1: i16_val = buf.read_i16(); break;
                case 2: i32_val = buf.read_i32(); break;
                case 3: f32_val = buf.read_f32(); break;
            }
        }
    };
    std::vector<Field> dirty_fields;

    int getId() const override { return VOXEL_ENTITY_DATA; }
    int estimatedSize() const override { return 8 + (int)dirty_fields.size() * 5; }

    void write(ByteBuffer& buf) const override {
        buf.write_u32(entity_id);
        buf.write_varint((int32_t)dirty_fields.size());
        for (auto& f : dirty_fields) f.write(buf);
    }
    void read(ByteBuffer& buf) override {
        entity_id = buf.read_u32();
        int count = buf.read_varint();
        if (count < 0 || count > 32) throw std::runtime_error("field count");
        dirty_fields.resize(count);
        for (auto& f : dirty_fields) f.read(buf);
    }
    void handle(PacketHandler& h) override { (void)h; }
};

// =============================================================================
// VoxelInventorySlotPacket - single inventory slot change
// =============================================================================
class VoxelInventorySlotPacket : public Packet {
public:
    uint8_t container_id;
    uint8_t slot_index;
    uint16_t item_id;
    uint16_t count;

    int getId() const override { return VOXEL_INVENTORY_SLOT; }
    int estimatedSize() const override { return 6; }

    void write(ByteBuffer& buf) const override {
        buf.write_u8(container_id);
        buf.write_u8(slot_index);
        buf.write_u16(item_id);
        buf.write_u16(count);
    }
    void read(ByteBuffer& buf) override {
        container_id = buf.read_u8();
        slot_index = buf.read_u8();
        item_id = buf.read_u16();
        count = buf.read_u16();
    }
    void handle(PacketHandler& h) override { (void)h; }
};

// =============================================================================
// Packet registration
// =============================================================================
inline void registerVoxelPackets() {
    Packet::registerPacket(VOXEL_CONNECT, false, true, false,
        []() { return std::make_unique<VoxelConnectPacket>(); });
    Packet::registerPacket(VOXEL_ACCEPT, true, false, false,
        []() { return std::make_unique<VoxelAcceptPacket>(); });
    Packet::registerPacket(VOXEL_CHUNK_VISIBILITY, true, false, true,
        []() { return std::make_unique<VoxelChunkVisibilityPacket>(); });
    Packet::registerPacket(VOXEL_CHUNK_DATA, true, false, true,
        []() { return std::make_unique<VoxelChunkDataPacket>(); });
    Packet::registerPacket(VOXEL_EDIT_BATCH, true, false, true,
        []() { return std::make_unique<VoxelEditBatchPacket>(); });
    Packet::registerPacket(VOXEL_PLAYER_ACTION, false, true, false,
        []() { return std::make_unique<VoxelPlayerActionPacket>(); });
    Packet::registerPacket(VOXEL_PLAYER_MOVE, true, true, true,
        []() { return std::make_unique<VoxelPlayerMovePacket>(); });
    Packet::registerPacket(VOXEL_PLAYER_TELEPORT, true, false, true,
        []() { return std::make_unique<VoxelPlayerTeleportPacket>(); });
    Packet::registerPacket(VOXEL_ENTITY_DATA, true, false, true,
        []() { return std::make_unique<VoxelEntityDataPacket>(); });
    Packet::registerPacket(VOXEL_INVENTORY_SLOT, true, false, false,
        []() { return std::make_unique<VoxelInventorySlotPacket>(); });
}

} // namespace voxel
} // namespace net
