#pragma once
// Cross-platform UDP socket wrapper
#include <cstdint>
#include <cstring>
#include <string>
#include <iostream>

#ifdef _WIN32
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET SocketHandle;
#define INVALID_SOCK INVALID_SOCKET
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
typedef int SocketHandle;
#define INVALID_SOCK (-1)
#endif

struct NetAddress {
    uint32_t ip;
    uint16_t port;
    bool operator==(const NetAddress& o) const { return ip == o.ip && port == o.port; }
};

class UDPSocket {
public:
    SocketHandle sock = INVALID_SOCK;

    static bool init_network() {
#ifdef _WIN32
        WSADATA wsa;
        return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
        return true;
#endif
    }

    static void shutdown_network() {
#ifdef _WIN32
        WSACleanup();
#endif
    }

    bool create() {
        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock == INVALID_SOCK) return false;
        set_nonblocking();
        return true;
    }

    bool bind_port(uint16_t port) {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = INADDR_ANY;
        return ::bind(sock, (sockaddr*)&addr, sizeof(addr)) == 0;
    }

    int send_to(const void* data, int size, NetAddress dest) {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(dest.port);
        addr.sin_addr.s_addr = htonl(dest.ip);
        return sendto(sock, (const char*)data, size, 0, (sockaddr*)&addr, sizeof(addr));
    }

    int recv_from(void* buf, int buf_size, NetAddress& from) {
        sockaddr_in addr{};
        int addr_len = sizeof(addr);
#ifdef _WIN32
        int n = recvfrom(sock, (char*)buf, buf_size, 0, (sockaddr*)&addr, &addr_len);
#else
        socklen_t slen = sizeof(addr);
        int n = recvfrom(sock, (char*)buf, buf_size, 0, (sockaddr*)&addr, &slen);
#endif
        if (n > 0) {
            from.ip = ntohl(addr.sin_addr.s_addr);
            from.port = ntohs(addr.sin_port);
        }
        return n;
    }

    void close_socket() {
        if (sock != INVALID_SOCK) {
#ifdef _WIN32
            closesocket(sock);
#else
            close(sock);
#endif
            sock = INVALID_SOCK;
        }
    }

private:
    void set_nonblocking() {
#ifdef _WIN32
        u_long mode = 1;
        ioctlsocket(sock, FIONBIO, &mode);
#else
        int flags = fcntl(sock, F_GETFL, 0);
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);
#endif
    }
};
