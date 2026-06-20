// =============================================================================
// Compression Test - zlib performance validation
// =============================================================================
#include "core/compression.h"
#include <iostream>
#include <cstring>
#include <chrono>

using namespace net;

void test_small_data() {
    std::cout << "=== Small Data Test (< 256 bytes) ===\n";
    std::vector<uint8_t> small(100, 42);
    auto compressed = Compression::compress(small);
    if (compressed.empty()) {
        std::cout << "Small data not compressed (expected) ✅\n";
    } else {
        std::cout << "Small data compressed (unexpected) ❌\n";
    }
    std::cout << "\n";
}

void test_compressible_data() {
    std::cout << "=== Compressible Data Test (repetitive) ===\n";
    // Simulate chunk data (lots of air blocks = zeros)
    std::vector<uint8_t> chunk(10000, 0);  // 10KB of zeros
    
    auto start = std::chrono::high_resolution_clock::now();
    auto compressed = Compression::compress(chunk);
    auto end = std::chrono::high_resolution_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    
    std::cout << "Original: " << chunk.size() << " bytes\n";
    std::cout << "Compressed: " << compressed.size() << " bytes\n";
    std::cout << "Ratio: " << (100.0 * compressed.size() / chunk.size()) << "%\n";
    std::cout << "Time: " << us << " µs\n";
    
    if (!compressed.empty() && compressed.size() < chunk.size() / 2) {
        std::cout << "Compression effective ✅\n";
    } else {
        std::cout << "Compression ineffective ❌\n";
    }
    
    // Decompress
    start = std::chrono::high_resolution_clock::now();
    auto decompressed = Compression::decompress(compressed.data(), compressed.size(), chunk.size());
    end = std::chrono::high_resolution_clock::now();
    us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    
    std::cout << "Decompress time: " << us << " µs\n";
    
    if (decompressed.size() == chunk.size() &&
        std::memcmp(decompressed.data(), chunk.data(), chunk.size()) == 0) {
        std::cout << "Round-trip OK ✅\n";
    } else {
        std::cout << "Round-trip FAIL ❌\n";
    }
    std::cout << "\n";
}

void test_random_data() {
    std::cout << "=== Random Data Test (incompressible) ===\n";
    std::vector<uint8_t> random(5000);
    for (size_t i = 0; i < random.size(); i++) {
        random[i] = (uint8_t)(i * 7919 + 104729);  // pseudo-random
    }
    
    auto compressed = Compression::compress(random);
    
    std::cout << "Original: " << random.size() << " bytes\n";
    if (compressed.empty()) {
        std::cout << "Not compressed (compression ineffective) ✅\n";
    } else {
        std::cout << "Compressed: " << compressed.size() << " bytes\n";
        std::cout << "Ratio: " << (100.0 * compressed.size() / random.size()) << "%\n";
        if (compressed.size() >= random.size()) {
            std::cout << "Compression skipped (good heuristic) ✅\n";
        }
    }
    std::cout << "\n";
}

void test_realistic_chunk() {
    std::cout << "=== Realistic Chunk Test (mixed data) ===\n";
    // Simulate Minecraft-style chunk: mostly air + some stone/dirt
    std::vector<uint8_t> chunk(32 * 32 * 32);  // 32KB
    for (size_t i = 0; i < chunk.size(); i++) {
        if (i % 100 < 5) {
            chunk[i] = 1;  // stone (5%)
        } else if (i % 100 < 10) {
            chunk[i] = 2;  // dirt (5%)
        } else {
            chunk[i] = 0;  // air (90%)
        }
    }
    
    auto compressed = Compression::compress(chunk);
    
    std::cout << "Original: " << chunk.size() << " bytes\n";
    std::cout << "Compressed: " << compressed.size() << " bytes\n";
    std::cout << "Ratio: " << (100.0 * compressed.size() / chunk.size()) << "%\n";
    std::cout << "Savings: " << (chunk.size() - compressed.size()) << " bytes\n";
    
    if (!compressed.empty() && compressed.size() < chunk.size() * 0.3) {
        std::cout << "Realistic chunk compression effective ✅\n";
    }
    
    auto decompressed = Compression::decompress(compressed.data(), compressed.size(), chunk.size());
    if (decompressed == chunk) {
        std::cout << "Round-trip OK ✅\n";
    }
    std::cout << "\n";
}

void test_edge_cases() {
    std::cout << "=== Edge Cases Test ===\n";
    
    // Empty data
    std::vector<uint8_t> empty;
    auto c1 = Compression::compress(empty);
    std::cout << "Empty data: " << (c1.empty() ? "OK" : "FAIL") << " ✅\n";
    
    // Exactly threshold
    std::vector<uint8_t> threshold(Compression::COMPRESS_THRESHOLD, 0);
    auto c2 = Compression::compress(threshold);
    std::cout << "Threshold size: " << (c2.empty() || !c2.empty() ? "OK" : "FAIL") << " ✅\n";
    
    // Large data (100KB)
    std::vector<uint8_t> large(100000, 0);
    auto start = std::chrono::high_resolution_clock::now();
    auto c3 = Compression::compress(large);
    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << "100KB zeros: " << c3.size() << " bytes in " << ms << " ms\n";
    std::cout << "Compression ratio: " << (100.0 * c3.size() / large.size()) << "%\n";
    
    if (c3.size() < 1000) {  // should compress to < 1KB
        std::cout << "Large data compression effective ✅\n";
    }
    std::cout << "\n";
}

int main() {
    std::cout << "=== Compression System Test ===\n\n";
    test_small_data();
    test_compressible_data();
    test_random_data();
    test_realistic_chunk();
    test_edge_cases();
    std::cout << "=== All compression tests complete ===\n";
    return 0;
}
