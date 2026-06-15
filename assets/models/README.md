# CC0 Models — drop-in folder

Put `.obj` model files here to replace the built-in procedural unit meshes.
Files are matched by name (lowercase). Missing files are skipped — the
procedural mesh stays — so you can add models one at a time.

## Filenames the game looks for
| File              | Unit       |
|-------------------|------------|
| `infantry.obj`    | Infantry   |
| `cavalry.obj`     | Cavalry    |
| `archer.obj`      | Archer     |
| `bomber.obj`      | Bomber     |
| `artillery.obj`   | Artillery  |
| `shield.obj`      | Shield     |
| `samurai.obj`     | Samurai    |
| `militia.obj`     | Militia    |
| `wall.obj`        | Wall       |
| `turret.obj`      | Turret     |

## Where to get free CC0 models (no attribution required)
- **Quaternius** — https://quaternius.com  (RTS units, modular characters, animals)
- **Kenney** — https://kenney.nl/assets  (Tower Defense Kit, Castle Kit, Medieval Town)

Both ship as `.obj` + `.mtl`. Only the `.obj` geometry is used; color/tint is
applied per-unit (faction color), so untextured models still look correct.

## Notes
- Models are auto-centered and scaled so their footprint = 1 unit, then
  multiplied by each unit's in-game scale. Any source scale works.
- Keep polycount low (under ~2k tris) — tens of thousands are drawn at once.
- The build copies `assets/` next to the exe; if you add files after building,
  copy them into `build/Release/assets/models/` too (or just place them in both).

## How it's loaded
See `src/render/model_library.h` (registry + normalize) and the auto-load
block in `src/render/renderer.h::init()`.
