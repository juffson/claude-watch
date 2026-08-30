#!/usr/bin/env python3
"""
Generate the built-in wallpaper set (sunset / dusk / starry sky themes) and upload it to the board.

  python3 host/default_images.py            generate + upload all, set wallpaper mode to rotate
  python3 host/default_images.py --out DIR  only write .bin files to DIR
  python3 host/default_images.py --only starry_night sunset_sea

Images are 466x466 RGB565 big-endian raw (what the firmware expects). Pure Python, no Pillow needed.
Environment: CLAUDE_WATCH_HOST (default claude-watch.local)
"""
import math
import os
import random
import sys
import urllib.request

HOST = os.environ.get("CLAUDE_WATCH_HOST", "claude-watch.local")
W = 466
C = (W - 1) / 2


def clamp(v, lo=0.0, hi=1.0):
    return lo if v < lo else hi if v > hi else v


def lerp(a, b, t):
    return a + (b - a) * t


def mix(c1, c2, t):
    return tuple(lerp(c1[i], c2[i], t) for i in range(3))


def gradient(stops, t):
    """stops: [(pos, (r,g,b)), ...] sorted by pos; colours 0..1"""
    if t <= stops[0][0]:
        return stops[0][1]
    for (p0, c0), (p1, c1) in zip(stops, stops[1:]):
        if t <= p1:
            return mix(c0, c1, (t - p0) / (p1 - p0) if p1 > p0 else 0)
    return stops[-1][1]


def noise2(x, y, seed=0):
    """cheap smooth value noise via summed sines"""
    return 0.5 + 0.5 * (
        math.sin(x * 0.021 + seed) * math.cos(y * 0.017 + seed * 1.3)
        + 0.5 * math.sin(x * 0.053 - y * 0.031 + seed * 2.1)
        + 0.25 * math.sin(x * 0.11 + y * 0.09 + seed * 3.7)
    ) / 1.75


# ---------------- scenes ----------------

def sunset_sea(x, y):
    t = y / W
    horizon = 0.58
    if t < horizon:
        sky = gradient([(0, (0.10, 0.05, 0.25)), (0.25, (0.55, 0.15, 0.30)), (0.45, (0.95, 0.45, 0.20)), (0.58, (1.0, 0.75, 0.35))], t)
        # sun
        sx, sy = C, horizon * W - 30
        d = math.hypot(x - sx, y - sy)
        sun = clamp(1 - (d - 34) / 6) if d > 34 else 1
        glow = math.exp(-d / 110) * 0.55
        col = mix(sky, (1.0, 0.92, 0.70), sun)
        col = tuple(clamp(c + glow * g) for c, g in zip(col, (0.9, 0.5, 0.1)))
        return col
    # sea
    tt = (t - horizon) / (1 - horizon)
    sea = gradient([(0, (0.85, 0.45, 0.25)), (0.25, (0.45, 0.20, 0.30)), (1, (0.05, 0.03, 0.12))], tt)
    ripple = 0.5 + 0.5 * math.sin(y * 0.9 + math.sin(x * 0.05) * 3)
    band = math.exp(-abs(x - C) / (40 + 160 * tt))  # sun reflection column
    col = tuple(clamp(c + band * ripple * g * (1 - tt)) for c, g in zip(sea, (0.55, 0.30, 0.05)))
    return col


def dusk_mountains(x, y):
    t = y / W
    sky = gradient([(0, (0.05, 0.03, 0.18)), (0.35, (0.35, 0.12, 0.35)), (0.6, (0.95, 0.40, 0.30)), (0.72, (1.0, 0.70, 0.40))], t)
    # ridges
    col = sky
    for k, (base, amp, shade) in enumerate([(0.70, 40, (0.20, 0.08, 0.20)), (0.78, 30, (0.12, 0.05, 0.14)), (0.86, 22, (0.05, 0.02, 0.08))]):
        ridge = base * W + amp * (math.sin(x * 0.013 + k) + 0.5 * math.sin(x * 0.037 + k * 2) + 0.25 * math.sin(x * 0.09 + k * 5))
        if y > ridge:
            edge = clamp((y - ridge) / 3)
            col = mix(col, shade, edge)
    # a few stars up top
    random.seed(7)
    return col


def starry_night(x, y):
    t = y / W
    sky = gradient([(0, (0.01, 0.01, 0.05)), (0.5, (0.03, 0.04, 0.13)), (0.85, (0.08, 0.06, 0.20)), (1, (0.12, 0.08, 0.20))], t)
    neb = noise2(x * 0.6, y * 0.6, 31) * noise2(x * 1.4, y * 0.9, 17)
    return tuple(clamp(c + neb * g) for c, g in zip(sky, (0.08, 0.04, 0.14)))


def milky_way(x, y):
    t = y / W
    sky = gradient([(0, (0.01, 0.01, 0.04)), (1, (0.04, 0.03, 0.10))], t)
    # diagonal band
    d = abs((x - C) * 0.7 + (y - C) * 0.7) / W
    band = math.exp(-(d * 4.2) ** 2) * (0.35 + 0.65 * noise2(x, y, 3))
    dust = noise2(x * 1.7, y * 1.7, 9)
    col = tuple(clamp(c + band * g * (0.5 + 0.5 * dust)) for c, g in zip(sky, (0.55, 0.50, 0.70)))
    return col


def aurora(x, y):
    t = y / W
    sky = gradient([(0, (0.01, 0.02, 0.06)), (1, (0.03, 0.05, 0.10))], t)
    curtain = 0
    for k in range(3):
        cy = 0.33 + 0.10 * math.sin(x * 0.010 + k * 2.1) + 0.04 * math.sin(x * 0.045 + k)
        streak = 0.55 + 0.45 * noise2(x * 5.0, y * 0.25, k + 20)          # vertical rays
        curtain += math.exp(-((t - cy) * 7.5) ** 2) * streak * (0.5 + 0.5 * noise2(x, y * 0.3, k))
    curtain = clamp(curtain * 0.55)
    col = tuple(clamp(c + curtain * g) for c, g in zip(sky, (0.06, 0.42, 0.26)))
    col = tuple(clamp(c + curtain * curtain * g) for c, g in zip(col, (0.25, 0.05, 0.25)))  # magenta tops
    if t > 0.86:
        col = mix(col, (0.01, 0.02, 0.03), clamp((t - 0.86) / 0.02))
    return col


def golden_hour(x, y):
    t = y / W
    sky = gradient([(0, (0.25, 0.10, 0.30)), (0.4, (0.90, 0.35, 0.25)), (0.65, (1.0, 0.65, 0.30)), (1, (0.45, 0.20, 0.20))], t)
    sx, sy = C + 60, 0.62 * W
    d = math.hypot(x - sx, y - sy)
    glow = math.exp(-d / 90) * 0.7
    sun = 1 if d < 46 else clamp(1 - (d - 46) / 5)
    col = mix(sky, (1.0, 0.95, 0.80), sun)
    col = tuple(clamp(c + glow * g) for c, g in zip(col, (0.8, 0.45, 0.1)))
    # clouds
    cl = noise2(x * 1.3, y * 3.5, 5)
    if 0.25 < t < 0.6 and cl > 0.62:
        col = mix(col, (0.55, 0.20, 0.30), clamp((cl - 0.62) * 4) * 0.7)
    return col


def _ridge(x, base, amp, k):
    return base * W + amp * (math.sin(x * 0.011 + k) + 0.55 * math.sin(x * 0.029 + k * 2.3)
                             + 0.3 * math.sin(x * 0.071 + k * 4.1) + 0.15 * math.sin(x * 0.17 + k * 7))


def _peaks(x, k):
    """ridge line for the main range: rolling base + a few broad summits (no high-frequency jaggies)"""
    h = _ridge(x, 0.58, 30, k)
    for cx, hp, wd in ((60, 70, 90), (170, 120, 95), (262, 175, 80), (350, 125, 100), (440, 80, 80)):
        h -= max(0.0, hp * (1 - (abs(x - cx) / wd) ** 1.3))
    return h


def golden_mountain(x, y):
    """日照金山: dawn sky, snow peaks catching the first gold light from the upper right, blue shadows."""
    t = y / W
    sky = gradient([(0, (0.05, 0.07, 0.22)), (0.28, (0.16, 0.19, 0.42)), (0.48, (0.62, 0.42, 0.48)), (0.60, (0.98, 0.72, 0.48))], t)
    col = sky
    h = _peaks(x, 1)
    if y > h:
        depth = (y - h) / W
        slope = (_peaks(x + 12, 1) - _peaks(x - 12, 1)) / 24       # smooth large-scale slope
        lit = 0.5 + 0.5 * math.tanh(-slope * 2.2 + 0.25)         # right-facing faces lit (sun upper right)
        # striations following the slope direction + fine grain
        strata = noise2(x * 2.6 + y * (1.6 if slope < 0 else -1.6), y * 0.9, 11)
        grain = noise2(x * 6, y * 6, 23)
        tex = 0.6 * strata + 0.4 * grain
        # ragged snow line
        snowline = 0.11 + 0.06 * noise2(x * 3.0, 7.0, 5) + 0.03 * (tex - 0.5)
        snow = clamp((snowline - depth) / 0.035 + 0.5)
        snow_lit, snow_shade = (1.00, 0.80, 0.42), (0.52, 0.60, 0.84)
        rock_lit, rock_shade = (0.40, 0.25, 0.18), (0.13, 0.12, 0.21)
        base_c = mix(mix(rock_shade, rock_lit, lit * 0.8), mix(snow_shade, snow_lit, lit), snow)
        glow = clamp(1 - depth / 0.08) * lit                      # alpenglow along the sunlit summits
        base_c = tuple(clamp(c + glow * g) for c, g in zip(base_c, (0.28, 0.10, -0.06)))
        base_c = tuple(clamp(c * (0.88 + 0.24 * tex)) for c in base_c)
        base_c = tuple(c * (1 - 0.55 * clamp(depth / 0.38)) for c in base_c)   # darker towards the valley
        col = mix(col, base_c, clamp((y - h) / 2.0))
    h2 = _ridge(x, 0.78, 34, 5) - 16 * max(0.0, math.sin(x * 0.02 + 1)) ** 4
    if y > h2:
        d2 = (y - h2) / W
        tex2 = noise2(x * 1.5, y * 1.5, 4)
        c2 = mix((0.05, 0.05, 0.10), (0.15, 0.11, 0.16), clamp(1 - d2 / 0.12) * (0.4 + 0.6 * tex2))
        col = mix(col, c2, clamp((y - h2) / 2.0))
    if t > 0.84:
        col = mix(col, (0.09, 0.09, 0.16), clamp((t - 0.84) / 0.16) * 0.85)
    return col


SCENES = {
    "golden_mountain": golden_mountain,
    "sunset_sea": sunset_sea,
    "dusk_mountains": dusk_mountains,
    "starry_night": starry_night,
    "milky_way": milky_way,
    "aurora": aurora,
}
STARS = {"starry_night": 420, "milky_way": 700, "dusk_mountains": 90, "aurora": 160}


def render(name):
    fn = SCENES[name]
    px = [[None] * W for _ in range(W)]
    for y in range(W):
        row = px[y]
        for x in range(W):
            row[x] = fn(x, y)
    # stars: small bright points with soft falloff
    n = STARS.get(name, 0)
    if n:
        random.seed(hash(name) & 0xFFFF)
        limit = {"dusk_mountains": 0.62, "aurora": 0.8}.get(name, 0.92)
        for _ in range(n):
            sx, sy = random.uniform(0, W - 1), random.uniform(0, W * limit)
            b = random.choice([0.5, 0.7, 0.9, 1.0, 1.0]) * random.uniform(0.6, 1.0)
            r = 0.9 if random.random() < 0.85 else 1.8
            tint = random.choice([(1, 1, 1), (0.85, 0.9, 1.0), (1.0, 0.9, 0.75)])
            for dy in range(-2, 3):
                for dx in range(-2, 3):
                    xx, yy = int(sx) + dx, int(sy) + dy
                    if 0 <= xx < W and 0 <= yy < W:
                        d = math.hypot(xx - sx, yy - sy)
                        a = b * math.exp(-(d / r) ** 2)
                        if a > 0.02:
                            c = px[yy][xx]
                            px[yy][xx] = tuple(clamp(cc + a * tt) for cc, tt in zip(c, tint))
    out = bytearray(W * W * 2)
    bayer = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]
    i = 0
    for y in range(W):
        for x in range(W):
            r, g, b = px[y][x]
            dth = bayer[y & 3][x & 3] / 16.0 - 0.5          # ordered dither, +-0.5 LSB
            r5 = min(31, max(0, int(r * 31 + 0.5 + dth)))
            g6 = min(63, max(0, int(g * 63 + 0.5 + dth)))
            b5 = min(31, max(0, int(b * 31 + 0.5 + dth)))
            v = (r5 << 11) | (g6 << 5) | b5
            out[i] = v >> 8
            out[i + 1] = v & 0xFF
            i += 2
    return bytes(out)


def upload(name, data):
    boundary = "----cwimg"
    body = (f"--{boundary}\r\nContent-Disposition: form-data; name=\"file\"; filename=\"{name}.bin\"\r\n"
            f"Content-Type: application/octet-stream\r\n\r\n").encode() + data + f"\r\n--{boundary}--\r\n".encode()
    req = urllib.request.Request(f"http://{HOST}/api/img", data=body, method="POST",
                                 headers={"Content-Type": f"multipart/form-data; boundary={boundary}"})
    with urllib.request.urlopen(req, timeout=120) as r:
        return r.status == 200


def post_json(path, payload):
    req = urllib.request.Request(f"http://{HOST}{path}", data=payload.encode(), method="POST",
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=10) as r:
        return r.status == 200


def main():
    args = sys.argv[1:]
    out_dir = None
    names = list(SCENES)
    if "--out" in args:
        out_dir = args[args.index("--out") + 1]
        os.makedirs(out_dir, exist_ok=True)
    if "--only" in args:
        names = args[args.index("--only") + 1:]
    for name in names:
        print(f"rendering {name} ...", end=" ", flush=True)
        data = render(name)
        if out_dir:
            with open(os.path.join(out_dir, name + ".bin"), "wb") as f:
                f.write(data)
            print("written")
        else:
            print("uploading ...", end=" ", flush=True)
            print("ok" if upload(name, data) else "FAILED")
    if not out_dir:
        post_json("/api/config", '{"wallMode":2,"wallRotateMin":30,"wallDim":45}')
        print("wallpaper: rotate every 30 min, dim 45%")


if __name__ == "__main__":
    main()
