#!/usr/bin/env python3
"""Embed the 8 AI-persona avatars from 2dpool (assets/avatars/{initial}.png)
as RGB565 + 8-bit alpha C arrays, at two sizes:
  32x32 — in-play "thinking" corner avatar
  24x24 — PLAY-menu opponent avatars
Persona order matches CUE_PERSONAS in cue_ai.c (R,S,H,P,C,D,N,M)."""
import numpy as np, os
from PIL import Image

SRC = os.path.expanduser("~/2dpool/2dpool/assets/avatars")
OUT = os.path.expanduser("~/thumby-color/ThumbyCue/game/cue_faces.h")
INITIALS = ["R", "S", "H", "P", "C", "D", "N", "M"]   # CUE_PERSONAS order
SIZES = [32, 24]

def to565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)

faces = {s: [] for s in SIZES}
for ini in INITIALS:
    im = Image.open(f"{SRC}/{ini}.png").convert("RGBA")
    for s in SIZES:
        a = np.array(im.resize((s, s), Image.LANCZOS))
        rgb = [to565(int(p[0]), int(p[1]), int(p[2])) for row in a for p in row]
        alp = [int(p[3]) for row in a for p in row]
        faces[s].append((rgb, alp))

def emit_arr(f, ctype, name, rows, perline):
    f.write(f"static const {ctype} {name} = {{\n")
    for r in rows:
        f.write("  {")
        for i in range(0, len(r), perline):
            f.write(",".join(str(v) for v in r[i:i+perline]) + ",")
            f.write("\n   " if i + perline < len(r) else "")
        f.write("},\n")
    f.write("};\n")

with open(OUT, "w") as f:
    f.write("/* AI persona avatars from 2dpool (assets/avatars/<initial>.png), RGB565 + 8-bit\n")
    f.write(" * alpha, persona order R,S,H,P,C,D,N,M. tools/gen_cue_faces.py — no hand-edit. */\n")
    f.write("#ifndef CUE_FACES_H\n#define CUE_FACES_H\n#include <stdint.h>\n")
    f.write(f"#define CUE_NUM_FACES {len(INITIALS)}\n")
    for s in SIZES:
        f.write(f"#define CUE_FACE{s}_W {s}\n#define CUE_FACE{s}_H {s}\n")
        emit_arr(f, "uint16_t", f"cue_face{s}_rgb[CUE_NUM_FACES][{s}*{s}]",
                 [r for r, _ in faces[s]], s)
        emit_arr(f, "uint8_t", f"cue_face{s}_a[CUE_NUM_FACES][{s}*{s}]",
                 [a for _, a in faces[s]], s)
    f.write("#endif\n")
print(f"wrote {OUT}  ({len(INITIALS)} faces @ {SIZES})")
