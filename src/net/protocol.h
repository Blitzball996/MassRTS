#pragma once
// Network protocol definitions for MassRTS multiplayer
#include <cstdint>
#include <glm/glm.hpp>

constexpr uint16_t NET_PORT = 27015;
constexpr uint32_t NET_MAGIC = 0x4D525453; // "MRTS"
constexpr int MAX_PLAYERS = 4;
constexpr int NET_TICK_RATE = 20; // 20 ticks/sec for commands
constexpr int MAX_PACKET_SIZE = 1400;

enum class PacketType : uint8_t {
    // Client -> Server
    Connect = 1,
    Disconnect = 2,
    Command = 3,      // Player issued a move/attack command
    Heartbeat = 4,

    // Server -> Client
    AcceptConnect = 10,
    RejectConnect = 11,
    GameState = 12,   // Full state sync (on join)
    CommandBroadcast = 13, // Relay command to all clients
    PlayerJoined = 14,
    PlayerLeft = 15,
    TickSync = 16,    // Lockstep tick confirmation
};

#pragma pack(push, 1)
struct PacketHeader {
    uint32_t magic;
    PacketType type;
    uint8_t player_id;
    uint32_t tick;
    uint16_t payload_size;
};

struct ConnectPacket {
    PacketHeader header;
    char player_name[32];
};

struct AcceptPacket {
    PacketHeader header;
    uint8_t assigned_id;
    uint8_t faction; // assigned faction
    uint32_t start_tick;
};

// Command from player: move selected units to target
struct MoveCommand {
    glm::vec2 target;
    uint32_t unit_start; // range of unit IDs
    uint32_t unit_end;
};

struct CommandPacket {
    PacketHeader header;
    uint8_t command_type; // 0=move, 1=attack-move, 2=stop
    MoveCommand move;
};

struct TickSyncPacket {
    PacketHeader header;
    uint32_t confirmed_tick;
    uint8_t player_count;
};
#pragma pack(pop)
