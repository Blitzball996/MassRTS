#pragma once
// =============================================================================
// TCP Socket - reliable connection for large transfers
// -----------------------------------------------------------------------------
// Use cases:
//   * Initial world sync (large snapshot on join)
//   * File transfers (mods, resource packs)
//   * Reliable command channel (alternative to UDP)
//
// UDP vs TCP:
//   UDP: low latency, packet loss OK (real-time gameplay)
//   TCP: reliable, ordered, higher latency (bulk data)
//
// Hybrid approach (like Minecraft):
//   * UDP for gameplay (player moves, edits, commands)
//   * TCP for initial handshake + large transfers
// =============================================================================
#include <cstdint>
#include <string>

#ifdef _WIN32
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET SocketHandle;
#define INVALID_SOCK INVALID_SOCKET
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
typedef int SocketHandle;
#define INVALID_SOCK -1
#define closesocket close
#endif

namespace net {

class TCPSocket {
public:
    TCPSocket() : sock(INVALID_SOCK) {}
    ~TCPSocket() { close_socket(); }

    // Non-copyable
    TCPSocket(const TCPSocket&) = delete;
    TCPSocket& operator=(const TCPSocket&) = delete;

    // ---- Creation ----------------------------------------------------------
    bool create() {
        sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock == INVALID_SOCK) return false;

        // Disable Nagle's algorithm (reduce latency for small packets)
        int flag = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(flag));

        return true;
    }

    // ---- Server: bind + listen ---------------------------------------------
    bool bind_port(uint16_t port) {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = INADDR_ANY;
        return bind(sock, (sockaddr*)&addr, sizeof(addr)) == 0;
    }

    bool listen(int backlog = 8) {
        return ::listen(sock, backlog) == 0;
    }

    TCPSocket* accept() {
        sockaddr_in client_addr{};
        socklen_t len = sizeof(client_addr);
        SocketHandle client_sock = ::accept(sock, (sockaddr*)&client_addr, &len);
        if (client_sock == INVALID_SOCK) return nullptr;

        TCPSocket* client = new TCPSocket();
        client->sock = client_sock;
        return client;
    }

    // ---- Client: connect ---------------------------------------------------
    bool connect(const std::string& ip, uint16_t port) {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);

        // Parse IP
        int a, b, c, d;
        if (sscanf(ip.c_str(), "%d.%d.%d.%d", &a, &b, &c, &d) != 4)
            return false;
        addr.sin_addr.s_addr = htonl(((uint32_t)a << 24) | ((uint32_t)b << 16) |
                                      ((uint32_t)c << 8) | (uint32_t)d);

        return ::connect(sock, (sockaddr*)&addr, sizeof(addr)) == 0;
    }

    // ---- Send/Recv ---------------------------------------------------------
    int send(const void* data, int size) {
        return ::send(sock, (const char*)data, size, 0);
    }

    int recv(void* buf, int buf_size) {
        return ::recv(sock, (char*)buf, buf_size, 0);
    }

    // Send all (blocks until all sent or error)
    bool send_all(const void* data, int size) {
        int sent = 0;
        while (sent < size) {
            int n = send((const char*)data + sent, size - sent);
            if (n <= 0) return false;
            sent += n;
        }
        return true;
    }

    // Recv all (blocks until all received or error)
    bool recv_all(void* buf, int size) {
        int received = 0;
        while (received < size) {
            int n = recv((char*)buf + received, size - received);
            if (n <= 0) return false;
            received += n;
        }
        return true;
    }

    // ---- Non-blocking mode -------------------------------------------------
    void set_nonblocking() {
#ifdef _WIN32
        u_long mode = 1;
        ioctlsocket(sock, FIONBIO, &mode);
#else
        int flags = fcntl(sock, F_GETFL, 0);
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);
#endif
    }

    // ---- Close -------------------------------------------------------------
    void close_socket() {
        if (sock != INVALID_SOCK) {
            closesocket(sock);
            sock = INVALID_SOCK;
        }
    }

    bool is_valid() const { return sock != INVALID_SOCK; }

private:
    SocketHandle sock;
};

} // namespace net
