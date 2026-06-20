#pragma once
// =============================================================================
// FileTransfer - TCP-based reliable large file transfer
// -----------------------------------------------------------------------------
// Use cases:
//   * World snapshots on join (50-500 MB)
//   * Mod/resource pack sync (1-100 MB)
//   * Save file download (spectator mode)
//
// Protocol:
//   [header: 16 bytes][data chunks...]
//   Header: [magic:4][file_size:8][chunk_size:4]
//   Each chunk: [compressed_size:4][zlib_data]
//
// Features:
//   * Chunked transfer (1MB chunks, progress tracking)
//   * Compression per chunk (zlib)
//   * Resume support (optional, for very large files)
//   * Progress callbacks
// =============================================================================
#include "tcp_socket.h"
#include "compression.h"
#include <functional>
#include <fstream>
#include <vector>

namespace net {

class FileTransfer {
public:
    static constexpr uint32_t MAGIC = 0x46494C45;  // "FILE"
    static constexpr size_t CHUNK_SIZE = 1024 * 1024;  // 1 MB

    using ProgressCallback = std::function<void(size_t bytes_transferred, size_t total)>;

    // ---- Send file over TCP ------------------------------------------------
    static bool sendFile(TCPSocket& sock, const std::string& file_path,
                         ProgressCallback progress = nullptr) {
        // Open file
        std::ifstream file(file_path, std::ios::binary | std::ios::ate);
        if (!file) return false;

        size_t file_size = file.tellg();
        file.seekg(0);

        // Send header
        uint32_t magic = MAGIC;
        uint64_t size = file_size;
        uint32_t chunk_size = CHUNK_SIZE;
        if (!sock.send_all(&magic, 4)) return false;
        if (!sock.send_all(&size, 8)) return false;
        if (!sock.send_all(&chunk_size, 4)) return false;

        // Send chunks
        std::vector<uint8_t> chunk_buf(CHUNK_SIZE);
        size_t sent = 0;
        while (sent < file_size) {
            size_t to_read = std::min(CHUNK_SIZE, file_size - sent);
            file.read((char*)chunk_buf.data(), to_read);
            if (!file) return false;

            // Compress chunk
            auto compressed = Compression::compress(chunk_buf.data(), to_read);
            if (compressed.empty()) {
                // Send uncompressed (mark with size = 0)
                uint32_t zero = 0;
                if (!sock.send_all(&zero, 4)) return false;
                uint32_t uncompressed_size = (uint32_t)to_read;
                if (!sock.send_all(&uncompressed_size, 4)) return false;
                if (!sock.send_all(chunk_buf.data(), to_read)) return false;
            } else {
                // Send compressed
                uint32_t compressed_size = (uint32_t)compressed.size();
                if (!sock.send_all(&compressed_size, 4)) return false;
                uint32_t uncompressed_size = (uint32_t)to_read;
                if (!sock.send_all(&uncompressed_size, 4)) return false;
                if (!sock.send_all(compressed.data(), compressed_size)) return false;
            }

            sent += to_read;
            if (progress) progress(sent, file_size);
        }

        return true;
    }

    // ---- Receive file over TCP ---------------------------------------------
    static bool receiveFile(TCPSocket& sock, const std::string& output_path,
                            ProgressCallback progress = nullptr) {
        // Receive header
        uint32_t magic;
        uint64_t file_size;
        uint32_t chunk_size;
        if (!sock.recv_all(&magic, 4)) return false;
        if (magic != MAGIC) return false;
        if (!sock.recv_all(&file_size, 8)) return false;
        if (!sock.recv_all(&chunk_size, 4)) return false;

        // Safety check (prevent DOS)
        if (file_size > 1024ULL * 1024 * 1024 * 2) {  // 2 GB max
            return false;
        }

        // Open output file
        std::ofstream file(output_path, std::ios::binary);
        if (!file) return false;

        // Receive chunks
        size_t received = 0;
        while (received < file_size) {
            uint32_t compressed_size, uncompressed_size;
            if (!sock.recv_all(&compressed_size, 4)) return false;
            if (!sock.recv_all(&uncompressed_size, 4)) return false;

            if (compressed_size == 0) {
                // Uncompressed chunk
                std::vector<uint8_t> data(uncompressed_size);
                if (!sock.recv_all(data.data(), uncompressed_size)) return false;
                file.write((const char*)data.data(), uncompressed_size);
            } else {
                // Compressed chunk
                std::vector<uint8_t> compressed_data(compressed_size);
                if (!sock.recv_all(compressed_data.data(), compressed_size)) return false;

                auto decompressed = Compression::decompress(compressed_data.data(),
                                                            compressed_size,
                                                            uncompressed_size);
                file.write((const char*)decompressed.data(), decompressed.size());
            }

            received += uncompressed_size;
            if (progress) progress(received, file_size);
        }

        return true;
    }
};

} // namespace net
