#include "voxel_net_engine.h"
#include <iostream>
#include <cmath>

namespace net {
namespace voxel {

static ChunkCoord voxelToChunk(int32_t x, int32_t y, int32_t z) {
    auto fd = [](int32_t v) {
        return (v >= 0) ? (v / VoxelNetEngine::CHUNK_DIM)
                        : ((v - VoxelNetEngine::CHUNK_DIM + 1) / VoxelNetEngine::CHUNK_DIM);
    };
    return ChunkCoord{ fd(x), fd(y), fd(z) };
}

// Helper: send a packet over UDP to an address
static void sendPacket(UDPSocket& sock, NetAddress addr, const Packet& pkt) {
    ByteBuffer buf;
    buf.write_varint(pkt.getId());
    ByteBuffer payload;
    pkt.write(payload);
    buf.write_varint((int32_t)payload.size());
    buf.write_bytes(payload.data);
    sock.send_to(buf.ptr(), buf.size(), addr);
}

VoxelNetEngine::VoxelNetEngine() {
    registerVoxelPackets();
}

VoxelNetEngine::~VoxelNetEngine() {
    disconnect();
}

bool VoxelNetEngine::hostVoxelWorld(uint16_t port, uint64_t seed) {
    if (!UDPSocket::init_network()) return false;
    if (!socket.create()) return false;
    if (!socket.bind_port(port)) return false;
    socket.set_nonblocking();

    is_server = true;
    world_seed = seed;
    std::cout << "[VoxelNet] Hosting voxel world on port " << port
              << " seed=" << seed << "\n";
    return true;
}

bool VoxelNetEngine::joinVoxelWorld(const std::string& ip, uint16_t port,
                                     const std::string& name) {
    if (!UDPSocket::init_network()) return false;
    if (!socket.create()) return false;
    socket.set_nonblocking();

    int a, b, c, d;
    if (sscanf(ip.c_str(), "%d.%d.%d.%d", &a, &b, &c, &d) != 4) return false;
    server_addr.ip = ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | (uint32_t)d;
    server_addr.port = port;

    VoxelConnectPacket pkt;
    pkt.player_name = name;
    pkt.version = 1;
    sendPacket(socket, server_addr, pkt);

    is_client = true;
    std::cout << "[VoxelNet] Joining voxel world " << ip << ":" << port << "\n";
    return true;
}

void VoxelNetEngine::disconnect() {
    socket.close_socket();
    is_server = false;
    is_client = false;
    UDPSocket::shutdown_network();
}

// ---- Server: receive packets, handle player actions ------------------------
void VoxelNetEngine::pollServer() {
    if (!is_server) return;
    uint8_t recv_buf[2048];
    NetAddress from;

    while (true) {
        int n = socket.recv_from(recv_buf, sizeof(recv_buf), from);
        if (n <= 0) break;
        try {
            ByteBuffer buf(recv_buf, n);
            int id = buf.read_varint();
            int payload_size = buf.read_varint();
            if (payload_size < 0 || payload_size > (int)buf.remaining()) continue;
            if (!Packet::canReceive(id, true)) continue;  // direction check (anti-cheat)

            auto packet = Packet::create(id);
            if (!packet) continue;
            packet->read(buf);

            if (id == VOXEL_CONNECT) {
                auto* p = (VoxelConnectPacket*)packet.get();
                // Accept new player
                PlayerInfo info;
                info.entity_id = (uint32_t)players.size() + 1;
                info.name = p->player_name;
                info.addr = from;
                info.position = glm::vec3(0, 64, 0);  // spawn
                info.yaw = 0; info.pitch = 0;
                players.push_back(info);

                VoxelAcceptPacket accept;
                accept.player_entity_id = info.entity_id;
                accept.world_seed = world_seed;
                accept.spawn_x = 0; accept.spawn_y = 64; accept.spawn_z = 0;
                sendPacket(socket, from, accept);
                std::cout << "[VoxelNet] Player joined: " << p->player_name
                          << " id=" << info.entity_id << "\n";

            } else if (id == VOXEL_PLAYER_ACTION) {
                auto* p = (VoxelPlayerActionPacket*)packet.get();
                // Find player by address
                for (auto& pl : players) {
                    if (pl.addr == from) {
                        serverOnPlayerAction(pl, *p);
                        break;
                    }
                }
            } else if (id == VOXEL_PLAYER_MOVE) {
                // Relay player move to all other players (broadcast)
                for (auto& pl : players) {
                    if (!(pl.addr == from)) {
                        sendPacket(socket, pl.addr, *packet);
                    }
                }
            }
        } catch (...) {}
    }

    // Stream chunks to all players
    for (auto& pl : players) {
        serverUpdatePlayerView(pl);
    }
}

void VoxelNetEngine::serverOnPlayerAction(PlayerInfo& player,
                                           const VoxelPlayerActionPacket& act) {
    // Anti-cheat validation
    if (!serverValidateEdit(player, act.voxel_key)) {
        std::cout << "[VoxelNet] Rejected edit from " << player.name << "\n";
        return;
    }

    int32_t vx, vy, vz;
    VoxelEdit::unpack(act.voxel_key, vx, vy, vz);
    ChunkCoord chunk = voxelToChunk(vx, vy, vz);

    VoxelEdit edit;
    edit.voxel_key = act.voxel_key;
    edit.flags = 1;  // player edit

    if (act.action == VoxelPlayerActionPacket::FINISH_DIG) {
        edit.block_type = 0;  // air (removed)
        // TODO: give player the mined block in inventory
    } else if (act.action == VoxelPlayerActionPacket::PLACE_BLOCK) {
        edit.block_type = act.face;  // block type carried in face field for now
        // TODO: verify player has block in inventory, consume it
    } else {
        return;  // START_DIG / CANCEL don't modify world
    }

    serverAddEdit(chunk, edit);
    serverBroadcastEdit(chunk, edit);
}

bool VoxelNetEngine::serverValidateEdit(const PlayerInfo& player, int64_t voxel_key) {
    int32_t vx, vy, vz;
    VoxelEdit::unpack(voxel_key, vx, vy, vz);
    // Distance check (anti-cheat: can't edit blocks too far away)
    glm::vec3 voxel_world(vx, vy, vz);
    float dist = glm::length(voxel_world - player.position);
    const float MAX_REACH = 8.0f;  // blocks
    if (dist > MAX_REACH) return false;
    // TODO: permission check (protected regions)
    return true;
}

void VoxelNetEngine::serverAddEdit(const ChunkCoord& chunk, const VoxelEdit& edit) {
    auto& delta = chunk_deltas[chunk];
    delta.coord = chunk;
    // Replace existing edit at same voxel, or add new
    for (auto& e : delta.edits) {
        if (e.voxel_key == edit.voxel_key) {
            e = edit;
            return;
        }
    }
    delta.edits.push_back(edit);
}

void VoxelNetEngine::serverBroadcastEdit(const ChunkCoord& chunk, const VoxelEdit& edit) {
    VoxelChunkDataPacket pkt;
    pkt.cx = chunk.cx; pkt.cy = chunk.cy; pkt.cz = chunk.cz;
    pkt.edits.push_back(edit);

    // Send to all players who have this chunk loaded
    for (auto& pl : players) {
        if (pl.loaded_chunks.count(chunk)) {
            sendPacket(socket, pl.addr, pkt);
        }
    }
}

void VoxelNetEngine::serverUpdatePlayerView(PlayerInfo& player) {
    // Determine chunks within view distance
    ChunkCoord center = voxelToChunk((int32_t)player.position.x,
                                      (int32_t)player.position.y,
                                      (int32_t)player.position.z);
    std::set<ChunkCoord> needed;
    for (int dx = -VIEW_DISTANCE; dx <= VIEW_DISTANCE; dx++)
        for (int dz = -VIEW_DISTANCE; dz <= VIEW_DISTANCE; dz++) {
            // 2D horizontal streaming (y limited)
            for (int dy = -2; dy <= 2; dy++) {
                needed.insert(ChunkCoord{center.cx + dx, center.cy + dy, center.cz + dz});
            }
        }

    // Send newly visible chunks
    for (auto& c : needed) {
        if (!player.loaded_chunks.count(c)) {
            VoxelChunkVisibilityPacket vis;
            vis.cx = c.cx; vis.cy = c.cy; vis.cz = c.cz; vis.visible = true;
            sendPacket(socket, player.addr, vis);

            // Send chunk delta (only modified chunks have data)
            auto it = chunk_deltas.find(c);
            if (it != chunk_deltas.end() && !it->second.edits.empty()) {
                VoxelChunkDataPacket data;
                data.cx = c.cx; data.cy = c.cy; data.cz = c.cz;
                data.edits = it->second.edits;
                sendPacket(socket, player.addr, data);
            }
            player.loaded_chunks.insert(c);
        }
    }

    // Unload chunks out of view
    for (auto it = player.loaded_chunks.begin(); it != player.loaded_chunks.end(); ) {
        if (!needed.count(*it)) {
            VoxelChunkVisibilityPacket vis;
            vis.cx = it->cx; vis.cy = it->cy; vis.cz = it->cz; vis.visible = false;
            sendPacket(socket, player.addr, vis);
            it = player.loaded_chunks.erase(it);
        } else {
            ++it;
        }
    }
}

// ---- Client: receive packets -----------------------------------------------
void VoxelNetEngine::pollClient() {
    if (!is_client) return;
    uint8_t recv_buf[2048];
    NetAddress from;

    while (true) {
        int n = socket.recv_from(recv_buf, sizeof(recv_buf), from);
        if (n <= 0) break;
        try {
            ByteBuffer buf(recv_buf, n);
            int id = buf.read_varint();
            int payload_size = buf.read_varint();
            if (payload_size < 0 || payload_size > (int)buf.remaining()) continue;
            if (!Packet::canReceive(id, false)) continue;

            auto packet = Packet::create(id);
            if (!packet) continue;
            packet->read(buf);

            if (id == VOXEL_ACCEPT) {
                auto* p = (VoxelAcceptPacket*)packet.get();
                local_entity_id = p->player_entity_id;
                world_seed = p->world_seed;
                local_position = glm::vec3(p->spawn_x, p->spawn_y, p->spawn_z);
                std::cout << "[VoxelNet] Accepted: entity_id=" << local_entity_id
                          << " seed=" << world_seed << "\n";

            } else if (id == VOXEL_CHUNK_DATA) {
                auto* p = (VoxelChunkDataPacket*)packet.get();
                // Apply chunk edits to local world
                for (auto& e : p->edits) {
                    clientApplyEdit(e.voxel_key, e.block_type);
                    // Remove from pending if this confirms our prediction
                    for (auto it = pending_edits.begin(); it != pending_edits.end(); ) {
                        if (it->voxel_key == e.voxel_key) it = pending_edits.erase(it);
                        else ++it;
                    }
                }
            } else if (id == VOXEL_CHUNK_VISIBILITY) {
                auto* p = (VoxelChunkVisibilityPacket*)packet.get();
                (void)p;  // TODO: load/unload chunk mesh locally
            } else if (id == VOXEL_PLAYER_MOVE) {
                auto* p = (VoxelPlayerMovePacket*)packet.get();
                (void)p;  // TODO: interpolate other player position
            }
        } catch (...) {}
    }
}

void VoxelNetEngine::clientDigBlock(int32_t x, int32_t y, int32_t z) {
    int64_t key = VoxelEdit::pack(x, y, z);
    // 1. Predict locally (instant feedback)
    clientApplyEdit(key, 0);  // air
    pending_edits.push_back({key, 0, edit_seq++});

    // 2. Send to server
    VoxelPlayerActionPacket act;
    act.player_id = local_entity_id;
    act.action = VoxelPlayerActionPacket::FINISH_DIG;
    act.voxel_key = key;
    act.face = 0;
    sendPacket(socket, server_addr, act);
}

void VoxelNetEngine::clientPlaceBlock(int32_t x, int32_t y, int32_t z, uint8_t block_type) {
    int64_t key = VoxelEdit::pack(x, y, z);
    clientApplyEdit(key, block_type);
    pending_edits.push_back({key, block_type, edit_seq++});

    VoxelPlayerActionPacket act;
    act.player_id = local_entity_id;
    act.action = VoxelPlayerActionPacket::PLACE_BLOCK;
    act.voxel_key = key;
    act.face = block_type;  // carry block type in face field
    sendPacket(socket, server_addr, act);
}

void VoxelNetEngine::clientMove(float dx, float dy, float dz, float yaw) {
    local_position += glm::vec3(dx, dy, dz);
    auto pkt = VoxelPlayerMovePacket::fromDelta(local_entity_id, dx, dy, dz, yaw);
    sendPacket(socket, server_addr, pkt);
}

void VoxelNetEngine::clientApplyEdit(int64_t voxel_key, uint8_t block_type) {
    (void)voxel_key; (void)block_type;
    // TODO: hook into local voxel world / SDF terrain
    // localWorld.set(voxel_key, block_type);
    // rebuildMeshLocal(voxel_key);
}

void VoxelNetEngine::clientRejectEdit(int64_t voxel_key, uint8_t authoritative_type) {
    // Server rejected our prediction → restore authoritative value
    clientApplyEdit(voxel_key, authoritative_type);
}

} // namespace voxel
} // namespace net
