# ThumbyCue AI — port plan (from ~/2dpool ai.js)

Replicate the 2dpool opponent AI: simulation-driven shot selection with
positional play, and distinct persona skill levels. Drop the cut-induced-throw
compensation (our engine pots cleanly; theirs needed it). New module
`game/cue_ai.{c,h}`, replacing the `cpu_plan()` stub in cue_game.c.

## Architecture

- `cue_ai.c/.h` — the planner. Input: world + balls + rules + persona. Output:
  a `CueShot { float aim, power01, tip_side, tip_vert; }` (matches what
  begin_shot consumes), or a "play safe" shot, or a ball-in-hand placement.
- Reuse `cue_phys_step()` as the headless simulator: clone the live ball array
  into a scratch buffer, strike the cue, step to settle, read potted + leaves.
- Valid targets come from `cue_rules_ball_legal()`; snooker state from CueRules.

## Unit adaptation (pixels → SI)

2dpool is in pixels (~800px table, power clamp [5,55]). ThumbyCue is SI
(~1.8 m table, strike speed 0..MAX_STRIKE_SPEED m/s). Port the **unitless**
parts verbatim (scoring weights, normalized distances, cut-angle score) and
recalibrate only the power mapping against our physics by measated pot range.

## Simulation budget on device (the key perf decision)

The full variant sweep is large (≈14 powers × ≤7 spins per pot). Strategy:
- **Analytic** cue-path prediction for the wide sweep (cheap): ghost-ball
  deflection + follow/draw/stun roll + one cushion bounce — port
  `predictEndPosition` / `predictCueBallPath`.
- **Full physics sim** only for the top ~6 viable variants, at a coarser AI
  timestep (1/600 s, not 1/2000) with a hard substep cap and early-out when
  the cue + relevant balls settle.
- Run synchronously during the think delay (table is idle — dropping a few
  frames of a static scene is invisible). Move to core1 only if it stutters.

## Phases

- **A — Core potting AI (pool).** Enumerate pots, ghost-ball geometry, cut
  angle, path-clear, scoreShot, persona aim/power noise, selectShot. Basic
  safety fallback. Playable opponent for UK8/US8/US9. (cue_ai core + geometry)
- **B — Positional play.** Power/spin variant sweep, analytic predictEndPosition,
  evaluatePositionQuality (best next-shot from the leave), full-physics
  resimulation of the top pool, pot↔position blend by persona.position. This is
  what makes it feel strong.
- **C — Snooker layer.** Ball values + bonuses, safety/snooker gates, snooker
  urgency thresholds, free balls, colour nomination, break positioning.
- **D — Personas + polish.** Port the 8-persona roster, thinking delay, foul
  avoidance, difficulty/opponent selection in the UI.

## Key functions to port (ai.js → cue_ai.c)

geometry: calculateGhostBall, getPocketAimPoint, calculateCutAngle,
calculatePottingDifficulty, isPathClear, calculateBankShot
scoring:  scoreShot, calculatePower, analyzeShotVariants, selectShot
position: predictEndPosition, predictCueBallPath, evaluatePositionQuality,
          resimulateTopCandidates (→ cue_phys_step), findBestShot
safety:   findBestSafetyShot, snooker gates, makeSnookerFoulDecision
placement: findBestCueBallPosition, findBestDZonePosition
