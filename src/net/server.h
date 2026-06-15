#pragma once
#include "socket.h"
#include "protocol.h"
#include <vector>
#include <functional>
#include <chrono>

struct PlayerSlot {
    NetAddress address;
    std::string name;
    uint8_t faction;
    bool connected = false;
    double last_heartbeat = 0;
};

// Dedicated server for lockstep multiplayer
class GameServer {
public:
    UDPSocket socket;
    PlayerSlot players[MAX_PLAYERS];
    uint32_t current_tick = 0;
    int player_count = 0;
    bool running = false;

    bool start(uint16_t port = NET_PORT) {
        if (!socket.create()) return false;
        if (!socket.bind_port(port)) { socket.close_socket(); return false; }
        running = true;
        std::cout << "[Server] Started on port " << port << "\n";
        return true;
    }

    void update() {
        if (!running) return;

        // Receive packets
        uint8_t buf[MAX_PACKET_SIZE];
        NetAddress from;
        while (true) {
            int n = socket.recv_from(buf, MAX_PACKET_SIZE, from);
            if (n <= 0) break;
            if (n < (int)sizeof(PacketHeader)) continue;

            PacketHeader* hdr = (PacketHeader*)buf;
            if (hdr->magic != NET_MAGIC) continue;

            switch (hdr->type) {
                case PacketType::Connect: handle_connect(buf, n, from); break;
                case PacketType::Disconnect: handle_disconnect(hdr->player_id); break;
                case PacketType::Command: broadcast_command(buf, n, hdr->player_id); break;
                case PacketType::Heartbeat: update_heartbeat(hdr->player_id); break;
                default: break;
            }
        }
    }

    void stop() {
        running = false;
        socket.close_socket();
    }

private:
    void handle_connect(uint8_t* data, int size, NetAddress from) {
        if (player_count >= MAX_PLAYERS) {
            send_reject(from);
            return;
        }
        ConnectPacket* pkt = (ConnectPacket*)data;

        int slot = player_count++;
        players[slot].address = from;
        players[slot].name = std::string(pkt->player_name, strnlen(pkt->player_name, 31));
        players[slot].faction = (uint8_t)slot;
        players[slot].connected = true;
        players[slot].last_heartbeat = get_time();

        // Send accept
        AcceptPacket accept{};
        accept.header.magic = NET_MAGIC;
        accept.header.type = PacketType::AcceptConnect;
        accept.header.player_id = (uint8_t)slot;
        accept.header.tick = current_tick;
        accept.assigned_id = (uint8_t)slot;
        accept.faction = players[slot].faction;
        accept.start_tick = current_tick;
        socket.send_to(&accept, sizeof(accept), from);

        std::cout << "[Server] Player " << players[slot].name << " joined (slot " << slot << ")\n";

        // Notify others
        PacketHeader notify{};
        notify.magic = NET_MAGIC;
        notify.type = PacketType::PlayerJoined;
        notify.player_id = (uint8_t)slot;
        notify.tick = current_tick;
        broadcast(&notify, sizeof(notify), slot);
    }

    void handle_disconnect(uint8_t id) {
        if (id >= MAX_PLAYERS || !players[id].connected) return;
        players[id].connected = false;
        std::cout << "[Server] Player " << players[id].name << " disconnected\n";

        PacketHeader notify{};
        notify.magic = NET_MAGIC;
        notify.type = PacketType::PlayerLeft;
        notify.player_id = id;
        broadcast(&notify, sizeof(notify), id);
    }

    void broadcast_command(uint8_t* data, int size, uint8_t sender_id) {
        // Relay command to all clients (including sender for confirmation)
        PacketHeader* hdr = (PacketHeader*)data;
        hdr->type = PacketType::CommandBroadcast;
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (players[i].connected) {
                socket.send_to(data, size, players[i].address);
            }
        }
    }

    void broadcast(void* data, int size, int exclude = -1) {
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (i == exclude || !players[i].connected) continue;
            socket.send_to(data, size, players[i].address);
        }
    }

    void send_reject(NetAddress to) {
        PacketHeader pkt{};
        pkt.magic = NET_MAGIC;
        pkt.type = PacketType::RejectConnect;
        socket.send_to(&pkt, sizeof(pkt), to);
    }

    void update_heartbeat(uint8_t id) {
        if (id < MAX_PLAYERS) players[id].last_heartbeat = get_time();
    }

    double get_time() {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    }
};
