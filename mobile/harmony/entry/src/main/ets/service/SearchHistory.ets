/**
 * SearchHistory.ets —— 目的地搜索历史（本地 preferences，最多 12 条）
 *
 * 为什么要落盘：出门前联网搜一次、到现场再下发，中间 App 被杀很常见；
 * 历史同样要跨进程存活，否则每次都要重新打字。
 * 存的是整条 POI（名字/地址/经纬度），点历史项 = 直接定终点并取路线，不用再搜一次。
 */
import { preferences } from '@kit.ArkData';
import { common } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { PoiItem } from './PlaceSearch';
import { AppCfg } from '../common/Config';
import { CoordConv } from '../common/CoordConv';

const PREF_NAME: string = 'evanav';
const PREF_KEY: string = 'search_history';
/** 同名且 50 m 内视为同一条，避免同一个地点的不同写法堆满列表 */
const SAME_PLACE_M: number = 50;

export class SearchHistory {
  static async load(ctx: common.UIAbilityContext): Promise<PoiItem[]> {
    const out: PoiItem[] = [];
    try {
      const pref = await preferences.getPreferences(ctx, PREF_NAME);
      const s = await pref.get(PREF_KEY, '') as string;
      if (s === undefined || s === null || s.length < 4) {
        return out;
      }
      const rows = JSON.parse(s) as Array<Record<string, Object>>;
      if (rows === undefined || rows === null) {
        return out;
      }
      for (const r of rows) {
        const it = new PoiItem();
        it.name = SearchHistory.str(r['name']);
        it.addr = SearchHistory.str(r['addr']);
        it.area = SearchHistory.str(r['area']);
        it.lat = Number(r['lat'] ?? 0);
        it.lon = Number(r['lon'] ?? 0);
        it.ts = Number(r['ts'] ?? 0);
        if (it.name.length > 0 && it.lat !== 0) {
          out.push(it);
        }
      }
    } catch (e) {
      const err = e as BusinessError;
      console.error(`[EVA] 读搜索历史失败 ${err.code}: ${err.message}`);
    }
    return out;
  }

  /** 记一条并返回最新列表（新条目置顶、去重、截断） */
  static async push(ctx: common.UIAbilityContext, p: PoiItem): Promise<PoiItem[]> {
    let list = await SearchHistory.load(ctx);
    list = list.filter((it: PoiItem): boolean => {
      const sameName: boolean = it.name === p.name;
      const near: boolean = CoordConv.distanceM(it.lat, it.lon, p.lat, p.lon) < SAME_PLACE_M;
      return !(sameName || near);
    });
    const item = new PoiItem();
    item.name = p.name;
    item.addr = p.addr;
    item.area = p.area;
    item.lat = p.lat;
    item.lon = p.lon;
    item.ts = Date.now();
    list.unshift(item);
    if (list.length > AppCfg.SEARCH_HISTORY_MAX) {
      list = list.slice(0, AppCfg.SEARCH_HISTORY_MAX);
    }
    await SearchHistory.save(ctx, list);
    return list;
  }

  static async clear(ctx: common.UIAbilityContext): Promise<void> {
    await SearchHistory.save(ctx, []);
  }

  private static async save(ctx: common.UIAbilityContext, list: PoiItem[]): Promise<void> {    try {
      const pref = await preferences.getPreferences(ctx, PREF_NAME);
      await pref.put(PREF_KEY, JSON.stringify(list));
      await pref.flush();
    } catch (e) {
      const err = e as BusinessError;
      console.error(`[EVA] 写搜索历史失败 ${err.code}: ${err.message}`);
    }
  }

  /** 容错：旧版本把高德"空字段"的 [] 存了进来，这里兜底成空串 */
  private static str(v: Object | undefined): string {
    if (v === undefined || v === null) {
      return '';
    }
    return typeof v === 'string' ? (v as string) : '';
  }
}
