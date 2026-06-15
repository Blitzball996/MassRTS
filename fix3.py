#!/usr/bin/env python3
# Fix batch #3: entity slot recycling (fixes progressive black screen) + spawn distance
import os
def patch(path, repls, all_occ=True):
    t = open(path,'r',encoding='utf-8',newline='').read()
    orig=t
    nl='\r\n' if t.count('\r\n')>t.count('\n')-t.count('\r\n') else '\n'
    for old,new in repls:
        o=old.replace('\n',nl); n=new.replace('\n',nl)
        if o not in t:
            print(f"  SKIP: {old[:50]!r}"); continue
        t = t.replace(o,n) if all_occ else t.replace(o,n,1)
        print(f"  OK: {old[:45]!r}")
    if t==orig:
        print(f"  no change {path}"); return
    tmp=path+'.t'; open(tmp,'w',encoding='utf-8',newline='').write(t)
    bak=path+'.bak3'
    if os.path.exists(bak):
        try: os.remove(bak)
        except: bak=path+'.bak3b'
    os.rename(path,bak); os.rename(tmp,path)
    print(f"  WROTE {path}")

SRC=r"G:\CMakePJ\MassRTS\src"

# --- world.h: add free_list member + recycle in create_entity + push on death ---
patch(os.path.join(SRC,"ecs","world.h"), [
(
"""    uint32_t entity_count = 0;
    std::vector<bool> alive;""",
"""    uint32_t entity_count = 0;
    std::vector<bool> alive;
    std::vector<uint32_t> free_list; // recycled dead slots (prevents unbounded entity_count growth -> GPU TDR/black screen)"""
),
(
"""    Entity create_entity() {
        if (entity_count >= MAX_ENTITIES) return INVALID_ENTITY;
        Entity e = entity_count++;
        alive[e] = true;
        return e;
    }""",
"""    Entity create_entity() {
        // Reuse a freed slot first so entity_count stays bounded by the number
        // of *simultaneously* live units, not the lifetime total. Without this
        // the GPU dispatch/readback loop grows every frame until it trips the
        // driver watchdog (TDR) -> permanent black screen after a long game.
        if (!free_list.empty()) {
            Entity e = free_list.back();
            free_list.pop_back();
            alive[e] = true;
            return e;
        }
        if (entity_count >= MAX_ENTITIES) return INVALID_ENTITY;
        Entity e = entity_count++;
        alive[e] = true;
        return e;
    }"""
),
(
"""                units.hit_timer[i] -= dt;
                if (units.hit_timer[i] <= 0) alive[i] = false;""",
"""                units.hit_timer[i] -= dt;
                if (units.hit_timer[i] <= 0) {
                    alive[i] = false;
                    free_list.push_back(i); // recycle this slot
                }"""
),
])

# --- main_gpu.cpp: pull armies closer (900 was too far) -> 550 ---
patch(os.path.join(SRC,"main_gpu.cpp"), [
("spawn_army(world, Faction::Red, {-900, 0}, 30000",
 "spawn_army(world, Faction::Red, {-550, 0}, 30000"),
("spawn_army(world, Faction::Blue, {900, 0}, 30000",
 "spawn_army(world, Faction::Blue, {550, 0}, 30000"),
])
print("DONE")
