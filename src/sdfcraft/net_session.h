#pragma once
// =============================================================================
// SDFCraft - Multi-client TCP session layer (Phase N)
// -----------------------------------------------------------------------------
// TcpEditTransport (net_transport_tcp.h) framed ONE peer link. A real server
// needs to fan out to MANY clients, so this layer owns:
//   * NetServer - a listening socket + a list of connected clients, each with
//                 its own reassembly buffer. Polls per-client messages, and can
//                 broadcast / unicast length-framed messages.
//   * NetClient - a single connection to a server, same framing.
//
// Framing is identical to TcpEditTransport: [uint32 LE length][payload]. The
// payload here is a tagged protocol message (net_protocol.h). Sockets are
// non-blocking so the game/server loop never stalls.
//
// This layer is transport only — it has no idea what the bytes mean. The server
// simulation (server_sim.h) and Mode interpret them via net_protocol.h.
// =============================================================================
#include "../net/core/tcp_socket.h"
#include <cstdint>
#include <vector>
#include <memory>
#include <string>
#include <algorithm>

namespace sdfcraft {

#ifndef SDFCRAFT_TCPNETINIT_DEFINED
#define SDFCRAFT_TCPNETINIT_DEFINED
// RAII Winsock init (no-op on POSIX). One per process before any socket use.
// (TcpEditTransport defines its own TcpNetInit; this header may be included
// without that one, so we guard a local copy.)
struct NetInit {
    NetInit() {
#ifdef _WIN32
        WSADATA wsa; ok_ = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
#else
        ok_ = true;
#endif
    }
    ~NetInit() {
#ifdef _WIN32
        if (ok_) WSACleanup();
#endif
    }
    bool ok() const { return ok_; }
    NetInit(const NetInit&) = delete;
    NetInit& operator=(const NetInit&) = delete;
private:
    bool ok_ = false;
};
#endif

// A single received, fully-reassembled message tagged with its source client.
struct InMsg {
    int                  client_id;   // server-assigned slot (client side: 0)
    std::vector<uint8_t> bytes;       // one complete protocol message
};

// ----------------------------------------------------------------------------
// Framed peer: wraps a TCPSocket with length-prefixed send + reassembling poll.
// Shared internals for both server-side client links and the client's link.
// ----------------------------------------------------------------------------
class FramedPeer {
public:
    explicit FramedPeer(net::TCPSocket* s) : sock_(s) { if (sock_) sock_->set_nonblocking(); }
    ~FramedPeer() { delete sock_; }
    FramedPeer(const FramedPeer&) = delete;
    FramedPeer& operator=(const FramedPeer&) = delete;

    bool alive() const { return sock_ && sock_->is_valid() && !closed_; }

    void send(const std::vector<uint8_t>& payload) {
        if (!alive()) return;
        uint32_t len = (uint32_t)payload.size();
        uint8_t hdr[4] = { uint8_t(len), uint8_t(len>>8), uint8_t(len>>16), uint8_t(len>>24) };
        if (!sock_->send_all(hdr, 4) ||
            (len && !sock_->send_all(payload.data(), (int)len)))
            closed_ = true;
    }

    // Pull one complete message into `out`; false when none ready. Call in a loop.
    bool poll(std::vector<uint8_t>& out) {
        drain();
        if (rx_.size() < 4) return false;
        uint32_t len = uint32_t(rx_[0]) | (uint32_t(rx_[1])<<8) |
                       (uint32_t(rx_[2])<<16) | (uint32_t(rx_[3])<<24);
        if (rx_.size() < 4u + len) return false;
        out.assign(rx_.begin()+4, rx_.begin()+4+len);
        rx_.erase(rx_.begin(), rx_.begin()+4+len);
        return true;
    }

private:
    void drain() {
        if (closed_) return;
        uint8_t buf[8192];
        for (;;) {
            int n = sock_->recv(buf, (int)sizeof(buf));
            if (n > 0) { rx_.insert(rx_.end(), buf, buf+n); if (n < (int)sizeof(buf)) break; }
            else if (n == 0) { closed_ = true; break; }   // peer closed
            else break;                                    // would-block
        }
    }
    net::TCPSocket* sock_ = nullptr;
    std::vector<uint8_t> rx_;
    bool closed_ = false;
};

// ----------------------------------------------------------------------------
// NetServer: listens, accepts many clients, fans out messages.
// ----------------------------------------------------------------------------
class NetServer {
public:
    bool start(uint16_t port, int backlog = 8) {
        listener_ = std::make_unique<net::TCPSocket>();
        if (!listener_->create() || !listener_->bind_port(port) || !listener_->listen(backlog)) {
            listener_.reset();
            return false;
        }
        listener_->set_nonblocking();
        return true;
    }

    // Accept any pending connections; returns the list of newly-joined client ids.
    std::vector<int> acceptNew() {
        std::vector<int> joined;
        if (!listener_) return joined;
        for (;;) {
            net::TCPSocket* c = listener_->accept();
            if (!c) break;                    // none pending (non-blocking)
            int id = next_id_++;
            clients_.push_back({ id, std::make_unique<FramedPeer>(c) });
            joined.push_back(id);
        }
        return joined;
    }

    // Drain every client; returns all complete messages received this tick.
    // Disconnected clients are detected here and reported via takeDisconnects().
    std::vector<InMsg> poll() {
        std::vector<InMsg> msgs;
        for (auto& c : clients_) {
            std::vector<uint8_t> m;
            while (c.peer->poll(m)) msgs.push_back({ c.id, m });
            if (!c.peer->alive()) dropped_.push_back(c.id);
        }
        // prune dropped clients
        if (!dropped_.empty()) {
            clients_.erase(std::remove_if(clients_.begin(), clients_.end(),
                [&](const Client& c){
                    for (int d : dropped_) if (d == c.id) return true;
                    return false; }), clients_.end());
        }
        return msgs;
    }

    // Client ids that disconnected since the last call (consumes the list).
    std::vector<int> takeDisconnects() { auto d = dropped_; dropped_.clear(); return d; }

    void sendTo(int client_id, const std::vector<uint8_t>& bytes) {
        for (auto& c : clients_) if (c.id == client_id) { c.peer->send(bytes); return; }
    }
    void broadcast(const std::vector<uint8_t>& bytes) {
        for (auto& c : clients_) c.peer->send(bytes);
    }
    void broadcastExcept(int except_id, const std::vector<uint8_t>& bytes) {
        for (auto& c : clients_) if (c.id != except_id) c.peer->send(bytes);
    }

    size_t clientCount() const { return clients_.size(); }
    std::vector<int> clientIds() const {
        std::vector<int> v; v.reserve(clients_.size());
        for (auto& c : clients_) v.push_back(c.id);
        return v;
    }

private:
    struct Client { int id; std::unique_ptr<FramedPeer> peer; };
    std::unique_ptr<net::TCPSocket> listener_;
    std::vector<Client> clients_;
    std::vector<int>    dropped_;
    int next_id_ = 1;   // 0 reserved for the host's local player
};

// ----------------------------------------------------------------------------
// NetClient: a single connection to a server.
// ----------------------------------------------------------------------------
class NetClient {
public:
    bool connect(const std::string& ip, uint16_t port) {
        net::TCPSocket* s = new net::TCPSocket();
        if (!s->create() || !s->connect(ip, port)) { delete s; return false; }
        peer_ = std::make_unique<FramedPeer>(s);
        return true;
    }
    bool connected() const { return peer_ && peer_->alive(); }
    void send(const std::vector<uint8_t>& bytes) { if (peer_) peer_->send(bytes); }
    // Collect all complete messages received this tick.
    std::vector<std::vector<uint8_t>> poll() {
        std::vector<std::vector<uint8_t>> out;
        if (!peer_) return out;
        std::vector<uint8_t> m;
        while (peer_->poll(m)) out.push_back(m);
        return out;
    }
private:
    std::unique_ptr<FramedPeer> peer_;
};

} // namespace sdfcraft
