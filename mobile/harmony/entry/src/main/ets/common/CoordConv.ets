/**
 * CoordConv.ets —— WGS-84（GNSS）↔ GCJ-02（高德），与 iOS CoordConv.swift 同一套公式
 *
 * 坑（本轮实测）：国内华为/国行机的 geoLocationManager 直出**已经是 GCJ-02**，
 * 若再按 WGS-84 转一次，会平白多偏 574 m（海拉尔）/ 555 m（北京）——
 * 离线复算见 tests/coord_probe.py。是否转换由 AppCfg.LOC_SYS_IS_GCJ02 与运行时开关决定，
 * 不要在这里无条件转。
 */

const A: number = 6378245.0;
const EE: number = 0.00669342162296594323;
const EARTH_R: number = 6371000.0;

function outOfChina(lat: number, lon: number): boolean {
  return lon < 72.004 || lon > 137.8347 || lat < 0.8293 || lat > 55.8271;
}

function transformLat(x: number, y: number): number {
  let r = -100.0 + 2.0 * x + 3.0 * y + 0.2 * y * y + 0.1 * x * y + 0.2 * Math.sqrt(Math.abs(x));
  r += (20.0 * Math.sin(6.0 * x * Math.PI) + 20.0 * Math.sin(2.0 * x * Math.PI)) * 2.0 / 3.0;
  r += (20.0 * Math.sin(y * Math.PI) + 40.0 * Math.sin(y / 3.0 * Math.PI)) * 2.0 / 3.0;
  r += (160.0 * Math.sin(y / 12.0 * Math.PI) + 320 * Math.sin(y * Math.PI / 30.0)) * 2.0 / 3.0;
  return r;
}

function transformLon(x: number, y: number): number {
  let r = 300.0 + x + 2.0 * y + 0.1 * x * x + 0.1 * x * y + 0.1 * Math.sqrt(Math.abs(x));
  r += (20.0 * Math.sin(6.0 * x * Math.PI) + 20.0 * Math.sin(2.0 * x * Math.PI)) * 2.0 / 3.0;
  r += (20.0 * Math.sin(x * Math.PI) + 40.0 * Math.sin(x / 3.0 * Math.PI)) * 2.0 / 3.0;
  r += (150.0 * Math.sin(x / 12.0 * Math.PI) + 300.0 * Math.sin(x / 30.0 * Math.PI)) * 2.0 / 3.0;
  return r;
}

export class CoordConv {
  static wgs84ToGcj02(lat: number, lon: number): number[] {
    if (outOfChina(lat, lon)) {
      return [lat, lon];
    }
    let dLat = transformLat(lon - 105.0, lat - 35.0);
    let dLon = transformLon(lon - 105.0, lat - 35.0);
    const radLat = lat / 180.0 * Math.PI;
    let magic = Math.sin(radLat);
    magic = 1 - EE * magic * magic;
    const sqrtMagic = Math.sqrt(magic);
    dLat = (dLat * 180.0) / ((A * (1 - EE)) / (magic * sqrtMagic) * Math.PI);
    dLon = (dLon * 180.0) / (A / sqrtMagic * Math.cos(radLat) * Math.PI);
    return [lat + dLat, lon + dLon];
  }

  /** GCJ-02 → WGS-84：迭代反解（正变换无解析逆，4 次迭代误差 < 1 cm） */
  static gcj02ToWgs84(lat: number, lon: number): number[] {
    if (outOfChina(lat, lon)) {
      return [lat, lon];
    }
    let wLat: number = lat;
    let wLon: number = lon;
    for (let i = 0; i < 4; i++) {
      const g = CoordConv.wgs84ToGcj02(wLat, wLon);
      wLat -= (g[0] - lat);
      wLon -= (g[1] - lon);
    }
    return [wLat, wLon];
  }

  /** 两点球面距离（米），用于"附近 N km 内"过滤与偏移体检 */
  static distanceM(lat1: number, lon1: number, lat2: number, lon2: number): number {
    const p1 = lat1 * Math.PI / 180.0;
    const p2 = lat2 * Math.PI / 180.0;
    const dp = p2 - p1;
    const dl = (lon2 - lon1) * Math.PI / 180.0;
    const a = Math.sin(dp / 2) * Math.sin(dp / 2) +
      Math.cos(p1) * Math.cos(p2) * Math.sin(dl / 2) * Math.sin(dl / 2);
    const c = 2 * Math.asin(Math.min(1, Math.sqrt(a)));
    return EARTH_R * c;
  }
}
