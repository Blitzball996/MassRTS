#pragma once
// =============================================================================
// NetSession - Lockstep multiplayer session for MassRTS
// -----------------------------------------------------------------------------
// Wraps GameServer/GameClient and implements deterministic lockstep:
//   * Player inputs are NOT applied immediately. They are queued for a future
//     tick (current_tick + INPUT_DELAY) and only executed when ALL players'
//     inputs for that tick have arrived. This keeps every client's simulation
//     bit-identical.
//   * Any input that changes world state (move, attack-move, stop, buy, nuke)
//     MUST be routed through queue_command(). Local-only changes WILL desync.
//   * A periodic state checksum is reported; the host can broadcast a snapshot
//     to correct drift (e.g. cross-GPU float divergence). [hook provided]
//
// Usage (host or client):
//   NetSession net;
//   net.host(NET_PORT);                 // OR net.join("192.168.1.5", NET_PORT, "Name");
//   ...
//   // game loop, fixed timestep:
//   net.poll();                         // pump packets
//   net.queue_local_command(cmd);       // on right-click etc.
//   while (net.tick_ready(sim_tick)) {  // only advance when inputs ready
//       for (auto& c : net.commands_for(sim_tick)) apply_command(world, c);
//       step_simulation(FIXED_DT);
//       net.advance(sim_tick);
//       sim_tick++;
//   }
//
// NOTE: This is the transport-agnostic lockstep layer. The actual command
// application (apply_command) lives in game code, because only the game knows
// how to mutate the World. See NetCommand below for the wire format.
// =============================================================================

#include "socket.h"
#include "protocol.h"
#include "server.h"
#include "client.h"
#include <map>
#include <vector>
#include <cstdint>
#include <cstring>

// How many ticks ahead inputs are scheduled. 3 ticks @ 30Hz ~= 100ms of
// hidden latency that masks network jitter. Tune per target connection.
constexpr uint32_t INPUT_DELAY = 3;
constexpr int FIXED_HZ = 30;
constexpr float FIXED_DT = 1.0f / (float)FIXED_HZ;

// Extended command set. ANY world-mutating player action must have a code here.
enum class CmdKind : uint8_t {
    Move = 0,
    AttackMove = 1,
    Stop = 2,
    Buy = 3,        // param_a = shop index, param_b = count
    Nuke = 4,       // target = impact point
    Rally = 5,
};

#pragma pack(push, 1)
struct NetCommand {
    uint32_t tick;       // tick on which this command executes
    uint8_t  player_id;
    uint8_t  faction;
    CmdKind  kind;
    uint8_t  _pad;
    glm::vec2 target;    // world position (move/attack/nuke/rally)
    uint32_t param_a;    // shop index / etc.
    uint32_t param_b;    // count / etc.
    // For unit selection: a hash/seed the receiver can reproduce, OR an explicit
    // id range. Lockstep requires the SAME units be affected on every client, so
    // selection must be derivable deterministically (e.g. "all selected of my
    // faction"). Here we carry an explicit contiguous range as a first cut.
    uint32_t unit_start;
    uint32_t unit_end;
};

struct CmdBatchPacket {
    PacketHeader header;   // type = CommandBroadcast, tick = batch tick
    uint16_t     count;    // number of NetCommand following
    // NetCommand[count] payload follows
};

struct ChecksumPacket {
    PacketHeader header;   // type = TickSync (reused), tick = checksum tick
    uint32_t     checksum;
};
#pragma pack(pop)

class NetSession {
public:
    enum class Role { None, Host, Client } role = Role::None;

    GameServer server;   // valid when Host
    GameClient client;   // always used for send/recv of our own commands

    uint8_t  local_player = 0;
    uint8_t  local_faction = 0;
    int      player_count = 1;

    // tick -> commands scheduled for that tick (from ALL players)
    std::map<uint32_t, std::vector<NetCommand>> schedule;
    // tick -> how many players we have confirmed input for
    std::map<uint32_t, int> confirms;

    bool active() const { return role != Role::None; }

    // ---- Setup -------------------------------------------------------------
    bool host(uint16_t port = NET_PORT, const char* name = "Host") {
        UDPSocket::init_network();
        if (!server.start(port)) return false;
        // Host is also a player: loopback client to itself.
        client.connect("127.0.0.1", port, name);
        role = Role::Host;
        local_player = 0;
        local_faction = 0;
        return true;
    }

    bool join(const std::string& ip, uint16_t port, const char* name = "Player") {
        UDPSocket::init_network();
        if (!client.connect(ip, port, name)) return false;
        role = Role::Client;
        return true;
    }

    // ---- Per-frame pump ----------------------------------------------------
    void poll() {
        if (role == Role::Host) server.update();   // relay incoming commands
        client.update();
        if (client.connected) {
            local_player  = client.my_player_id;
            local_faction = client.my_faction;
        }
        drain_incoming();
    }

    // ---- Issue a local command (call from input handlers) ------------------
    // The command is scheduled for current_tick + INPUT_DELAY and sent to the
    // server, which relays it to everyone (including us, for confirmation).
    void queue_local_command(uint32_t current_tick, CmdKind kind, glm::vec2 target,
                             uint32_t unit_start = 0, uint32_t unit_end = 0,
                             uint32_t a = 0, uint32_t b = 0) {
        NetCommand c{};
        c.tick      = current_tick + INPUT_DELAY;
        c.player_id = local_player;
        c.faction   = local_faction;
        c.kind      = kind;
        c.target    = target;
        c.param_a   = a;
        c.param_b   = b;
        c.unit_start= unit_start;
        c.unit_end  = unit_end;
        send_command(c);
    }

    // Even when a player has no input on a tick, they must confirm an empty tick
    // so others can advance. Call this every fixed step.
    void confirm_empty_tick(uint32_t current_tick) {
        NetCommand c{};
        c.tick = current_tick + INPUT_DELAY;
        c.player_id = local_player;
        c.faction = local_faction;
        c.kind = CmdKind::Stop;        // sentinel "no-op" not applied; see commands_for
        c.unit_start = 0xFFFFFFFFu;    // marker = empty confirm
        send_command(c);
    }

    // ---- Lockstep barrier --------------------------------------------------
    // A tick is ready when every connected player has sent at least one packet
    // (command or empty-confirm) for it.
    bool tick_ready(uint32_t tick) const {
        if (!active()) return true;            // single player: always ready
        auto it = confirms.find(tick);
        int needed = std::max(1, player_count);
        return it != confirms.end() && it->second >= needed;
    }

    std::vector<NetCommand> commands_for(uint32_t tick) {
        std::vector<NetCommand> out;
        auto it = schedule.find(tick);
        if (it != schedule.end()) {
            for (auto& c : it->second)
                if (c.unit_start != 0xFFFFFFFFu) out.push_back(c); // skip empty markers
        }
        return out;
    }

    void advance(uint32_t tick) {
        schedule.erase(tick);
        confirms.erase(tick);
    }

    // ---- Desync detection (hook) -------------------------------------------
    // Caller computes a checksum over the deterministic world state and passes
    // it here every N ticks. Mismatch handling (host snapshot) is a TODO hook.
    void report_checksum(uint32_t tick, uint32_t checksum) {
        ChecksumPacket pkt{};
        pkt.header.magic = NET_MAGIC;
        pkt.header.type  = PacketType::TickSync;
        pkt.header.player_id = local_player;
        pkt.header.tick  = tick;
        pkt.checksum     = checksum;
        client.socket.send_to(&pkt, sizeof(pkt), client.server_addr);
        // TODO(host): collect checksums, on mismatch broadcast a full snapshot
        // using PacketType::GameState to force-correct drifting clients.
    }

    void shutdown() {
        if (role == Role::Client || role == Role::Host) client.disconnect();
        if (role == Role::Host) server.stop();
        role = Role::None;
        UDPSocket::shutdown_network();
    }

private:
    void send_command(const NetCommand& c) {
        // Wrap in a CommandPacket-compatible envelope. We piggyback on the
        // existing Command/CommandBroadcast path by sending the raw NetCommand
        // behind a PacketHeader.
        uint8_t buf[sizeof(PacketHeader) + sizeof(NetCommand)];
        PacketHeader* h = (PacketHeader*)buf;
        h->magic = NET_MAGIC;
        h->type  = PacketType::Command;
        h->player_id = local_player;
        h->tick  = c.tick;
        h->payload_size = sizeof(NetCommand);
        memcpy(buf + sizeof(PacketHeader), &c, sizeof(NetCommand));
        client.socket.send_to(buf, sizeof(buf), client.server_addr);
    }

    // Pull NetCommands the client received (relayed by server) into the schedule.
    // We intercept here instead of using GameClient::on_command because our wire
    // format is NetCommand, not the legacy CommandPacket.
    void drain_incoming() {
        uint8_t buf[MAX_PACKET_SIZE];
        NetAddress from;
        while (true) {
            int n = client.socket.recv_from(buf, MAX_PACKET_SIZE, from);
            if (n <= 0) break;
            if (n < (int)sizeof(PacketHeader)) continue;
            PacketHeader* h = (PacketHeader*)buf;
            if (h->magic != NET_MAGIC) continue;
            if (h->type == PacketType::CommandBroadcast &&
                n >= (int)(sizeof(PacketHeader) + sizeof(NetCommand))) {
                NetCommand c;
                memcpy(&c, buf + sizeof(PacketHeader), sizeof(NetCommand));
                schedule[c.tick].push_back(c);
                confirms[c.tick]++;
            } else if (h->type == PacketType::AcceptConnect) {
                AcceptPacket* a = (AcceptPacket*)buf;
                local_player = a->assigned_id;
                local_faction = a->faction;
                client.connected = true;
            } else if (h->type == PacketType::PlayerJoined) {
                player_count++;
            } else if (h->type == PacketType::PlayerLeft) {
                if (player_count > 1) player_count--;
            }
        }
    }
};
