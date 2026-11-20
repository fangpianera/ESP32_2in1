/**
 * WeatherService.ets —— 今日天气（板端不联网，只能由手机拉取后随 STATUS 下发）
 *
 * 数据源用 Open-Meteo（开源、免 Key、支持按经纬度直接查）：
 *   https://api.open-meteo.com/v1/forecast
 *     ?latitude=..&longitude=..&daily=temperature_2m_max,temperature_2m_min,weather_code
 *     &timezone=auto&forecast_days=1
 * 返回的 weather_code 是 WMO 天气码，板端按码映射成中文单字（晴/少云/阴/雾/雨/雪…）。
 *
 * 为什么不走高德：高德的天气接口要 adcode（行政区划码），而定位拿到的只有经纬度，
 * 再做逆地理编码是额外一次请求；Open-Meteo 直接吃经纬度，一步到位。
 */
import { http } from '@kit.NetworkKit';
import { BusinessError } from '@kit.BasicServicesKit';

export class WeatherNow {
  hi: number = 0;
  lo: number = 0;
  code: number = -1;
  ts: number = 0;          // 拉取时刻（ms），超过 30 分钟视为过期

  valid(): boolean {
    return this.code >= 0;
  }
}

export class WeatherService {
  private cur: WeatherNow | null = null;
  private inflight: boolean = false;

  latest(): WeatherNow | null {
    return this.cur;
  }

  /** 距离上次拉取是否已过期（默认 30 分钟；天气一天两三次够了） */
  stale(maxMs: number = 30 * 60 * 1000): boolean {
    if (this.cur === null) {
      return true;
    }
    return (Date.now() - this.cur.ts) > maxMs;
  }

  /** 按经纬度拉今日天气；失败返回 null（保留旧值，不让板端闪 "--"） */
  async refresh(lat: number, lon: number): Promise<WeatherNow | null> {
    if (this.inflight || lat <= 0 || lon <= 0) {
      return this.cur;
    }
    this.inflight = true;
    const url: string = 'https://api.open-meteo.com/v1/forecast'
      + `?latitude=${lat.toFixed(4)}&longitude=${lon.toFixed(4)}`
      + '&daily=temperature_2m_max,temperature_2m_min,weather_code'
      + '&timezone=auto&forecast_days=1';

    const req = http.createHttp();
    try {
      const resp = await req.request(url, {
        method: http.RequestMethod.GET,
        connectTimeout: 8000,
        readTimeout: 8000,
        header: { 'Accept': 'application/json' }
      });
      if (resp.responseCode !== 200) {
        console.warn(`[EVA] 天气 HTTP ${resp.responseCode}`);
        return this.cur;
      }
      const txt: string = typeof resp.result === 'string'
        ? resp.result : JSON.stringify(resp.result);
      const o = JSON.parse(txt) as Record<string, Object>;
      const daily = o['daily'] as Record<string, Object>;
      if (daily === undefined || daily === null) {
        return this.cur;
      }
      const hiArr = daily['temperature_2m_max'] as number[];
      const loArr = daily['temperature_2m_min'] as number[];
      const cdArr = daily['weather_code'] as number[];
      if (hiArr === undefined || loArr === undefined || cdArr === undefined) {
        return this.cur;
      }
      const w = new WeatherNow();
      w.hi = Math.round(hiArr[0]);
      w.lo = Math.round(loArr[0]);
      w.code = Math.round(cdArr[0]);
      w.ts = Date.now();
      this.cur = w;
      console.info(`[EVA] 今日天气 ${w.lo}°~${w.hi}° code=${w.code}`);
      return w;
    } catch (e) {
      const err = e as BusinessError;
      console.warn(`[EVA] 天气拉取失败 ${err.code}: ${err.message}`);
      return this.cur;
    } finally {
      this.inflight = false;
      req.destroy();
    }
  }
}
