"""叠图校验：完全复现 MapPreview.ets 的投影数学，把高德路线折线画到高德静态底图上。

用途：验证"App 地图与实际路线整体偏移"是不是投影/坐标问题。
若折线压在街道上 => 投影正确，偏移另有来源；若整体平移 => 能量化偏移像素。
用法: python amap_overlay_check.py <origLon> <origLat> <destLon> <destLat> <out.png>
"""
import json
import math
import sys
import urllib.parse
import urllib.request

KEY = "REDACTED_AMAP_KEY"
W, H = 900, 600          # 与 MapPreview.W / H 一致
PAD = 56


def px(lon, world):
    return (lon + 180.0) / 360.0 * world


def py(lat, world):
    rad = math.radians(lat)
    y = (1 - math.log(math.tan(rad) + 1 / math.cos(rad)) / math.pi) / 2
    return y * world


def lat_from_py(pyv, world):
    n = math.pi * (1 - 2 * pyv / world)
    return math.degrees(math.atan(math.sinh(n)))


def get(url):
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
    with urllib.request.urlopen(req, timeout=20) as r:
        return r.read()


olo, ola, dlo, dla = map(float, sys.argv[1:5])
out = sys.argv[5]

route_url = (
    "https://restapi.amap.com/v5/direction/walking?key=%s&origin=%.6f,%.6f&destination=%.6f,%.6f"
    "&show_fields=polyline&isindoor=0" % (KEY, olo, ola, dlo, dla)
)
body = json.loads(get(route_url))
if body.get("status") != "1":
    print("direction error:", body.get("info"), body.get("infocode"))
    sys.exit(1)

path = body["route"]["paths"][0]
pts = []
for st in path["steps"]:
    for kv in st["polyline"].split(";"):
        a = kv.split(",")
        if len(a) == 2:
            pts.append((float(a[1]), float(a[0])))   # lat, lon
print("route pts =", len(pts), " distance =", path["distance"])

min_la = min(p[0] for p in pts); max_la = max(p[0] for p in pts)
min_lo = min(p[1] for p in pts); max_lo = max(p[1] for p in pts)

z = 17
while z > 8:
    world = 256 * (2 ** z)
    sx = abs(px(max_lo, world) - px(min_lo, world))
    sy = abs(py(min_la, world) - py(max_la, world))
    if sx <= W - PAD and sy <= H - PAD:
        break
    z -= 1
world = 256 * (2 ** z)
print("zoom =", z)

c_lon = (min_lo + max_lo) / 2
cy_px = (py(min_la, world) + py(max_la, world)) / 2
c_lat = lat_from_py(cy_px, world)
cx_px = px(c_lon, world)

static_url = (
    "https://restapi.amap.com/v3/staticmap?location=%.6f,%.6f&zoom=%d&size=%d*%d&scale=1&key=%s"
    % (c_lon, c_lat, z, W, H, KEY)
)
raw = get(static_url)
print("static bytes =", len(raw))
with open(out.replace(".png", "_base.png"), "wb") as f:
    f.write(raw)

from PIL import Image, ImageDraw

im = Image.open(out.replace(".png", "_base.png")).convert("RGB")
print("base image =", im.size)
dr = ImageDraw.Draw(im)

xy = []
for (la, lo) in pts:
    x = 0.5 + (px(lo, world) - cx_px) / W
    y = 0.5 + (py(la, world) - cy_px) / H
    xy.append((x * im.size[0], y * im.size[1]))

dr.line(xy, fill=(166, 255, 0), width=4)
dr.ellipse([xy[0][0] - 7, xy[0][1] - 7, xy[0][0] + 7, xy[0][1] + 7], fill=(96, 38, 158))
dr.ellipse([xy[-1][0] - 8, xy[-1][1] - 8, xy[-1][0] + 8, xy[-1][1] + 8], outline=(255, 51, 51), width=3)

im.save(out)
print("saved", out)
print("origin px =", xy[0], " dest px =", xy[-1])
