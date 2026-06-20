// =============================================================================
// SDFCraft - Real TCP EditTransport self-test (headless)
// Spins a host + client over loopback, replicates carve/place edits both ways
// through the actual socket path, and asserts both worlds converge — the same
// invariants selftest.cpp checks over LoopbackTransport, now over real TCP.
// =============================================================================
#include "net_ops.h"
#include "net_transport_tcp.h"

#include <chrono>
#include <cstdio>
#include <thread>

using namespace sdfcraft;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL: %s (line %d)\n", #c, __LINE__); ++fails; } } while (0)

// Pump until `expect` remote edits land (or timeout). Returns total applied.
static int pump_until(EditReplicator& repl, int expect, int timeout_ms = 3000) {
    using clock = std::chrono::steady_clock;
    auto deadline = clock::now() + std::chrono::milliseconds(timeout_ms);
    int applied = 0;
    while (applied < expect && clock::now() < deadline) {
        applied += repl.pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return applied;
}

int main() {
    TcpNetInit net;
    CHECK(net.ok());
    const uint16_t PORT = 55037;

    // --- establish host <-> client link over loopback ----------------------
    TcpEditTransport ta, tb;
    CHECK(ta.listen(PORT));
    CHECK(tb.connect("127.0.0.1", PORT));

    bool accepted = false;
    for (int i = 0; i < 1000 && !accepted; ++i) {
        accepted = ta.acceptPeer();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(accepted);
    if (!accepted) { printf("%d TEST(S) FAILED\n", fails); return 1; }
    printf("LINK: host<->client established on %u\n", PORT);

    // --- two independent worlds + replicators ------------------------------
    World wa, wb;
    EditReplicator host(wa, ta, /*author*/ 0);
    EditReplicator client(wb, tb, /*author*/ 1);

    float sy = (float)wa.surface_height(8, 8);
    // host carves, client places — each must replicate to the other over TCP.
    host.carve(8, sy, 8, 2.5f, -1, nullptr);
    client.place(8, (int)sy + 3, 8, BLOCK_STONE);

    // drain both directions through the sockets
    int got_on_client = pump_until(client, /*host's carve*/ 1);
    int got_on_host   = pump_until(host,   /*client's place*/ 1);
    CHECK(got_on_client == 1);
    CHECK(got_on_host == 1);

    // both worlds must now agree at the touched voxels
    CHECK(wa.get_block(8, (int)sy + 3, 8) == wb.get_block(8, (int)sy + 3, 8));
    CHECK(wa.get_block(8, (int)sy + 3, 8) == BLOCK_STONE);
    CHECK(wa.sdf_at(8, (int)sy, 8) == wb.sdf_at(8, (int)sy, 8));

    // idempotency: nothing new should arrive on a second drain
    int again = pump_until(host, 1, 200) + pump_until(client, 1, 200);
    CHECK(again == 0);
    printf("NET-TCP: peers converged over sockets (host seq=%u client seq=%u)\n",
           host.outboundSeq(), client.outboundSeq());

    if (fails == 0) printf("ALL TCP TRANSPORT TESTS PASSED\n");
    else printf("%d TEST(S) FAILED\n", fails);
    return fails ? 1 : 0;
}
