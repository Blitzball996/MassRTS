#!/usr/bin/env python3
# Add a sculpt-mode HUD indicator (brush name + radius).
import os
def patch(path, old, new):
    t=open(path,'r',encoding='utf-8',newline='').read()
    nl='\r\n' if t.count('\r\n')>t.count('\n')-t.count('\r\n') else '\n'
    o=old.replace('\n',nl); n=new.replace('\n',nl)
    assert o in t, "NOT FOUND in "+path+":\n"+old[:120]
    t=t.replace(o,n,1)
    tmp=path+'.t'; open(tmp,'w',encoding='utf-8',newline='').write(t)
    bak=path+'.hbak'; i=0
    while os.path.exists(bak): i+=1; bak=path+'.hbak'+str(i)
    os.rename(path,bak); os.rename(tmp,path); print("WROTE",path)

H=r"G:\CMakePJ\MassRTS\src\ui\hud.h"

# fields
patch(H,
"    bool nuke_ready = false;",
"""    bool nuke_ready = false;
    // Sculpt indicator
    bool sculpt_mode = false;
    int  sculpt_brush = 1;   // 0 Raise 1 Dig 2 Smooth 3 Flatten
    float sculpt_radius = 60.0f;""")

# draw a colored banner top-center when sculpting (brush color = mode)
patch(H,
"    void render() {",
"""    void draw_sculpt_banner() {
        if (!sculpt_mode) return;
        // brush color: Raise=green, Dig=red, Smooth=blue, Flatten=yellow
        glm::vec4 bc[4] = {{0.3f,0.9f,0.3f,0.9f},{0.95f,0.4f,0.3f,0.9f},
                           {0.4f,0.6f,1.0f,0.9f},{0.95f,0.85f,0.3f,0.9f}};
        int bi = (sculpt_brush>=0&&sculpt_brush<4)?sculpt_brush:1;
        float cx = (float)screen_w*0.5f;
        draw_rect(cx-150, 40, 300, 30, {0.05f,0.05f,0.08f,0.85f});
        draw_rect(cx-150, 40, 300, 4, bc[bi]);           // brush color strip
        draw_rect(cx-145, 46, 18, 18, bc[bi]);           // brush swatch
        // radius bar (right side of banner)
        float rb = (sculpt_radius-15.0f)/(250.0f-15.0f);
        draw_rect(cx+40, 50, 100, 8, {0.15f,0.15f,0.15f,0.8f});
        draw_rect(cx+40, 50, 100*rb, 8, bc[bi]);
        draw_number(cx+40, 50, (int)sculpt_radius, {0.9f,0.9f,0.9f,0.9f}, 0.8f);
    }

    void render() {""")

# call the banner inside render() (after the first draw, find a stable anchor)
patch(H,
"        draw_number(170, 12, red_alive, {1.0f, 0.3f, 0.3f, 1.0f}, 1.2f);",
"        draw_number(170, 12, red_alive, {1.0f, 0.3f, 0.3f, 1.0f}, 1.2f);\n        draw_sculpt_banner();")
print("DONE")
