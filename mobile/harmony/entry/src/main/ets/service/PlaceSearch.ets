/**
 * PlaceSearch.ets —— 目的地"搜索—选择"
 *
 * 用高德 Web 服务「关键字搜索」v3/place/text：
 *   GET https://restapi.amap.com/v3/place/text?key=&keywords=&location=&offset=25&page=1
 *   返回 pois[].name / address / location("lon,lat") / distance
 * 坐标同样是 GCJ-02，与方向 API、静态底图完全同一坐标系，选中后可直接当终点。
 *
 * 【近域搜索 · 本轮改动】
 *   高德 place/text 的 radius 只到 50 km，撑不住 100 km；而且不传城市时会全国撒网。
 *   所以改成：请求带 location 拿 25 条候选，**客户端按球面距离过滤到附近 100 km 内**
 *   并就近排序——这样不用再手打城市名，也不会搜出一千公里外的同名胜。
 *
 * 注意：本接口需要外网。手机连着板端热点（无外网）时会抛解析失败，
 * 文案按"当前没有外网"处理（与取路线同一套坑）。
 */
import { http } from '@kit.NetworkKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { AppCfg } from '../common/Config';
import { CoordConv } from '../common/CoordConv';

export class PoiItem {
  name: string = '';
  /** 门牌/地址，可能为空 */
  addr: string = '';
  /** 所在区/商圈等，用于给用户做区分 */
  area: string = '';
  lat: number = 0;
  lon: number = 0;
  /** 距离指定中心点的米数，无中心点时 0 */
  dist: number = 0;
  /** 最近一次使用的时间戳（搜索历史用，0 = 非历史项） */
  ts: number = 0;

  /** 列表里显示的第二行：地址 + 距离 */
  subText(): string {
    const parts: string[] = [];
    if (this.area.length > 0) {
      parts.push(this.area);
    }
    if (this.addr.length > 0) {
      parts.push(this.addr);
    }
    if (this.dist > 0) {
      parts.push(this.dist >= 1000 ? `${(this.dist / 1000).toFixed(1)} km` : `${Math.round(this.dist)} m`);
    }
    return parts.join(' · ');
  }
}

export class PlaceSearch {
  private key: string;

  constructor(webServiceKey: string) {
    this.key = webServiceKey;
  }

  /**
   * @param kw       用户输入的关键字（如 "天安门"、"星巴克"）
   * @param city     可选城市限定（城市名或 adcode），空则不限定（当前 UI 恒传空）
   * @param nearLat  搜索中心（一般是手机定位）；为 0 时不限域，退回全国搜
   * @param nearLon  同上
   * @param radiusM  近域半径，默认 AppCfg.SEARCH_RADIUS_M（100 km）
   */
  async text(kw: string, city: string, nearLat: number, nearLon: number,
             radiusM: number = AppCfg.SEARCH_RADIUS_M): Promise<PoiItem[]> {
    const q = encodeURIComponent(kw);
    const hasNear: boolean = Math.abs(nearLat) > 1e-6 && Math.abs(nearLon) > 1e-6;
    let url: string = `https://restapi.amap.com/v3/place/text?key=${this.key}` +
      `&keywords=${q}&offset=${AppCfg.SEARCH_PAGE_SIZE}&page=1&extensions=base`;
    if (city.length > 0) {
      url += `&city=${encodeURIComponent(city)}&citylimit=true`;
    }
    if (hasNear) {
      // location 只影响距离计算/排序，不做命中过滤，所以还要客户端自己筛
      url += `&location=${nearLon.toFixed(6)},${nearLat.toFixed(6)}`;
    }

    const req = http.createHttp();
    let body: string = '';
    try {
      const resp = await req.request(url, {
        method: http.RequestMethod.GET,
        expectDataType: http.HttpDataType.STRING,
        connectTimeout: 8000,
        readTimeout: 12000
      });
      body = resp.result as string;
    } catch (e) {
      const err = e as BusinessError;
      if (err.code === 2300006) {
        throw new Error('当前没有外网（可能连着板端热点）。请先在手机网络下搜索目的地');
      }
      throw new Error(`搜索请求失败 ${err.code}: ${err.message}`);
    } finally {
      req.destroy();
    }

    const root = JSON.parse(body) as Record<string, Object>;
    if (root['status'] !== '1') {
      throw new Error(`高德返回: ${root['info']}`);
    }
    const raw = root['pois'] as Array<Record<string, Object>>;
    const out: PoiItem[] = [];
    if (raw === undefined || raw === null) {
      return out;
    }
    for (const p of raw) {
      const loc = (p['location'] as string ?? '');
      const pair = loc.split(',');
      if (pair.length < 2) {
        continue;
      }
      const item = new PoiItem();
      item.name = PlaceSearch.str(p['name']);
      item.addr = PlaceSearch.str(p['address']);
      item.area = PlaceSearch.str(p['adname']);
      item.lon = Number(pair[0]);
      item.lat = Number(pair[1]);
      const d = p['distance'];
      item.dist = d === undefined || d === null ? 0 : Number(d);
      if (isNaN(item.lat) || isNaN(item.lon)) {
        continue;
      }
      if (hasNear) {
        // 用本地球面距离覆盖（服务端 distance 有的是空串，且单位/精度不统一）
        item.dist = CoordConv.distanceM(nearLat, nearLon, item.lat, item.lon);
        if (item.dist > radiusM) {
          continue;
        }
      }
      out.push(item);
    }
    if (hasNear) {
      out.sort((a: PoiItem, b: PoiItem): number => a.dist - b.dist);
    }
    return out;
  }

  /**
   * 高德对"空字段"返回的是 **空数组 []** 而不是空字符串（实测），
   * 直接 `as string` 会得到 "[]" 并被存进搜索历史、显示成乱码，这里统一收口。
   */
  private static str(v: Object | undefined): string {
    if (v === undefined || v === null) {
      return '';
    }
    return typeof v === 'string' ? (v as string) : '';
  }
}
