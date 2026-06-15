import sys, shutil

# Fix 1: combat shader - increase idle speed, fix stuck state transitions
path = "G:/CMakePJ/MassRTS/shaders/compute_combat.glsl"
out = path + ".new"
with open(path, 'r') as f:
    content = f.read()

# Increase search range from 300 to 500
content = content.replace(
    "(u.type == 4u) ? 400.0 : 300.0",
    "(u.type == 4u) ? 600.0 : 500.0"
)

# Increase idle advance speed from 0.4 to 0.6
content = content.replace(
    "float spd_mult = (u.type == 4u) ? 0.15 : 0.4;",
    "float spd_mult = (u.type == 4u) ? 0.3 : 0.6;"
)

# When target becomes invalid, also clear velocity
content = content.replace(
    '''        tgt = 0xFFFFFFFFu;
        units[idx].target = 0xFFFFFFFFu;
        units[idx].state = 0u;''',
    '''        tgt = 0xFFFFFFFFu;
        units[idx].target = 0xFFFFFFFFu;
        units[idx].state = 0u;
        units[idx].velocity = vec2(0.0);'''
)

with open(out, 'w') as f:
    f.write(content)
shutil.copy2(out, path)
import os
os.remove(out)
print("Fixed combat shader")

# Fix 2: Shop names in both main files
for main_path in ["G:/CMakePJ/MassRTS/src/main.cpp", "G:/CMakePJ/MassRTS/src/main_gpu.cpp"]:
    with open(main_path, 'r') as f:
        mc = f.read()
    old_names = '"Militia","Infantry","Cavalry","Archer","Bomber","Artillery","Shield","Samurai","Wall"'
    new_names = '"Militia","Infantry","Archer","Shield","Cavalry","Bomber","Artillery","Wall","Turret"'
    if old_names in mc:
        mc = mc.replace(old_names, new_names)
        out2 = main_path + ".new"
        with open(out2, 'w') as f:
            f.write(mc)
        shutil.copy2(out2, main_path)
        os.remove(out2)
        print(f"Fixed shop names in {main_path}")
    else:
        print(f"Shop names not found in {main_path}")

print("All done")
