"""gen_selector_bg.py — 生成三程序选择器底图（466x466 RGB565 LE）

严格按用户 2026-11-21 版设计稿 `assets/selector.svg` 复刻：
    viewBox 192.756 x 192.756 → 466 px（缩放 S = 466 / 192.756）
    左上 紫 #5B1E7A + 内环 #2A0A3D + NERV 徽标（translate(27,25) scale(0.36)）
    右上 红 #E10600 + 内环 #5C0300 + F1 logo（translate(109,23) scale(2.3)）
    下半 蓝 #1E6FB8 + 内环 #0E3A5F + 摩托车（x46.4 y105.6 w100 h58.1，白底抠透明）

徽标仍由内置 SVG path 解析器展平后光栅化（evenodd / nonzero 各按原样），
摩托车由 `moto_rgba.raw`（242x141 RGBA，白底→alpha 已按 knockoutWhite 的
tableValues="1 1 0 0" 算好）alpha 混合贴入。

纯标准库实现，无 PIL / cairosvg 依赖。

用法：
    python gen_selector_bg.py
产物：
    firmware/app_sel/main/assets/sel_bg.bin   （编译用，CMake EMBED_FILES）
    firmware/app_sel/assets/sel_bg.bin        （源副本）
    D:\\豆包ESP32项目\\selector_preview.png
"""
import os, re, struct, zlib, math

W = H = 466
VB = 192.756
S = W / VB                     # 2.417512

CX = CY = 96.378 * S           # 233.0
R_OUT = 96.378 * S             # 233.0
RING_R = 90.0 * S              # 217.58
RING_HW = 3.0 * S / 2.0        # 3.63（stroke 3 → 半宽）

SPLIT_X = SPLIT_Y = 233

PURPLE = (0x5B, 0x1E, 0x7A)
PURPLE_RING = (0x2A, 0x0A, 0x3D)
RED = (0xE1, 0x06, 0x00)
RED_RING = (0x5C, 0x03, 0x00)
BLUE = (0x1E, 0x6F, 0xB8)
BLUE_RING = (0x0E, 0x3A, 0x5F)

NERV_COLOR = (0x2E, 0x0B, 0x45)
F1_COLOR = (0x1A, 0x1A, 0x1A)

MOTO_RAW = "D:\\豆包ESP32项目\\moto_rgba.raw"
MOTO_W, MOTO_H = 242, 141
MOTO_X, MOTO_Y = 112, 255

NERV_RAW = "D:\\豆包ESP32项目\\nerv_rgba.raw"
NERV_W = NERV_H = 168
NERV_X, NERV_Y = 65, 60

buf = [[(0, 0, 0)] * W for _ in range(H)]


def zone(x, y):
    if y < SPLIT_Y:
        return 0 if x < SPLIT_X else 1
    return 2


def clamp(v, lo, hi):
    return lo if v < lo else (hi if v > hi else v)


def inside(x, y, margin=1.0):
    dx = x + 0.5 - CX
    dy = y + 0.5 - CY
    return dx * dx + dy * dy < (R_OUT - margin) ** 2


# ---------- 底色：三块纯色 ----------
for y in range(H):
    row = buf[y]
    for x in range(W):
        dx = x + 0.5 - CX
        dy = y + 0.5 - CY
        r = math.hypot(dx, dy)
        if r > R_OUT + 1.5:
            continue
        col = (PURPLE, RED, BLUE)[zone(x, y)]
        a = clamp((R_OUT + 0.5 - r) / 1.5, 0.0, 1.0)      # 1.5 px 软边
        row[x] = (int(col[0] * a), int(col[1] * a), int(col[2] * a))

# ---------- 内环：按区域上色 ----------
for y in range(H):
    for x in range(W):
        if not inside(x, y):
            continue
        d = abs(math.hypot(x + 0.5 - CX, y + 0.5 - CY) - RING_R)
        a = clamp(RING_HW + 0.5 - d, 0.0, 1.0)
        if a <= 0:
            continue
        rc = (PURPLE_RING, RED_RING, BLUE_RING)[zone(x, y)]
        base = buf[y][x]
        buf[y][x] = (int(base[0] * (1 - a) + rc[0] * a),
                     int(base[1] * (1 - a) + rc[1] * a),
                     int(base[2] * (1 - a) + rc[2] * a))


REGION = "all"   # all / nav / f1 / moto


def px(x, y, color):
    if not (0 <= x < W and 0 <= y < H and inside(x, y)):
        return
    if REGION == "nav" and not (x < SPLIT_X and y < SPLIT_Y):
        return
    if REGION == "f1" and not (x >= SPLIT_X and y < SPLIT_Y):
        return
    if REGION == "moto" and not (y >= SPLIT_Y):
        return
    buf[y][x] = color


def blend(x, y, color, a):
    if not (0 <= x < W and 0 <= y < H and inside(x, y)):
        return
    base = buf[y][x]
    buf[y][x] = (int(base[0] * (1 - a) + color[0] * a),
                 int(base[1] * (1 - a) + color[1] * a),
                 int(base[2] * (1 - a) + color[2] * a))


# ===========================================================================
# 内置 SVG path 解析：M L H V C S Q T A Z（大小写）
# ===========================================================================
CMDCH = "MmLlHhVvCcSsQqTtAaZz"
NUMRE = r"[-+]?(?:\d+\.\d+|\.\d+|\d+)(?:[eE][-+]?\d+)?"
TOKRE = re.compile(r"[" + CMDCH + r"]|" + NUMRE)


def _bez3(p0, p1, p2, p3, n=14):
    out = []
    for i in range(1, n + 1):
        t = i / n
        mt = 1 - t
        out.append((mt * mt * mt * p0[0] + 3 * mt * mt * t * p1[0] + 3 * mt * t * t * p2[0] + t * t * t * p3[0],
                    mt * mt * mt * p0[1] + 3 * mt * mt * t * p1[1] + 3 * mt * t * t * p2[1] + t * t * t * p3[1]))
    return out


def _bez2(p0, p1, p2, n=12):
    out = []
    for i in range(1, n + 1):
        t = i / n
        mt = 1 - t
        out.append((mt * mt * p0[0] + 2 * mt * t * p1[0] + t * t * p2[0],
                    mt * mt * p0[1] + 2 * mt * t * p1[1] + t * t * p2[1]))
    return out


def _arc(x0, y0, rx, ry, phi_deg, large, sweep, x1, y1, n=18):
    if rx == 0 or ry == 0:
        return [(x1, y1)]
    rx, ry = abs(rx), abs(ry)
    phi = math.radians(phi_deg)
    dx2 = (x0 - x1) / 2.0
    dy2 = (y0 - y1) / 2.0
    x1p = math.cos(phi) * dx2 + math.sin(phi) * dy2
    y1p = -math.sin(phi) * dx2 + math.cos(phi) * dy2
    lam = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry)
    if lam > 1:
        k = math.sqrt(lam)
        rx *= k
        ry *= k
    num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p
    den = rx * rx * y1p * y1p + ry * ry * x1p * x1p
    co = math.sqrt(max(0.0, num / den)) if den else 0.0
    if large == sweep:
        co = -co
    cxp = co * rx * y1p / ry
    cyp = -co * ry * x1p / rx
    ccx = math.cos(phi) * cxp - math.sin(phi) * cyp + (x0 + x1) / 2.0
    ccy = math.sin(phi) * cxp + math.cos(phi) * cyp + (y0 + y1) / 2.0

    def ang(ux, uy, vx, vy):
        n1 = math.hypot(ux, uy) * math.hypot(vx, vy)
        if n1 == 0:
            return 0.0
        d = max(-1.0, min(1.0, (ux * vx + uy * vy) / n1))
        a = math.acos(d)
        if ux * vy - uy * vx < 0:
            a = -a
        return a

    th1 = ang(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry)
    dth = ang((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry)
    if not sweep and dth > 0:
        dth -= 2 * math.pi
    if sweep and dth < 0:
        dth += 2 * math.pi
    out = []
    for i in range(1, n + 1):
        th = th1 + dth * i / n
        out.append((ccx + rx * math.cos(th) * math.cos(phi) - ry * math.sin(th) * math.sin(phi),
                    ccy + rx * math.cos(th) * math.sin(phi) + ry * math.sin(th) * math.cos(phi)))
    return out


def _close(sub):
    """填充语义下子路径隐式闭合：末点若不在起点则补回起点（否则扫描线填充会漏边）。"""
    if sub and (abs(sub[-1][0] - sub[0][0]) > 1e-9 or abs(sub[-1][1] - sub[0][1]) > 1e-9):
        sub.append(sub[0])
    return sub


def parse_path(d):
    parts = TOKRE.findall(d)
    idx, n = 0, len(parts)
    cx = cy = sx = sy = 0.0
    subs, pts = [], []
    cmd = None
    last_c = last_q = None

    def rd():
        nonlocal idx
        v = float(parts[idx])
        idx += 1
        return v

    while idx < n:
        t = parts[idx]
        if t in CMDCH:
            cmd = t
            idx += 1
            if cmd in "Zz":
                if pts:
                    pts.append((sx, sy))
                    subs.append(_close(pts))
                    pts = []
                cx, cy = sx, sy
                last_c = last_q = None
                continue
        else:
            if cmd is None:
                break
            if cmd in "Mm":
                cmd = "L" if cmd == "M" else "l"

        if cmd in "Mm":
            x, y = rd(), rd()
            if cmd == "m":
                x += cx; y += cy
            if pts:
                subs.append(_close(pts))
            pts = [(x, y)]
            cx = sx = x
            cy = sy = y
            last_c = last_q = None

        elif cmd in "Ll":
            x, y = rd(), rd()
            if cmd == "l":
                x += cx; y += cy
            pts.append((x, y)); cx, cy = x, y
            last_c = last_q = None

        elif cmd in "Hh":
            x = rd()
            if cmd == "h":
                x += cx
            pts.append((x, cy)); cx = x
            last_c = last_q = None

        elif cmd in "Vv":
            y = rd()
            if cmd == "v":
                y += cy
            pts.append((cx, y)); cy = y
            last_c = last_q = None

        elif cmd in "Cc":
            x1, y1, x2, y2, x, y = rd(), rd(), rd(), rd(), rd(), rd()
            if cmd == "c":
                x1 += cx; y1 += cy; x2 += cx; y2 += cy; x += cx; y += cy
            pts += _bez3((cx, cy), (x1, y1), (x2, y2), (x, y))
            last_c = (x2, y2); last_q = None
            cx, cy = x, y

        elif cmd in "Ss":
            x2, y2, x, y = rd(), rd(), rd(), rd()
            if cmd == "s":
                x2 += cx; y2 += cy; x += cx; y += cy
            x1, y1 = (2 * cx - last_c[0], 2 * cy - last_c[1]) if last_c else (cx, cy)
            pts += _bez3((cx, cy), (x1, y1), (x2, y2), (x, y))
            last_c = (x2, y2); last_q = None
            cx, cy = x, y

        elif cmd in "Qq":
            x1, y1, x, y = rd(), rd(), rd(), rd()
            if cmd == "q":
                x1 += cx; y1 += cy; x += cx; y += cy
            pts += _bez2((cx, cy), (x1, y1), (x, y))
            last_q = (x1, y1); last_c = None
            cx, cy = x, y

        elif cmd in "Tt":
            x, y = rd(), rd()
            if cmd == "t":
                x += cx; y += cy
            x1, y1 = (2 * cx - last_q[0], 2 * cy - last_q[1]) if last_q else (cx, cy)
            pts += _bez2((cx, cy), (x1, y1), (x, y))
            last_q = (x1, y1); last_c = None
            cx, cy = x, y

        elif cmd in "Aa":
            rx, ry, rot, large, sweep, x, y = rd(), rd(), rd(), rd(), rd(), rd(), rd()
            if cmd == "a":
                x += cx; y += cy
            pts += _arc(cx, cy, rx, ry, rot, int(large), int(sweep), x, y)
            cx, cy = x, y
            last_c = last_q = None

        else:
            idx += 1

    if pts:
        subs.append(_close(pts))
    return [s for s in subs if len(s) >= 2]


def fill_subs(subs, rule, color):
    edges = []
    ymin, ymax = 1e9, -1e9
    for pts in subs:
        for i in range(len(pts) - 1):
            x0, y0 = pts[i]
            x1, y1 = pts[i + 1]
            if abs(y1 - y0) < 1e-12:
                continue
            edges.append((x0, y0, x1, y1))
            ymin = min(ymin, y0, y1)
            ymax = max(ymax, y0, y1)
    if not edges:
        return
    for yi in range(max(0, int(math.floor(ymin))), min(H - 1, int(math.ceil(ymax))) + 1):
        sy = yi + 0.5
        xs = []
        for (x0, y0, x1, y1) in edges:
            lo, hi = (y0, y1) if y0 < y1 else (y1, y0)
            if lo <= sy < hi:
                k = (sy - y0) / (y1 - y0)
                xs.append((x0 + k * (x1 - x0), 1 if y1 > y0 else -1))
        if len(xs) < 2:
            continue
        xs.sort()
        spans = []
        if rule == "evenodd":
            for k in range(0, len(xs) - 1, 2):
                spans.append((xs[k][0], xs[k + 1][0]))
        else:
            wind = 0
            startx = None
            for (xv, d) in xs:
                prev = wind
                wind += d
                if prev == 0 and wind != 0:
                    startx = xv
                elif prev != 0 and wind == 0 and startx is not None:
                    spans.append((startx, xv))
                    startx = None
        for (a, b) in spans:
            for xi in range(max(0, int(math.floor(a))), min(W - 1, int(math.ceil(b))) + 1):
                if a <= xi + 0.5 <= b:
                    px(xi, yi, color)


def load_svg():
    here = os.path.dirname(os.path.abspath(__file__))
    path = os.path.join(os.path.dirname(here), "firmware", "app_sel", "main", "assets", "selector.svg")
    if not os.path.exists(path):
        print("!! 找不到 %s" % path)
        return None
    return open(path, encoding="utf-8").read()


def group_paths(svg, tx, ty, sc):
    pat = r'<g transform="translate\(%s, %s\) scale\(%s\)"[^>]*>(.*?)</g>' % (tx, ty, sc)
    m = re.search(pat, svg, re.S)
    return re.findall(r'd="([^"]+)"', m.group(1)) if m else []


def to_px(subs, sc, tx, ty):
    k = sc * S
    ox, oy = tx * S, ty * S
    return [[(x * k + ox, y * k + oy) for (x, y) in sub] for sub in subs]


# ---------- NERV 徽标 ----------
svg = load_svg()
nerv_d, f1_d = [], []
if svg:
    nerv_d = group_paths(svg, "27", "25", "0\\.36") or group_paths(svg, "27", "25", "0.36")
    f1_d = group_paths(svg, "109", "23", "2\\.3") or group_paths(svg, "109", "23", "2.3")

def blit_rgba(path, w, h, ox, oy, region="all"):
    """把 RGBA raw 按 alpha 混合贴入。"""
    data = open(path, "rb").read()
    if len(data) < w * h * 4:
        print("!! %s 尺寸不足" % path)
        return 0
    n = 0
    for yy in range(h):
        Y = oy + yy
        if Y >= H:
            break
        for xx in range(w):
            i = (yy * w + xx) * 4
            aa = data[i + 3]
            if aa == 0:
                continue
            X = ox + xx
            if not (0 <= X < W):
                continue
            if region == "nav" and not (X < SPLIT_X and Y < SPLIT_Y):
                continue
            if region == "f1" and not (X >= SPLIT_X and Y < SPLIT_Y):
                continue
            if region == "moto" and not (Y >= SPLIT_Y):
                continue
            blend(X, Y, (data[i], data[i + 1], data[i + 2]), aa / 255.0)
            n += 1
    return n


# ---------- NERV 徽标（白/浅底已抠透明，统一 #2E0B45） ----------
if os.path.exists(NERV_RAW):
    cnt = blit_rgba(NERV_RAW, NERV_W, NERV_H, NERV_X, NERV_Y, "nav")
    print("NERV 徽标 %dx%d @ (%d,%d)，%d px" % (NERV_W, NERV_H, NERV_X, NERV_Y, cnt))
else:
    print("!! 缺 %s，NERV 跳过" % NERV_RAW)

REGION = "f1"
f1_subs = []
for d in f1_d:
    f1_subs += parse_path(d)
if f1_subs:
    fill_subs(to_px(f1_subs, 2.3, 109, 23), "nonzero", F1_COLOR)
    print("F1 logo：%d 个子路径" % len(f1_subs))

# ---------- 摩托车：alpha 混合贴入 ----------
REGION = "moto"
if os.path.exists(MOTO_RAW):
    data = open(MOTO_RAW, "rb").read()
    for yy in range(MOTO_H):
        Y = MOTO_Y + yy
        if Y >= H:
            break
        for xx in range(MOTO_W):
            i = (yy * MOTO_W + xx) * 4
            rr, gg, bb, aa = data[i], data[i + 1], data[i + 2], data[i + 3]
            if aa == 0:
                continue
            a = aa / 255.0
            blend(MOTO_X + xx, Y, (rr, gg, bb), a)
    print("摩托车贴图 %dx%d @ (%d,%d)" % (MOTO_W, MOTO_H, MOTO_X, MOTO_Y))
else:
    print("!! 缺 %s，摩托车跳过" % MOTO_RAW)


# ---------- 输出 ----------
def rgb565(c):
    return ((c[0] >> 3) << 11) | ((c[1] >> 2) << 5) | (c[2] >> 3)


def write_png(path, rows):
    raw = b"".join(b"\x00" + r for r in rows)

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    open(path, "wb").write(png)


def main():
    bin_data = bytearray()
    rgb_rows = []
    for y in range(H):
        row = bytearray()
        prow = bytearray()
        for x in range(W):
            c = buf[y][x]
            row += struct.pack("<H", rgb565(c))
            prow += bytes(c)
        bin_data += row
        rgb_rows.append(bytes(prow))

    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)
    for t in (os.path.join(root, "firmware", "app_sel", "main", "assets", "sel_bg.bin"),
              os.path.join(root, "firmware", "app_sel", "assets", "sel_bg.bin")):
        os.makedirs(os.path.dirname(t), exist_ok=True)
        open(t, "wb").write(bin_data)
        print("写出 %s  %d B" % (t, len(bin_data)))

    prev = "D:\\豆包ESP32项目\\selector_preview.png"
    write_png(prev, rgb_rows)
    print("预览 %s" % prev)


if __name__ == "__main__":
    main()
