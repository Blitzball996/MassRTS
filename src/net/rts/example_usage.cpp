// =============================================================================
// RTS Network Stack Usage Example
// =============================================================================
// This demonstrates how to integrate the new RTS lockstep networking into
// the existing MassRTS game loop.
//
// Key changes from old session.h:
//   1. Uses net::rts::RtsNetEngine instead of NetSession
//   2. Commands use fixed-point positions (deterministic)
//   3. Terrain seed synchronized automatically
//   4. Packet system is extensible (easy to add new command types)
// =============================================================================

#include "net/rts/rts_net_engine.h"
#include <iostream>

// Placeholder game state
struct GameState {
    uint32_t sim_tick = 0;
    // ... units, terrain, etc
    
    void applyCommand(const net::rts::RtsCommand& cmd) {
        using namespace net::rts;
        float x = RtsCommand::fromFixed(cmd.target_x_fp);
        float z = RtsCommand::fromFixed(cmd.target_z_fp);
        
        switch (cmd.type) {
            case CmdType::Move:
                std::cout << "[Game] Move units " << cmd.unit_start << "-" 
                          << cmd.unit_end << " to (" << x << ", " << z << ")\n";
                // TODO: actual pathfinding + unit movement
                break;
            case CmdType::AttackMove:
                std::cout << "[Game] Attack-move to (" << x << ", " << z << ")\n";
                break;
            case CmdType::TerrainCarve:
                std::cout << "[Game] Carve terrain at (" << x << ", " << z 
                          << ") radius=" << cmd.param_b << "\n";
                // TODO: call sdf_terrain.carve()
                break;
            default:
                break;
        }
    }
    
    void step(float dt) {
        // Fixed timestep physics, AI, etc
        sim_tick++;
    }
    
    uint32_t computeChecksum() const {
        // FNV-1a hash of deterministic state (unit positions in fixed-point, hp, etc)
        uint32_t h = 2166136261u;
        // TODO: hash all unit positions/hp (must use fixed-point, not float!)
        return h;
    }
};

void example_host() {
    net::rts::RtsNetEngine net;
    if (!net.hostGame(27015, "HostPlayer")) {
        std::cerr << "Failed to host\n";
        return;
    }
    
    GameState game;
    const float FIXED_DT = 1.0f / 30.0f;  // 30 Hz tick
    
    // Game loop (pseudo-code)
    while (true) {
        net.poll();  // receive packets
        
        // Example: player right-clicks at (100, 200) with units 0-50 selected
        // net.queueLocalCommand(game.sim_tick, net::rts::CmdType::Move,
        //                       100.0f, 200.0f, 0, 50);
        
        // Lockstep barrier: only advance when all players' commands arrived
        if (net.canAdvanceTick(game.sim_tick)) {
            auto cmds = net.getCommandsForTick(game.sim_tick);
            for (auto& cmd : cmds) {
                game.applyCommand(cmd);
            }
            game.step(FIXED_DT);
            net.advanceTick(game.sim_tick);
            
            // Periodic sync check (every 60 ticks = 2 sec)
            if (game.sim_tick % 60 == 0) {
                net.reportChecksum(game.sim_tick, game.computeChecksum());
            }
        } else {
            // Waiting for other players → send empty confirm to keep lockstep alive
            net.confirmEmptyTick(game.sim_tick);
        }
        
        // TODO: rendering (interpolate between sim_tick and sim_tick+1 for smooth 60fps)
        
        std::this_thread::sleep_for(std::chrono::milliseconds(16));  // ~60fps render
    }
}

void example_client() {
    net::rts::RtsNetEngine net;
    if (!net.joinGame("127.0.0.1", 27015, "ClientPlayer")) {
        std::cerr << "Failed to join\n";
        return;
    }
    
    GameState game;
    const float FIXED_DT = 1.0f / 30.0f;
    
    // Wait for accept packet (receives terrain seed, player id, faction)
    while (net.localPlayerId() == 0) {
        net.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    std::cout << "Connected! Player ID=" << (int)net.localPlayerId()
              << " Faction=" << (int)net.localFaction()
              << " Seed=" << net.terrainSeed() << "\n";
    
    // TODO: generate terrain using net.terrainSeed() → ensures identical map on all clients
    
    // Same game loop as host
    while (true) {
        net.poll();
        
        if (net.canAdvanceTick(game.sim_tick)) {
            auto cmds = net.getCommandsForTick(game.sim_tick);
            for (auto& cmd : cmds) game.applyCommand(cmd);
            game.step(FIXED_DT);
            net.advanceTick(game.sim_tick);
            
            if (game.sim_tick % 60 == 0) {
                net.reportChecksum(game.sim_tick, game.computeChecksum());
            }
        } else {
            net.confirmEmptyTick(game.sim_tick);
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "host") {
        example_host();
    } else if (argc > 1 && std::string(argv[1]) == "client") {
        example_client();
    } else {
        std::cout << "Usage: " << argv[0] << " [host|client]\n";
    }
    return 0;
}
