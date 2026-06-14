# ThumbyCue tools

- `phystest.c` — standalone physics self-tests (12 checks). Build/run:
  `gcc -O2 -ffast-math -I../game phystest.c ../game/cue_physics.c \
   ../game/cue_table.c -lm -o /tmp/phystest && /tmp/phystest`

Rules/scoring reference for the upcoming rules pass: ~/2dpool/2dpool/js/game.js
(US/UK 8-ball, 9-ball, snooker — fouls, free ball, nomination, colour sequence).
