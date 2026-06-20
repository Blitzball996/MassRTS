#include "rts_net_engine.h"
#include <iostream>
#include <ctime>
#include <set>

namespace net {
namespace rts {

RtsNetEngine::RtsNetEngine() {
    registerRtsPackets();
}

RtsNetEngine::~RtsNetEngine() {
    disconnect();
}

bool RtsNetEngine::hostGame(uint16_t port, const std::string& player_name) {
    if (!UDPSocket::init_network()) return false;
    if (!socket.create()) return false;
    if (!socket.bind_port(port)) return false;

    is_host = true;
    is_client = true;   // host is also a player
    local_player_id = 0;
    local_faction = 0;
    player_count = 1;

    terrain_seed = (uint64_t)std::time(nullptr);

    server_addr.ip  = 0x7F000001;   // 127.0.0.1 loopback to self
    server_addr.port = port;

    std::cout << "[RtsNet] Hosting game on port " << port
              << ", seed=" << terrain_seed << "\n";
    return true;
}
bool RtsNetEngine::joinGame(const std::string& ip, uint16_t port,
                             const std::string& player_name) {
    if (!UDPSocket::init_network()) return false;
    if (!socket.create()) return false;

    uint32_t ip_u32 = 0;
    int a, b, c, d;
    if (sscanf(ip.c_str(), "%d.%d.%d.%d", &a, &b, &c, &d) == 4)
        ip_u32 = ((uint32_t)a << 24) | ((uint32_t)b << 16) |
                 ((uint32_t)c << 8)  | (uint32_t)d;
    else return false;

    server_addr.ip   = ip_u32;
    server_addr.port = port;

    RtsConnectPacket pkt;
    pkt.player_name = player_name;
    pkt.version = 1;
    ByteBuffer buf;
    buf.write_varint(pkt.getId());
    ByteBuffer payload; pkt.write(payload);
    buf.write_varint((int32_t)payload.size());
    buf.write_bytes(payload.data);
    socket.send_to(buf.ptr(), buf.size(), server_addr);

    is_client = true;
    std::cout << "[RtsNet] Connecting to " << ip << ":" << port << "\n";
    return true;
}

void RtsNetEngine::disconnect() {
    if (connection) connection->stop();
    connection.reset();
    socket.close_socket();
    is_host = false;
    is_client = false;
    UDPSocket::shutdown_network();
}
// ---------------------------------------------------------------------------
// poll() — receive packets, drive host relay
// ---------------------------------------------------------------------------
void RtsNetEngine::poll() {
    uint8_t recv_buf[4096];
    NetAddress from;

    while (true) {
        int n = socket.recv_from(recv_buf, sizeof(recv_buf), from);
        if (n <= 0) break;

        try {
            ByteBuffer buf(recv_buf, n);
            int id         = buf.read_varint();
            int payload_sz = buf.read_varint();
            if (payload_sz < 0 || payload_sz > (int)buf.remaining()) continue;

            auto packet = Packet::create(id);
            if (!packet) continue;
            packet->read(buf);

            if (id == RTS_CONNECT && is_host) {
                acceptClient(from, *(RtsConnectPacket*)packet.get());

            } else if (id == RTS_ACCEPT && is_client && !is_host) {
                auto* p = (RtsAcceptPacket*)packet.get();
                local_player_id = p->assigned_player_id;
                local_faction   = p->assigned_faction;
                terrain_seed    = p->terrain_seed;
                std::cout << "[RtsNet] Accepted: player_id=" << (int)local_player_id
                          << " faction=" << (int)local_faction
                          << " seed=" << terrain_seed << "\n";

            } else if (id == RTS_COMMAND && is_host) {
                // Client -> host: relay to everyone (incl. back to sender + host self)
                auto* p = (RtsCommandPacket*)packet.get();
                for (auto& cmd : p->commands)
                    relayCommandToAll(cmd);

            } else if (id == RTS_COMMAND_BATCH) {
                // Server -> client: record into local schedule
                auto* p = (RtsCommandBatchPacket*)packet.get();
                onCommandBatchReceived(*p);
            }
        } catch (...) {}
    }
}

void RtsNetEngine::hostUpdate() {}
void RtsNetEngine::acceptClient(NetAddress addr, const RtsConnectPacket& pkt) {
    // Reject duplicate (same addr already registered)
    for (auto& c : clients) {
        if (c.addr == addr) return;
    }
    uint8_t new_id = (uint8_t)clients.size() + 1;   // host is 0

    ClientInfo info;
    info.addr      = addr;
    info.player_id = new_id;
    info.name      = pkt.player_name;
    clients.push_back(std::move(info));
    player_count = (int)clients.size() + 1;          // +1 for host

    RtsAcceptPacket accept;
    accept.assigned_player_id = new_id;
    accept.assigned_faction   = new_id % 4;
    accept.start_tick         = 0;
    accept.terrain_seed       = terrain_seed;

    ByteBuffer buf;
    buf.write_varint(accept.getId());
    ByteBuffer payload; accept.write(payload);
    buf.write_varint((int32_t)payload.size());
    buf.write_bytes(payload.data);
    socket.send_to(buf.ptr(), buf.size(), addr);

    std::cout << "[RtsNet] Client accepted: " << pkt.player_name
              << " id=" << (int)new_id
              << " (player_count=" << player_count << ")\n";
}

void RtsNetEngine::queueLocalCommand(uint32_t current_tick, CmdType type,
                                      float target_x, float target_z,
                                      uint32_t unit_start, uint32_t unit_end,
                                      uint32_t param_a, uint32_t param_b) {
    RtsCommand cmd{};
    cmd.tick        = current_tick + INPUT_DELAY;   // execute in the future
    cmd.player_id   = local_player_id;
    cmd.faction     = local_faction;
    cmd.type        = type;
    cmd.target_x_fp = RtsCommand::toFixed(target_x);
    cmd.target_z_fp = RtsCommand::toFixed(target_z);
    cmd.param_a     = param_a;
    cmd.param_b     = param_b;
    cmd.unit_start  = unit_start;
    cmd.unit_end    = unit_end;
    submitCommand(cmd);
}

// Confirm "no command this tick" so other peers can advance. The argument is
// the FUTURE execution tick (caller already added INPUT_DELAY); do NOT add it
// again here.
void RtsNetEngine::confirmEmptyTick(uint32_t exec_tick) {
    RtsCommand cmd{};
    cmd.tick       = exec_tick;
    cmd.player_id  = local_player_id;
    cmd.faction    = local_faction;
    cmd.type       = CmdType::Stop;
    cmd.unit_start = EMPTY_MARKER;   // marker: empty confirm, not a real command
    submitCommand(cmd);
}
// Submit a local command/confirm. Host applies it directly (and broadcasts to
// clients); a pure client sends it to the host for relay.
void RtsNetEngine::submitCommand(const RtsCommand& cmd) {
    if (is_host) {
        relayCommandToAll(cmd);     // host is authoritative: record + broadcast
    } else {
        RtsCommandPacket pkt;
        pkt.commands.push_back(cmd);
        ByteBuffer buf;
        buf.write_varint(pkt.getId());
        ByteBuffer payload; pkt.write(payload);
        buf.write_varint((int32_t)payload.size());
        buf.write_bytes(payload.data);
        socket.send_to(buf.ptr(), buf.size(), server_addr);
    }
}

// Host-only: record a command locally and broadcast it to every client.
void RtsNetEngine::relayCommandToAll(const RtsCommand& cmd) {
    if (!is_host) return;

    // 1) record into host's own schedule
    recordCommand(cmd);

    // 2) broadcast to all clients
    RtsCommandBatchPacket batch;
    batch.tick = cmd.tick;
    batch.commands.push_back(cmd);

    ByteBuffer buf;
    buf.write_varint(batch.getId());
    ByteBuffer payload; batch.write(payload);
    buf.write_varint((int32_t)payload.size());
    buf.write_bytes(payload.data);
    for (auto& c : clients)
        socket.send_to(buf.ptr(), buf.size(), c.addr);
}

void RtsNetEngine::onCommandBatchReceived(const RtsCommandBatchPacket& pkt) {
    for (auto& cmd : pkt.commands)
        recordCommand(cmd);
}

// Record a command into the lockstep schedule. Confirms are tracked per-player
// so each player contributes at most one confirm per tick (no double counting).
void RtsNetEngine::recordCommand(const RtsCommand& cmd) {
    confirmed_players[cmd.tick].insert(cmd.player_id);
    if (cmd.unit_start != EMPTY_MARKER)
        schedule[cmd.tick].push_back(cmd);   // real command (empty confirms skipped)
}

bool RtsNetEngine::canAdvanceTick(uint32_t tick) const {
    if (!isActive()) return true;            // single player
    auto it = confirmed_players.find(tick);
    int needed = std::max(1, player_count);
    return it != confirmed_players.end() && (int)it->second.size() >= needed;
}

std::vector<RtsCommand> RtsNetEngine::getCommandsForTick(uint32_t tick) {
    std::vector<RtsCommand> out;
    auto it = schedule.find(tick);
    if (it != schedule.end()) out = it->second;
    return out;
}

void RtsNetEngine::advanceTick(uint32_t tick) {
    schedule.erase(tick);
    confirmed_players.erase(tick);
}

void RtsNetEngine::reportChecksum(uint32_t tick, uint32_t checksum) {
    RtsSyncCheckPacket pkt;
    pkt.tick = tick;
    pkt.checksum = checksum;
    ByteBuffer buf;
    buf.write_varint(pkt.getId());
    ByteBuffer payload; pkt.write(payload);
    buf.write_varint((int32_t)payload.size());
    buf.write_bytes(payload.data);
    socket.send_to(buf.ptr(), buf.size(), server_addr);
}

} // namespace rts
} // namespace net
