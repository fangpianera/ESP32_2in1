/**
 * EvaNavCoordinator.ets —— 鸿蒙端唯一对外门面
 *
 * 把 EvaLink（TCP）/ LocationService（定位）/ HealthStride（步长）/ AmapRoute（路线）
 * 四件事组装成一个对象，UI 只跟它打交道。
 *
 * 生命周期：
 *   start(ctx)  → 申请权限 → 健康数据 → 持续定位+后台长时任务 → TCP 连接
 *   stop()      → 全部停止
 *   pushRouteTo(destLat, destLon) → 高德取路线 → 整包下发
 */
import { common, abilityAccessCtrl, Permissions } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { LinkState, GpsSnapshot } from './EvaLink';
import { BleLink } from './BleLink';
import { LocationService } from './LocationService';
import { HealthStride } from './HealthStride';
import { AmapRoute, RouteResult } from './AmapRoute';
import { MapPlan, MapPreview } from './MapPreview';
import { WeatherService } from './WeatherService';
import { AppCfg, BoardCfg } from '../common/Config';

export class EvaNavCoordinator {
  private static inst: EvaNavCoordinator | null = null;

  /** 传输层：BLE 为主（Wi-Fi 热点那条路已验证在"切热点就没外网"上无解） */
  link: BleLink = new BleLink();
  loc: LocationService = new LocationService();
  health: HealthStride = new HealthStride();
  weather: WeatherService = new WeatherService();

  /** 板端界面要显示的目标地名（选点后由 UI 写入，随 STATUS 帧下发） */
  private destName: string = '';

  private started: boolean = false;
  private amapKey: string = AppCfg.AMAP_WEB_KEY;
  private linkState: LinkState = LinkState.IDLE;
  /** 已取到但还没下发的路线（联网阶段产出，热点阶段复用） */
  cachedRoute: RouteResult | null = null;
  /** 手动身高（cm）：优先用设置里记住的值，没有才用 Config 默认 172 */
  private manualHeightCm: number = AppCfg.MANUAL_HEIGHT_CM;

  /** UI 回调：一行状态文本 */
  onStatus: (text: string) => void = () => {};
  /** UI 回调：路线下发结果 */
  onRouteResult: (ok: boolean, pts: number, distM: number) => void = () => {};
  /** UI 回调：地图就绪（底图 + 归一化折线） */
  onMapPlan: (plan: MapPlan) => void = () => {};
  /** UI 回调：身高档案就绪（来自华为健康，或手动兜底） */
  onHealthProfile: (heightCm: number, source: string) => void = () => {};

  static get(): EvaNavCoordinator {
    if (EvaNavCoordinator.inst === null) {
      EvaNavCoordinator.inst = new EvaNavCoordinator();
    }
    return EvaNavCoordinator.inst;
  }

  setAmapKey(key: string): void {
    this.amapKey = key;
  }

  async start(ctx: common.UIAbilityContext): Promise<void> {
    if (this.started) {
      this.status('已经在运行');
      return;
    }
    this.status('申请定位权限…');
    const okLoc = await this.requestLocationPermission(ctx);
    this.status('申请蓝牙权限…');
    const okBt = await this.requestBluetoothPermission(ctx);
    if (!okBt) {
      this.status('蓝牙权限被拒，扫描会失败（去设置里打开“附近设备/蓝牙”）');
    }

    // 健康数据：失败会自动回落"身高经验式"，不阻断主流程
    // 顺序很重要：先给出手动档案，再去拉健康数据；反过来会把刚拉到的高度覆盖成经验值（旧版就是错的）
    this.status('读取健康数据…');
    this.health.setManualProfile(this.manualHeightCm, AppCfg.MANUAL_SEX);
    const okHealth = await this.health.requestPermission(ctx);
    await this.health.refresh();
    this.status(okHealth ? '健康数据：华为健康' : '健康数据：手动身高兜底');
    this.onHealthProfile(this.health.heightCm(), this.health.source());

    this.bindLink();

    if (!okLoc) {
      this.status('定位权限被拒，先只连板端');
    } else {
      this.status('启动持续定位 + 后台任务…');
      await this.loc.start(ctx);
    }

    this.link.start();
    this.started = true;
  }

  /**
   * 只拉起板端链路，不碰定位权限。
   * 用于 App 一打开就显示 LINK 状态（BLE 下先申请蓝牙权限，否则扫描直接抛 201）
   */
  async startLinkOnly(ctx?: common.UIAbilityContext): Promise<void> {
    if (ctx) {
      await this.requestBluetoothPermission(ctx);
    }
    this.bindLink();
    this.link.start();
  }

  /** 链路回调只绑一次，start() 与 startLinkOnly() 共用 */
  private bindLink(): void {
    this.link.locationProvider = (): GpsSnapshot | null => this.loc.latest;
    this.link.healthProvider = (): Record<string, Object> | null => this.health.payload();
    // 板端圆屏的环1 读数：目标地名 + 今日天气（板端不联网，全由手机侧抓取后下发）
    this.link.statusProvider = (): Record<string, Object> | null => {
      const o: Record<string, Object> = {};
      if (this.destName.length > 0) {
        o['dest'] = this.destName;
      }
      const w = this.weather.latest();
      if (w !== null && w.valid()) {
        o['hi'] = w.hi;
        o['lo'] = w.lo;
        o['code'] = w.code;
      }
      return Object.keys(o).length > 0 ? o : null;
    };
    this.link.onState = (s: LinkState, note?: string): void => {
      this.linkState = s;
      const tag = s === LinkState.READY ? '已连上板端' :
                  s === LinkState.CONNECTING ? '正在连板端…' :
                  s === LinkState.IDLE ? '已断开' : '连接失败，重试中';
      this.status(note === undefined ? tag : `${tag} (${note})`);
      if (s === LinkState.READY) {
        // 一连上就把天气/目标地补推一次，免得板端空等 30s 心跳
        this.refreshWeather();
        this.link.pushStatus();
      }
    };
    this.link.onRouteAck = (ok: boolean, pts: number, dist: number): void => {
      this.onRouteResult(ok, pts, dist);
    };
  }

  stop(): void {
    this.link.stop();
    this.loc.stop();
    this.started = false;
    this.status('已停止');
  }

  /**
   * 只拉健康档案（不启动定位、不碰链路）。
   * App 一打开就能知道身高，用户不必手填；拿不到就用手动兜底，不阻断任何流程。
   */
  async healthInit(ctx: common.UIAbilityContext): Promise<void> {
    this.health.setManualProfile(this.manualHeightCm, AppCfg.MANUAL_SEX);
    try {
      const ok = await this.health.requestPermission(ctx);
      await this.health.refresh();
      this.status(ok ? '健康数据：华为健康' : '健康数据：手动身高兜底');
    } catch (e) {
      const err = e as BusinessError;
      console.error(`[EVA] 健康档案初始化失败 ${err.code}: ${err.message}`);
    }
    this.onHealthProfile(this.health.heightCm(), this.health.source());
  }

  /** 手动改身高（UI 的"修改"入口），立即重建步长基准 */
  setManualHeight(cm: number): void {
    if (cm <= 100 || cm >= 230) {
      return;
    }
    this.manualHeightCm = cm;
    this.health.setManualProfile(cm, AppCfg.MANUAL_SEX);
    this.onHealthProfile(this.health.heightCm(), this.health.source());
  }

  heightCm(): number {
    return this.health.heightCm();
  }

  heightSource(): string {
    return this.health.source();
  }

  /** 切换"系统定位是否已是高德坐标"，切换后下一次定位回调即生效 */
  setGcjDirect(v: boolean): void {
    this.loc.setGcjDirect(v);
  }

  gcjDirect(): boolean {
    return this.loc.gcjDirect;
  }

  /** 两种坐标模式的差值（米）：用来判断"转没转"到底哪个对 */
  coordDeltaM(): number {
    return this.loc.deltaM();
  }

  linkReady(): boolean {
    return this.linkState === LinkState.READY;
  }

  isStarted(): boolean {
    return this.started;
  }

  hasRoute(): boolean {
    return this.cachedRoute !== null;
  }

  /** 目标地名：UI 选点后写入，随 STATUS 帧下发到板端圆屏 */
  setDestName(name: string): void {
    this.destName = name ?? '';
    this.link.pushStatus();
  }

  /**
   * 今日天气：手机侧联网抓取（Open-Meteo，吃经纬度、免 Key），30 分钟过期才重拉。
   * 板端永不联网，所以这就是"天气数据从手机侧抓"的唯一通路。
   */
  async refreshWeather(): Promise<void> {
    // 优先用实时定位；室内拿不到 fix 时退回路线起点（天气只到城市粒度，几分钟前的坐标完全够用）
    let lat: number = 0;
    let lon: number = 0;
    const fix = this.loc.latest;
    if (fix !== null) {
      lat = fix.lat;
      lon = fix.lon;
    } else if (this.cachedRoute !== null && this.cachedRoute.pts.length > 0) {
      lat = this.cachedRoute.pts[0].lat;
      lon = this.cachedRoute.pts[0].lon;
    }
    if (lat <= 0 || lon <= 0) {
      return;
    }
    if (!this.weather.stale()) {
      return;
    }
    await this.weather.refresh(lat, lon);
    this.link.pushStatus();
  }

  routeSummary(): string {
    const r = this.cachedRoute;
    if (r === null) {
      return '未取路线';
    }
    return `${r.pts.length} 点 · ${(r.distance / 1000).toFixed(2)} km · ${r.steps} 段`;
  }

  /**
   * 阶段①：联网取路线（必须外网）。
   * 产出两样东西：下发用的 JSON（缓存起来给阶段②），以及下半屏地图。
   *
   * 起点默认取手机定位：如果还没点「启动」、定位服务没开，这里自动拉起，
   * 并最多等 20 秒首次定位（室内+无网络辅助时可能要更久，等不到会给出明确提示）。
   * oLat/oLon 只在用户显式填了手动起点时才用。
   */
  async fetchRouteOnly(ctx: common.UIAbilityContext, destLat: number, destLon: number,
                       oLat?: number, oLon?: number): Promise<boolean> {
    let originLat: number = 0;
    let originLon: number = 0;
    const manual: boolean = (oLat !== undefined && oLon !== undefined &&
                             !isNaN(oLat) && !isNaN(oLon) && Math.abs(oLat) > 0.0001);
    if (manual) {
      originLat = oLat as number;
      originLon = oLon as number;
    } else {
      // 服务没开就先开（申请定位权限 + 持续定位 + 后台长时任务）
      if (!this.started) {
        this.status('取路线：自动启动定位服务…');
        await this.start(ctx);
      }
      let fix: GpsSnapshot | null = await this.waitFix(20000);
      if (fix === null || this.loc.ageMs() > 60000 || fix.acc >= 150) {
        this.status(fix === null
          ? '20 秒内没拿到手机定位：到室外/窗边再点一次；或暂时填手动起点'
          : `定位精度太差(±${Math.round(fix.acc)}m)：请到空旷处重试，或填手动起点`);
        return false;
      }
      originLat = fix.lat;
      originLon = fix.lon;
    }
    if (this.amapKey.startsWith('PASTE_')) {
      this.status('还没填高德 Web 服务 Key（common/Config.ets）');
      return false;
    }
    this.status(`请求高德步行路线（${manual ? '手动起点' : '手机定位'}，需有外网）…`);
    try {
      const amap = new AmapRoute(this.amapKey);
      const r = await amap.fetchRoute(originLat, originLon, destLat, destLon);
      this.cachedRoute = r;
      const plan = await MapPreview.build(this.amapKey, r.pts);
      if (plan.ok) {
        this.status(`路线就绪 · ${this.routeSummary()} · 地图 ${plan.note}`);
      } else {
        this.status(`路线就绪 · ${this.routeSummary()} · 地图不可用(${plan.note})`);
      }
      this.onMapPlan(plan);
      return true;
    } catch (e) {
      const err = e as BusinessError;
      this.status(`取路线失败: ${err.message}`);
      return false;
    }
  }

  /** 轮询等首次定位：拿到"新鲜且精度可接受"的快照就立刻返回 */
  private async waitFix(timeoutMs: number): Promise<GpsSnapshot | null> {
    const t0: number = Date.now();
    while (Date.now() - t0 < timeoutMs) {
      const f = this.loc.latest;
      if (f !== null && this.loc.ageMs() >= 0 && this.loc.ageMs() < 5000 && f.acc > 0 && f.acc < 60) {
        return f;
      }
      const sec = Math.round((Date.now() - t0) / 1000);
      this.status(`等待手机首次定位… ${sec}s / ${Math.round(timeoutMs / 1000)}s（${this.loc.describe()}）`);
      await new Promise<void>((resolve: () => void) => {
        setTimeout(resolve, 800);
      });
    }
    return this.loc.latest;
  }
  /**
   * 阶段②：蓝牙连上板端后，把阶段①缓存的路线整包下发。
   * BLE 与手机自己的 Wi-Fi/蜂窝互不影响，所以和"有没有外网"解耦。
   */
  pushCachedRoute(): boolean {
    if (this.cachedRoute === null) {
      this.status('还没有路线：先在联网状态点「① 取路线」');
      return false;
    }
    if (!this.linkReady()) {
      this.status('还没连上板端：等「LINK」变成 OK（蓝牙自动连接中）');
      return false;
    }
    this.link.pushRouteJson(this.cachedRoute.json);
    this.status('路线已下发，等板端 ACK');
    return true;
  }

  /** App 重启后恢复上次取到的路线：离线可用，只画折线不下载底图 */
  async restoreRoute(json: string): Promise<boolean> {
    if (json.length < 32) {
      return false;
    }
    const r = AmapRoute.fromJson(json);
    if (r.pts.length < 2) {
      return false;
    }
    this.cachedRoute = r;
    const plan = await MapPreview.build(this.amapKey, r.pts, false);
    this.onMapPlan(plan);
    this.status(`已恢复上次路线 · ${this.routeSummary()}（离线，可直接「② 下发」）`);
    return true;
  }

  /** 兼容旧调用：取路线成功且已连板端时直接下发 */
  async pushRouteTo(ctx: common.UIAbilityContext, destLat: number, destLon: number): Promise<void> {
    const ok = await this.fetchRouteOnly(ctx, destLat, destLon);
    if (ok) {
      this.pushCachedRoute();
    }
  }

  private async requestLocationPermission(ctx: common.UIAbilityContext): Promise<boolean> {
    const perms: Permissions[] = [
      'ohos.permission.LOCATION',
      'ohos.permission.APPROXIMATELY_LOCATION'
    ];
    try {
      const at = abilityAccessCtrl.createAtManager();
      const res = await at.requestPermissionsFromUser(ctx, perms);
      const granted = res.authResults.every((v: number): boolean => v === 0);
      return granted;
    } catch (e) {
      const err = e as BusinessError;
      console.error(`[EVA] 权限申请异常 ${err.code}: ${err.message}`);
      return false;
    }
  }

  /** 蓝牙权限（user_grant）：module.json5 里已声明 ACCESS_BLUETOOTH，这里只是拉起授权弹窗 */
  private async requestBluetoothPermission(ctx: common.UIAbilityContext): Promise<boolean> {
    const perms: Permissions[] = ['ohos.permission.ACCESS_BLUETOOTH'];
    try {
      const at = abilityAccessCtrl.createAtManager();
      const r = await at.requestPermissionsFromUser(ctx, perms);
      const ok = r.authResults.every((v: number): boolean => v === 0);
      console.info(`[EVA] 蓝牙权限 granted=${ok}`);
      return ok;
    } catch (e) {
      const err = e as BusinessError;
      console.error(`[EVA] 蓝牙权限申请异常 ${err.code}: ${err.message}`);
      return false;
    }
  }

  private status(t: string): void {
    console.info(`[EVA] ${t}`);
    this.onStatus(t);
  }

  /** 板端地址（供 UI 展示）：BLE 按广播名识别 */
  targetText(): string {
    return `BLE: ${BoardCfg.SSID_PREFIX}*（无需连热点）`;
  }
}
