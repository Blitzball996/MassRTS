#pragma once
// =============================================================================
// RegionFile - Minecraft-style world storage format
// -----------------------------------------------------------------------------
// Region file format (Minecraft Anvil):
//   * World divided into regions (32x32 chunks per region)
//   * Each region = 1 file (.mca)
//   * File structure:
//     [header: 8KB][timestamps: 4KB][chunk data...]
//     Header: [offset:3B][size:1B] per chunk (4096 entries)
//     Chunk data: [length:4B][compression:1B][compressed_data]
//
// Benefits:
//   * Random access to chunks (no need to read entire file)
//   * Sparse storage (only modified chunks saved)
//   * Incremental saves (only write dirty chunks)
//   * Compatible with Minecraft world viewers
//
// File naming: r.x.z.mca (region coords)
//   Example: r.0.0.mca = region at (0, 0)
//            r.-1.2.mca = region at (-1, 2)
//
// Usage:
//   RegionFile region("world/region/r.0.0.mca");
//   region.writeChunk(5, 10, chunk_data);  // chunk (5, 10) in region
//   auto data = region.readChunk(5, 10);
// =============================================================================
#include "../core/compression.h"
#include <fstream>
#include <vector>
#include <cstring>
#include <array>

namespace net {

class RegionFile {
public:
    static constexpr int REGION_SIZE = 32;  // 32x32 chunks per region
    static constexpr int SECTOR_SIZE = 4096;  // 4 KB sectors
    static constexpr int HEADER_SECTORS = 2;  // 8 KB header + 4 KB timestamps

    RegionFile(const std::string& file_path) : path(file_path) {
        // Try open existing file
        std::ifstream test(path, std::ios::binary);
        if (test) {
            // Load header
            test.read((char*)offsets.data(), offsets.size() * 4);
            test.read((char*)timestamps.data(), timestamps.size() * 4);
        }
    }

    // ---- Write chunk -------------------------------------------------------
    bool writeChunk(int cx, int cz, const std::vector<uint8_t>& data) {
        if (cx < 0 || cx >= REGION_SIZE || cz < 0 || cz >= REGION_SIZE)
            return false;

        // Compress chunk data
        auto compressed = Compression::compress(data);
        bool is_compressed = !compressed.empty();
        if (!is_compressed) compressed = data;  // fallback uncompressed

        // Prepare chunk payload: [length:4][compression:1][uncompressed:4][data]
        std::vector<uint8_t> payload(9 + compressed.size());
        uint32_t length = (uint32_t)(5 + compressed.size());  // compression(1)+usize(4)+data
        payload[0] = (length >> 24) & 0xFF;
        payload[1] = (length >> 16) & 0xFF;
        payload[2] = (length >> 8) & 0xFF;
        payload[3] = length & 0xFF;
        payload[4] = is_compressed ? 2 : 0;  // 2=zlib, 0=uncompressed
        uint32_t usize = (uint32_t)data.size();
        payload[5] = (usize >> 24) & 0xFF;
        payload[6] = (usize >> 16) & 0xFF;
        payload[7] = (usize >> 8) & 0xFF;
        payload[8] = usize & 0xFF;
        std::memcpy(payload.data() + 9, compressed.data(), compressed.size());

        // Calculate sectors needed
        int sectors_needed = (int)((payload.size() + SECTOR_SIZE - 1) / SECTOR_SIZE);

        // Open file for read/write
        std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
        bool new_file = !file.is_open();
        if (new_file) {
            file.open(path, std::ios::out | std::ios::binary);
            if (!file) return false;
            // Write empty header
            std::vector<uint8_t> empty_header(SECTOR_SIZE * HEADER_SECTORS, 0);
            file.write((char*)empty_header.data(), empty_header.size());
        }

        // Find free space or reuse existing chunk location
        int chunk_index = cx + cz * REGION_SIZE;
        uint32_t old_offset = offsets[chunk_index];
        int old_sectors = old_offset & 0xFF;
        int old_sector_offset = (old_offset >> 8) & 0xFFFFFF;

        int sector_offset;
        if (old_sectors >= sectors_needed && old_sector_offset > 0) {
            // Reuse existing space
            sector_offset = old_sector_offset;
        } else {
            // Allocate at end of file
            file.seekg(0, std::ios::end);
            sector_offset = (int)(file.tellg() / SECTOR_SIZE);
        }

        // Write chunk data
        file.seekp(sector_offset * SECTOR_SIZE);
        file.write((char*)payload.data(), payload.size());
        // Pad to sector boundary
        int pad = SECTOR_SIZE - (payload.size() % SECTOR_SIZE);
        if (pad < SECTOR_SIZE) {
            std::vector<uint8_t> padding(pad, 0);
            file.write((char*)padding.data(), padding.size());
        }

        // Update header
        offsets[chunk_index] = (sector_offset << 8) | (sectors_needed & 0xFF);
        timestamps[chunk_index] = (uint32_t)std::time(nullptr);

        // Write header
        file.seekp(0);
        file.write((char*)offsets.data(), offsets.size() * 4);
        file.write((char*)timestamps.data(), timestamps.size() * 4);

        return true;
    }

    // ---- Read chunk --------------------------------------------------------
    std::vector<uint8_t> readChunk(int cx, int cz) {
        if (cx < 0 || cx >= REGION_SIZE || cz < 0 || cz >= REGION_SIZE)
            return {};

        int chunk_index = cx + cz * REGION_SIZE;
        uint32_t offset_entry = offsets[chunk_index];
        if (offset_entry == 0) return {};  // chunk not saved

        int sector_offset = (offset_entry >> 8) & 0xFFFFFF;
        int sectors = offset_entry & 0xFF;
        if (sector_offset == 0 || sectors == 0) return {};

        // Open file
        std::ifstream file(path, std::ios::binary);
        if (!file) return {};

        // Read chunk header
        file.seekg(sector_offset * SECTOR_SIZE);
        uint8_t hdr[9];
        file.read((char*)hdr, 9);
        uint32_t length = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) |
                          ((uint32_t)hdr[2] << 8) | (uint32_t)hdr[3];
        uint8_t compression = hdr[4];
        uint32_t usize = ((uint32_t)hdr[5] << 24) | ((uint32_t)hdr[6] << 16) |
                         ((uint32_t)hdr[7] << 8) | (uint32_t)hdr[8];

        if (length < 5 || length > (uint32_t)sectors * SECTOR_SIZE)
            return {};  // corrupted

        // Read chunk data (length includes compression(1)+usize(4))
        std::vector<uint8_t> compressed_data(length - 5);
        file.read((char*)compressed_data.data(), compressed_data.size());

        // Decompress if needed
        if (compression == 2) {
            try {
                return Compression::decompress(compressed_data.data(),
                                               compressed_data.size(),
                                               usize);
            } catch (...) {
                return {};
            }
        } else {
            // Uncompressed
            return compressed_data;
        }
    }

    // ---- Delete chunk ------------------------------------------------------
    bool deleteChunk(int cx, int cz) {
        if (cx < 0 || cx >= REGION_SIZE || cz < 0 || cz >= REGION_SIZE)
            return false;

        int chunk_index = cx + cz * REGION_SIZE;
        offsets[chunk_index] = 0;
        timestamps[chunk_index] = 0;

        // Update header in file
        std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
        if (!file) return false;
        file.seekp(0);
        file.write((char*)offsets.data(), offsets.size() * 4);
        file.write((char*)timestamps.data(), timestamps.size() * 4);
        return true;
    }

    // ---- Helpers -----------------------------------------------------------
    bool hasChunk(int cx, int cz) const {
        if (cx < 0 || cx >= REGION_SIZE || cz < 0 || cz >= REGION_SIZE)
            return false;
        int chunk_index = cx + cz * REGION_SIZE;
        return offsets[chunk_index] != 0;
    }

    uint32_t getTimestamp(int cx, int cz) const {
        if (cx < 0 || cx >= REGION_SIZE || cz < 0 || cz >= REGION_SIZE)
            return 0;
        return timestamps[cx + cz * REGION_SIZE];
    }

private:
    std::string path;
    std::array<uint32_t, REGION_SIZE * REGION_SIZE> offsets{};
    std::array<uint32_t, REGION_SIZE * REGION_SIZE> timestamps{};
};

} // namespace net
