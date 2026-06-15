#!/usr/bin/env python3
# Wire BaseSystem into gameplay in main_gpu.cpp (rename-workaround for mmap lock)
import os
def patch(path, repls):
    t = open(path,'r',encoding='utf-8',newline='').read(); orig=t
    nl='\r\n' if t.count('\r\n')>t.count('\n')-t.count('\r\n') else '\n'
    for old,new in repls:
        o=old.replace('\n',nl); n=new.replace('\n',nl)
        if o not in t: print(f"  SKIP: {old[:50]!r}"); continue
        t=t.replace(o,n,1); print(f"  OK: {old[:45]!r}")
    if t==orig: print("  no change"); return
    tmp=path+'.t'; open(tmp,'w',encoding='utf-8',newline='').write(t)
    bak=path+'.gbak'
    if os.path.exists(bak):
        try: os.remove(bak)
        except: bak=path+'.gbak2'
    os.rename(path,bak); os.rename(tmp,path); print(f"  WROTE {path}")

M=r"G:\CMakePJ\MassRTS\src\main_gpu.cpp"
patch(M, [
# 1) Player buy via number keys -> spawn at red base ring instead of rally_point
('            // Buy at player spawn area\n'
 '            glm::vec2 spawn = g_game_state.rally_point;\n'
 '            int bought = world.buy_batch(idx, g_buy_count, spawn, Faction::Red);',
 '            // Buy at the player base (units spawn from the home keep)\n'
 '            glm::vec2 spawn = g_renderer->bases.bases[0].position;\n'
 '            int bought = world.buy_batch(idx, g_buy_count, spawn, Faction::Red);'),
# 2) Player buy via shop panel -> same
('            if (shop_result >= 0) {\n'
 '                glm::vec2 spawn = g_game_state.rally_point;\n'
 '                int bought = world.buy_batch(shop_result, g_buy_count, spawn, Faction::Red);',
 '            if (shop_result >= 0) {\n'
 '                glm::vec2 spawn = g_renderer->bases.bases[0].position;\n'
 '                int bought = world.buy_batch(shop_result, g_buy_count, spawn, Faction::Red);'),
# 3) Enemy AI buy -> spawn at blue base
('            glm::vec2 espawn(400, std::uniform_real_distribution<float>(-200,200)(erng));',
 '            glm::vec2 espawn = g_renderer->bases.bases[1].position;'),
# 4) After alive-count loop, add base damage + base-destruction victory
('            // Count alive for HUD\n'
 '            combat->faction_alive[0] = 0;\n'
 '            combat->faction_alive[1] = 0;\n'
 '            for (uint32_t i = 0; i < world.entity_count; i++) {\n'
 '                if (world.is_alive(i)) {\n'
 '                    combat->faction_alive[(int)world.units.faction[i]]++;\n'
 '                }\n'
 '            }\n'
 '        } else {',
 '            // Count alive for HUD + siege damage to bases\n'
 '            combat->faction_alive[0] = 0;\n'
 '            combat->faction_alive[1] = 0;\n'
 '            float base_dmg[2] = {0,0};\n'
 '            for (uint32_t i = 0; i < world.entity_count; i++) {\n'
 '                if (!world.is_alive(i)) continue;\n'
 '                int f = (int)world.units.faction[i];\n'
 '                combat->faction_alive[f]++;\n'
 '                // Enemy units inside a base ring chip its health\n'
 '                int enemy_base = 1 - f;\n'
 '                if (g_renderer->bases.bases[enemy_base].alive &&\n'
 '                    g_renderer->bases.point_in_base(enemy_base, world.transforms.position[i])) {\n'
 '                    base_dmg[enemy_base] += world.units.damage[i] * dt;\n'
 '                }\n'
 '            }\n'
 '            if (base_dmg[0] > 0) g_renderer->bases.damage(0, base_dmg[0]);\n'
 '            if (base_dmg[1] > 0) g_renderer->bases.damage(1, base_dmg[1]);\n'
 '            // Base destroyed = instant win for the other side\n'
 '            if (!g_renderer->bases.bases[0].alive) g_game_state.phase = GamePhase::Defeat;\n'
 '            else if (!g_renderer->bases.bases[1].alive) g_game_state.phase = GamePhase::Victory;\n'
 '        } else {'),
])
print("DONE")
