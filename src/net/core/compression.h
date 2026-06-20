#pragma once
// =============================================================================
// Compression - zlib compression for large packets
// -----------------------------------------------------------------------------
// MinecraftConsoles uses zlib to compress large packets (chunks, snapshots).
// Threshold: only compress if payload > 256 bytes (compression overhead).
//
// Wire format with compression:
//   [packet_id: varint][compressed_size: varint][uncompressed_size: varint][zlib_data]
//
// Without compression:
//   [packet_id: varint][payload_size: varint][payload]
//
// Benefits:
//   - Chunk data: ~10KB → ~2KB (80% reduction)
//   - Snapshots: ~50KB → ~8KB (84% reduction)
//   - Small packets: no overhead (skip compression)
// =============================================================================
#include <cstdint>
#include <vector>
#include <stdexcept>

#ifdef _WIN32
#pragma comment(lib, "zlibstatic.lib")
#endif

#include <zlib.h>

namespace net {

class Compression {
public:
    static constexpr size_t COMPRESS_THRESHOLD = 256;  // bytes
    static constexpr int COMPRESS_LEVEL = 6;           // 0-9, 6=default balance

    // ---- Compress (returns empty if data too small or compression failed) --
    static std::vector<uint8_t> compress(const uint8_t* data, size_t size) {
        if (size < COMPRESS_THRESHOLD) {
            return {};  // too small, don't compress
        }

        // Allocate output buffer (worst case: slightly larger than input)
        size_t bound = compressBound((uLong)size);
        std::vector<uint8_t> compressed(bound);
        uLongf compressed_size = (uLongf)bound;

        int result = compress2(compressed.data(), &compressed_size,
                               data, (uLong)size, COMPRESS_LEVEL);
        if (result != Z_OK) {
            return {};  // compression failed
        }

        // Check if compression actually saved space (sometimes it doesn't)
        if (compressed_size >= size) {
            return {};  // not worth compressing
        }

        compressed.resize(compressed_size);
        return compressed;
    }

    // ---- Decompress --------------------------------------------------------
    static std::vector<uint8_t> decompress(const uint8_t* data, size_t compressed_size,
                                            size_t uncompressed_size) {
        if (uncompressed_size > 100 * 1024 * 1024) {
            throw std::runtime_error("decompressed size too large (DOS protection)");
        }

        std::vector<uint8_t> decompressed(uncompressed_size);
        uLongf dest_len = (uLongf)uncompressed_size;

        int result = uncompress(decompressed.data(), &dest_len,
                                data, (uLong)compressed_size);
        if (result != Z_OK) {
            throw std::runtime_error("decompression failed");
        }

        if (dest_len != uncompressed_size) {
            throw std::runtime_error("decompressed size mismatch");
        }

        return decompressed;
    }

    // ---- Helper: compress vector -------------------------------------------
    static std::vector<uint8_t> compress(const std::vector<uint8_t>& data) {
        return compress(data.data(), data.size());
    }
};

} // namespace net
