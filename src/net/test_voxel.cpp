// =============================================================================
// Voxel Network Test - server/client voxel sync
// =============================================================================
// Tests the voxel sandbox networking:
//   1. Voxel coordinate packing/unpacking
//   2. Packet serialization round-trip
//   3. 4-byte player move delta packing
//   4. Edit batch
// =============================================================================
#include "voxel/voxel_packets.h"
#include <iostream>
#include <cassert>

using namespace net;
using namespace net::voxel;

void test_voxel_packing() {
    std::cout << "=== Voxel Coordinate Packing Test ===\n\n";

    struct TestCase { int32_t x, y, z; };
    TestCase cases[] = {
        {0, 0, 0},
        {100, 64, 200},
        {-50, 32, -100},
        {1000, 255, -1000},
        {-1, -1, -1},
    };

    for (auto& tc : cases) {
        int64_t key = VoxelEdit::pack(tc.x, tc.y, tc.z);
        int32_t x, y, z;
        VoxelEdit::unpack(key, x, y, z);
        std::cout << "(" << tc.x << ", " << tc.y << ", " << tc.z << ") -> key=" 
                  << key << " -> (" << x << ", " << y << ", " << z << ") ";
        if (x == tc.x && y == tc.y && z == tc.z) {
            std::cout << "OK\n";
        } else {
            std::cout << "FAIL!\n";
        }
    }
    std::cout << "\n";
}

void test_packet_roundtrip() {
    std::cout << "=== Packet Serialization Round-trip Test ===\n\n";

    // Test VoxelChunkDataPacket
    {
        VoxelChunkDataPacket pkt;
        pkt.cx = 10; pkt.cy = 2; pkt.cz = -5;
        for (int i = 0; i < 5; i++) {
            VoxelEdit e;
            e.voxel_key = VoxelEdit::pack(i, 64, i * 2);
            e.block_type = (uint8_t)(i + 1);
            e.flags = 1;
            pkt.edits.push_back(e);
        }

        ByteBuffer buf;
        pkt.write(buf);
        std::cout << "VoxelChunkDataPacket: " << buf.size() << " bytes for 5 edits\n";

        VoxelChunkDataPacket decoded;
        decoded.read(buf);
        bool ok = (decoded.cx == 10 && decoded.cy == 2 && decoded.cz == -5 
                   && decoded.edits.size() == 5);
        for (size_t i = 0; i < decoded.edits.size(); i++) {
            if (decoded.edits[i].block_type != (uint8_t)(i + 1)) ok = false;
        }
        std::cout << "  Round-trip: " << (ok ? "OK" : "FAIL") << "\n";
    }

    // Test VoxelPlayerMovePacket (4-byte delta)
    {
        auto pkt = VoxelPlayerMovePacket::fromDelta(42, 1.5f, 0.0f, -2.0f, 90.0f);
        ByteBuffer buf;
        pkt.write(buf);
        std::cout << "VoxelPlayerMovePacket: " << buf.size() 
                  << " bytes (should be 4)\n";

        VoxelPlayerMovePacket decoded;
        decoded.read(buf);
        uint32_t player_id = decoded.id_and_yrot & 0x7FF;
        std::cout << "  Decoded player_id=" << player_id << " (should be 42)\n";
        std::cout << "  Round-trip: " << (player_id == 42 ? "OK" : "FAIL") << "\n";
    }

    // Test invalidation (bandwidth optimization)
    {
        auto p1 = VoxelPlayerMovePacket::fromDelta(5, 1.0f, 0, 0, 0);
        auto p2 = VoxelPlayerMovePacket::fromDelta(5, 2.0f, 0, 0, 0);
        auto p3 = VoxelPlayerMovePacket::fromDelta(7, 1.0f, 0, 0, 0);
        std::cout << "Invalidation test:\n";
        std::cout << "  p2 invalidates p1 (same player 5): " 
                  << (p1.isInvalidatedBy(p2) ? "OK" : "FAIL") << "\n";
        std::cout << "  p3 does NOT invalidate p1 (player 7 vs 5): " 
                  << (!p1.isInvalidatedBy(p3) ? "OK" : "FAIL") << "\n";
    }
    std::cout << "\n";
}

void test_edit_batch() {
    std::cout << "=== Edit Batch Test ===\n\n";
    VoxelEditBatchPacket pkt;
    pkt.cx = 1; pkt.cz = 2;
    pkt.count = 3;
    pkt.positions = {100, 200, 300};
    pkt.block_types = {1, 2, 3};

    ByteBuffer buf;
    pkt.write(buf);
    std::cout << "VoxelEditBatchPacket: " << buf.size() << " bytes for 3 blocks\n";

    VoxelEditBatchPacket decoded;
    decoded.read(buf);
    bool ok = (decoded.count == 3 && decoded.positions[1] == 200 
               && decoded.block_types[2] == 3);
    std::cout << "  Round-trip: " << (ok ? "OK" : "FAIL") << "\n\n";
}

int main() {
    registerVoxelPackets();
    test_voxel_packing();
    test_packet_roundtrip();
    test_edit_batch();
    std::cout << "=== All voxel tests complete ===\n";
    return 0;
}
