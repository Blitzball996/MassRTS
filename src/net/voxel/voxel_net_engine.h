#pragma once
// =============================================================================
// VoxelNetEngine - server-authoritative multiplayer for voxel sandbox mode
// -----------------------------------------------------------------------------
// Architecture (see DUAL_MODE_NETWORK.md + VOXEL_SANDBOX_NETWORK.md):
//   * Server authoritative: validates all edits, resolves conflicts
//   * Client prediction: dig/place instantly, server confirms/rejects
//   * Chunk streaming: only load visible chunks (view distance = 8 chunks)
//   * Delta persistence: only store player edits (base terrain from seed)
//   * 4-byte entity moves: MoveEntityPacketSmall-style delta compression
//
// Usage (server):
//   VoxelNetEngine net;
//   net.hostVoxelWorld(port, world_seed);
//   // game loop:
//   net.pollServer();
//   for (auto& player : net.getPlayers()) {
//       net.updatePlayerView(player);  // stream chunks
//   }
//
// Usage (client):
//   VoxelNetEngine net;
//   net.joinVoxelWorld(ip, port, "PlayerName");
//   net.pollClient();
//   // on player dig:
//   net.clientDigBlock(voxel_x, voxel_y, voxel_z);
// =============================================================================
#include "voxel_packets.h"
#include "../core/connection.h"
#include <map>
#include <set>
#include <memory>
#include <glm/glm.hpp>

namespace net {
namespace voxel {

struct ChunkCoord {
    int32_t cx, cy, cz;
    bool operator<(const ChunkCoord& o) const {
        if (cx != o.cx) return cx < o.cx;
        if (cy != o.cy) return cy < o.cy;
        return cz < o.cz;
    }
    bool operator==(const ChunkCoord& o) const {
        return cx == o.cx && cy == o.cy && cz == o.cz;
    }
};

struct ChunkCoordHash {
    size_t operator()(const ChunkCoord& c) const {
        return ((size_t)c.cx * 73856093) ^ ((size_t)c.cy * 19349663) ^ ((size_t)c.cz * 83492791);
    }
};

class VoxelNetEngine {
public:
    static constexpr int VIEW_DISTANCE = 8;  // chunks
    static constexpr int CHUNK_DIM = 32;     // voxels per chunk

    VoxelNetEngine();
    ~VoxelNetEngine();

    // ---- Server mode -------------------------------------------------------
    bool hostVoxelWorld(uint16_t port, uint64_t world_seed);
    void pollServer();  // receive/broadcast packets, update chunk streaming
    
    // ---- Client mode -------------------------------------------------------
    bool joinVoxelWorld(const std::string& ip, uint16_t port, const std::string& name);
    void pollClient();

    // ---- Client actions (prediction + send to server) ----------------------
    void clientDigBlock(int32_t x, int32_t y, int32_t z);
    void clientPlaceBlock(int32_t x, int32_t y, int32_t z, uint8_t block_type);
    void clientMove(float dx, float dy, float dz, float yaw);  // delta movement

    // ---- Server world management -------------------------------------------
    struct PlayerInfo {
        uint32_t entity_id;
        std::string name;
        NetAddress addr;
        glm::vec3 position;
        float yaw, pitch;
        std::set<ChunkCoord> loaded_chunks;
    };

    void serverUpdatePlayerView(PlayerInfo& player);  // stream chunks
    void serverOnPlayerAction(PlayerInfo& player, const VoxelPlayerActionPacket& act);
    bool serverValidateEdit(const PlayerInfo& player, int64_t voxel_key);  // anti-cheat

    // ---- World state (server) ----------------------------------------------
    struct ChunkDelta {
        ChunkCoord coord;
        std::vector<VoxelEdit> edits;  // player modifications
    };
    std::map<ChunkCoord, ChunkDelta> chunk_deltas;  // modified chunks

    void serverAddEdit(const ChunkCoord& chunk, const VoxelEdit& edit);
    void serverBroadcastEdit(const ChunkCoord& chunk, const VoxelEdit& edit);

    // ---- Client world state ------------------------------------------------
    struct PendingEdit {
        int64_t voxel_key;
        uint8_t predicted_type;
        uint32_t seq;
    };
    std::vector<PendingEdit> pending_edits;  // awaiting server confirm
    uint32_t edit_seq = 0;

    void clientApplyEdit(int64_t voxel_key, uint8_t block_type);  // local prediction
    void clientRejectEdit(int64_t voxel_key, uint8_t authoritative_type);  // server override

    // ---- Status ------------------------------------------------------------
    bool isActive() const { return is_server || is_client; }
    uint64_t worldSeed() const { return world_seed; }
    uint32_t localEntityId() const { return local_entity_id; }

private:
    bool is_server = false;
    bool is_client = false;

    UDPSocket socket;
    NetAddress server_addr;

    uint64_t world_seed = 0;
    uint32_t local_entity_id = 0;
    glm::vec3 local_position{0,0,0};

    // Server: connected players
    std::vector<PlayerInfo> players;

    void disconnect();
};

} // namespace voxel
} // namespace net
