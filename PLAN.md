# ThumbyCue — Accurate 3D Snooker & Pool for Thumby Color

A physically-accurate cue-sports simulation for the Thumby Color (RP2350 @
280 MHz dual Cortex-M33+FPU, 520 KB SRAM, 128×128 RGB565 GC9107, 22050 Hz PWM
audio). Built on the ThumbyElite flat-shaded depth-tested **dual-core triangle
rasterizer** (`r3d_raster`) plus **per-pixel sphere impostors** (adapted from
the Elite planet renderer) so balls are smooth shaded spheres that *visibly
spin* — their markings rotate with real angular velocity.

Two games on one shared engine: **8-ball pool** (7 ft table) and **snooker**
(12 ft table). Both share the physics, renderer, camera and controls; they
differ only in table dimensions, pocket geometry, ball set and rules.

## Locked design decisions (user, 2026-06-14)

- **Camera**: aim-cam (orbit the cue ball, low 3D angle, d-pad fine-aim) with a
  top-down overhead planning view you toggle to. LB/RB orbit + zoom.
- **Scope**: shared engine → BOTH pool and snooker rulesets, polish last.
- **Controls — backswing-timing strike**:
  - D-pad: rotate aim (fine when zoomed)
  - A: begin backswing; hold/draw the cue back (power = draw distance), A again
    to strike; (or release model — settle on device feel)
  - B: spin/english mode — d-pad moves the contact point on the cue ball
    (tip offset → side/top/back spin)
  - LB / RB: orbit camera / zoom (hold), tap = overhead toggle / fine-aim
  - MENU: short = pause; long-hold 1.2 s = return to ThumbyOne lobby (slot mode)

## Why this engine fits cue sports

- `r3d_raster.c` — flat-shaded, depth-tested (`u16`, LARGER = NEARER), incremental
  edge-function triangle fill; dual-core banded (core0 rows 0–63, core1 64–127).
  Renders the table bed, rails, cushions, pocket jaws as flat-shaded triangles.
- Sphere impostor (new `cue_balls.c`, technique from `r3d_planet.c`): per-pixel
  sphere-normal reconstruction inside the projected disc → Lambert + specular
  highlight + sample a small per-ball equirect texture rotated by the ball's
  orientation matrix. Balls roll and spin visibly, anti-aliased, cheap.
- Camera-relative world (camera = origin) keeps floats small and precise.

## Physics model (the crown jewel — `cue_physics.c`)

SI units (m, kg, s). Table plane = world X–Z, **Y up**, ball centre at Y = R.
Per ball: `pos` (Y fixed = R in v1, no jumps), `vel` (Vec3), angular velocity
`w` (Vec3, full 3D: horizontal = roll/top/back, vertical Y = side/english),
plus an orientation `Mat3` integrated from `w` for rendering.

Decoupled fixed-step integrator (≈1500 Hz substeps) that runs only while balls
move (the table is still between shots). Phases per ball, from the **cloth
contact-point velocity** `u = vel + w × r_c`, `r_c = (0, −R, 0)`:

1. **Sliding** (|u_horiz| > ε): kinetic friction `a = −μ_s·g·û` on `vel`;
   friction torque `dw/dt = (1/I)·(r_c × m·a)`, `I = (2/5)mR²` — this is what
   turns draw/follow/stun into the right post-contact roll.
2. **Rolling** (u → 0): rolling resistance `a = −μ_r·g·v̂`; `w` slaved to the
   rolling constraint `w_horiz = (ŷ × vel)/R`.
3. **Spinning**: residual vertical spin `w_y` decays at a tuned constant rate
   (`μ_sp` drilling friction) independent of motion → realistic side carry.

Collisions:
- **Ball–ball**: instantaneous, continuous-collision-detected within a substep
  (solve the quadratic for earliest |Δp|=2R, no tunnelling). Normal impulse with
  restitution `e_bb≈0.95`; tangential friction impulse (`μ_bb`) capped by the
  normal impulse → **throw / collision-induced spin transfer** (cut-induced
  throw is real and visible).
- **Ball–cushion**: cushion nose contacts **above centre** (height ≈ 1.4 R), so
  the model produces the spin-dependent rebound angle and side off the cushion
  (Han 2005 / Mathavan simplified): reflect normal vel with `e_c`, rail friction
  couples side-spin ↔ outgoing angle and `w`.
- **Pockets**: jaw tips modelled as small circle colliders so balls *rattle*;
  a capture test at each pocket centre pots the ball when geometry/speed allow.

Self-tests (host `CUE_PHYSTEST`): energy monotonic decay, stun/draw/follow stop
distances, straight-shot roll distance vs speed, 90°-rule on a half-ball cut,
throw angle sign. These guard "perfect physics" against regressions.

## Tables (`cue_table.c`)

- **Pool (7 ft)**: playing area 1.98 × 0.99 m, ball R = 28.575 mm, 6 pockets
  (corner mouth ~5", side ~5.5"), straight rubber cushions, ~37° rail.
- **Snooker (12 ft)**: playing area 3.569 × 1.778 m, ball R = 26.1875 mm,
  rounded pocket jaws, tight templates, baulk line + the "D", spots.
- Each table emits: bed triangles (cloth), 4–6 cushion rubbers (angled tops),
  rail/frame triangles, pocket cut-outs + jaw circles, spots/lines. Collision
  geometry is derived from the same dimension table so render and physics agree.

## Rendering (`cue_render.c`, `cue_balls.c`)

Lean scene built directly on `r3d_raster` (NOT Elite's `r3d_scene`/starfield):
1. Background: room/baize gradient fill, depth clear (banded, both cores).
2. Table triangles via `r3d_tri` (flat shade vs a fixed overhead key light +
   soft fill; cloth gets a subtle nap tint).
3. Ball impostors (`cue_balls.c`): project centre + screen radius, per-pixel
   sphere normal, Lambert + Blinn specular, rotated texture (numbers/stripes/
   spots/cue dot), shadow ellipse on the cloth. Depth-tested & writing.
4. Cue stick (aim state): a shaded line/thin quad from tip through the aim line;
   aiming line + ghost-ball target ring projected on the cloth.
5. HUD overlay (`craft_font`): power bar, spin indicator, score, fouls, turn.

## Controls / camera (`cue_input.c`, `cue_camera.c`)

Aim-cam orbits the cue ball at a low elevation; d-pad L/R rotates the shot
azimuth, U/D adjusts elevation/zoom. LB/RB orbit + zoom; tap toggles overhead.
Backswing-timing strike as above. Spin mode overlays a cue-ball face with a
movable contact dot (tip offset clamped to a safe miscue radius).

## Game / rules (`cue_game.c`, rules tables)

State machine: TITLE → (pick POOL/SNOOKER) → AIM → SHOT (physics runs, camera
follows) → RESOLVE (rules: legal pot? foul? respot?) → next turn / frame end.
- **8-ball pool**: open table, group assignment on first legal pot, must pot the
  8 last & call pocket, standard fouls (scratch, wrong ball first, no rail).
- **Snooker**: red→colour alternation, colours respot, ball-in-hand from the D
  after foul- on, foul point values, free ball (later), frame scoring.
v1 may ship pool rules first with snooker scoring close behind.

## Platform shells (mirror ThumbyElite exactly)

Game exposes: `cue_game_init(seed)`, `cue_game_tick(buttons,dt)`,
`cue_game_render_begin()`, `cue_game_render(fb,y0,y1)`,
`cue_game_draw_overlay(fb)`, `cue_game_set_frame_ms(ms)`.
- `host/` — SDL2: keyboard→`CraftRawButtons`, SDL audio, `CUE_SHOT=` PPM dump,
  128×128 scaled up. Lean rewrite (Elite's host_main is 306 KB and game-specific).
- `device/` — RP2350: vendored `craft_lcd_gc9107`, `craft_buttons`,
  `craft_audio_pwm`, `craft_rumble`; dual-core go/done handshake; standalone UF2
  `firmware_thumbycue.uf2` + later a ThumbyOne slot.

## Build

```bash
# Host (all dev first):
cmake -B build_host -S host && cmake --build build_host -j8
./build_host/thumbycue_host [pool|snooker]
CUE_SHOT=/tmp/s.ppm ./build_host/thumbycue_host pool   # headless screenshot
CUE_PHYSTEST=1 ./build_host/thumbycue_host             # physics self-tests

# Device (standalone):
cmake -S device -B build_device -DPICO_SDK_PATH=$HOME/mp-thumby/lib/pico-sdk
cmake --build build_device -j8
cp build_device/thumbycue.uf2 ../firmware_thumbycue.uf2
```

## Phases

1. **Engine + physics core** — vendored raster, `cue_physics` with self-tests,
   table dims, host build that renders a table and breaks a rack (host first).
2. **Rendering polish** — ball impostors w/ spin, cushions/jaws, aim line, cue.
3. **Camera + controls** — aim-cam orbit + overhead, backswing strike, spin UI.
4. **Pool rules** — full 8-ball turn loop, fouls, win.
5. **Snooker rules** — sequence, colours respot, scoring, ball-in-hand.
6. **Audio + rumble + save**, device validation + perf lock, ThumbyOne slot.

## Conventions

- Host build first; NEVER claim device perf from host. Frame-time on screen.
- Renderer: camera-relative world, view +Z forward, depth u16 LARGER=NEARER,
  table triangles CCW-from-visible.
- Dual-core: core0 builds, both cores raster banded to their half; park core1
  before flash writes.
- Never push without approval. No Co-Authored-By in commits (own repo).
</invoke>
