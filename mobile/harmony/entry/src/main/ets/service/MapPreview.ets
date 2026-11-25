/**
 * MapPreview.ets —— 首页下半屏的路线地图
 *
 * 为什么不用高德「静态地图」自带的 paths 参数：
 *   [实测] 本 Key 调 v3/staticmap 带 paths 时稳定返回
 *   {"status":"0","info":"UNKNOWN_ERROR","infocode":"20003"}，
 *   去掉 paths 只留 location/zoom/size 则正常返回 image/png；
 *   markers 参数也可用。所以底图交给高德，路线折线由本地按墨卡托投影自己画。
 *
 * 坐标：高德底图与路线都是 GCJ-02，两者在同一坐标空间，投影像素直接对得上。
 *   px = (lon+180)/360 * world，py = (1 - ln(tan(lat)+sec(lat))/PI)/2 * world，
 *   world = 256 * 2^zoom（高德 256px 瓦片，scale=1）。
 *
 * 网络前提：本模块必须联网（手机蜂窝/家用 Wi-Fi）。连着板端热点（无外网）时
 * 会失败，调用方要在此之前拦住并给出提示，见 Index.ets 的两阶段流程。
 */
import { http } from '@kit.NetworkKit';
import { image } from '@kit.ImageKit';
import { BusinessError } from '@kit.BasicServicesKit';

export class LatLon {
  lat: number = 0;
  lon: number = 0;

  constructor(lat: number, lon: number) {
    this.lat = lat;
    this.lon = lon;
  }
}

export class MapPlan {
  pix: image.PixelMap | null = null;
  /** 归一化坐标 0..1（相对底图宽高），Canvas 直接用 */
  fx: number[] = [];
  fy: number[] = [];
  /** 参照点（换一种坐标处理后应落的位置），-1 表示不画 */
  refFx: number = -1;
  refFy: number = -1;
  ok: boolean = false;
  note: string = '';
  zoom: number = 14;

  clear(): void {
    this.pix = null;
    this.fx = [];
    this.fy = [];
    this.refFx = -1;
    this.refFy = -1;
    this.ok = false;
    this.note = '';
  }
}

export class MapPreview {
  /** 底图请求尺寸（固定 3:2，UI 用 aspectRatio(1.5) 对齐，避免拉伸导致折线错位） */
  static readonly W: number = 900;
  static readonly H: number = 600;

  /**
   * @param fetchImage false 时只算投影、不下载底图（离线恢复路线时用，
   *                   这样没网也能在 Canvas 上画出荧光绿折线）
   * @param ref 参照点：另一种坐标处理下的位置，会单独标一个空心圈用于对照
   *
   * 视野：底图中心固定取整条路线的第一个点（= 当前位置），这样紫点永远在正中，
   * 一眼就能看出"我在哪"。zoom 仍按整条路线的包围盒选，保证起终点都进画布。
   */
  static async build(key: string, pts: LatLon[], fetchImage: boolean = true,
                     ref?: LatLon): Promise<MapPlan> {
    const plan = new MapPlan();
    if (pts.length < 2) {
      plan.note = '路线点不足，无法出图';
      return plan;
    }

    // ---- 选 zoom：以起点为中心，从 17 往下退，直到最远的那一点也进画布 ----
    // （起终点完全重合时 halfX/halfY 都是 0，直接停在 zoom 17，中心就是脚下）
    const pad: number = 56;
    let z: number = 17;
    while (z > 8) {
      const world: number = 256 * Math.pow(2, z);
      const ax: number = MapPreview.px(pts[0].lon, world);
      const ay: number = MapPreview.py(pts[0].lat, world);
      let halfX: number = 0;
      let halfY: number = 0;
      for (let i = 1; i < pts.length; i++) {
        const dx: number = Math.abs(MapPreview.px(pts[i].lon, world) - ax);
        const dy: number = Math.abs(MapPreview.py(pts[i].lat, world) - ay);
        if (dx > halfX) halfX = dx;
        if (dy > halfY) halfY = dy;
      }
      if (halfX * 2 <= MapPreview.W - pad && halfY * 2 <= MapPreview.H - pad) {
        break;
      }
      z--;
    }
    plan.zoom = z;

    const world: number = 256 * Math.pow(2, z);
    // 中心点固定取起点（当前位置）：经度直接取，纬度经墨卡托反算保持比例一致
    const cLon: number = pts[0].lon;
    const cyPx: number = MapPreview.py(pts[0].lat, world);
    const cLat: number = pts[0].lat;
    const cxPx: number = MapPreview.px(cLon, world);

    const url: string = `https://restapi.amap.com/v3/staticmap?location=${cLon.toFixed(6)},${cLat.toFixed(6)}` +
      `&zoom=${z}&size=${MapPreview.W}*${MapPreview.H}&scale=1&key=${key}`;

    if (fetchImage) {
      const req = http.createHttp();
      try {
        const resp = await req.request(url, {
          method: http.RequestMethod.GET,
          expectDataType: http.HttpDataType.ARRAY_BUFFER,
          connectTimeout: 8000,
          readTimeout: 15000
        });
        if (resp.responseCode !== 200) {
          plan.note = `底图 HTTP ${resp.responseCode}`;
          return plan;
        }
        const buf = resp.result as ArrayBuffer;
        // 高德出错时是 ~56 字节的 JSON；正常底图是几十 KB 的 PNG
        if (buf === null || buf.byteLength < 5000) {
          plan.note = '底图接口返回异常（可能是 Key 配额或网络）';
          return plan;
        }
        const src = image.createImageSource(buf);
        plan.pix = await src.createPixelMap();
      } catch (e) {
        const err = e as BusinessError;
        plan.note = `底图下载失败 ${err.code}: ${err.message}`;
        return plan;
      } finally {
        req.destroy();
      }
    }

    // ---- 路线点 → 归一化坐标 ----
    for (let i = 0; i < pts.length; i++) {
      const p = pts[i];
      plan.fx.push(0.5 + (MapPreview.px(p.lon, world) - cxPx) / MapPreview.W);
      plan.fy.push(0.5 + (MapPreview.py(p.lat, world) - cyPx) / MapPreview.H);
    }
    if (ref !== undefined) {
      plan.refFx = 0.5 + (MapPreview.px(ref.lon, world) - cxPx) / MapPreview.W;
      plan.refFy = 0.5 + (MapPreview.py(ref.lat, world) - cyPx) / MapPreview.H;
    }
    plan.ok = plan.pix !== null;
    plan.note = fetchImage ? `zoom=${z}  ${pts.length} 点` : `离线投影 zoom=${z}  ${pts.length} 点`;
    return plan;
  }

  private static px(lon: number, world: number): number {
    return (lon + 180) / 360 * world;
  }

  private static py(lat: number, world: number): number {
    const rad = lat * Math.PI / 180;
    const y = (1 - Math.log(Math.tan(rad) + 1 / Math.cos(rad)) / Math.PI) / 2;
    return y * world;
  }

  private static latFromPy(py: number, world: number): number {
    const n = Math.PI * (1 - 2 * py / world);
    return Math.atan(Math.sinh(n)) * 180 / Math.PI;
  }
}
