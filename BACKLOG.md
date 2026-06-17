# ThumbyCue — backlog

Deferred items (not blocking; revisit when convenient).

- **AI scratches too often.** AI players (even high-skill personas) pot the cue
  ball / go in-off frequently. The shot sim hard-rejects scratch variants, so
  this is likely the *played* shot diverging from the *simulated* one — e.g. the
  chosen power/spin/aim variant isn't the one actually struck, or the sim's
  cue-path/cushion model under-predicts the real follow of the cue ball into a
  pocket. Investigate: (1) does the executed strike match the winning sim
  variant exactly (power01, tip side/vert, elev, aim)? (2) is the headless sim's
  pocket capture for the *cue ball* as aggressive as the live physics? Reported
  2026-06-16.

- ~~Push-out shot isn't specially planned~~ **DONE 2026-06-16.** `cue_ai_pushout()`
  searches a fan of directions × powers (sim each), rejects scratches, and leaves
  a *moderately* difficult shot (target pot confidence ~42, MED in cue_ai.c) — NOT
  the worst (a dead leave just gets passed back to us) and not a gift. Tunable:
  MED, the power set, and the direction count. Could be smarter still (model the
  opponent's actual take/pass threshold per persona instead of a fixed MED).
