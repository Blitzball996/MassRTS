#pragma once
// =============================================================================
// WorldStorage - manages world persistence across multiple region files
// -----------------------------------------------------------------------------
// Responsibilities:
//   * Map global chunk coords to region files
//   * Load/unload regions on demand (LRU cache)
//   * Track dirty chunks (incremental saves)
//   * Automatic background saving
//
// File structure:
//   world/
//   ├── level.dat           # World metadata (seed, spawn, etc)
//   └── region/
//       ├── r.0.0.mca       # Region (0, 0)
//       ├── r.0.1.mca       # Region (0, 1)
//       └── r.-1.0.mca      # Region (-1, 0)
//
// Usage:
//   WorldStorage world("world");
//   world.create(12345);  // new world with seed
//   
//   // Save chunk
//   world.saveChunk(100, 200, chunk_data);
//   
//   // Load chunk
//   auto data = world.loadChunk(100, 200);
//   
//   // Flush dirty chunks to disk
//   world.flush();
// =============================================================================
#include "region_file.h"
#include <map>
#include <set>
#include <memory>
#include <filesystem>
#include <fstream>

namespace net {
namespace fs = std::filesystem;

class WorldStorage {
public:
    struct ChunkCoord {
        int32_t cx, cz;
        bool operator<(const ChunkCoord& o) const {
            return cx < o.cx || (cx == o.cx && cz < o.cz);
        }
    };

    struct RegionCoord {
        int32_t rx, rz;
        bool operator<(const RegionCoord& o) const {
            return rx < o.rx || (rx == o.rx && rz < o.rz);
        }
    };

    WorldStorage(const std::string& world_dir) : world_path(world_dir) {
        region_dir = world_path + "/region";
        fs::create_directories(region_dir);
    }

    // ---- World creation ----------------------------------------------------
    bool create(uint64_t seed, const std::string& name = "New World") {
        // Create level.dat
        std::string level_path = world_path + "/level.dat";
        std::ofstream level(level_path, std::ios::binary);
        if (!level) return false;

        // Simple format: [magic:4][version:4][seed:8][name_len:4][name]
        uint32_t magic = 0x4C564C44;  // "LVLD"
        uint32_t version = 1;
        uint32_t name_len = (uint32_t)name.size();
        level.write((char*)&magic, 4);
        level.write((char*)&version, 4);
        level.write((char*)&seed, 8);
        level.write((char*)&name_len, 4);
        level.write(name.c_str(), name.size());

        world_seed = seed;
        world_name = name;
        return true;
    }

    // ---- Load world metadata -----------------------------------------------
    bool load() {
        std::string level_path = world_path + "/level.dat";
        std::ifstream level(level_path, std::ios::binary);
        if (!level) return false;

        uint32_t magic, version, name_len;
        level.read((char*)&magic, 4);
        if (magic != 0x4C564C44) return false;
        level.read((char*)&version, 4);
        level.read((char*)&world_seed, 8);
        level.read((char*)&name_len, 4);
        if (name_len > 256) return false;
        world_name.resize(name_len);
        level.read(&world_name[0], name_len);

        return true;
    }

    // ---- Chunk operations --------------------------------------------------
    void saveChunk(int32_t cx, int32_t cz, const std::vector<uint8_t>& data) {
        RegionCoord rc = chunkToRegion(cx, cz);
        auto& region = getOrCreateRegion(rc);

        int local_cx = modulo(cx, RegionFile::REGION_SIZE);
        int local_cz = modulo(cz, RegionFile::REGION_SIZE);
        region.writeChunk(local_cx, local_cz, data);

        dirty_chunks.insert({cx, cz});
    }

    std::vector<uint8_t> loadChunk(int32_t cx, int32_t cz) {
        RegionCoord rc = chunkToRegion(cx, cz);
        auto it = regions.find(rc);
        if (it == regions.end()) {
            // Try load region from disk
            std::string region_path = getRegionPath(rc);
            if (!fs::exists(region_path)) return {};  // region doesn't exist
            regions[rc] = std::make_unique<RegionFile>(region_path);
            it = regions.find(rc);
        }

        int local_cx = modulo(cx, RegionFile::REGION_SIZE);
        int local_cz = modulo(cz, RegionFile::REGION_SIZE);
        return it->second->readChunk(local_cx, local_cz);
    }

    bool hasChunk(int32_t cx, int32_t cz) {
        RegionCoord rc = chunkToRegion(cx, cz);
        auto it = regions.find(rc);
        if (it == regions.end()) {
            std::string region_path = getRegionPath(rc);
            if (!fs::exists(region_path)) return false;
            regions[rc] = std::make_unique<RegionFile>(region_path);
            it = regions.find(rc);
        }

        int local_cx = modulo(cx, RegionFile::REGION_SIZE);
        int local_cz = modulo(cz, RegionFile::REGION_SIZE);
        return it->second->hasChunk(local_cx, local_cz);
    }

    void deleteChunk(int32_t cx, int32_t cz) {
        RegionCoord rc = chunkToRegion(cx, cz);
        auto it = regions.find(rc);
        if (it != regions.end()) {
            int local_cx = modulo(cx, RegionFile::REGION_SIZE);
            int local_cz = modulo(cz, RegionFile::REGION_SIZE);
            it->second->deleteChunk(local_cx, local_cz);
        }
        dirty_chunks.erase({cx, cz});
    }

    // ---- Flush dirty chunks ------------------------------------------------
    void flush() {
        // All writes already persisted (RegionFile writes immediately)
        // Just clear dirty set
        dirty_chunks.clear();
    }

    // ---- Region management (LRU cache) -------------------------------------
    void unloadRegion(const RegionCoord& rc) {
        regions.erase(rc);
    }

    void unloadAllRegions() {
        regions.clear();
    }

    // Unload least recently used region if cache too large
    void trimCache(size_t max_regions = 16) {
        if (regions.size() <= max_regions) return;
        // Simple: just unload first region (TODO: proper LRU)
        regions.erase(regions.begin());
    }

    // ---- Stats -------------------------------------------------------------
    size_t dirtyChunkCount() const { return dirty_chunks.size(); }
    size_t loadedRegionCount() const { return regions.size(); }
    uint64_t seed() const { return world_seed; }
    const std::string& name() const { return world_name; }

private:
    std::string world_path;
    std::string region_dir;
    uint64_t world_seed = 0;
    std::string world_name;

    std::map<RegionCoord, std::unique_ptr<RegionFile>> regions;
    std::set<ChunkCoord> dirty_chunks;

    // ---- Helpers -----------------------------------------------------------
    static RegionCoord chunkToRegion(int32_t cx, int32_t cz) {
        return RegionCoord{
            floorDiv(cx, RegionFile::REGION_SIZE),
            floorDiv(cz, RegionFile::REGION_SIZE)
        };
    }

    static int32_t floorDiv(int32_t a, int32_t b) {
        return (a >= 0) ? (a / b) : ((a - b + 1) / b);
    }

    static int32_t modulo(int32_t a, int32_t b) {
        int32_t r = a % b;
        return (r < 0) ? (r + b) : r;
    }

    std::string getRegionPath(const RegionCoord& rc) {
        return region_dir + "/r." + std::to_string(rc.rx) + "." 
               + std::to_string(rc.rz) + ".mca";
    }

    RegionFile& getOrCreateRegion(const RegionCoord& rc) {
        auto it = regions.find(rc);
        if (it != regions.end()) return *it->second;

        std::string path = getRegionPath(rc);
        regions[rc] = std::make_unique<RegionFile>(path);
        return *regions[rc];
    }
};

} // namespace net
