#pragma once
#include "socket.h"
#include "protocol.h"
#include <functional>
#include <string>
#include <iostream>

// Client for connecting to game server
class GameClient {
public:
    UDPSocket socket;
    NetAddress server_addr;
    uint8_t my_player_id = 255;
    uint8_t my_faction = 0;
    bool connected = false;
    uint32_t current_tick = 0;

    // Callbacks
    std::function<void(const CommandPacket&)> on_command;
    std::function<void(uint8_t)> on_player_joined;
    std::function<void(uint8_t)> on_player_left;

    bool connect(const std::string& server_ip, uint16_t port, const std::string& player_name) {
        if (!socket.create()) return false;
        // Bind to any port
        socket.bind_port(0);

        server_addr.ip = ip_to_uint(server_ip);
        server_addr.port = port;

        // Send connect packet
        ConnectPacket pkt{};
        pkt.header.magic = NET_MAGIC;
        pkt.header.type = PacketType::Connect;
        pkt.header.player_id = 0;
        pkt.header.tick = 0;
        pkt.header.payload_size = sizeof(pkt.player_name);
        strncpy(pkt.player_name, player_name.c_str(), 31);

        socket.send_to(&pkt, sizeof(pkt), server_addr);
        std::cout << "[Client] Connecting to " << server_ip << ":" << port << "...\n";
        return true;
    }

    void update() {
        uint8_t buf[MAX_PACKET_SIZE];
        NetAddress from;
        while (true) {
            int n = socket.recv_from(buf, MAX_PACKET_SIZE, from);
            if (n <= 0) break;
            if (n < (int)sizeof(PacketHeader)) continue;

            PacketHeader* hdr = (PacketHeader*)buf;
            if (hdr->magic != NET_MAGIC) continue;

            switch (hdr->type) {
                case PacketType::AcceptConnect: {
                    AcceptPacket* accept = (AcceptPacket*)buf;
                    my_player_id = accept->assigned_id;
                    my_faction = accept->faction;
                    current_tick = accept->start_tick;
                    connected = true;
                    std::cout << "[Client] Connected! ID=" << (int)my_player_id
                              << " Faction=" << (int)my_faction << "\n";
                    break;
                }
                case PacketType::RejectConnect:
                    std::cout << "[Client] Connection rejected (server full)\n";
                    break;
                case PacketType::CommandBroadcast: {
                    if (on_command && n >= (int)sizeof(CommandPacket)) {
                        on_command(*(CommandPacket*)buf);
                    }
                    break;
                }
                case PacketType::PlayerJoined:
                    if (on_player_joined) on_player_joined(hdr->player_id);
                    break;
                case PacketType::PlayerLeft:
                    if (on_player_left) on_player_left(hdr->player_id);
                    break;
                default: break;
            }
        }
    }

    void send_move_command(glm::vec2 target, uint32_t unit_start, uint32_t unit_end) {
        if (!connected) return;
        CommandPacket pkt{};
        pkt.header.magic = NET_MAGIC;
        pkt.header.type = PacketType::Command;
        pkt.header.player_id = my_player_id;
        pkt.header.tick = current_tick;
        pkt.command_type = 0; // move
        pkt.move.target = target;
        pkt.move.unit_start = unit_start;
        pkt.move.unit_end = unit_end;
        socket.send_to(&pkt, sizeof(pkt), server_addr);
    }

    void send_heartbeat() {
        if (!connected) return;
        PacketHeader pkt{};
        pkt.magic = NET_MAGIC;
        pkt.type = PacketType::Heartbeat;
        pkt.player_id = my_player_id;
        pkt.tick = current_tick;
        socket.send_to(&pkt, sizeof(pkt), server_addr);
    }

    void disconnect() {
        if (!connected) return;
        PacketHeader pkt{};
        pkt.magic = NET_MAGIC;
        pkt.type = PacketType::Disconnect;
        pkt.player_id = my_player_id;
        socket.send_to(&pkt, sizeof(pkt), server_addr);
        connected = false;
        socket.close_socket();
    }

private:
    static uint32_t ip_to_uint(const std::string& ip) {
        uint32_t parts[4];
        sscanf(ip.c_str(), "%u.%u.%u.%u", &parts[0], &parts[1], &parts[2], &parts[3]);
        return (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    }
};
