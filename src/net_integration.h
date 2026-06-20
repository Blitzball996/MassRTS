#pragma once
// Network integration bridge between MassRTS game and net::rts::RtsNetEngine.
// Implements fixed-timestep lockstep: the sim only advances when ALL players
// have confirmed a tick. Empty ticks must be confirmed every step or the
// session deadlocks.
#include "net/rts/rts_net_engine.h"
#include "ecs/world.h"
#include <string>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <thread>

enum class NetworkMode { Offline, Host, Client };

struct NetworkState {
    NetworkMode mode = NetworkMode::Offline;
    net::rts::RtsNetEngine* engine = nullptr;

    // Lockstep runs at a fixed rate, decoupled from render framerate.
    static constexpr double TICK_HZ = 30.0;
    static constexpr double TICK_DT = 1.0 / TICK_HZ;
    double tick_accum = 0.0;
    uint32_t current_sim_tick = 0;     // next tick we want to simulate
    uint32_t confirmed_upto = 0;       // highest future tick we've sent a confirm for
    std::string connection_status;

    ~NetworkState() {
        if (engine) { engine->disconnect(); delete engine; }
    }

    bool isMultiplayer() const { return mode != NetworkMode::Offline; }

    // Deterministic-start accessors (valid after host/connect handshake).
    uint64_t terrainSeed()  const { return engine ? engine->terrainSeed() : 0; }
    uint8_t  localFaction() const { return engine ? engine->localFaction() : 0; }
    uint8_t  localPlayerId()const { return engine ? engine->localPlayerId() : 0; }
    int      playerCount()  const { return engine ? engine->playerCount() : 1; }

    bool waitForHandshake(double timeout_sec = 5.0) {
        if (mode != NetworkMode::Client || !engine) return true;
        auto start = std::chrono::steady_clock::now();
        while (engine->terrainSeed() == 0) {
            engine->poll();
            auto el = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            if (el > timeout_sec) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return true;
    }

    bool startHost(uint16_t port, const std::string& player_name) {
        engine = new net::rts::RtsNetEngine();
        if (!engine->hostGame(port, player_name)) {
            delete engine; engine = nullptr;
            connection_status = "Failed to host on port " + std::to_string(port);
            return false;
        }
        mode = NetworkMode::Host;
        connection_status = "Hosting on port " + std::to_string(port);
        return true;
    }

    bool startClient(const std::string& ip, uint16_t port, const std::string& player_name) {
        engine = new net::rts::RtsNetEngine();
        if (!engine->joinGame(ip, port, player_name)) {
            delete engine; engine = nullptr;
            connection_status = "Failed to connect to " + ip + ":" + std::to_string(port);
            return false;
        }
        mode = NetworkMode::Client;
        connection_status = "Connected to " + ip + ":" + std::to_string(port);
        return true;
    }

    // Called once per render frame. Drives the fixed-timestep lockstep loop.
    void update(World& world, float dt) {
        if (!engine) return;
        engine->poll();

        tick_accum += dt;
        // Process as many fixed ticks as elapsed time allows.
        while (tick_accum >= TICK_DT) {
            // Confirm all ticks up to current+INPUT_DELAY so other peers can advance.
            uint32_t horizon = current_sim_tick + net::rts::RtsNetEngine::INPUT_DELAY;
            while (confirmed_upto <= horizon) {
                engine->confirmEmptyTick(confirmed_upto);
                confirmed_upto++;
            }
            engine->poll();

            if (!engine->canAdvanceTick(current_sim_tick)) {
                // Waiting on a peer's command for this tick — stall, don't desync.
                break;
            }
            auto cmds = engine->getCommandsForTick(current_sim_tick);
            for (const auto& c : cmds) applyCommand(world, c);
            engine->advanceTick(current_sim_tick);
            current_sim_tick++;
            tick_accum -= TICK_DT;
        }
    }

    // Queue local player's move command for the affected unit-id range.
    void queueMoveCommand(glm::vec2 target, uint32_t unit_start, uint32_t unit_end) {
        if (!engine) return;
        engine->queueLocalCommand(current_sim_tick, net::rts::CmdType::Move,
                                  target.x, target.y, unit_start, unit_end);
    }

    // Apply a network command to the sim. Mirrors main.cpp RMB formation logic.
    void applyCommand(World& world, const net::rts::RtsCommand& cmd) {
        switch (cmd.type) {
            case net::rts::CmdType::Move:
            case net::rts::CmdType::AttackMove: {
                glm::vec2 target(net::rts::RtsCommand::fromFixed(cmd.target_x_fp),
                                 net::rts::RtsCommand::fromFixed(cmd.target_z_fp));
                uint32_t lo = cmd.unit_start;
                uint32_t hi = cmd.unit_end < world.entity_count ? cmd.unit_end : world.entity_count;
                uint32_t n = (hi > lo) ? (hi - lo) : 0;
                int cols = n > 0 ? std::max(1, (int)std::sqrt((float)n)) : 1;
                float spacing = 6.0f;
                uint32_t k = 0;
                for (uint32_t i = lo; i < hi; i++) {
                    if (!world.is_alive(i)) continue;
                    int row = (int)k / cols, col = (int)k % cols;
                    float ox = ((float)col - cols * 0.5f) * spacing;
                    float oy = ((float)row - (float)(n / cols) * 0.5f) * spacing;
                    world.commands.move_target[i] = target + glm::vec2(ox, oy);
                    world.commands.has_move_command[i] = true;
                    world.units.target[i] = INVALID_ENTITY;
                    world.units.state[i] = UnitState::Moving;
                    k++;
                }
                break;
            }
            default:
                break;
        }
    }
};
