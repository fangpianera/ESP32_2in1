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
  ok: boolean = false;
  note: string = '';
  zoom: number = 14;

  clear(): void {
    this.pix = null;
    this.fx = [];
    this.fy = [];
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
   */
  static async build(key: string, pts: LatLon[], fetchImage: boolean = true): Promise<MapPlan> {
    const plan = new MapPlan();
    if (pts.length < 2) {
      plan.note = '路线点不足，无法出图';
      return plan;
    }

    let minLat: number = pts[0].lat;
    let maxLat: number = pts[0].lat;
    let minLon: number = pts[0].lon;
    let maxLon: number = pts[0].lon;
    for (let i = 1; i < pts.length; i++) {
      const p = pts[i];
      if (p.lat < minLat) minLat = p.lat;
      if (p.lat > maxLat) maxLat = p.lat;
      if (p.lon < minLon) minLon = p.lon;
      if (p.lon > maxLon) maxLon = p.lon;
    }
    if (Math.abs(maxLat - minLat) < 1e-7 && Math.abs(maxLon - minLon) < 1e-7) {
      minLat -= 0.002;
      maxLat += 0.002;
      minLon -= 0.002;
      maxLon += 0.002;
    }

    // ---- 选 zoom：从 17 往下退，直到整条路线塞进底图（四周留 28px 边） ----
    const pad: number = 56;
    let z: number = 17;
    while (z > 8) {
      const world: number = 256 * Math.pow(2, z);
      const spanX: number = Math.abs(MapPreview.px(maxLon, world) - MapPreview.px(minLon, world));
      const spanY: number = Math.abs(MapPreview.py(minLat, world) - MapPreview.py(maxLat, world));
      if (spanX <= MapPreview.W - pad && spanY <= MapPreview.H - pad) {
        break;
      }
      z--;
    }
    plan.zoom = z;

    const world: number = 256 * Math.pow(2, z);
    // 中心点：经度直接取中点；纬度要在墨卡托空间取中点再反算（否则高纬会偏）
    const cLon: number = (minLon + maxLon) / 2;
    const cyPx: number = (MapPreview.py(minLat, world) + MapPreview.py(maxLat, world)) / 2;
    const cLat: number = MapPreview.latFromPy(cyPx, world);
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
