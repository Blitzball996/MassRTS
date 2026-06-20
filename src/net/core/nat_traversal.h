#pragma once
// =============================================================================
// NAT Traversal - simple STUN-like NAT穿透
// -----------------------------------------------------------------------------
// Problem: Players behind NAT can't directly connect (ports blocked)
// Solution: STUN server helps discover public IP:port, then P2P hole punching
//
// Full STUN is complex. This is a simplified version for game servers:
//   1. Client sends "ping" to STUN server
//   2. Server replies with client's public IP:port
//   3. Both clients send packets to each other's public address
//   4. NAT routers see outgoing packets → allow incoming from same peer
//
// Limitations:
//   * Symmetric NAT: may fail (need TURN relay)
//   * Cone NAT: usually works
//   * No NAT: always works
//
// Usage:
//   // Discover public address
//   NetAddress public_addr = NATTraversal::discover("stun.server.com", 3478);
//   // Exchange addresses with peer (via game server)
//   // Both send UDP packets to peer's public address
//   // NAT hole opened!
// =============================================================================
#include "../socket.h"
#include <string>
#include <chrono>

namespace net {

class NATTraversal {
public:
    // ---- STUN discovery (simplified) ---------------------------------------
    // Send ping to STUN server, receive our public IP:port
    static NetAddress discover(const std::string& stun_server, uint16_t stun_port) {
        UDPSocket sock;
        if (!sock.create()) return NetAddress{0, 0};
        sock.set_nonblocking();

        // Parse STUN server IP
        NetAddress server_addr;
        int a, b, c, d;
        if (sscanf(stun_server.c_str(), "%d.%d.%d.%d", &a, &b, &c, &d) != 4) {
            return NetAddress{0, 0};
        }
        server_addr.ip = ((uint32_t)a << 24) | ((uint32_t)b << 16) | 
                         ((uint32_t)c << 8) | (uint32_t)d;
        server_addr.port = stun_port;

        // Send STUN binding request (simplified format)
        uint8_t request[8] = {0x00, 0x01, 0, 0, 0, 0, 0, 0};  // type=0x0001 (binding)
        sock.send_to(request, sizeof(request), server_addr);

        // Wait for response (timeout 3 sec)
        uint8_t response[128];
        NetAddress from;
        auto start = std::chrono::steady_clock::now();
        while (true) {
            int n = sock.recv_from(response, sizeof(response), from);
            if (n > 0) {
                // Parse response (simplified: just echo back our address)
                // Real STUN uses XOR-MAPPED-ADDRESS attribute
                if (n >= 8) {
                    NetAddress public_addr;
                    // Format: [type:2][len:2][ip:4]
                    if (response[0] == 0x01 && response[1] == 0x01) {
                        public_addr.ip = ((uint32_t)response[4] << 24) |
                                         ((uint32_t)response[5] << 16) |
                                         ((uint32_t)response[6] << 8) |
                                         ((uint32_t)response[7]);
                        public_addr.port = ((uint16_t)response[2] << 8) | response[3];
                        return public_addr;
                    }
                }
            }

            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - start).count() > 3) {
                break;  // timeout
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        return NetAddress{0, 0};  // failed
    }

    // ---- Hole punching helper ----------------------------------------------
    // Both clients call this with peer's public address
    // Sends several packets to "punch" through NAT
    static void punchHole(UDPSocket& sock, NetAddress peer_public) {
        uint8_t punch[4] = {0x50, 0x55, 0x4E, 0x43};  // "PUNC"
        for (int i = 0; i < 10; i++) {
            sock.send_to(punch, sizeof(punch), peer_public);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    // ---- Simple STUN server (for testing) ----------------------------------
    // Run this on a server with public IP
    static void runSTUNServer(uint16_t port = 3478) {
        UDPSocket sock;
        if (!sock.create()) return;
        if (!sock.bind_port(port)) return;
        sock.set_nonblocking();

        std::cout << "[STUN Server] Running on port " << port << "\n";

        uint8_t buf[128];
        NetAddress from;
        while (true) {
            int n = sock.recv_from(buf, sizeof(buf), from);
            if (n > 0 && buf[0] == 0x00 && buf[1] == 0x01) {
                // Binding request → reply with client's public address
                uint8_t response[8];
                response[0] = 0x01; response[1] = 0x01;  // binding response
                response[2] = (from.port >> 8) & 0xFF;
                response[3] = from.port & 0xFF;
                response[4] = (from.ip >> 24) & 0xFF;
                response[5] = (from.ip >> 16) & 0xFF;
                response[6] = (from.ip >> 8) & 0xFF;
                response[7] = from.ip & 0xFF;
                sock.send_to(response, sizeof(response), from);

                std::cout << "[STUN] Client " << ipToString(from) << " discovered\n";
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

private:
    static std::string ipToString(NetAddress addr) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d.%d.%d.%d:%d",
                 (addr.ip >> 24) & 0xFF, (addr.ip >> 16) & 0xFF,
                 (addr.ip >> 8) & 0xFF, addr.ip & 0xFF, addr.port);
        return buf;
    }
};

} // namespace net
