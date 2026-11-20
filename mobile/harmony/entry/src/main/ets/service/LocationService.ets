/**
 * LocationService.ets —— 鸿蒙端持续定位（对应 iOS LocationService）
 *
 * 关键两点（和 iOS 同样的坑）：
 *  1) 用"持续订阅 + 维护最新快照"，而不是收到 GPS_REQ 才去单次定位；
 *  2) 必须申请后台定位权限 + 启动长时任务，否则锁屏后被冻结，30s 应答断链。
 *
 * 【坐标系 · 本轮修正】
 *   国内华为/国行机 geoLocationManager 直出就是 GCJ-02（高德坐标），再转一次会多偏
 *   574 m（海拉尔）/ 555 m（北京），这正是"App 地图与实际路线整体偏移"的来源。
 *   所以默认 sysIsGcj = true（直出不转），可在 App 内一键切换并落盘记住。
 *
 * [待确认] 不同 API 版本 geoLocationManager 的 LocationRequest 字段略有差异
 * （scenario / timeInterval / distanceInterval / maxAccuracy），以本机 d.ts 为准。
 */
import { geoLocationManager } from '@kit.LocationKit';
import { backgroundTaskManager } from '@kit.BackgroundTasksKit';
import { wantAgent, WantAgent } from '@kit.AbilityKit';
import { common } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { GpsSnapshot } from './EvaLink';
import { CoordConv } from '../common/CoordConv';
import { AppCfg } from '../common/Config';

export class LocationService {
  private snapshot: GpsSnapshot | null = null;
  private subId: number = -1;
  private bgStarted: boolean = false;

  /** 系统直出是否已是 GCJ-02（true = 不再转换）。运行时可由 UI 切换 */
  private sysIsGcj: boolean = AppCfg.LOC_SYS_IS_GCJ02;
  /** 原始（未处理）定位，用于体检：两种模式各取一个坐标，看哪个落在脚下 */
  private rawLat: number = 0;
  private rawLon: number = 0;

  get latest(): GpsSnapshot | null {
    return this.snapshot;
  }

  get gcjDirect(): boolean {
    return this.sysIsGcj;
  }

  /** 切换坐标模式；切换后下一次回调即生效（无需重开定位） */
  setGcjDirect(v: boolean): void {
    this.sysIsGcj = v;
  }

  /** 两种模式给出的坐标相距多少米（>400 m 说明"转没转"这件事必然错了其中一个） */
  deltaM(): number {
    if (this.rawLat === 0 && this.rawLon === 0) {
      return 0;
    }
    const g = CoordConv.wgs84ToGcj02(this.rawLat, this.rawLon);
    return CoordConv.distanceM(this.rawLat, this.rawLon, g[0], g[1]);
  }

  /** 上一次定位距今多少毫秒；从未定位返回 -1 */
  ageMs(): number {
    return this.snapshot === null ? -1 : Date.now() - this.snapshot.timestamp;
  }

  /** 给 UI 看的一行定位状态 */
  describe(): string {
    const s = this.snapshot;
    if (s === null) {
      return '定位: 等待首次定位（点「① 取路线」会自动开启定位，室内可能较慢）';
    }
    const age = Math.round((Date.now() - s.timestamp) / 1000);
    const acc = Math.round(s.acc);
    const tag = this.sysIsGcj ? 'GCJ直出' : 'WGS转GCJ';
    return `定位: ${s.lat.toFixed(6)}, ${s.lon.toFixed(6)}  ±${acc}m  ${age}s 前 · ${tag}`;
  }

  async start(context: common.UIAbilityContext): Promise<void> {
    // API 26 实测：LocationRequest 的 scenario 只剩 UNSET（旧的 SCENE_* 已废弃），
    // 持续定位改用 ContinuousLocationRequest + UserActivityScenario.NAVIGATION
    const req: geoLocationManager.ContinuousLocationRequest = {
      interval: 1,
      locationScenario: geoLocationManager.UserActivityScenario.NAVIGATION
    };

    geoLocationManager.on('locationChange', req, (loc: geoLocationManager.Location) => {
      const s = new GpsSnapshot();
      this.rawLat = loc.latitude;
      this.rawLon = loc.longitude;
      // 默认认为系统直出已是 GCJ-02（高德坐标），不再二次转换
      let lat: number = loc.latitude;
      let lon: number = loc.longitude;
      if (!this.sysIsGcj) {
        const g = CoordConv.wgs84ToGcj02(lat, lon);
        lat = g[0];
        lon = g[1];
      }
      s.lat = lat;
      s.lon = lon;
      s.acc = loc.accuracy;
      // 鸿蒙部分机型不返回 courseSpeed，缺失时给 -1（板端会当作无效航向）
      s.course = (loc.direction !== undefined && loc.direction >= 0) ? loc.direction : -1;
      s.courseAcc = 999;
      s.speed = (loc.speed !== undefined && loc.speed >= 0) ? loc.speed : 0;
      // [实测坑] Location.timeStamp 的语义在不同版本/机型上不一致（UTC 毫秒 vs sinceBoot），
      // 直接拿来算“是否新鲜”会永远判过旧。统一用回调到达时刻，最坏只差几十毫秒。
      s.timestamp = Date.now();
      this.snapshot = s;
    });

    await this.startBackgroundTask(context);
  }

  stop(): void {
    if (this.subId >= 0) {
      geoLocationManager.off('locationChange');
      this.subId = -1;
    }
  }

  private async startBackgroundTask(context: common.UIAbilityContext): Promise<void> {
    if (this.bgStarted) {
      return;
    }
    try {
      const info: wantAgent.WantAgentInfo = {
        wants: [{ bundleName: context.abilityInfo.bundleName,
                  abilityName: context.abilityInfo.name }],
        actionType: wantAgent.OperationType.START_ABILITY,
        requestCode: 0,
        actionFlags: [wantAgent.WantAgentFlags.UPDATE_PRESENT_FLAG]
      };
      const agent: WantAgent = await wantAgent.getWantAgent(info);
      await backgroundTaskManager.startBackgroundRunning(
        context,
        backgroundTaskManager.BackgroundMode.LOCATION,
        agent
      );
      this.bgStarted = true;
    } catch (e) {
      const err = e as BusinessError;
      console.error(`[EVA] 后台长时任务启动失败 ${err.code}: ${err.message}（锁屏后将无法应答 GPS_REQ）`);
    }
  }
}
