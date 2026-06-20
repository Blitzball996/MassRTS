#pragma once
// =============================================================================
// RtsNetEngine - lockstep deterministic multiplayer for RTS mode
// -----------------------------------------------------------------------------
// Implements the lockstep barrier described in NETWORK_ARCHITECTURE.md:
//   * Local commands queued for tick T = current_tick + INPUT_DELAY
//   * Commands only execute when ALL players' inputs for tick T arrived
//   * Periodic sync checksums detect desync
//   * Rollback support (future: integrate ROLLBACK_NETCODE.md)
//
// Usage:
//   RtsNetEngine net;
//   net.hostGame(port);  // or net.joinGame(ip, port, "PlayerName")
//   
//   // game loop (fixed timestep):
//   net.poll();
//   net.queueLocalCommand(...);  // on player input
//   if (net.canAdvanceTick(sim_tick)) {
//       auto cmds = net.getCommandsForTick(sim_tick);
//       for (auto& c : cmds) applyCommand(game, c);  // game applies commands
//       stepSimulation(game);
//       net.advanceTick(sim_tick);
//       sim_tick++;
//   }
// =============================================================================
#include "rts_packets.h"
#include "../core/connection.h"
#include <map>
#include <set>
#include <vector>
#include <memory>

namespace net {
namespace rts {

class RtsNetEngine {
public:
    static constexpr int INPUT_DELAY = 3;  // ticks ahead (hides ~100ms latency @ 30Hz)

    RtsNetEngine();
    ~RtsNetEngine();

    // ---- Setup -------------------------------------------------------------
    bool hostGame(uint16_t port, const std::string& player_name = "Host");
    bool joinGame(const std::string& ip, uint16_t port, const std::string& player_name);
    void disconnect();
    bool isActive() const { return is_host || is_client; }

    // ---- Per-frame pump ----------------------------------------------------
    void poll();  // receive packets, update schedule

    // ---- Command submission ------------------------------------------------
    void queueLocalCommand(uint32_t current_tick, CmdType type, 
                           float target_x, float target_z,
                           uint32_t unit_start = 0, uint32_t unit_end = 0,
                           uint32_t param_a = 0, uint32_t param_b = 0);

    // Send empty "confirm" if no command this tick (keep lockstep alive)
    void confirmEmptyTick(uint32_t current_tick);

    // ---- Lockstep barrier --------------------------------------------------
    bool canAdvanceTick(uint32_t tick) const;
    std::vector<RtsCommand> getCommandsForTick(uint32_t tick);
    void advanceTick(uint32_t tick);  // erase tick from schedule

    // ---- Sync check (call every N ticks) -----------------------------------
    void reportChecksum(uint32_t tick, uint32_t checksum);

    // ---- State -------------------------------------------------------------
    uint8_t  localPlayerId() const { return local_player_id; }
    uint8_t  localFaction() const { return local_faction; }
    uint64_t terrainSeed() const { return terrain_seed; }
    int      playerCount() const { return player_count; }

private:
    bool is_host = false;
    bool is_client = false;

    UDPSocket socket;
    NetAddress server_addr;
    std::unique_ptr<Connection> connection;  // to server (or loopback if host)

    uint8_t  local_player_id = 0;
    uint8_t  local_faction = 0;
    uint64_t terrain_seed = 0;
    int      player_count = 1;

    // Lockstep schedule: tick -> real commands from all players
    std::map<uint32_t, std::vector<RtsCommand>> schedule;
    // Per-tick set of player ids that have confirmed (command or empty).
    // Using a set guarantees each player counts at most once per tick.
    std::map<uint32_t, std::set<uint8_t>> confirmed_players;

    static constexpr uint32_t EMPTY_MARKER = 0xFFFFFFFFu;

    // Host state (if is_host)
    struct ClientInfo {
        NetAddress addr;
        std::unique_ptr<Connection> conn;
        uint8_t player_id;
        std::string name;
    };
    std::vector<ClientInfo> clients;  // host tracks connected clients

    void submitCommand(const RtsCommand& cmd);
    void relayCommandToAll(const RtsCommand& cmd);
    void recordCommand(const RtsCommand& cmd);
    void onCommandBatchReceived(const RtsCommandBatchPacket& pkt);

    void hostUpdate();
    void acceptClient(NetAddress addr, const RtsConnectPacket& pkt);
};

} // namespace rts
} // namespace net
