#!/usr/bin/env python3
"""
coord_probe.py —— 量化"对已是 GCJ-02 的坐标再转一次"会偏多少米

背景：鸿蒙 LocationService 会无条件把 geoLocationManager 的结果按 WGS-84 → GCJ-02 转一次。
     若系统直出已经是 GCJ-02（国内华为/国行机普遍如此），这一次转换就是白加偏移，
     表现为"App 地图 / 路线起点整体偏移"。

本脚本只做离线数学复算，不联网、不依赖手机。公式与 entry/src/main/ets/common/CoordConv.ets 一致。

用法：python coord_probe.py
"""
import math

A = 6378245.0
EE = 0.00669342162296594323
EARTH_R = 6371000.0


def _tlat(x, y):
    r = -100.0 + 2.0 * x + 3.0 * y + 0.2 * y * y + 0.1 * x * y + 0.2 * math.sqrt(abs(x))
    r += (20.0 * math.sin(6.0 * x * math.pi) + 20.0 * math.sin(2.0 * x * math.pi)) * 2.0 / 3.0
    r += (20.0 * math.sin(y * math.pi) + 40.0 * math.sin(y / 3.0 * math.pi)) * 2.0 / 3.0
    r += (160.0 * math.sin(y / 12.0 * math.pi) + 320 * math.sin(y * math.pi / 30.0)) * 2.0 / 3.0
    return r


def _tlon(x, y):
    r = 300.0 + x + 2.0 * y + 0.1 * x * x + 0.1 * x * y + 0.1 * math.sqrt(abs(x))
    r += (20.0 * math.sin(6.0 * x * math.pi) + 20.0 * math.sin(2.0 * x * math.pi)) * 2.0 / 3.0
    r += (20.0 * math.sin(x * math.pi) + 40.0 * math.sin(x / 3.0 * math.pi)) * 2.0 / 3.0
    r += (150.0 * math.sin(x / 12.0 * math.pi) + 300.0 * math.sin(x / 30.0 * math.pi)) * 2.0 / 3.0
    return r


def wgs84_to_gcj02(lat, lon):
    if lon < 72.004 or lon > 137.8347 or lat < 0.8293 or lat > 55.8271:
        return lat, lon
    dlat = _tlat(lon - 105.0, lat - 35.0)
    dlon = _tlon(lon - 105.0, lat - 35.0)
    rad = lat / 180.0 * math.pi
    magic = 1 - EE * math.sin(rad) ** 2
    sq = math.sqrt(magic)
    dlat = (dlat * 180.0) / ((A * (1 - EE)) / (magic * sq) * math.pi)
    dlon = (dlon * 180.0) / (A / sq * math.cos(rad) * math.pi)
    return lat + dlat, lon + dlon


def gcj02_to_wgs84(lat, lon):
    """迭代反解，与 CoordConv.gcj02ToWgs84 同算法"""
    wlat, wlon = lat, lon
    for _ in range(4):
        g = wgs84_to_gcj02(wlat, wlon)
        wlat -= (g[0] - lat)
        wlon -= (g[1] - lon)
    return wlat, wlon


def dist_m(lat1, lon1, lat2, lon2):
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dp = p2 - p1
    dl = math.radians(lon2 - lon1)
    a = math.sin(dp / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    return 2 * EARTH_R * math.asin(min(1, math.sqrt(a)))


CASES = [
    ("海拉尔（用户常驻）", 49.2120, 119.7657),
    ("北京（天安门）", 39.9087, 116.3975),
    ("呼伦贝尔南", 49.1500, 119.7000),
]

print("=" * 78)
print("场景 A：系统直出是 WGS-84 → 转一次 = 正确（得到高德坐标）")
print("场景 B：系统直出已是 GCJ-02 → 再转一次 = 白加偏移（错误）")
print("=" * 78)
for name, lat, lon in CASES:
    g1 = wgs84_to_gcj02(lat, lon)
    g2 = wgs84_to_gcj02(*g1)
    d1 = dist_m(lat, lon, *g1)
    d2 = dist_m(*g1, *g2)
    print(f"\n{name}  ({lat:.4f}, {lon:.4f})")
    print(f"  A 正常转换偏移        : {d1:8.1f} m   → GCJ ({g1[0]:.6f}, {g1[1]:.6f})")
    print(f"  B 二次转换多出的偏移  : {d2:8.1f} m   → 实际坐标被推到 ({g2[0]:.6f}, {g2[1]:.6f})")

print("\n" + "=" * 78)
print("反解校验：gcj02_to_wgs84 是否能把上面场景 A 的 GCJ 坐标还原回 WGS")
print("=" * 78)
for name, lat, lon in CASES:
    g = wgs84_to_gcj02(lat, lon)
    back = gcj02_to_wgs84(*g)
    print(f"  {name:16s} 还原误差 {dist_m(lat, lon, *back) * 100:6.2f} cm")

print("\n结论：国内机型（场景 B）下这次多余的转换会把位置整体推走约 550~580 m，")
print("      与用户看到的『地图与实际路线整体偏移』量级一致。正确做法是默认不再转换（CoordConv 由调用方决定）。")
