#pragma once
// =============================================================================
// SDFCraft - Real TCP backend for the EditTransport seam (networking phase)
// -----------------------------------------------------------------------------
// LoopbackTransport (net_ops.h) exercises the replication logic in-process.
// This file provides the over-the-wire implementation so a host and remote
// clients can share carve/place edits across machines.
//
// DESIGN
//   * One reliable, ordered TCP stream per peer link (TCP_NODELAY already set
//     by TCPSocket so small edits flush immediately).
//   * Datagram framing: each EditTransport::send() payload is written as
//     [uint32 LE length][payload bytes]. poll() reassembles exactly one
//     datagram per call, buffering partial reads, so EditReplicator::pump()
//     sees the same datagram boundaries it does over LoopbackTransport.
//   * Non-blocking sockets: poll() never stalls the game loop; it returns
//     false when no *complete* datagram is available yet.
//
// USAGE
//   TcpNetInit guard;                       // once per process (WSAStartup)
//   // Host:
//   TcpEditTransport host; host.listen(55001); ... host.acceptPeer();
//   // Client:
//   TcpEditTransport cli;  cli.connect("127.0.0.1", 55001);
//   EditReplicator repl(world, transport, author);
// =============================================================================
#include "net_ops.h"
#include "../net/core/tcp_socket.h"

#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <vector>

namespace sdfcraft {

// TCPSocket lives in namespace net; alias it for brevity here.
using TCPSocket = net::TCPSocket;

// RAII Winsock init (no-op on POSIX). Construct one before any TcpEditTransport.
struct TcpNetInit {
    TcpNetInit() {
#ifdef _WIN32
        WSADATA wsa;
        ok_ = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
#else
        ok_ = true;
#endif
    }
    ~TcpNetInit() {
#ifdef _WIN32
        if (ok_) WSACleanup();
#endif
    }
    bool ok() const { return ok_; }
    TcpNetInit(const TcpNetInit&) = delete;
    TcpNetInit& operator=(const TcpNetInit&) = delete;
private:
    bool ok_ = false;
};

// Real over-the-wire EditTransport. One instance owns exactly one peer link.
// Host: listen() then acceptPeer() (poll-able, non-blocking accept).
// Client: connect() to the host.
class TcpEditTransport : public EditTransport {
public:
    TcpEditTransport() = default;
    ~TcpEditTransport() override { delete peer_; delete listener_; }

    TcpEditTransport(const TcpEditTransport&) = delete;
    TcpEditTransport& operator=(const TcpEditTransport&) = delete;

    // ---- Host side ----------------------------------------------------------
    // Begin listening; non-blocking so acceptPeer() can be polled each frame.
    bool listen(uint16_t port, int backlog = 4) {
        delete listener_;
        listener_ = new TCPSocket();
        if (!listener_->create() || !listener_->bind_port(port) ||
            !listener_->listen(backlog)) {
            delete listener_; listener_ = nullptr;
            return false;
        }
        listener_->set_nonblocking();
        return true;
    }
    // Try to accept one pending client. Returns true once a peer is linked.
    bool acceptPeer() {
        if (!listener_ || peer_) return peer_ != nullptr;
        TCPSocket* c = listener_->accept();
        if (!c) return false;          // none pending (non-blocking)
        c->set_nonblocking();
        peer_ = c;
        return true;
    }

    // ---- Client side --------------------------------------------------------
    bool connect(const std::string& ip, uint16_t port) {
        delete peer_;
        peer_ = new TCPSocket();
        if (!peer_->create() || !peer_->connect(ip, port)) {
            delete peer_; peer_ = nullptr;
            return false;
        }
        peer_->set_nonblocking();
        return true;
    }

    bool connected() const { return peer_ && peer_->is_valid() && !closed_; }

    // ---- EditTransport ------------------------------------------------------
    // Frame = [uint32 LE payload length][payload]. send_all blocks only on the
    // socket buffer, which TCP_NODELAY keeps small for these tiny edit msgs.
    void send(const std::vector<uint8_t>& bytes) override {
        if (!peer_ || closed_) return;
        uint32_t len = static_cast<uint32_t>(bytes.size());
        uint8_t hdr[4] = { uint8_t(len), uint8_t(len >> 8),
                           uint8_t(len >> 16), uint8_t(len >> 24) };
        if (!peer_->send_all(hdr, 4) ||
            (len > 0 && !peer_->send_all(bytes.data(), int(len)))) {
            closed_ = true;
        }
    }

    // Returns one complete datagram per call, or false if none is ready yet.
    bool poll(std::vector<uint8_t>& out) override {
        if (!peer_) return false;
        drain();
        // Need a full 4-byte header first.
        if (rx_.size() < 4) return false;
        uint32_t len = uint32_t(rx_[0]) | (uint32_t(rx_[1]) << 8) |
                       (uint32_t(rx_[2]) << 16) | (uint32_t(rx_[3]) << 24);
        if (rx_.size() < 4u + len) return false;   // payload not fully arrived
        out.assign(rx_.begin() + 4, rx_.begin() + 4 + len);
        rx_.erase(rx_.begin(), rx_.begin() + 4 + len);
        return true;
    }

private:
    // Pull whatever bytes are available on the socket into rx_ (non-blocking).
    void drain() {
        if (closed_) return;
        uint8_t buf[4096];
        for (;;) {
            int n = peer_->recv(buf, int(sizeof(buf)));
            if (n > 0) {
                rx_.insert(rx_.end(), buf, buf + n);
                if (n < int(sizeof(buf))) break;    // likely drained
            } else if (n == 0) {
                closed_ = true; break;               // peer closed
            } else {
                break;                               // would-block: no data now
            }
        }
    }

    TCPSocket* listener_ = nullptr;
    TCPSocket* peer_     = nullptr;
    std::vector<uint8_t> rx_;   // reassembly buffer (partial frames persist)
    bool closed_ = false;
};

} // namespace sdfcraft
