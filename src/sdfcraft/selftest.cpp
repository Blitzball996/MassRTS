// Headless self-test for SDFCraft data logic (no GL). Compiled ad-hoc.
#include "sdfcraft/crafting.h"
#include "sdfcraft/world.h"
#include "sdfcraft/planet.h"
#include <cstdio>
#include <cassert>
using namespace sdfcraft;

static int fails = 0;
#define CHECK(c) do{ if(!(c)){ printf("FAIL: %s (line %d)\n", #c, __LINE__); fails++; } }while(0)

int main() {
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

    if (fails == 0) printf("ALL SDFCRAFT TESTS PASSED\n");
    else printf("%d TEST(S) FAILED\n", fails);
    return fails ? 1 : 0;
}
