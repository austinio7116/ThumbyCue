# ThumbyCue — agent notes

Accurate 3D snooker & pool for Thumby Color (RP2350 @ 280 MHz, 520 KB SRAM,
128×128 RGB565). Own git repo. Full design: PLAN.md. Built on ThumbyElite's
flat-shaded dual-core triangle rasterizer + sphere impostors.

## Build

```bash
# Host (all development happens here first):
cmake -B build_host -S host && cmake --build build_host -j8
./build_host/thumbycue_host [pool|snooker] [seed]
CUE_SHOT=/tmp/s.ppm ./build_host/thumbycue_host pool   # headless screenshot
CUE_PHYSTEST=1 ./build_host/thumbycue_host             # physics self-tests

# Device (standalone):
cmake -S device -B build_device -DPICO_SDK_PATH=$HOME/mp-thumby/lib/pico-sdk
cmake --build build_device -j8
cp build_device/thumbycue.uf2 ../firmware_thumbycue.uf2
```

## Rules

- Host build first; the user flashes for device testing. NEVER claim device
  performance from host runs. Frame-time readout is on screen.
- Never push without user approval. No Co-Authored-By in commits.
- Renderer convention: camera-relative world (camera = origin), view +Z
  forward, depth u16 LARGER = NEARER (d = K/z). Table tris wound CCW-from-front.
- World axes: X along table length, Z across width, **Y up**; cloth at Y=0,
  ball centre at Y=R. SI units everywhere in physics (m, kg, s).
- Dual-core: core0 builds the draw list / steps physics, both cores raster
  banded to their screen half. Park core1 before flash writes.

## Module map (game/)

- `cue_types.h`   — fb constants, RGB565 helpers (vendored from Elite)
- `vec.h`         — Vec3/Mat3 (vendored)
- `r3d_raster.*`  — flat-shaded depth-tested triangle/line/disc raster (vendored)
- `cue_physics.*` — THE ball dynamics (friction phases, collisions, pockets)
- `cue_table.*`   — pool/snooker dimensions, render geometry, collision geometry
- `cue_balls.*`   — sphere-impostor ball renderer (spin texture + specular)
- `cue_render.*`  — scene: background, table tris, balls, cue/aim line
- `cue_camera.*`  — aim-cam orbit + overhead
- `cue_input.*`   — aim / backswing strike / spin selection
- `cue_game.*`    — state machine + rules glue; platform interface
- `craft_font.*`  — 8×8 bitmap font (vendored)

## Current state

Phase 1: physics core + table + host render of a rack. See PLAN.md "Phases".
