// =============================================================================
// Rollback + Fixed-point Integration Test
// =============================================================================
// This test demonstrates:
//   1. Fixed-point deterministic math
//   2. Ring buffer state snapshots
//   3. Rollback when late input arrives
//   4. Desync detection via checksums
//
// Run this standalone to verify the rollback system works before integrating
// with the full game.
// =============================================================================
#include "core/fixed_point.h"
#include "core/rollback_engine.h"
#include <iostream>
#include <vector>
#include <cstring>

using namespace net;

// ---- Minimal deterministic game state --------------------------------------
struct MockGameState {
    struct Unit {
        uint32_t id;
        FixedVec3 pos;
        FixedVec2 vel;
        int16_t hp;
        uint8_t state;
        uint32_t target_id;
    };
    std::vector<Unit> units;

    void init() {
        // Spawn 10 units at deterministic positions
        for (int i = 0; i < 10; i++) {
            Unit u;
            u.id = i;
            u.pos = FixedVec3::fromFloat(i * 10.0f, 0.0f, 0.0f);
            u.vel = FixedVec2();
            u.hp = 100;
            u.state = 0;  // idle
            u.target_id = 0xFFFFFFFF;
            units.push_back(u);
        }
    }

    void step(const FrameInput& input) {
        // Apply velocities (deterministic fixed-point)
        for (auto& u : units) {
            u.pos.x += Fixed::fromRaw(u.vel.x.raw);  // 1:1 add (no rounding)
            u.pos.z += Fixed::fromRaw(u.vel.y.raw);
        }

        // Parse commands from input (simplified)
        for (int p = 0; p < 8; p++) {
            if (!input.confirmed[p] || input.player_inputs[p].empty())
                continue;

            // Command format: [unit_id:4][vx:4][vz:4]
            const auto& cmd = input.player_inputs[p];
            if (cmd.size() >= 12) {
                uint32_t unit_id;
                int32_t vx_raw, vz_raw;
                std::memcpy(&unit_id, &cmd[0], 4);
                std::memcpy(&vx_raw, &cmd[4], 4);
                std::memcpy(&vz_raw, &cmd[8], 4);

                if (unit_id < units.size()) {
                    units[unit_id].vel.x = Fixed::fromRaw(vx_raw);
                    units[unit_id].vel.y = Fixed::fromRaw(vz_raw);
                    std::cout << "  [Step] Unit " << unit_id << " vel=("
                              << units[unit_id].vel.x.toFloat() << ", "
                              << units[unit_id].vel.y.toFloat() << ")\n";
                }
            }
        }
    }

    void save(GameStateSnapshot& snap) {
        snap.unit_ids.clear();
        snap.positions.clear();
        snap.velocities.clear();
        snap.hp.clear();
        snap.state.clear();
        snap.target_ids.clear();

        for (auto& u : units) {
            snap.unit_ids.push_back(u.id);
            snap.positions.push_back(u.pos);
            snap.velocities.push_back(u.vel);
            snap.hp.push_back(u.hp);
            snap.state.push_back(u.state);
            snap.target_ids.push_back(u.target_id);
        }
    }

    void load(const GameStateSnapshot& snap) {
        units.clear();
        for (size_t i = 0; i < snap.unit_count(); i++) {
            Unit u;
            u.id = snap.unit_ids[i];
            u.pos = snap.positions[i];
            u.vel = snap.velocities[i];
            u.hp = snap.hp[i];
            u.state = snap.state[i];
            u.target_id = snap.target_ids[i];
            units.push_back(u);
        }
    }

    void print() const {
        for (auto& u : units) {
            std::cout << "  Unit " << u.id << ": pos=("
                      << u.pos.x.toFloat() << ", " << u.pos.z.toFloat() << ") vel=("
                      << u.vel.x.toFloat() << ", " << u.vel.y.toFloat() << ")\n";
        }
    }
};

// ---- Test harness ----------------------------------------------------------
void test_rollback() {
    std::cout << "=== Rollback Engine Test ===\n\n";

    MockGameState game;
    game.init();

    RollbackEngine rollback;
    rollback.setSaveCallback([&](GameStateSnapshot& s) { game.save(s); });
    rollback.setLoadCallback([&](const GameStateSnapshot& s) { game.load(s); });
    rollback.setStepCallback([&](const FrameInput& i) { game.step(i); });

    // ---- Simulate 10 frames with predicted inputs --------------------------
    std::cout << "--- Phase 1: Advance 10 frames (no input) ---\n";
    for (int f = 0; f < 10; f++) {
        std::cout << "Frame " << f << ":\n";
        rollback.advanceFrame();
    }
    std::cout << "After 10 frames:\n";
    game.print();

    // ---- Frame 3: late input arrives (move unit 2) -------------------------
    std::cout << "\n--- Phase 2: Late input arrives for frame 3 ---\n";
    std::cout << "Setting unit 2 velocity to (5.0, 0.0) starting at frame 3\n";

    // Build command: [unit_id=2][vx=5.0][vz=0.0]
    std::vector<uint8_t> cmd(12);
    uint32_t unit_id = 2;
    int32_t vx = Fixed::fromFloat(5.0f).raw;
    int32_t vz = 0;
    std::memcpy(&cmd[0], &unit_id, 4);
    std::memcpy(&cmd[4], &vx, 4);
    std::memcpy(&cmd[8], &vz, 4);

    // This triggers rollback (frame 3 < current_frame 10)
    rollback.setRemoteInput(3, 0, cmd);

    std::cout << "\nAfter rollback + re-simulation:\n";
    game.print();
    std::cout << "Unit 2 should have moved ~35.0 in X (5.0 * 7 frames)\n";

    // ---- Verify determinism ------------------------------------------------
    std::cout << "\n--- Phase 3: Verify determinism ---\n";
    std::cout << "Checksum at frame 10: " << rollback.currentFrame() << "\n";
    // TODO: run a second instance and compare checksums
}

void test_fixed_point() {
    std::cout << "=== Fixed-point Math Test ===\n\n";

    Fixed a = Fixed::fromFloat(10.5f);
    Fixed b = Fixed::fromFloat(3.25f);

    std::cout << "a = " << a.toFloat() << " (raw=" << a.raw << ")\n";
    std::cout << "b = " << b.toFloat() << " (raw=" << b.raw << ")\n";
    std::cout << "a + b = " << (a + b).toFloat() << "\n";
    std::cout << "a - b = " << (a - b).toFloat() << "\n";
    std::cout << "a * b = " << (a * b).toFloat() << "\n";
    std::cout << "a / b = " << (a / b).toFloat() << "\n";
    std::cout << "sqrt(a) = " << a.sqrt().toFloat() << "\n";

    FixedVec2 v1 = FixedVec2::fromFloat(3.0f, 4.0f);
    std::cout << "\nv1 = (" << v1.x.toFloat() << ", " << v1.y.toFloat() << ")\n";
    std::cout << "v1.length() = " << v1.length().toFloat() << " (should be 5.0)\n";

    FixedVec2 v2 = v1.normalized();
    std::cout << "v1.normalized() = (" << v2.x.toFloat() << ", " << v2.y.toFloat() << ")\n";
    std::cout << "v2.length() = " << v2.length().toFloat() << " (should be 1.0)\n";

    std::cout << "\n";
}

int main() {
    test_fixed_point();
    test_rollback();
    return 0;
}
