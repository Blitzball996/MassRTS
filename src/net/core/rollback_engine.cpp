#include "rollback_engine.h"
#include <iostream>
#include <cstring>

namespace net {

RollbackEngine::RollbackEngine() {
    // Pre-allocate ring buffer (zero runtime allocation during rollback)
    for (auto& snap : history) {
        snap.unit_ids.reserve(2000);
        snap.positions.reserve(2000);
        snap.velocities.reserve(2000);
        snap.hp.reserve(2000);
        snap.state.reserve(2000);
        snap.target_ids.reserve(2000);
    }
}

void RollbackEngine::advanceFrame() {
    // 1. Save current state to ring buffer (before simulation)
    auto& snap = getSnapshot(current_frame);
    snap.frame = current_frame;
    if (save_state) {
        save_state(snap);
    }
    snap.checksum = snap.compute_checksum();

    // 2. Get input for this frame (predicted if remote not arrived yet)
    auto& input = getInput(current_frame);
    input.frame = current_frame;
    // Input is set by setLocalInput/setRemoteInput; if missing, use empty (prediction)

    // 3. Step game simulation
    if (step_game) {
        step_game(input);
    }

    current_frame++;
}

void RollbackEngine::setLocalInput(uint32_t frame, uint8_t player, 
                                    const std::vector<uint8_t>& input) {
    if (frame >= current_frame + ROLLBACK_WINDOW) return;  // too far in future
    auto& inp = getInput(frame);
    inp.player_inputs[player] = input;
    inp.confirmed[player] = true;
}

void RollbackEngine::setRemoteInput(uint32_t frame, uint8_t player,
                                     const std::vector<uint8_t>& input) {
    if (frame >= current_frame) {
        // Future input → store for later
        auto& inp = getInput(frame);
        inp.player_inputs[player] = input;
        inp.confirmed[player] = true;
        return;
    }

    // Past frame → check if we predicted wrong
    auto& inp = getInput(frame);
    if (inp.confirmed[player]) {
        // Already had real input, no rollback needed
        return;
    }

    // We predicted this frame, but now real input arrived → ROLLBACK
    inp.player_inputs[player] = input;
    inp.confirmed[player] = true;

    std::cout << "[Rollback] Frame " << frame << " predicted wrong, rolling back from "
              << current_frame << " to " << frame << "\n";
    rollbackTo(frame);
}

void RollbackEngine::rollbackTo(uint32_t frame) {
    if (frame >= current_frame) return;  // can't rollback to future
    if (current_frame - frame > ROLLBACK_WINDOW) {
        std::cerr << "[Rollback] ERROR: rollback distance " << (current_frame - frame)
                  << " exceeds window " << ROLLBACK_WINDOW << "\n";
        return;  // too far, would corrupt ring buffer
    }

    uint32_t saved_frame = current_frame;

    // 1. Restore state to rollback point
    const auto& snap = getSnapshot(frame);
    if (load_state) {
        load_state(snap);
    }
    current_frame = frame;

    // 2. Re-simulate forward to present with corrected inputs
    while (current_frame < saved_frame) {
        // Re-save snapshot (inputs are now corrected)
        auto& new_snap = getSnapshot(current_frame);
        new_snap.frame = current_frame;
        if (save_state) {
            save_state(new_snap);
        }
        new_snap.checksum = new_snap.compute_checksum();

        // Re-simulate with real input
        auto& input = getInput(current_frame);
        if (step_game) {
            step_game(input);
        }
        current_frame++;
    }

    std::cout << "[Rollback] Re-simulated " << (saved_frame - frame) 
              << " frames, back to present\n";
}

bool RollbackEngine::hasAllInputs(uint32_t frame) const {
    const auto& inp = input_history[frame % FRAME_HISTORY];
    // Check all active players confirmed (TODO: track active player count)
    for (int i = 0; i < 2; i++) {  // assume 2 players for now
        if (!inp.confirmed[i]) return false;
    }
    return true;
}

void RollbackEngine::checkDesync(uint32_t frame, uint32_t remote_checksum) {
    if (frame >= current_frame) return;  // future frame
    const auto& snap = getSnapshot(frame);
    if (snap.checksum != remote_checksum) {
        std::cerr << "[DESYNC] Frame " << frame << " local=" << snap.checksum
                  << " remote=" << remote_checksum << "\n";
        // TODO: trigger desync recovery (request full state from server)
    }
}

} // namespace net
