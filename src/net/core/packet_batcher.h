#pragma once
// =============================================================================
// PacketBatcher - coalesce multiple small packets into one UDP datagram
// -----------------------------------------------------------------------------
// Problem: Sending many small packets = high overhead
//   * Each UDP packet has ~28 bytes IP/UDP header
//   * 100 small packets/sec = 2.8 KB/s header overhead
//   * More syscalls = more CPU
//
// Solution: Batch multiple packets into one datagram
//   Wire format: [packet_count:varint][packet1][packet2]...
//   Each packet: [id:varint][size:varint][payload]
//
// Benefits:
//   * Reduce header overhead (1 UDP header for N packets)
//   * Reduce syscalls (1 send() for N packets)
//   * Better compression (compress entire batch)
//
// Tradeoffs:
//   * Slight latency increase (wait for batch to fill)
//   * More complex deserialization
//
// Usage:
//   PacketBatcher batcher;
//   batcher.add(packet1);
//   batcher.add(packet2);
//   if (batcher.shouldFlush()) {
//       auto batch = batcher.flush();
//       socket.send_to(batch.data(), batch.size(), addr);
//   }
// =============================================================================
#include "packet.h"
#include "byte_buffer.h"
#include "compression.h"
#include <vector>
#include <chrono>

namespace net {

class PacketBatcher {
public:
    static constexpr size_t MAX_BATCH_SIZE = 1200;  // MTU - safety margin
    static constexpr int MAX_BATCH_COUNT = 32;      // max packets per batch
    static constexpr int MAX_BATCH_DELAY_MS = 5;    // max wait time

    PacketBatcher() {
        last_flush = std::chrono::steady_clock::now();
    }

    // ---- Add packet to batch -----------------------------------------------
    bool add(const Packet& packet) {
        // Serialize packet to temp buffer
        ByteBuffer temp;
        temp.write_varint(packet.getId());
        ByteBuffer payload;
        packet.write(payload);
        temp.write_varint((int32_t)payload.size());
        temp.write_bytes(payload.data);

        // Check if adding this packet would exceed limits
        if (batch_buffer.size() + temp.size() > MAX_BATCH_SIZE ||
            packet_count >= MAX_BATCH_COUNT) {
            return false;  // batch full, need to flush
        }

        // Add to batch
        batch_buffer.write_bytes(temp.data);
        packet_count++;
        return true;
    }

    // ---- Check if should flush ---------------------------------------------
    bool shouldFlush() const {
        if (packet_count == 0) return false;

        // Flush if batch full
        if (batch_buffer.size() >= MAX_BATCH_SIZE || 
            packet_count >= MAX_BATCH_COUNT) {
            return true;
        }

        // Flush if timeout
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_flush).count();
        return elapsed >= MAX_BATCH_DELAY_MS;
    }

    // ---- Flush batch (returns serialized data) -----------------------------
    std::vector<uint8_t> flush() {
        if (packet_count == 0) return {};

        // Build final batch: [count:varint][packets...]
        ByteBuffer result;
        result.write_varint(packet_count);
        result.write_bytes(batch_buffer.data);

        // Optionally compress entire batch
        auto compressed = Compression::compress(result.data);
        std::vector<uint8_t> output;
        if (!compressed.empty()) {
            // Compressed batch: [negative_count][uncompressed_size][zlib_data]
            ByteBuffer header;
            header.write_varint(-packet_count);  // negative = compressed
            header.write_varint((int32_t)result.size());
            output.insert(output.end(), header.data.begin(), header.data.end());
            output.insert(output.end(), compressed.begin(), compressed.end());
        } else {
            output = result.data;
        }

        // Reset state
        batch_buffer.clear();
        packet_count = 0;
        last_flush = std::chrono::steady_clock::now();

        return output;
    }

    // ---- Unbatch (receiver side) -------------------------------------------
    static std::vector<std::unique_ptr<Packet>> unbatch(const uint8_t* data, size_t size) {
        std::vector<std::unique_ptr<Packet>> packets;
        ByteBuffer buf(data, size);

        try {
            int count = buf.read_varint();
            bool compressed = (count < 0);
            if (compressed) count = -count;

            ByteBuffer payload_buf;
            if (compressed) {
                // Decompress
                int uncompressed_size = buf.read_varint();
                size_t compressed_size = buf.remaining();
                auto decompressed = Compression::decompress(
                    buf.data.data() + buf.read_pos, compressed_size, uncompressed_size);
                payload_buf = ByteBuffer(decompressed);
            } else {
                payload_buf = buf;
            }

            // Read individual packets
            for (int i = 0; i < count; i++) {
                int id = payload_buf.read_varint();
                int pkt_size = payload_buf.read_varint();
                if (pkt_size < 0 || pkt_size > (int)payload_buf.remaining())
                    break;

                auto packet = Packet::create(id);
                if (packet) {
                    packet->read(payload_buf);
                    packets.push_back(std::move(packet));
                } else {
                    // Skip unknown packet
                    payload_buf.read_pos += pkt_size;
                }
            }
        } catch (...) {
            // Parse error
        }

        return packets;
    }

    // ---- Stats -------------------------------------------------------------
    size_t currentSize() const { return batch_buffer.size(); }
    int currentCount() const { return packet_count; }

private:
    ByteBuffer batch_buffer;
    int packet_count = 0;
    std::chrono::steady_clock::time_point last_flush;
};

} // namespace net
