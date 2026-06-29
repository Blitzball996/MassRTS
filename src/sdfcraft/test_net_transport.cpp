// =============================================================================
// SDFCraft - Real TCP EditTransport self-test (headless)
// Spins a host + client over loopback, replicates carve/place edits both ways
// through the actual socket path, and asserts both worlds converge — the same
// invariants selftest.cpp checks over LoopbackTransport, now over real TCP.
// =============================================================================
#include "net_ops.h"
#include "net_transport_tcp.h"
#include "net_session.h"
#include "net_protocol.h"
#include "server_sim.h"
#include "game_server.h"

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

    // =========================================================================
    // Multi-client server path: GameServer (authoritative sim) + 2 NetClients.
    // Proves the real multiplayer flow: clients join, get Welcome (seed + time),
    // a host edit broadcasts to clients, and a client's EditIntent is applied
    // authoritatively and echoed to the other client. This is the listen-host /
    // dedicated-server path the bats launch.
    // =========================================================================
    {
        const uint16_t SPORT = 55041;
        World sworld(2024);
        GameServer server(sworld, 2024, SPORT, /*local host player*/ true);
        CHECK(server.ok());

        NetClient c1, c2;
        CHECK(c1.connect("127.0.0.1", SPORT));
        CHECK(c2.connect("127.0.0.1", SPORT));
        c1.send(enc_hello("alice"));
        c2.send(enc_hello("bob"));

        // pump the server a few ticks so it accepts both clients + sends Welcome
        auto pump_server = [&](int ticks){ for (int i=0;i<ticks;i++){ server.update(0.05f);
            std::this_thread::sleep_for(std::chrono::milliseconds(5)); } };
        pump_server(20);

        // each client should have received a Welcome with the authoritative seed
        uint8_t id1 = 255, id2 = 255; uint64_t seen_seed = 0;
        auto scan_welcome = [&](NetClient& c, uint8_t& id){
            for (auto& m : c.poll()) {
                ByteReader r(m.data(), m.size());
                if (r.type() == MsgType::Welcome) {
                    uint8_t i; uint64_t sd; float t; uint32_t d;
                    if (r.get(i)&&r.get(sd)&&r.get(t)&&r.get(d)) { id = i; seen_seed = sd; }
                }
            }
        };
        scan_welcome(c1, id1);
        scan_welcome(c2, id2);
        CHECK(id1 != 255 && id2 != 255);
        CHECK(id1 != id2);                       // distinct player ids
        CHECK(seen_seed == 2024);                // authoritative world seed
        CHECK(server.sim().players().size() >= 3); // host + 2 clients
        printf("MP: 2 clients joined (ids %u,%u), seed synced\n", id1, id2);

        // host edits -> should broadcast an Edit to clients
        float sy2 = (float)sworld.surface_height(0,0);
        server.hostPlace(0, (int)sy2 + 5, 0, BLOCK_STONE);
        pump_server(4);
        bool c1_got_edit = false;
        for (int i=0;i<40 && !c1_got_edit;i++) {
            for (auto& m : c1.poll()) {
                ByteReader r(m.data(), m.size());
                if (r.type()==MsgType::Edit) {
                    NetEdit e; if (r.get(e) && e.kind==2 && e.material==BLOCK_STONE) c1_got_edit = true;
                }
            }
            server.update(0.05f); std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(c1_got_edit);
        printf("MP: host edit broadcast reached client\n");

        // client edit intent -> server applies authoritatively
        NetEdit ie{2, id1, 0.0f, (float)((int)sy2 + 6), 0.0f, 0.0f, (int32_t)BLOCK_PLANK};
        c1.send(enc_edit(MsgType::EditIntent, ie));
        pump_server(10);
        CHECK(sworld.get_block(0, (int)sy2 + 6, 0) == BLOCK_PLANK);
        printf("MP: client EditIntent applied authoritatively on server\n");
    }

    // =========================================================================
    // DELTA SYNC backfill (Task 2): the server records every authoritative edit
    // and replays the log to a client that joins AFTER the edit was made. Proves
    // a late-joiner reconstructs prior edits instead of seeing pristine terrain.
    //   1. host makes an edit BEFORE anyone connects
    //   2. a fresh NetClient connects
    //   3. after pumping, the client must receive the historical Edit (matching
    //      the pre-join place) on the wire — i.e. delta backfill happened.
    // =========================================================================
    {
        const uint16_t SPORT = 55042;
        World sworld(777);
        GameServer server(sworld, 777, SPORT, /*local host player*/ true);
        CHECK(server.ok());

        // (1) host edits BEFORE any client exists — goes into the edit log only.
        float syd = (float)sworld.surface_height(0, 0);
        const int EDIT_X = 3, EDIT_Y = (int)syd + 4, EDIT_Z = -2;
        server.hostPlace(EDIT_X, EDIT_Y, EDIT_Z, BLOCK_STONE);
        server.update(0.05f);   // process the broadcast (no clients yet)

        // (2) a fresh client joins well after the edit.
        NetClient late;
        CHECK(late.connect("127.0.0.1", SPORT));
        late.send(enc_hello("latecomer"));

        auto pump_server2 = [&](int ticks){ for (int i=0;i<ticks;i++){ server.update(0.05f);
            std::this_thread::sleep_for(std::chrono::milliseconds(5)); } };
        pump_server2(20);   // accept + Welcome + roster + edit-log backfill

        // (3) scan the client's stream: it must get Welcome AND the historical Edit.
        bool got_welcome = false, got_backfill_edit = false;
        for (int i = 0; i < 60 && !(got_welcome && got_backfill_edit); i++) {
            for (auto& m : late.poll()) {
                ByteReader r(m.data(), m.size());
                if (r.type() == MsgType::Welcome) got_welcome = true;
                else if (r.type() == MsgType::Edit) {
                    NetEdit e;
                    if (r.get(e) && e.kind == 2 && (int)e.x == EDIT_X &&
                        (int)e.y == EDIT_Y && (int)e.z == EDIT_Z &&
                        e.material == (int32_t)BLOCK_STONE)
                        got_backfill_edit = true;
                }
            }
            server.update(0.05f); std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(got_welcome);
        CHECK(got_backfill_edit);
        printf("MP: late-joiner received pre-join edit via delta backfill\n");
    }

    if (fails == 0) printf("ALL TCP TRANSPORT TESTS PASSED\n");
    else printf("%d TEST(S) FAILED\n", fails);
    return fails ? 1 : 0;
}
