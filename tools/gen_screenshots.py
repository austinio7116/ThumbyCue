#!/usr/bin/env python3
"""Regenerate all README screenshots into docs/img/ from build_host/thumbycue_host.
Run from anywhere:  python3 tools/gen_screenshots.py
"""
import os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOST = os.path.join(ROOT, "build_host", "thumbycue_host")
IMG  = os.path.join(ROOT, "docs", "img")
os.makedirs(IMG, exist_ok=True)

# half_len*0.5 (foot spot x) per CueGameKind: UK8 US8 US9 CN8 SNK15 SNK10 SNK6
FOOT = {0:0.495, 1:0.635, 2:0.635, 3:0.71, 4:0.89, 5:0.71, 6:0.495}

def shot(name, env, size=256, ppm="/tmp/_shot.ppm"):
    from PIL import Image
    e = dict(os.environ, CUE_SHOT=ppm)
    e.update({k: str(v) for k, v in env.items()})
    subprocess.run([HOST], env=e, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    im = Image.open(ppm).convert("RGB").resize((size, size), Image.NEAREST)
    im.save(os.path.join(IMG, name)); print("  ", name)

def rack_cam(mode):
    fx = FOOT[mode]
    return f"{fx*1.86:.3f},0.14,0.11,{fx*0.866:.3f},0.02,0.0,42"

def main():
    if not os.path.exists(HOST): sys.exit(f"build the host first: {HOST}")

    print("in-game views:")
    shot("title.png",   {"CUE_MENU":1},                          320)
    shot("menu.png",    {"CUE_MENU":1, "CUE_PLAYMENU":1},        320)
    shot("pool.png",    {"CUE_MODE":1, "CUE_CLOTH":0},           250)
    shot("snooker.png", {"CUE_MODE":4},                          250)
    shot("snooker_score.png", {"CUE_MODE":4},                    250)
    shot("nineball.png",{"CUE_MODE":2, "CUE_FREELOOK":1},        250)
    shot("overhead.png",{"CUE_MODE":1, "CUE_TOPDOWN":1},         250)
    shot("rack.png",    {"CUE_MODE":1, "CUE_NOHUD":1, "CUE_BALLSET":0,
                         "CUE_CLOTH":0, "CUE_CAM":rack_cam(1)},  250)

    print("felt colours (US8 rack):")
    felt = ["green","blue","teal","red","claret","purple","slate","tan","navy","black"]
    for i, nm in enumerate(felt):
        shot(f"felt-{nm}.png", {"CUE_MODE":1, "CUE_NOHUD":1, "CUE_BALLSET":0,
                                "CUE_CLOTH":i, "CUE_CAM":rack_cam(1)}, 180)

    print("ball sets:")
    # (file, mode, ballset)
    sets = [
        ("rack-uk8-yb.png",      0, 1), ("rack-uk8-yr.png",      0, 2),
        ("rack-us8-pro.png",     1, 0), ("rack-us8-dyna.png",    1, 3),
        ("rack-us8-protour.png", 1, 4), ("rack-us8-hotpink.png", 1, 5),
        ("rack-us8-space.png",   1, 6), ("rack-us8-vintage.png", 1, 7),
        ("rack-9ball.png",       2, 0), ("rack-cn8.png",         3, 0),
    ]
    for nm, mode, bs in sets:
        shot(nm, {"CUE_MODE":mode, "CUE_NOHUD":1, "CUE_BALLSET":bs,
                  "CUE_CLOTH":0, "CUE_CAM":rack_cam(mode)}, 180)
    # snooker rack uses the in-game snooker camera (reds triangle + colours)
    shot("rack-snooker.png", {"CUE_MODE":4, "CUE_NOHUD":1, "CUE_CAM":rack_cam(4)}, 180)

    print("done")

if __name__ == "__main__":
    main()
