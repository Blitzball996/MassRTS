// =============================================================================
// Persistence Test - RegionFile + WorldStorage
// =============================================================================
#include "persistence/region_file.h"
#include "persistence/world_storage.h"
#include <iostream>
#include <cstring>
#include <filesystem>

using namespace net;
namespace fs = std::filesystem;

void test_region_file() {
    std::cout << "=== RegionFile Test ===\n";

    // Create test directory
    fs::create_directories("test_world/region");
    std::string region_path = "test_world/region/r.0.0.mca";

    // Delete existing file
    fs::remove(region_path);

    RegionFile region(region_path);

    // Write some chunks
    for (int i = 0; i < 5; i++) {
        std::vector<uint8_t> data(1000 + i * 100);
        for (size_t j = 0; j < data.size(); j++) {
            data[j] = (uint8_t)((i * 7919 + j) % 256);
        }
        region.writeChunk(i, i, data);
    }
    std::cout << "Wrote 5 chunks\n";

    // Read back and verify
    int ok_count = 0;
    for (int i = 0; i < 5; i++) {
        auto data = region.readChunk(i, i);
        if (data.size() == 1000 + i * 100) {
            bool match = true;
            for (size_t j = 0; j < data.size(); j++) {
                if (data[j] != (uint8_t)((i * 7919 + j) % 256)) {
                    match = false;
                    break;
                }
            }
            if (match) ok_count++;
        }
    }
    std::cout << "Read back: " << ok_count << "/5 chunks OK "
              << (ok_count == 5 ? "✅" : "❌") << "\n";

    // Test hasChunk
    bool has0 = region.hasChunk(0, 0);
    bool has10 = region.hasChunk(10, 10);
    std::cout << "hasChunk(0,0): " << (has0 ? "true" : "false") << " (expected true) "
              << (has0 ? "✅" : "❌") << "\n";
    std::cout << "hasChunk(10,10): " << (has10 ? "true" : "false") << " (expected false) "
              << (!has10 ? "✅" : "❌") << "\n";

    // Test delete
    region.deleteChunk(2, 2);
    bool has2_after = region.hasChunk(2, 2);
    std::cout << "After delete(2,2): " << (has2_after ? "still exists" : "deleted") << " "
              << (!has2_after ? "✅" : "❌") << "\n";

    // Reopen file and verify persistence
    {
        RegionFile region2(region_path);
        auto data = region2.readChunk(1, 1);
        bool ok = (data.size() == 1100);
        std::cout << "Reopen file: chunk(1,1) size=" << data.size() 
                  << " (expected 1100) " << (ok ? "✅" : "❌") << "\n";
    }

    std::cout << "\n";
}

void test_world_storage() {
    std::cout << "=== WorldStorage Test ===\n";

    // Clean test directory
    fs::remove_all("test_world2");
    fs::create_directories("test_world2");

    WorldStorage world("test_world2");
    world.create(123456789, "Test World");
    std::cout << "Created world: seed=" << world.seed() 
              << " name=\"" << world.name() << "\"\n";

    // Save chunks across multiple regions
    std::cout << "Saving chunks...\n";
    for (int cx = -10; cx <= 10; cx += 5) {
        for (int cz = -10; cz <= 10; cz += 5) {
            std::vector<uint8_t> data(500);
            for (size_t i = 0; i < data.size(); i++) {
                data[i] = (uint8_t)((cx + cz + i) % 256);
            }
            world.saveChunk(cx, cz, data);
        }
    }
    std::cout << "Saved 25 chunks\n";
    std::cout << "Dirty chunks: " << world.dirtyChunkCount() << "\n";
    std::cout << "Loaded regions: " << world.loadedRegionCount() << "\n";

    // Flush
    world.flush();
    std::cout << "Flushed (dirty count now: " << world.dirtyChunkCount() << ")\n";

    // Reload world
    {
        WorldStorage world2("test_world2");
        if (world2.load()) {
            std::cout << "Reloaded world: seed=" << world2.seed() 
                      << " name=\"" << world2.name() << "\" ✅\n";
        } else {
            std::cout << "Failed to reload world ❌\n";
        }

        // Verify chunks
        int found = 0;
        for (int cx = -10; cx <= 10; cx += 5) {
            for (int cz = -10; cz <= 10; cz += 5) {
                if (world2.hasChunk(cx, cz)) {
                    auto data = world2.loadChunk(cx, cz);
                    if (data.size() == 500) found++;
                }
            }
        }
        std::cout << "Found " << found << "/25 chunks " 
                  << (found == 25 ? "✅" : "❌") << "\n";
    }

    std::cout << "\n";
}

void test_large_world() {
    std::cout << "=== Large World Test ===\n";

    fs::remove_all("test_world3");
    WorldStorage world("test_world3");
    world.create(999, "Large Test");

    // Save 1000 chunks
    std::cout << "Saving 1000 chunks...\n";
    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < 1000; i++) {
        int cx = (i % 100) - 50;
        int cz = (i / 100) - 5;
        std::vector<uint8_t> data(2000);  // 2KB per chunk
        for (size_t j = 0; j < data.size(); j++) {
            data[j] = (uint8_t)((i + j) % 256);
        }
        world.saveChunk(cx, cz, data);
    }
    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << "Saved 1000 chunks in " << ms << " ms\n";
    std::cout << "Throughput: " << (1000.0 / ms * 1000) << " chunks/sec\n";
    std::cout << "Data rate: " << (2000.0 * 1000 / ms / 1024) << " KB/s\n";

    // Check disk usage
    size_t total_size = 0;
    for (const auto& entry : fs::recursive_directory_iterator("test_world3")) {
        if (entry.is_regular_file()) {
            total_size += entry.file_size();
        }
    }
    std::cout << "Disk usage: " << (total_size / 1024) << " KB\n";
    std::cout << "Compression ratio: " 
              << (100.0 * total_size / (1000 * 2000)) << "%\n";

    // Load random chunks
    start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < 100; i++) {
        int cx = (i % 10) * 10 - 50;
        int cz = (i / 10) - 5;
        auto data = world.loadChunk(cx, cz);
    }
    end = std::chrono::high_resolution_clock::now();
    ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << "Loaded 100 random chunks in " << ms << " ms\n";

    std::cout << "\n";
}

int main() {
    std::cout << "=== Persistence System Test ===\n\n";
    test_region_file();
    test_world_storage();
    test_large_world();
    std::cout << "=== All persistence tests complete ===\n";
    return 0;
}
