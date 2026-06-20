#pragma once
// =============================================================================
// Packet - base class for all network packets (RTS + Voxel modes)
// -----------------------------------------------------------------------------
// Architecture ported from MinecraftConsoles Packet.h:
//   * Each packet type has a unique ID
//   * Factory pattern: id → create() function
//   * Direction validation: clientReceivedPackets / serverReceivedPackets
//   * Invalidation mechanism: old MoveEntity packets can be discarded when
//     a newer one for the same entity arrives (bandwidth optimization)
//
// Two sync stacks (RTS lockstep + voxel server-authoritative) share this
// transport layer but use different packet ID ranges:
//   RTS:   1-99
//   Voxel: 100-255
// =============================================================================
#include "byte_buffer.h"
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <typeinfo>
#include <functional>

namespace net {

class PacketHandler;  // forward decl

class Packet {
public:
    virtual ~Packet() = default;

    // ---- Core interface (pure virtual) -------------------------------------
    virtual int getId() const = 0;
    virtual void read(ByteBuffer& buf) = 0;
    virtual void write(ByteBuffer& buf) const = 0;
    virtual void handle(PacketHandler& handler) = 0;
    virtual int estimatedSize() const = 0;

    // ---- Optional: packet invalidation (bandwidth opt) ---------------------
    // If canBeInvalidated() returns true, the send queue can discard this
    // packet if a newer isInvalidatedBy() packet arrives for the same entity.
    // Example: old MoveEntityPacketSmall discarded when new one queued.
    // (See MinecraftConsoles MoveEntityPacketSmall.cpp:63-72)
    virtual bool canBeInvalidated() const { return false; }
    virtual bool isInvalidatedBy(const Packet& other) const { 
        (void)other; return false; 
    }

    // ---- Packet registration (global registry) -----------------------------
    using FactoryFunc = std::function<std::unique_ptr<Packet>()>;

    static void registerPacket(int id, bool clientRecv, bool serverRecv,
                                bool sendToAnyClient, FactoryFunc factory);
    static std::unique_ptr<Packet> create(int id);
    static bool canReceive(int id, bool isServer);
    static bool canSendToAnyClient(int id);

    // Metadata for debugging/stats
    const int64_t createTime = 0;  // could use steady_clock for latency tracking
    bool shouldDelay = false;      // true → slow queue (chunks), false → fast queue (player input)

private:
    static std::unordered_map<int, FactoryFunc>& idToFactory();
    static std::unordered_set<int>& clientPackets();
    static std::unordered_set<int>& serverPackets();
    static std::unordered_set<int>& anyClientPackets();
};

// =============================================================================
// PacketHandler - double-dispatch visitor for packet handling
// -----------------------------------------------------------------------------
// Each sync stack (RTS / Voxel) implements this interface to handle its packets.
// =============================================================================
class PacketHandler {
public:
    virtual ~PacketHandler() = default;
    // Each concrete packet type calls the appropriate handler method.
    // Subclasses implement the methods for packets they care about.
    // (We'll add handleXXX methods as we define concrete packet types)
};

// =============================================================================
// Packet registry implementation (static storage)
// =============================================================================
inline std::unordered_map<int, Packet::FactoryFunc>& Packet::idToFactory() {
    static std::unordered_map<int, FactoryFunc> map;
    return map;
}
inline std::unordered_set<int>& Packet::clientPackets() {
    static std::unordered_set<int> set;
    return set;
}
inline std::unordered_set<int>& Packet::serverPackets() {
    static std::unordered_set<int> set;
    return set;
}
inline std::unordered_set<int>& Packet::anyClientPackets() {
    static std::unordered_set<int> set;
    return set;
}

inline void Packet::registerPacket(int id, bool clientRecv, bool serverRecv,
                                    bool sendToAnyClient, FactoryFunc factory) {
    idToFactory()[id] = std::move(factory);
    if (clientRecv) clientPackets().insert(id);
    if (serverRecv) serverPackets().insert(id);
    if (sendToAnyClient) anyClientPackets().insert(id);
}

inline std::unique_ptr<Packet> Packet::create(int id) {
    auto& map = idToFactory();
    auto it = map.find(id);
    if (it == map.end()) return nullptr;
    return it->second();
}

inline bool Packet::canReceive(int id, bool isServer) {
    if (isServer) return serverPackets().count(id) > 0;
    else          return clientPackets().count(id) > 0;
}

inline bool Packet::canSendToAnyClient(int id) {
    return anyClientPackets().count(id) > 0;
}

// =============================================================================
// Helper macro for packet registration (reduces boilerplate)
// =============================================================================
#define REGISTER_PACKET(ID, ClientRecv, ServerRecv, SendAnyClient, ClassType) \
    namespace { \
        struct ClassType##Registrar { \
            ClassType##Registrar() { \
                net::Packet::registerPacket(ID, ClientRecv, ServerRecv, SendAnyClient, \
                    []() -> std::unique_ptr<net::Packet> { \
                        return std::make_unique<ClassType>(); \
                    }); \
            } \
        }; \
        static ClassType##Registrar g_##ClassType##_reg; \
    }

} // namespace net
