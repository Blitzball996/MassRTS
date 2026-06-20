#include "connection.h"
#include <algorithm>

namespace net {

Connection::Connection(UDPSocket& sock, NetAddress remote)
    : socket(sock), remote_addr(remote) {}

Connection::~Connection() {
    stop();
}

void Connection::start() {
    if (running) return;
    running = true;
    quitting = false;
    read_thread = std::thread(&Connection::readLoop, this);
    write_thread = std::thread(&Connection::writeLoop, this);
}

void Connection::stop() {
    if (!running) return;
    quitting = true;
    write_cv.notify_all();  // wake write thread
    if (read_thread.joinable()) read_thread.join();
    if (write_thread.joinable()) write_thread.join();
    running = false;
}

void Connection::send(std::unique_ptr<Packet> packet) {
    if (quitting) return;
    std::lock_guard<std::mutex> lock(write_mtx);
    
    // Flow control: if buffered > 1MB, drop (MC applies backpressure here)
    if (estimatedRemaining > BACKPRESSURE_LIMIT) {
        // In production: log warning, or queue for later retry
        return;
    }

    estimatedRemaining += packet->estimatedSize() + 8;  // +8 for header overhead

    // Auto-route: shouldDelay → slow queue, else fast
    if (packet->shouldDelay) {
        outgoing_slow.push(std::move(packet));
    } else {
        // Invalidation: remove old packets this one supersedes (MoveEntity optimization)
        if (packet->canBeInvalidated()) removeInvalidated(*packet);
        outgoing.push(std::move(packet));
    }
    write_cv.notify_one();
}

void Connection::queueSlow(std::unique_ptr<Packet> packet) {
    if (quitting) return;
    std::lock_guard<std::mutex> lock(write_mtx);
    estimatedRemaining += packet->estimatedSize() + 8;
    outgoing_slow.push(std::move(packet));
    write_cv.notify_one();
}

void Connection::flush() {
    while (running && estimatedRemaining > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

std::vector<std::unique_ptr<Packet>> Connection::poll() {
    std::lock_guard<std::mutex> lock(incoming_mtx);
    std::vector<std::unique_ptr<Packet>> result;
    while (!incoming.empty()) {
        result.push_back(std::move(incoming.front()));
        incoming.pop();
    }
    return result;
}

void Connection::readLoop() {
    uint8_t recv_buf[2048];
    NetAddress from;
    while (!quitting) {
        int n = socket.recv_from(recv_buf, sizeof(recv_buf), from);
        if (n <= 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            noInputTicks++;
            if (noInputTicks > MAX_TICKS_WITHOUT_INPUT) {
                quitting = true;  // timeout disconnect
                break;
            }
            continue;
        }
        noInputTicks = 0;

        // Deserialize: [packet_id: varint][payload_size: varint][payload]
        // If payload_size < 0: compressed → [uncompressed_size: varint][zlib_data]
        try {
            ByteBuffer buf(recv_buf, n);
            int id = buf.read_varint();
            int payload_size = buf.read_varint();
            
            std::vector<uint8_t> payload_data;
            if (payload_size < 0) {
                // Compressed
                int compressed_size = -payload_size;
                int uncompressed_size = buf.read_varint();
                if (compressed_size < 0 || compressed_size > (int)buf.remaining())
                    continue;
                payload_data = Compression::decompress(buf.data.data() + buf.read_pos,
                                                       compressed_size, uncompressed_size);
            } else {
                // Uncompressed
                if (payload_size < 0 || payload_size > (int)buf.remaining())
                    continue;
                payload_data.assign(buf.data.begin() + buf.read_pos,
                                    buf.data.begin() + buf.read_pos + payload_size);
            }

            auto packet = Packet::create(id);
            if (!packet) continue;

            ByteBuffer payload_buf(payload_data);
            packet->read(payload_buf);

            {
                std::lock_guard<std::mutex> lock(incoming_mtx);
                incoming.push(std::move(packet));
            }
        } catch (...) {
            // Parse error, drop packet
        }
    }
}

void Connection::writeLoop() {
    ByteBuffer send_buf;
    while (!quitting) {
        std::unique_ptr<Packet> packet;
        {
            std::unique_lock<std::mutex> lock(write_mtx);
            // Wait for packets or quit signal
            write_cv.wait(lock, [this]() {
                return quitting || !outgoing.empty() || !outgoing_slow.empty();
            });
            if (quitting && outgoing.empty() && outgoing_slow.empty())
                break;

            // Priority: fast queue first
            if (!outgoing.empty()) {
                packet = std::move(outgoing.front());
                outgoing.pop();
            } else if (slowWriteDelay-- <= 0 && !outgoing_slow.empty()) {
                // Throttle slow queue: only send one slow packet per SLOW_WRITE_DELAY cycles
                packet = std::move(outgoing_slow.front());
                outgoing_slow.pop();
                slowWriteDelay = SLOW_WRITE_DELAY;
            }
        }

        if (!packet) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        // Serialize: [packet_id: varint][payload_size: varint][payload]
        // Try compression if payload large enough
        try {
            send_buf.clear();
            send_buf.write_varint(packet->getId());

            // Write payload to temp buffer first to know size
            ByteBuffer payload;
            packet->write(payload);

            // Try compress (returns empty if not worth it)
            auto compressed = Compression::compress(payload.data);
            if (!compressed.empty()) {
                // Compressed: negative size indicates compression
                send_buf.write_varint(-(int32_t)compressed.size());
                send_buf.write_varint((int32_t)payload.size());  // uncompressed size
                send_buf.write_bytes(compressed);
            } else {
                // Uncompressed
                send_buf.write_varint((int32_t)payload.size());
                send_buf.write_bytes(payload.data);
            }

            socket.send_to(send_buf.ptr(), send_buf.size(), remote_addr);
            estimatedRemaining -= packet->estimatedSize() + 8;
        } catch (...) {
            // Serialize error, drop packet
        }
    }
}

void Connection::removeInvalidated(const Packet& newest) {
    // Scan outgoing queue, remove any packet that newest.isInvalidatedBy()
    // (e.g., old MoveEntityPacketSmall for same entity)
    // This requires scanning the queue; in production MC uses a more efficient
    // id->packet map. For now, simple linear scan.
    std::queue<std::unique_ptr<Packet>> filtered;
    while (!outgoing.empty()) {
        auto& p = outgoing.front();
        if (!newest.isInvalidatedBy(*p)) {
            filtered.push(std::move(p));
        } else {
            estimatedRemaining -= p->estimatedSize() + 8;
        }
        outgoing.pop();
    }
    outgoing = std::move(filtered);
}

} // namespace net
