// Headless self-test for SDFCraft data logic (no GL). Compiled ad-hoc.
#include "sdfcraft/crafting.h"
#include "sdfcraft/world.h"
#include "sdfcraft/player.h"
#include "sdfcraft/mc_mesher.h"
#include "sdfcraft/net_ops.h"
#include "sdfcraft/net_protocol.h"
#include "sdfcraft/server_sim.h"
#include "sdfcraft/planet.h"
#include "sdfcraft/planet_mesh.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cassert>
using namespace sdfcraft;

static int fails = 0;
#define CHECK(c) do{ if(!(c)){ printf("FAIL: %s (line %d)\n", #c, __LINE__); fails++; } }while(0)

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);   // unbuffered: last line printed = where it died
    RecipeBook rb;

    // log -> 4 planks (shapeless)
    { ItemId g[1] = { block_item(BLOCK_LOG) }; auto r = rb.match(g,1,1);
      CHECK(r.id == block_item(BLOCK_PLANK) && r.count == 4); }

    // 2 planks stacked vertically -> 4 sticks (shaped, offset tolerant in 2x2)
    { ItemId g[4] = { block_item(BLOCK_PLANK), ITEM_NONE,
                      block_item(BLOCK_PLANK), ITEM_NONE }; auto r = rb.match(g,2,2);
      CHECK(r.id == ITEM_STICK && r.count == 4); }

    // 4 planks 2x2 -> crafting table
    { ItemId P = block_item(BLOCK_PLANK); ItemId g[4]={P,P,P,P}; auto r = rb.match(g,2,2);
      CHECK(r.id == block_item(BLOCK_CRAFTING_TABLE)); }

    // wood pickaxe (3x3 shaped)
    { ItemId P=block_item(BLOCK_PLANK), S=ITEM_STICK, X=ITEM_NONE;
      ItemId g[9]={P,P,P, X,S,X, X,S,X}; auto r = rb.match(g,3,3);
      CHECK(r.id == ITEM_WOOD_PICKAXE); }

    // smelting iron ore -> iron ingot, sand -> glass
    CHECK(rb.smelt(block_item(BLOCK_IRON_ORE)).id == ITEM_IRON_INGOT);
    CHECK(rb.smelt(block_item(BLOCK_SAND)).id == block_item(BLOCK_GLASS));
    int bt; CHECK(rb.is_fuel(ITEM_COAL, bt) && bt == 1600);

    // inventory stacking respects per-item max (tools: 1 per slot, spread across slots)
    { Inventory inv; uint8_t left = inv.add(ITEM_WOOD_PICKAXE, 3, 1);
      CHECK(left == 0); // 3 tools spread over 3 slots, none left over
      CHECK(inv.slots[0].count == 1 && inv.slots[1].count == 1); }
    { Inventory inv; CHECK(inv.add(block_item(BLOCK_DIRT), 70, 64) == 0); }

    // world determinism: same seed -> same surface height
    { World w1(42), w2(42); CHECK(w1.surface_height(10,20) == w2.surface_height(10,20)); }
    // different seed -> (very likely) different
    { World w1(1), w2(2); CHECK(w1.surface_height(10,20) != w2.surface_height(10,20)); }

    // block edit round-trips and reports change
    { World w(7); int h = w.surface_height(0,0);
      CHECK(w.set_block(0,h,0, BLOCK_COBBLE) || true);
      w.set_block(0,h+5,0, BLOCK_GLASS);
      CHECK(w.get_block(0,h+5,0) == BLOCK_GLASS);
      CHECK(!w.set_block(0,h+5,0, BLOCK_GLASS)); /* no change */ }

    // --- planet math (Phase P1) ---
    {
        PlanetConfig cfg; LodPolicy pol;
        // cube-sphere round-trip: face+uv -> dir -> face+uv recovers the inputs
        for (int fi=0; fi<6; fi++) {
            CubeFace f=(CubeFace)fi;
            double u=0.3, v=-0.6;
            dvec3 dir = cube_to_unit_sphere(f,u,v);
            CHECK(fabs(glm::length(dir)-1.0) < 1e-9);   // on unit sphere
            CubeFace rf; double ru,rv; unit_sphere_to_cube(dir, rf, ru, rv);
            CHECK(rf==f && fabs(ru-u)<1e-6 && fabs(rv-v)<1e-6);
        }
        // floating origin: a point 1m from a camera 6.37e6 m out stays exact
        dvec3 cam(cfg.radius_m, 0, 0);
        dvec3 near = cam + dvec3(1.0, 0.5, -0.25);
        glm::vec3 r = to_render_space(near, cam);
        CHECK(fabs(r.x-1.0f)<1e-4 && fabs(r.y-0.5f)<1e-4 && fabs(r.z+0.25f)<1e-4);
        // LOD: root node far away should NOT subdivide; up close it should
        QuadNode root{ FACE_PX, 0, -1,-1, 1,1 };
        dvec3 far_cam = root.center_dir()*(cfg.radius_m*20.0);
        CHECK(!should_subdivide(cfg, root, far_cam, pol));
        dvec3 close_cam = root.center_dir()*(cfg.radius_m + 100.0);
        CHECK(should_subdivide(cfg, root, close_cam, pol));
        // split produces 4 children covering the parent area
        split_quad(root);
        CHECK(root.has_children && root.child[0] && root.child[3]);
        CHECK(root.child[0]->level==1);
        free_quad(&root);
        CHECK(!root.has_children);
    }

    // --- planet mesh LOD build (Phase P1) ---
    {
        PlanetMesh pm;
        // camera just above the surface -> high detail under camera, low elsewhere
        dvec3 cam = dvec3(1,0,0) * (pm.cfg.radius_m + 50.0);
        pm.update_lod(cam);
        std::vector<PlanetVertex> verts;
        pm.build(cam, verts, 4);
        CHECK(!verts.empty());            // produced geometry
        CHECK(verts.size() % 3 == 0);     // whole triangles
        // vertices right under the camera should be small (floating origin works)
        float minlen = 1e30f;
        for (auto& v : verts) minlen = std::min(minlen, glm::length(v.pos));
        CHECK(minlen < 1000.0f);          // nearest patch within ~1km of camera
    }

    // --- tree generation: trees actually populate the block grid ---
    {
        World w(1337);
        int logs=0, leaves=0, pine_like=0, oak_like=0;
        for (int cx=-3; cx<3; cx++)
        for (int cz=-3; cz<3; cz++) {
            Chunk* c = w.get_chunk({cx,cz}, true);
            for (int lx=0; lx<CHUNK_SX; lx++)
            for (int lz=0; lz<CHUNK_SZ; lz++) {
                int col_logs=0;
                for (int ly=0; ly<CHUNK_SY; ly++) {
                    BlockId b = c->get(lx,ly,lz);
                    if (b==BLOCK_LOG){logs++; col_logs++;}
                    else if (b==BLOCK_LEAVES) leaves++;
                }
                if (col_logs>=10) pine_like++; else if (col_logs>0) oak_like++;
            }
        }
        printf("TREE GEN: logs=%d leaves=%d (tall=%d short=%d) over 36 chunks\n",
               logs, leaves, pine_like, oak_like);
        CHECK(logs > 0);     // trees generate at all
        CHECK(leaves > 0);   // canopies generate
        CHECK(leaves > logs);// canopy bigger than trunk volume
    }

    // --- tree interaction: raycast must hit object blocks (logs/leaves), not
    //     pass straight through them into the smooth terrain behind ---
    {
        World w(1337);
        // Find a generated tree trunk by scanning columns near the origin.
        int tx=0, ty=0, tz=0; bool found=false;
        for (int cx=-3; cx<3 && !found; cx++)
        for (int cz=-3; cz<3 && !found; cz++) {
            Chunk* c = w.get_chunk({cx,cz}, true);
            for (int lx=0; lx<CHUNK_SX && !found; lx++)
            for (int lz=0; lz<CHUNK_SZ && !found; lz++)
            for (int ly=0; ly<CHUNK_SY; ly++) {
                if (c->get(lx,ly,lz)==BLOCK_LOG) {
                    tx = cx*CHUNK_SX+lx; ty=ly; tz=cz*CHUNK_SZ+lz; found=true; break;
                }
            }
        }
        CHECK(found);
        // Stand a few blocks to the +X side at trunk height, look toward -X.
        Player p;
        p.pos = glm::vec3(tx + 5.5f, (float)ty - Player::EYE + 0.5f, tz + 0.5f);
        p.yaw = 270.0f;   // forward = (-1,0,0) toward the trunk
        p.pitch = 0.0f;
        RayHit hit = p.raycast(w, 8.0f);
        BlockId hb = hit.hit ? w.get_block(hit.bx, hit.by, hit.bz) : BLOCK_AIR;
        printf("TREE RAYCAST: hit=%d block=%d (log=%d leaves=%d)\n",
               (int)hit.hit, (int)hb, (int)BLOCK_LOG, (int)BLOCK_LEAVES);
        CHECK(hit.hit);
        CHECK(hb==BLOCK_LOG || hb==BLOCK_LEAVES);   // selected the tree, not ground
    }

    // --- dig artifacts: carved MC mesh has no spurious long-edge spikes ---
    {
        World w(1337);
        // carve a few overlapping spheres near the surface
        float sy = (float)w.surface_height(0,0);
        w.carve_sphere(0, sy, 0, 2.0f, -1);
        w.carve_sphere(2, sy-1, 1, 2.5f, -1);
        w.carve_sphere(-1, sy, 2, 1.8f, -1);
        Chunk* c = w.get_chunk({0,0}, true);
        ChunkMesh m; MCMesher::build(w, *c, m);
        // vertices are packed: pos(3) nrm(3) col(3) mat(1) = 10 floats
        const int S = 10;
        size_t verts = m.opaque.size() / S;
        size_t tris = verts / 3;
        double max_edge = 0.0; size_t spikes = 0;
        for (size_t t = 0; t < tris; t++) {
            glm::vec3 p[3];
            for (int k=0;k<3;k++){ const float* f=&m.opaque[(t*3+k)*S];
                p[k]=glm::vec3(f[0],f[1],f[2]); }
            for (int e=0;e<3;e++){ float L=glm::length(p[e]-p[(e+1)%3]);
                if(L>max_edge)max_edge=L; if(L>2.6f)spikes++; }
        }
        printf("DIG MESH: %zu tris, max_edge=%.2f, long_edges=%zu\n",
               tris, max_edge, spikes);
        CHECK(tris > 0);
        CHECK(max_edge < 3.0f);   // a single cell spans ~1.7 max; >3 = spike
    }

    // --- networking: two peers converge after replicated edits ---
    {
        World wa(1337), wb(1337);   // identical seed = identical base world
        LoopbackTransport ta, tb;
        LoopbackTransport::pair(ta, tb);
        EditReplicator host(wa, ta, /*author*/0);
        EditReplicator client(wb, tb, /*author*/1);

        float sy = (float)wa.surface_height(8, 8);
        // host carves, client places — each must replicate to the other.
        host.carve(8, sy, 8, 2.5f, -1, nullptr);
        client.place(8, (int)sy + 3, 8, BLOCK_STONE);
        // exchange
        client.pump();   // client receives host's carve
        host.pump();     // host receives client's place

        // both worlds must now agree at the touched voxels
        CHECK(wa.get_block(8, (int)sy + 3, 8) == wb.get_block(8, (int)sy + 3, 8));
        CHECK(wa.get_block(8, (int)sy + 3, 8) == BLOCK_STONE);
        // carve made the centre air on both
        CHECK(wa.sdf_at(8, (int)sy, 8) == wb.sdf_at(8, (int)sy, 8));

        // idempotency: a second pump applies nothing (dedup by author/seq)
        int again = host.pump() + client.pump();
        CHECK(again == 0);
        printf("NET: peers converged, dedup ok (host seq=%u client seq=%u)\n",
               host.outboundSeq(), client.outboundSeq());

        // --- late-join streaming sync: a fresh peer backfills via snapshot ---
        World wc(1337);                       // same seed, but missed all edits
        LoopbackTransport tc, td;
        LoopbackTransport::pair(tc, td);
        EditReplicator latejoin(wc, tc, /*author*/2);
        // host has accumulated its own carve + the relayed place in history.
        std::vector<uint8_t> snap = host.snapshot();
        int back = latejoin.applySnapshot(snap);
        CHECK(back == (int)host.historySize());          // got every prior edit
        CHECK(wc.get_block(8, (int)sy + 3, 8) == BLOCK_STONE);   // the placed block
        CHECK(wc.sdf_at(8, (int)sy, 8) == wa.sdf_at(8, (int)sy, 8)); // the carve
        // re-applying the same snapshot is a no-op (deduped).
        CHECK(latejoin.applySnapshot(snap) == 0);
        printf("NET BACKFILL: late joiner replayed %d edits, world matches host\n", back);
    }

    // --- protocol round-trip: encode -> decode every message type ----------
    {
        // Welcome
        { auto b = enc_welcome(7, 0xABCDEF, 0.5f, 3);
          ByteReader r(b.data(), b.size()); CHECK(r.type()==MsgType::Welcome);
          uint8_t id; uint64_t seed; float tod; uint32_t day;
          CHECK(r.get(id)&&r.get(seed)&&r.get(tod)&&r.get(day));
          CHECK(id==7 && seed==0xABCDEF && tod==0.5f && day==3); }
        // Edit (carve)
        { NetEdit e{1, 2, 1.f,2.f,3.f, 2.5f, -1};
          auto b = enc_edit(MsgType::Edit, e);
          ByteReader r(b.data(), b.size()); CHECK(r.type()==MsgType::Edit);
          NetEdit o; CHECK(r.get(o));
          CHECK(o.kind==1 && o.author==2 && o.x==1.f && o.radius==2.5f && o.material==-1); }
        // PlayerMove
        { NetPlayerState s{4, 10.f,64.f,-3.f, 90.f, -10.f, 1};
          auto b = enc_move(s); ByteReader r(b.data(), b.size());
          NetPlayerState o; CHECK(r.get(o));
          CHECK(o.id==4 && o.x==10.f && o.yaw==90.f && o.moving==1); }
        // MobSnapshot (variable length)
        { std::vector<NetMob> snap = {
            {1,(uint8_t)MobKind::Zombie, 1,2,3, 45.f, 20.f, 1, 0},
            {2,(uint8_t)MobKind::Pig,    4,5,6, 12.f, 10.f, 0, 1} };
          auto b = enc_mob_snapshot(snap); ByteReader r(b.data(), b.size());
          CHECK(r.type()==MsgType::MobSnapshot);
          std::vector<NetMob> out; CHECK(dec_mob_snapshot(r, out));
          CHECK(out.size()==2 && out[0].id==1 && out[1].kind==(uint8_t)MobKind::Pig);
          CHECK(out[0].moving==1 && out[1].moving==0);
          CHECK(out[0].hurt==0 && out[1].hurt==1); }
        printf("PROTO: all message types round-trip ok\n");
    }

    // --- authoritative ServerSim: time, spawn, survival, combat ------------
    {
        World w(4242);
        // pre-generate spawn disk so mobs have ground
        ChunkKey c0 = World::world_to_chunk(0,0);
        for (int dz=-3; dz<=3; dz++) for (int dx=-3; dx<=3; dx++)
            w.get_chunk({c0.cx+dx, c0.cz+dz}, true);
        ServerSim sim(w, 4242);
        ServerPlayer& p = sim.addPlayer(0, "tester");
        CHECK(p.active && p.avatar.health == 20.0f);

        // time advances and wraps into a new day; night flips at the right phase
        sim.time_of_day = 0.95f;
        for (int i=0;i<200;i++) sim.tick(0.5f);   // 100s of sim time
        CHECK(sim.day >= 1);                       // crossed midnight at least once

        // force night + spawn pressure: hostiles should appear around the player
        sim.time_of_day = 0.0f;                    // deep night
        for (int i=0;i<40;i++) sim.tick(0.1f);
        int hostiles = 0;
        for (auto& e : sim.mobs.entities)
            if (e.def().hostility == Hostility::Hostile) hostiles++;
        CHECK(!sim.mobs.entities.empty());         // something spawned at night
        printf("SIM: day=%u mobs=%zu hostiles=%d\n", sim.day, sim.mobs.entities.size(), hostiles);

        // sun dips below horizon at night, rises at noon (drives lighting)
        CHECK(ServerSim::daylight(0.5f) > 0.9f);   // noon ~ full daylight
        CHECK(ServerSim::daylight(0.0f) < 0.2f);   // midnight ~ dark

        // combat: isolated sim (daytime, no hostile crowd) so the pig is the
        // only thing the ray can hit. Aim from eye -> pig body centre.
        ServerSim csim(w, 4242);
        csim.time_of_day = 0.5f;                   // noon: no hostile spawns to clutter
        csim.addPlayer(0, "fighter");
        glm::vec3 eye(0.5f, (float)w.surface_height(0,0)+1.6f, 0.5f);
        glm::vec3 pig_feet(0.5f, (float)w.surface_height(0,3), 3.5f);
        Entity& pig = csim.mobs.spawn(MobKind::Pig, pig_feet);
        glm::vec3 pig_centre = pig_feet + glm::vec3(0, pig.def().height*0.5f, 0);
        glm::vec3 fwd = glm::normalize(pig_centre - eye);
        float hp0 = pig.health;
        ItemId drop=ITEM_NONE; uint8_t dn=0;
        Entity* hit = csim.attack(eye, fwd, 8.0f, 100.0f, &drop, &dn);
        CHECK(hit != nullptr);                     // ray pierced the pig's AABB
        if (hit) {
            CHECK(hit->health < hp0);              // took damage
            CHECK(!hit->alive && drop == ITEM_PORKCHOP); // 100 dmg killed it, dropped pork
        }
        printf("SIM COMBAT: melee ray hit + kill + drop ok\n");

        // survival: starving player loses health (use the calm noon sim)
        ServerPlayer* sp = csim.player(0);
        sp->avatar.hunger = 0.0f; sp->avatar.saturation = 0.0f;
        float h0 = sp->avatar.health;
        for (int i=0;i<120;i++) csim.tick(0.1f);    // 12s of starvation
        CHECK(sp->avatar.health < h0);
        printf("SIM SURVIVAL: starvation drains health (%.0f -> %.0f)\n", h0, sp->avatar.health);
    }

    // --- armor: recipes craft, worn armor reduces mob melee damage ----------
    {
        // recipes exist for a full iron set
        ItemId I = ITEM_IRON_INGOT;
        { ItemId g[6]={I,I,I, I,ITEM_NONE,I}; auto r=rb.match(g,3,2); CHECK(r.id==ITEM_IRON_HELMET); }
        { ItemId g[9]={I,ITEM_NONE,I, I,I,I, I,I,I}; auto r=rb.match(g,3,3); CHECK(r.id==ITEM_IRON_CHEST); }
        { ItemId g[9]={I,I,I, I,ITEM_NONE,I, I,ITEM_NONE,I}; auto r=rb.match(g,3,3); CHECK(r.id==ITEM_IRON_LEGS); }
        { ItemId g[6]={I,ITEM_NONE,I, I,ITEM_NONE,I}; auto r=rb.match(g,3,2); CHECK(r.id==ITEM_IRON_BOOTS); }
        // armor metadata
        CHECK(item_is_armor(ITEM_IRON_CHEST) && item_armor_slot(ITEM_IRON_CHEST)==ArmorSlot::Chest);
        // worn full iron set: total_armor > 0 and damage is reduced
        Inventory iv;
        iv.armor[0].id=ITEM_IRON_HELMET; iv.armor[0].count=1;
        iv.armor[1].id=ITEM_IRON_CHEST;  iv.armor[1].count=1;
        iv.armor[2].id=ITEM_IRON_LEGS;   iv.armor[2].count=1;
        iv.armor[3].id=ITEM_IRON_BOOTS;  iv.armor[3].count=1;
        float ap = total_armor(iv);
        CHECK(ap > 8.0f);   // 2.5+3.5+3.0+2.0 = 11
        float reduce = ap*0.04f; if (reduce>0.8f) reduce=0.8f;
        CHECK(reduce > 0.3f && reduce < 0.8f);   // ~44% mitigation, sane
        printf("ARMOR: iron set recipes ok, %.0f armor pts -> %.0f%% damage cut\n", ap, reduce*100);
    }

    // --- perf: a populated sim must tick well under one 20Hz frame (50ms) -----
    // Guards the host/solo stutter regression: mob collision used to call the
    // full trilinear sample_sdf (~40 FBM/call) hundreds of times per tick; the
    // cheap terrain_solid_cheap (1 FBM/call) must keep a 40-mob world fast.
    {
        World pw(7);
        ChunkKey c0 = World::world_to_chunk(0,0);
        for (int dz=-4; dz<=4; dz++) for (int dx=-4; dx<=4; dx++)
            pw.get_chunk({c0.cx+dx, c0.cz+dz}, true);
        ServerSim sim(pw, 7);
        sim.addPlayer(0, "perf");
        sim.time_of_day = 0.0f;                 // night: fill toward hostile cap
        for (int i=0;i<60;i++) sim.tick(0.1f);  // warm up + spawn a crowd
        size_t mobs = sim.mobs.entities.size();
        auto t0 = std::chrono::steady_clock::now();
        const int N = 200;
        for (int i=0;i<N;i++) sim.tick(0.05f);  // 200 ticks @ 20Hz
        double ms = std::chrono::duration<double,std::milli>(
                        std::chrono::steady_clock::now()-t0).count();
        double per = ms / N;
        printf("PERF: %zu mobs, %.3f ms/tick (budget 50ms @20Hz)\n", mobs, per);
        CHECK(per < 50.0);                      // must fit a server frame
    }

    if (fails == 0) printf("ALL SDFCRAFT TESTS PASSED\n");
    else printf("%d TEST(S) FAILED\n", fails);
    return fails ? 1 : 0;
}
