/**
 * AmapRoute.ets —— 鸿蒙端步行路线获取
 *
 * 路线选型：
 *  A. 高德开放平台「Web 服务 API」(当前采用，零 SDK 依赖，最稳)
 *     https://restapi.amap.com/v5/direction/walking
 *     只需一个 Web 服务 Key（与 iOS/鸿蒙地图 SDK 的 Key 不是同一个，别混用）
 *  B. 高德 HarmonyOS SDK（ohpm 包）——[待确认] 版本与 API 形态随高德发布变化，
 *     若 SDK 行为变动，A 路径仍与板端协议完全兼容。
 *
 * 注意：高德所有坐标是 GCJ-02，手机 GNSS 是 WGS-84，两者在城区差 50~500m。
 * 本文件输出的点是 GCJ-02；手机回传给板端的 GPS 也必须先转 GCJ-02（见 CoordConv）。
 *
 * [实测] 网络前提：本接口必须能解析 restapi.amap.com。手机连着板端热点（无外网）时
 * DNS 解析失败，会抛 23000006 Failed to resolve the host name —— 这是必然结果，
 * 不是 Key 写错。正确顺序：先联网取路线 → 再连板端热点下发（见 Index.ets）。
 */
import { http } from '@kit.NetworkKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { LatLon } from './MapPreview';

class RouteStep {
  i: number = 0;          // 转向点索引（对应 pts 下标）
  d: number = 0;          // 距下一点米
  act: string = 'straight';
  road: string = '';
  tip: string = '';
}

/** 一次取路线同时产出：下发用 JSON + 画图用点位 + 里程 */
export class RouteResult {
  json: string = '';
  pts: LatLon[] = [];
  distance: number = 0;
  steps: number = 0;
}

export class AmapRoute {
  private key: string;

  constructor(webServiceKey: string) {
    this.key = webServiceKey;
  }

  /** 兼容旧调用：只要 JSON */
  async fetchWalking(originLat: number, originLon: number,
                     destLat: number, destLon: number): Promise<string> {
    const r = await this.fetchRoute(originLat, originLon, destLat, destLon);
    return r.json;
  }

  /** 返回下发 JSON + 画图点位（GCJ-02） */
  async fetchRoute(originLat: number, originLon: number,
                   destLat: number, destLon: number): Promise<RouteResult> {
    // 高德要求 lon,lat 顺序（6 位小数），写反会得到"起点终点太近"的错误
    const url: string = `https://restapi.amap.com/v5/direction/walking?key=${this.key}` +
      `&origin=${originLon.toFixed(6)},${originLat.toFixed(6)}` +
      `&destination=${destLon.toFixed(6)},${destLat.toFixed(6)}` +
      `&show_fields=cost,polyline&isindoor=0`;

    const req = http.createHttp();
    let body: string = '';
    try {
      const resp = await req.request(url, {
        method: http.RequestMethod.GET,
        expectDataType: http.HttpDataType.STRING,
        connectTimeout: 8000,
        readTimeout: 15000
      });
      body = resp.result as string;
    } catch (e) {
      const err = e as BusinessError;
      // 23000006 的根因是"当前无外网"（连了板端热点），文案要说清而不是丢个错误码
      if (err.code === 2300006) {
        throw new Error('当前没有外网（可能连着板端热点）。请先用手机网络取路线，再连热点下发');
      }
      throw new Error(`高德请求失败 ${err.code}: ${err.message}`);
    } finally {
      req.destroy();
    }

    const root = JSON.parse(body) as Record<string, Object>;
    if (root['status'] !== '1') {
      throw new Error(`高德返回错误: ${root['info']}`);
    }

    const paths = (root['route'] as Record<string, Object>)['paths'] as Array<Record<string, Object>>;
    if (!paths || paths.length === 0) {
      throw new Error('无可行步行路线');
    }
    const steps = paths[0]['steps'] as Array<Record<string, Object>>;
    const distM: number = Number(paths[0]['distance'] ?? 0);
    return this.buildResult(steps, distM);
  }

  private buildResult(steps: Array<Record<string, Object>>, distM: number): RouteResult {
    const pts: number[][] = [];
    const stepObjs: RouteStep[] = [];

    for (const s of steps) {
      const poly = (s['polyline'] as string ?? '');
      for (const kv of poly.split(';')) {
        const pair = kv.split(',');
        if (pair.length < 2) {
          continue;
        }
        const lon = Number(pair[0]);
        const lat = Number(pair[1]);
        if (isNaN(lat) || isNaN(lon)) {
          continue;
        }
        const last = pts.length > 0 ? pts[pts.length - 1] : null;
        if (last !== null && Math.abs(last[0] - lat) < 1e-7 && Math.abs(last[1] - lon) < 1e-7) {
          continue;
        }
        pts.push([lat, lon, -1]);
      }
      if (stepObjs.length > 0) {
        stepObjs[stepObjs.length - 1].i = pts.length - 1;
      }
      const st = new RouteStep();
      st.i = pts.length - 1;
      st.d = Number(s['step_distance'] ?? s['distance'] ?? 0);
      st.act = this.mapAction(s['instruction'] as string ?? '');
      st.road = (s['road_name'] as string ?? s['road'] as string ?? '');
      st.tip = this.tipText(st.act, st.d, st.road);
      stepObjs.push(st);
    }

    if (pts.length < 2 || pts.length > 4000) {
      throw new Error(`点数 ${pts.length} 超出板端上限(4000)，请缩短行程`);
    }

    // 补 bearing（板端在 GPS 丢失时依赖它）
    for (let i = 0; i < pts.length; i++) {
      const j = Math.min(i + 1, pts.length - 1);
      pts[i][2] = this.bearing(pts[i][0], pts[i][1], pts[j][0], pts[j][1]);
    }

    const payload: Record<string, Object> = {
      'origin': [pts[0][0], pts[0][1]],
      'pts': pts,
      'steps': stepObjs,
      'src': 'amap-harmony'
    };
    if (stepObjs.length > 0) {
      stepObjs[stepObjs.length - 1].act = 'arrive';
      stepObjs[stepObjs.length - 1].d = 0;
    }

    const out = new RouteResult();
    out.json = JSON.stringify(payload);
    out.distance = distM;
    out.steps = stepObjs.length;
    for (let i = 0; i < pts.length; i++) {
      out.pts.push(new LatLon(pts[i][0], pts[i][1]));
    }
    return out;
  }

  private bearing(lat1: number, lon1: number, lat2: number, lon2: number): number {
    const p1 = lat1 * Math.PI / 180;
    const p2 = lat2 * Math.PI / 180;
    const dl = (lon2 - lon1) * Math.PI / 180;
    const y = Math.sin(dl) * Math.cos(p2);
    const x = Math.cos(p1) * Math.sin(p2) - Math.sin(p1) * Math.cos(p2) * Math.cos(dl);
    let deg = Math.atan2(y, x) * 180 / Math.PI;
    if (deg < 0) {
      deg += 360;
    }
    return Math.round(deg * 10) / 10;
  }

  private mapAction(ins: string): string {
    if (ins.includes('左') && ins.includes('急')) return 'left_hard';
    if (ins.includes('右') && ins.includes('急')) return 'right_hard';
    if (ins.includes('左')) return 'left';
    if (ins.includes('右')) return 'right';
    if (ins.includes('掉头')) return 'uturn';
    if (ins.includes('到达')) return 'arrive';
    return 'straight';
  }

  /** 从已缓存的推送 JSON 还原 RouteResult（App 重启后恢复路线用） */
  static fromJson(json: string): RouteResult {
    const root = JSON.parse(json) as Record<string, Object>;
    const out = new RouteResult();
    out.json = json;
    const raw = root['pts'] as Array<Array<number>>;
    if (raw !== undefined && raw !== null) {
      for (const p of raw) {
        out.pts.push(new LatLon(p[0], p[1]));
      }
    }
    const st = root['steps'] as Array<Record<string, Object>>;
    let sum: number = 0;
    if (st !== undefined && st !== null) {
      out.steps = st.length;
      for (const s of st) {
        sum += Number(s['d'] ?? 0);
      }
    }
    out.distance = sum;
    return out;
  }

  private tipText(act: string, d: number, road: string): string {
    const m = Math.round(d);
    switch (act) {
      case 'left': case 'left_hard': return `左转 后 ${m} 米`;
      case 'right': case 'right_hard': return `右转 后 ${m} 米`;
      case 'uturn': return `掉头 后 ${m} 米`;
      case 'arrive': return '到达终点';
      default: return road.length > 0 ? `沿${road} ${m} 米` : `直行 ${m} 米`;
    }
  }
}
