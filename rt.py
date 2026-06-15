#!/usr/bin/env python3
# Route distinct effects in main_gpu hit loop. Rename-workaround for mmap lock.
import os
def patch(path, old, new):
    t=open(path,'r',encoding='utf-8',newline='').read()
    nl='\r\n' if t.count('\r\n')>t.count('\n')-t.count('\r\n') else '\n'
    o=old.replace('\n',nl); n=new.replace('\n',nl)
    assert o in t, "NOT FOUND:\n"+old[:120]
    t=t.replace(o,n,1)
    tmp=path+'.t'; open(tmp,'w',encoding='utf-8',newline='').write(t)
    bak=path+'.rbak'; i=0
    while os.path.exists(bak): i+=1; bak=path+'.rbak'+str(i)
    os.rename(path,bak); os.rename(tmp,path); print("WROTE",path)

M=r"G:\CMakePJ\MassRTS\src\main_gpu.cpp"
patch(M,
"""            if (hit.radius > 5.0f) {
                bool is_nuke = hit.radius > 80.0f;
                renderer.spawn_explosion_particles(hit.position, hit.radius, is_nuke);
                if (is_nuke) audio.trigger_nuke();
                else audio.trigger_cannon();""",
"""            if (hit.radius > 5.0f) {
                bool is_nuke = hit.radius > 80.0f;
                if (is_nuke) {
                    renderer.particles.spawn_nuke_blast(hit.position);
                    audio.trigger_nuke();
                } else {
                    renderer.particles.spawn_cannon_blast(hit.position);
                    audio.trigger_cannon();
                }""")
patch(M,
"""            } else {
                audio.trigger_arrow();
            }
        }""",
"""            } else {
                renderer.particles.spawn_arrow_impact(hit.position);
                audio.trigger_arrow();
            }
        }""")
print("DONE")
