# SDF Voxel Terrain + Sculpt — Technical Design

Goal: replace the current heightmap terrain with a **true 3D voxel field** stored
as a signed-distance field (SDF), rendered with Marching Cubes, so the player can
**smoothly sculpt** the ground (dig craters, raise mounds, even overhangs/caves) by
spending money, selecting a brush mode with a long mouse press.

This is the largest feature in the project and replaces `terrain.h`/`terrain.*`
shaders. It is split into phases so each lands independently and stays shippable.

---

## 1. Why SDF instead of editing the heightmap

The current terrain is a 512×512 heightmap: each (x,z) has exactly one height, so
you can only ever push the surface up/down — no caves, no overhangs, and digging a
sharp crater looks blocky. An SDF stores `distance-to-surface` per 3D cell, so:

- Smooth blending: brushes add/subtract a smooth falloff to the field → rounded
  craters and mounds for free (that's the "圆滑" look you want).
- Real 3D: overhangs, holes, tunnels are representable.
- Marching Cubes turns the SDF into a watertight mesh every time it changes.

---

## 2. Data layout

```
World: 3000 × 3000 (XZ), height band say -40 .. +120  => 160 tall
Voxel size: 4.0 world units  (tune for cost vs detail)
Grid: 750 × 40 × 750  ≈ 22.5M cells  (1 float each = 90 MB)  -> too big at 4u.

=> Chunked. Split into CHUNKS of 32³ voxels.
   Chunk world size = 32 * 4 = 128 units.
   Grid of chunks: 24 × ~2 × 24 (vertical band small) ≈ ~1150 chunks.
   Only chunks the player actually edits get a dense SDF buffer; the rest are
   procedurally implicit (height function -> sign) and never stored.
```

Each chunk:
- `float sdf[33][33][33]` (need +1 for shared face vertices) — lazily allocated.
- `GLuint vao,vbo` for its Marching-Cubes mesh.
- `dirty` flag → remesh next frame.
- `active` flag → has been sculpted (otherwise use base height field).

Base field (unsculpted) = `sdf(p) = p.y - terrain_height(p.xz)` so the initial
world matches today's terrain exactly; sculpting just adds local deltas.

---

## 3. Sculpt brushes (the gameplay)

Input model (matches your spec):
- **Long-press** left mouse → enters Sculpt mode, shows a radial menu of brush
  types under the cursor: `Dig` (挖坑), `Raise` (隆起), `Smooth`, `Flatten`.
- Release on a wedge selects that brush; subsequent drags apply it.
- Each application costs money (`cost = brush_volume * rate`), deducted per tick
  while painting; if broke, painting stops.

Brush math (CSG on the SDF):
```
Raise:  sdf = min(sdf, sdf_sphere(p, center, radius))   // union -> adds material
Dig:    sdf = max(sdf, -sdf_sphere(p, center, radius))  // subtract -> removes
Smooth: sdf = blur3x3x3(sdf) in brush region
Flatten:sdf = lerp(sdf, plane(target_y), strength)
```
Use **smooth-min** (`smin`) for Raise so mounds blend into existing ground:
```
smin(a,b,k) = -log(exp(-k*a)+exp(-k*b))/k
```

Only chunks overlapping the brush get marked dirty + remeshed. Brush radius is
clamped to a few voxels so each edit touches 1–8 chunks → cheap remesh.

---

## 4. Meshing (Marching Cubes)

- CPU Marching Cubes per dirty chunk (classic 256-case edge table). ~32³ cells →
  a few thousand triangles per chunk; remeshing 1–8 chunks per edit is sub-ms.
- Normals from SDF gradient (central differences) → smooth shading for free.
- Optionally move MC to a compute shader later (Phase 4) if edits feel heavy.
- Re-use the existing terrain.frag biome/texturing logic almost unchanged — it's
  driven by world position, slope and height, all available on the new mesh.

---

## 5. Collision / gameplay integration

Units currently sample `terrain.get_height_at(x,z)`. With an SDF we still expose
the same call:
```
float height_at(x,z):
   if chunk(x,z).active:  ray-march the SDF downward from the top to find surface
   else:                  return terrain_height(x,z)   // base field, unchanged
```
This keeps unit movement/spawning working without touching combat/movement code.
Overhangs/caves don't affect 2D unit AI (units stay on the topmost surface), which
is the right call for an RTS — keep pathing 2.5D.

---

## 6. Phasing

| Phase | Deliverable | Risk |
|------|-------------|------|
| 0 | This doc + decide voxel size / chunk size | none |
| 1 | ✅ SDF chunk store + base field = current terrain; render via MC; **visually identical** to today | medium (MC correctness) |
| 2 | Dig/Raise brushes with smooth blending, no UI yet (hotkey-driven), free | medium |
| 3 | Long-press radial brush menu + money cost + Smooth/Flatten | low |
| 4 | (optional) GPU compute meshing for big brushes; LOD for distant chunks | high |

> **SDFCraft status:** Phase 1 landed for the survival/build mode. Opaque terrain
> is now meshed with Marching Cubes (`src/sdfcraft/mc_mesher.h`, full 256-entry
> Lorensen-Cline tables) from the block occupancy field, giving smooth shaded
> isosurfaces instead of blocky cube faces. Non-opaque blocks (water/glass/leaves)
> still use the cube mesher's transparent pass. Vertex format and shaders are
> unchanged, so the renderer needed only a one-line swap in `ChunkRenderer::sync`.
> Next: hook dig/place edits to remesh affected chunks (already wired via
> `dirty_mesh`) and add the sculpt brushes (Phase 2).

Phase 1 is the make-or-break: until MC reproduces the current look, we don't ship
it. We keep the old heightmap terrain behind a compile flag until Phase 3 is solid.

---

## 7. Estimated effort

- Phase 1: ~1 focused session (MC tables are boilerplate; chunk plumbing is the work).
- Phase 2: ~half session.
- Phase 3: ~half session (UI + economy hook).
- Total ~2–3 sessions. This is why it's scheduled last.

## 8. Open decisions for you
1. Voxel size 4.0 (smoother, costlier) vs 6.0 (cheaper, blockier)? Recommend start at 4.0.
2. Should sculpting be allowed anywhere, or only near your own base? (balance)
3. Should the AI also sculpt? Recommend: no, player-only, to keep it deterministic.
