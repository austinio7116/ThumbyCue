#!/usr/bin/env python3
"""Render a ThumbyCue feature-showcase video: back-to-back AI-vs-AI gameplay
across game modes at varied skill levels, felts, frames and ball sets, with the
game's real audio and NO title cards — each native 128x128 frame perspective-
composited onto a photo of the Thumby Color hardware (1:1 on the device screen).

Per segment: warm past the break to a clean "thinking" view, capture video +
frame-synced audio (735 samples/frame @ 22050, ThumbyElite-style), composite
each frame onto the device screen, encode H.264 + AAC, then concatenate.

Usage:  python3 tools/make_video.py [out.mp4] [frames_per_segment]
"""
import os, subprocess, sys, shutil
import numpy as np
from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOST = os.path.join(ROOT, "build_host", "thumbycue_host")
DEVICE = os.path.join(ROOT, "..", "ThumbyOne", "docs", "screenshots", "thumbycue-device.jpg")
WORK = "/tmp/cue_video"
FPS = 30
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "..", "thumbycue_showcase.mp4")
NF = int(sys.argv[2]) if len(sys.argv) > 2 else 450
W = H = 128
# device LCD active-area corners (TL,TR,BR,BL), ~3px overscan so no original-LCD
# rim shows and the bottom stays clear of the THUMBY COLOR logo.
QUAD = [(362,103),(679,106),(680,421),(362,423)]

SEGMENTS = [
    ("UK 8-Ball  slate/ebony/dyna",   0, 2, 4, 6, 4, 3),
    ("US 8-Ball  teal/ebony/protour", 1, 5, 6, 2, 4, 4),
    ("UK 8-Ball  green/walnut/Y-R",   0, 0, 1, 0, 0, 2),
    ("US 9-Ball  navy/silver/vintage",2, 6, 7, 8, 6, 7),
    ("UK 8-Ball  purple/wenge/pink",  0, 1, 2, 5, 3, 5),
    ("Chinese 8  tan/mahogany/space", 3, 3, 5, 7, 2, 6),
    ("Snooker 15  green/oak",         4, 3, 5, 0, 1, 0),
    ("Snooker 10  claret/charcoal",   5, 7, 6, 4, 5, 0),
]

def run(cmd, **kw):
    subprocess.run(cmd, check=True, **kw)

def find_coeffs(out_quad, src):          # PIL PERSPECTIVE maps OUTPUT -> INPUT
    M = []
    for (X, Y), (x, y) in zip(out_quad, src):
        M.append([X, Y, 1, 0, 0, 0, -x*X, -x*Y])
        M.append([0, 0, 0, X, Y, 1, -y*X, -y*Y])
    return np.linalg.solve(np.array(M, float), np.array(src, float).reshape(8))

def main():
    if not os.path.exists(HOST): sys.exit(f"host binary not found: {HOST}")
    if not os.path.exists(DEVICE): sys.exit(f"device image not found: {DEVICE}")
    if shutil.which("ffmpeg") is None: sys.exit("ffmpeg not found")

    dev = Image.open(DEVICE).convert("RGB")
    DW, DH = dev.size
    coeffs = find_coeffs(QUAD, [(0,0),(W,0),(W,H),(0,H)])
    mask = Image.new("L", (DW, DH), 0)
    ImageDraw.Draw(mask).polygon(QUAD, fill=255)

    shutil.rmtree(WORK, ignore_errors=True); os.makedirs(WORK)
    clips = []
    for i, (label, mode, p1, p2, cl, fr, bs) in enumerate(SEGMENTS):
        fdir = os.path.join(WORK, f"s{i}"); os.makedirs(fdir)
        cdir = os.path.join(WORK, f"c{i}"); os.makedirs(cdir)
        env = dict(os.environ, CUE_VIDEO=fdir, CUE_MODE=str(mode), CUE_P1=str(p1),
                   CUE_P2=str(p2), CUE_CLOTH=str(cl), CUE_FRAME=str(fr),
                   CUE_BALLSET=str(bs), CUE_VFRAMES=str(NF))
        print(f"[{i+1}/{len(SEGMENTS)}] {label} — capture...")
        run([HOST], env=env)
        print(f"    compositing {NF} frames onto the device...")
        for f in range(NF):
            fr_img = Image.open(os.path.join(fdir, f"f{f:05d}.ppm")).convert("RGB")
            warp = fr_img.transform((DW, DH), Image.PERSPECTIVE, coeffs, Image.NEAREST)
            out = dev.copy(); out.paste(warp, (0, 0), mask)
            out.save(os.path.join(cdir, f"c{f:05d}.png"))
        spath = os.path.join(WORK, f"s{i}.mp4")
        run(["ffmpeg","-y","-loglevel","error",
             "-framerate",str(FPS),"-i",os.path.join(cdir,"c%05d.png"),
             "-f","s16le","-ar","22050","-ac","1","-i",os.path.join(fdir,"audio.raw"),
             "-vf","scale=trunc(iw/2)*2:trunc(ih/2)*2,format=yuv420p",
             "-c:v","libx264","-r",str(FPS),"-c:a","aac","-b:a","96k","-shortest",spath])
        clips.append(spath)
        shutil.rmtree(fdir, ignore_errors=True); shutil.rmtree(cdir, ignore_errors=True)

    listf = os.path.join(WORK, "list.txt")
    with open(listf, "w") as f:
        for c in clips: f.write(f"file '{c}'\n")
    print("concatenating...")
    run(["ffmpeg","-y","-loglevel","error","-f","concat","-safe","0","-i",listf,
         "-c:v","libx264","-pix_fmt","yuv420p","-r",str(FPS),
         "-c:a","aac","-b:a","96k", os.path.abspath(OUT)])
    print(f"\nDONE -> {os.path.abspath(OUT)}")

if __name__ == "__main__":
    main()
