#pragma once
// =============================================================================
// Connection - core network connection with dual-threaded dual-queue design
// -----------------------------------------------------------------------------
// Architecture ported from MinecraftConsoles Connection.h:
//   * Separate read/write threads (don't block each other)
//   * Three queues:
//     - incoming:      packets received from network
//     - outgoing:      high-priority send queue (player input, entity moves)
//     - outgoing_slow: low-priority send queue (chunk data, large blobs)
//   * Flow control: estimatedRemaining tracks buffered bytes, applies
//     backpressure if > 1MB to prevent memory exhaustion
//   * Packet invalidation: old MoveEntity packets discarded when newer arrives
//   * Timeout: disconnect if no input for MAX_TICKS_WITHOUT_INPUT
//   * Compression: zlib for packets > 256 bytes (Phase 3)
//
// Wire format (uncompressed):
//   [packet_id: varint][payload_size: varint][payload bytes]
//
// Wire format (compressed):
//   [packet_id: varint][-compressed_size: varint][uncompressed_size: varint][zlib_data]
//   (negative compressed_size indicates compression)
//
// Both RTS and Voxel sync stacks use this connection primitive.
// =============================================================================
#include "packet.h"
#include "byte_buffer.h"
#include "compression.h"
#include "../socket.h"
#include <thread>
#include <queue>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <memory>

namespace net {

class Connection {
public:
    // ---- Config constants (tunable) ----------------------------------------
    static constexpr size_t BACKPRESSURE_LIMIT = 1024 * 1024;  // 1MB
    static constexpr int MAX_TICKS_WITHOUT_INPUT = 20 * 60;    // 60 sec @ 20Hz
    static constexpr int SLOW_WRITE_DELAY = 1;  // send one slow packet per N fast packets

    // ---- Construction ------------------------------------------------------
    Connection(UDPSocket& sock, NetAddress remote);
    ~Connection();

    // Non-copyable
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    // ---- Sending (thread-safe) ---------------------------------------------
    void send(std::unique_ptr<Packet> packet);      // auto-routes to fast/slow queue
    void queueSlow(std::unique_ptr<Packet> packet); // force slow queue (chunks)
    void flush();  // blocks until outgoing queues drain (shutdown helper)

    // ---- Receiving (call from main thread) ---------------------------------
    // Pump incoming queue; returns packets in receive order.
    // Caller must handle them and delete.
    std::vector<std::unique_ptr<Packet>> poll();

    // ---- Connection management ---------------------------------------------
    void start();   // spawn threads
    void stop();    // signal threads to exit, join
    bool isRunning() const { return running; }

    NetAddress remoteAddress() const { return remote_addr; }

    // ---- Stats (read-only, atomic) -----------------------------------------
    std::atomic<size_t> estimatedRemaining{0};  // bytes in send queues
    std::atomic<int>    noInputTicks{0};        // ticks since last recv

private:
    UDPSocket& socket;
    NetAddress remote_addr;

    std::atomic<bool> running{false};
    std::atomic<bool> quitting{false};

    // ---- Queues + locks ----------------------------------------------------
    std::queue<std::unique_ptr<Packet>> incoming;
    std::mutex incoming_mtx;

    std::queue<std::unique_ptr<Packet>> outgoing;       // fast
    std::queue<std::unique_ptr<Packet>> outgoing_slow;  // slow (chunks)
    std::mutex write_mtx;
    std::condition_variable write_cv;  // wake write thread when packets queued

    // ---- Threads -----------------------------------------------------------
    std::thread read_thread;
    std::thread write_thread;

    void readLoop();
    void writeLoop();

    // Packet invalidation: scan outgoing queue, remove packets invalidated by 'p'
    void removeInvalidated(const Packet& p);

    int slowWriteDelay = SLOW_WRITE_DELAY;
};

} // namespace net
