# MassRTS

> 🌐 **Language / 语言**：**English** · [简体中文](README.zh-CN.md)

A large-scale real-time strategy (RTS) and mass-battle simulator built on a
custom OpenGL engine. The goal is to push **tens of thousands to hundreds of
thousands of units** on screen at once, with a fully **destructible volumetric
terrain** you can sculpt and tunnel through in real time.

> Status: active prototype. The GPU build (`MassRTS_GPU`) is the main target —
> unit simulation and combat run on GPU compute shaders. See
> [`PROJECT_PROGRESS.md`](PROJECT_PROGRESS.md) for the detailed roadmap,
> design notes, and known issues.

---

## Highlights

- **Massive unit counts** — GPU-driven movement, spatial hashing, and combat
  (compute shaders) sustain ~190k units at interactive frame rates.
- **Volumetric destructible terrain** — the ground is a true Signed Distance
  Field rendered with chunked Marching Cubes (a solid body with thickness, not a
  height-field skin). Dig pits, raise hills, bore tunnels, all at runtime.
- **In-game terrain sculpting** — raise / dig / smooth / tunnel brushes with
  adjustable radius and strength, plus a surface-hugging brush cursor.
- **Biome terrain** — grass, forest, swamp, mountain (with snow caps), rivers
  and water, each affecting unit movement speed (uphill slows you down).
- **Autonomous AI commander** — settlement system that marches a vanguard,
  plants an HQ, builds a barracks, and produces reinforcements (Phase 1).

---

## Build

Requires **CMake ≥ 3.20** and a **C++20** compiler. Dependencies (GLFW, GLAD,
GLM) are fetched automatically via CMake `FetchContent` — no manual setup.

```bash
cmake -B build
cmake --build build --config Release --target MassRTS_GPU
```

The GPU target needs **OpenGL 4.3+** (compute shaders); 4.6 recommended.

Two executables are produced:

| Target         | Description                                         |
| -------------- | --------------------------------------------------- |
| `MassRTS_GPU`  | Main build — GPU-accelerated simulation & combat.   |
| `MassRTS`      | Original CPU build (stable reference).              |

Run from the build output directory so the `shaders/` and `assets/` folders are
found:

```bash
cd build/Release
./MassRTS_GPU.exe
```

### Auto-screenshot mode

```bash
./MassRTS_GPU.exe --shots
```

Warms up, orbits the camera through preset angles, writes one PNG per angle, and
exits — handy for quickly inspecting terrain from multiple viewpoints.

---

## Controls

### Camera
| Key            | Action                          |
| -------------- | ------------------------------- |
| `W` `A` `S` `D`| Pan camera                      |
| `Q` / `E`      | Rotate (yaw)                    |
| `R` / `F`      | Raise / lower altitude          |
| Mouse wheel    | Zoom (in/out)                   |
| Left drag      | Box-select units                |
| Right click    | Move / attack order             |

### Terrain sculpting (press `B` to toggle sculpt mode)
| Key            | Action                                       |
| -------------- | -------------------------------------------- |
| `1`            | Raise soil                                   |
| `2`            | Dig (rounded bowl)                           |
| `3`            | Smooth                                        |
| `4`            | Tunnel / cave (drag to bore a continuous shaft) |
| `[` / `]`      | Brush radius − / +                           |
| `,` / `.`      | Brush strength − / +                         |
| Mouse wheel    | Brush strength (in sculpt mode)              |
| Left hold      | Paint                                        |

### Misc
| Key            | Action                          |
| -------------- | ------------------------------- |
| `N`            | Nuke (when ready)               |
| `-` / `=`      | Buy count − / +                 |
| Numpad `0`–`5` | Switch map                      |
| `Esc`          | Quit                            |

---

## Project layout

```
src/
  ai/          movement, combat, spatial grid systems
  audio/       miniaudio-based sound
  core/        asset manifest
  ecs/         entity-component world
  game/        game state, config, settlement (AI commander) system
  input/       camera / ray casting
  net/         lockstep networking protocol
  render/      renderer, SDF terrain (marching cubes), legacy heightmap,
               GPU compute, particles, models, fluid system (SWE scaffold)
  ui/          HUD, menu
  main_gpu.cpp main loop (GPU target)
shaders/       GLSL (terrain, billboard, compute, particle, brush, ...)
assets/        models / textures
```

---

## Terrain architecture (short version)

The terrain is stored as a Signed Distance Field over sparse 32³-voxel chunks.
Only chunks the surface passes through allocate memory and mesh (Marching
Cubes); pure-air / pure-solid chunks stay sparse. The legacy heightmap is kept
as a cheap query surface for units / camera / combat.

The SDF is **gradient-normalized** (`distance ≈ (y − h) / √(1 + |∇h|²)`) so the
mesh stays clean on steep slopes instead of shattering into spikes. Carving is a
deterministic event `{center, radius, op, strength}`, so the same edit applied
on every client yields identical state (lockstep multiplayer ready). Digging is
clamped above the slab bottom, so you can tunnel deep but never punch a hole
clean through the map.

---

## Roadmap (see `PROJECT_PROGRESS.md`)

- High-performance fluid (Shallow Water Equations on GPU) — scaffold in
  `src/render/fluid_system.h`.
- Infinite / streaming map (World Partition style chunk streaming).
- Settlement AI Phases 2–5: expansion & garrisons, economy units, anti-snowball
  guerrilla/spy harassment, player-issued build/rally commands.

---

## License

No license specified yet.
