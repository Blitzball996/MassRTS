# Audio assets

Drop your music and sound-effect files here. miniaudio decodes **.ogg / .mp3 /
.wav / .flac** — any of those extensions work. The loader probes extensions in
the order ogg → mp3 → wav → flac and uses the first match, so you only need one
file per name.

Missing files are skipped silently: the game falls back to the built-in
procedural ambience/melody, so it is never silent even with nothing here.

## Music (loops automatically)

| File stem      | When it plays                          |
|----------------|----------------------------------------|
| `music_menu`   | Main menu / map select / settings      |
| `music_battle` | During a battle (starts after loading) |

The menu and battle tracks are independent files. The engine stops one and
starts the other on transition (menu → loading → battle → back to menu).

## Sound effects (one-shot, can overlap)

| File stem       | Trigger                                  |
|-----------------|------------------------------------------|
| `ui_click`      | Pressing any main-menu / map button      |
| `ui_hover`      | (optional) hovering a button             |
| `sfx_arrow`     | Arrow impact                             |
| `sfx_cannon`    | Cannon detonation                        |
| `sfx_nuke`      | Nuke detonation                          |
| `sfx_explosion` | Generic explosion (reserved)             |

If a combat SFX file is absent, the matching procedural sound plays instead, so
arrows/cannons/nukes always make noise.

## Volume

The Settings → Audio sliders drive two buses:
- **Master volume** → whole engine (music + sfx + procedural ambience)
- **SFX volume**    → procedural ambience layer (and is reserved for sfx bus)

Example layout:

```
assets/audio/
  music_menu.ogg
  music_battle.ogg
  ui_click.wav
  sfx_arrow.wav
  sfx_cannon.wav
  sfx_nuke.wav
```
