#!/usr/bin/env python3
# Integrate BaseSystem into renderer.h via rename-workaround (mmap lock bypass)
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
    bak=path+'.ibak'
    if os.path.exists(bak):
        try: os.remove(bak)
        except: bak=path+'.ibak2'
    os.rename(path,bak); os.rename(tmp,path); print(f"  WROTE {path}")

R=r"G:\CMakePJ\MassRTS\src\render\renderer.h"
patch(R, [
('#include "decor.h"', '#include "decor.h"\n#include "base_system.h"'),
('    BattlefieldDecor decor;', '    BattlefieldDecor decor;\n    BaseSystem bases;'),
# init bases after decor.generate
('        decor.generate(3000.0f, [this](float x, float z){ return terrain.get_height_at(x, z); });',
 '        decor.generate(3000.0f, [this](float x, float z){ return terrain.get_height_at(x, z); });\n'
 '        bases.init({-550, 0}, {550, 0}, [this](float x, float z){ return terrain.get_height_at(x, z); });'),
# render bases right after decor.render (same shader already bound)
('        decor.render(unit_shader);',
 '        decor.render(unit_shader);\n        bases.render(unit_shader);'),
])
print("DONE")
