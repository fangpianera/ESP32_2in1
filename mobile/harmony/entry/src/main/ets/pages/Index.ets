/**
 * Index.ets —— 鸿蒙端主页面
 *
 * 流程（两阶段，避免"连着板端热点没外网"的死结）：
 *   ① 搜索目的地（高德 POI 关键字搜索，需外网）→ 选中即取路线并出地图
 *   ② 连 EVA-NAV 蓝牙 → 把缓存的路线整包下发板端
 * 起点固定用手机定位（自动启动定位服务并等首次定位），不再手填经纬度。
 *
 * 下半屏是路线地图：高德静态底图（Image）+ Canvas 自绘荧光绿折线/起终点。
 * 底图不经过 paths 参数（该 Key 被 20003 拒），投影由 MapPreview 自己算。
 *
 * 【本轮四项改动】
 *   ① 搜索历史：最近搜过的地点落盘，点一条直接取路线
 *   ② 近域搜索：只搜附近 100 km，不再手打城市名
 *   ③ 坐标偏移：默认认为系统直出即高德坐标（不二次转换），并给出一键切换
 *   ④ 身高：先读华为健康，读不到用记住的手动值，界面不再逼着每次输入
 */
import { common } from '@kit.AbilityKit';
import { image } from '@kit.ImageKit';
import { preferences } from '@kit.ArkData';
import { BusinessError } from '@kit.BasicServicesKit';
import { EvaNavCoordinator } from '../service/EvaNavCoordinator';
import { MapPlan } from '../service/MapPreview';
import { PoiItem, PlaceSearch } from '../service/PlaceSearch';
import { SearchHistory } from '../service/SearchHistory';
import { GpsSnapshot } from '../service/EvaLink';
import { AppCfg } from '../common/Config';

const PREF_NAME: string = 'evanav';
const PREF_ROUTE: string = 'route_json';
const PREF_DEST: string = 'route_dest';
const PREF_HEIGHT: string = 'height_cm';
const PREF_COORD: string = 'loc_gcj_direct_v2';   // 换 key 让旧版存下的相反设定失效

@Entry
@Component
struct Index {
  @State status: string = '未启动';
  @State running: boolean = false;
  @State routeInfo: string = '';
  @State mapPix: image.PixelMap | null = null;
  @State mapNote: string = '未加载地图';
  @State summary: string = '未取路线';
  @State locText: string = '定位: 等待首次定位';
  @State linkText: string = 'LINK —';

  // 搜索—选择
  @State keyword: string = '';
  @State pois: PoiItem[] = [];
  @State destName: string = '';
  @State destSub: string = '';
  @State history: PoiItem[] = [];
  private destLat: number = 0;
  private destLon: number = 0;

  // 身高：默认只读展示，点「修改」才出现输入框
  @State heightText: string = `${AppCfg.MANUAL_HEIGHT_CM}`;
  @State heightSrc: string = '默认';
  @State heightEdit: boolean = false;
  @State heightInput: string = `${AppCfg.MANUAL_HEIGHT_CM}`;

  // 坐标模式
  @State gcjDirect: boolean = AppCfg.LOC_SYS_IS_GCJ02;
  @State coordDelta: number = 0;

  /** 下发按钮的可用态：不能直接读 co.hasRoute()，那不是响应式状态，按钮颜色会永不刷新 */
  @State routeReady: boolean = false;
  /** 是否已有折线：mfx 不是 @State，改了不会重渲染，所以单独用一个响应式布尔位驱动占位图的显隐 */
  @State hasLine: boolean = false;

  private locTimer: number = -1;
  private wxTimer: number = -1;

  private co: EvaNavCoordinator = EvaNavCoordinator.get();
  private cctx: CanvasRenderingContext2D = new CanvasRenderingContext2D(new RenderingContextSettings(true));
  /** 归一路线点（0..1），Canvas 绘制前按控件尺寸乘开放大 */
  private mfx: number[] = [];
  private mfy: number[] = [];

  aboutToAppear(): void {
    this.co.onStatus = (t: string): void => {
      this.status = t;
      this.summary = this.co.routeSummary();
    };
    // 定位/链路状态每 2 秒刷新：让用户看得见"到底有没有定位、有没有连上板子"
    this.locTimer = setInterval(() => {
      this.locText = this.co.loc.describe();
      this.linkText = this.co.linkReady() ? 'LINK OK' : 'LINK —';
      this.coordDelta = this.co.coordDeltaM();
    }, 2000);
    // 一打开就起 BLE 链路（先要蓝牙权限，板子在线时立刻 LINK OK，锁屏前也能收到 GPS_REQ）
    this.co.startLinkOnly(getContext(this) as common.UIAbilityContext);
    // 天气：板端不联网，只能手机侧抓（有定位后 30 分钟内只拉一次）
    this.wxTimer = setInterval(() => {
      this.co.refreshWeather();
    }, 60000);
    this.co.refreshWeather();
    // 恢复上次取到的路线：App 被杀/重装后不至于白跑一趟
    this.loadSavedRoute();
    // 身高档案：先载入记住的值，再尝试拉华为健康（拿不到就用手动值，不打断用户）
    this.loadSavedProfile().then(() => {
      this.co.healthInit(this.ctx());
    });
    SearchHistory.load(this.ctx()).then((list: PoiItem[]) => {
      this.history = list;
    });
    this.co.onHealthProfile = (cm: number, src: string): void => {
      this.heightText = `${cm}`;
      this.heightSrc = src;
      this.heightInput = `${cm}`;
    };
    this.co.onRouteResult = (ok: boolean, pts: number, distM: number): void => {
      this.routeInfo = ok ? `ACK ok  ${pts} 点 / ${Math.round(distM)} m` : 'ACK 失败';
    };
    this.co.onMapPlan = (plan: MapPlan): void => {
      this.mfx = plan.fx;
      this.mfy = plan.fy;
      this.hasLine = plan.fx.length >= 2;
      this.mapPix = plan.pix;
      this.mapNote = plan.ok ? `AMAP STATIC · ${plan.note}` : `折线就绪（无底图）· ${plan.note}`;
      this.summary = this.co.routeSummary();
      this.redraw();
    };
  }

  aboutToDisappear(): void {
    if (this.locTimer !== -1) {
      clearInterval(this.locTimer);
      this.locTimer = -1;
    }
    if (this.wxTimer !== -1) {
      clearInterval(this.wxTimer);
      this.wxTimer = -1;
    }
  }

  build() {
    Scroll() {
      Column({ space: 8 }) {
        // ---------- 头部 ----------
        Row() {
          Text('EVA NAV').fontSize(20).fontColor('#A6FF00').fontWeight(FontWeight.Bold)
          Blank()
          Text(this.linkText)
            .fontSize(11).fontColor(this.co.linkReady() ? '#A6FF00' : '#60269E')
        }.width('100%')

        Text(this.co.targetText()).fontSize(11).fontColor('#60269E')

        Text(this.status).fontSize(12).fontColor('#E6E6E6')
          .width('100%').padding(8)
          .backgroundColor('#141414').borderRadius(4)
          .maxLines(3)

        if (this.routeInfo.length > 0) {
          Text(this.routeInfo).fontSize(12).fontColor('#A6FF00')
        }

        // ---------- 目的地搜索 ----------
        Row({ space: 8 }) {
          TextInput({ text: this.keyword, placeholder: '搜附近地名，如：火车站 / 星巴克' })
            .fontSize(13).fontColor('#FFFFFF').backgroundColor('#141414')
            .height(36)
            .layoutWeight(1)
            .onChange((v: string) => { this.keyword = v; })
            .onSubmit(() => { this.doSearch(); })
          Button('搜索')
            .fontSize(13).height(36).width(72)
            .backgroundColor('#60269E').fontColor('#FFFFFF')
            .onClick(() => { this.doSearch(); })
        }

        if (this.pois.length > 0) {
          List({ space: 6 }) {
            ForEach(this.pois, (p: PoiItem, i: number) => {
              ListItem() {
                Column() {
                  Text(p.name).fontSize(13).fontColor('#FFFFFF').maxLines(1)
                  Text(p.subText()).fontSize(10).fontColor('#8A8A8A').maxLines(1)
                }
                .alignItems(HorizontalAlign.Start)
                .width('100%')
                .padding({ left: 8, right: 8, top: 6, bottom: 6 })
                .backgroundColor('#121212')
                .borderRadius(4)
              }
              .onClick(() => { this.pickPoi(p); })
            }, (p: PoiItem, i: number) => `${i}-${p.name}-${p.lon}`)
          }
          .width('100%')
          .height(this.pois.length > 3 ? 170 : this.pois.length * 52)
          .borderRadius(4)
        }

        // ---------- 搜索历史（没在搜的时候显示） ----------
        if (this.pois.length === 0 && this.history.length > 0) {
          Row() {
            Text('最近搜索').fontSize(11).fontColor('#60269E')
            Blank()
            Text('清空').fontSize(11).fontColor('#8A8A8A')
              .onClick(() => { this.clearHistory(); })
          }.width('100%')

          List({ space: 6 }) {
            ForEach(this.history.slice(0, 6), (p: PoiItem, i: number) => {
              ListItem() {
                Row({ space: 8 }) {
                  Column() {
                    Text(p.name).fontSize(12).fontColor('#D8D8D8').maxLines(1)
                    Text(p.subText()).fontSize(10).fontColor('#7A7A7A').maxLines(1)
                  }
                  .alignItems(HorizontalAlign.Start)
                  .layoutWeight(1)
                  Text('›').fontSize(14).fontColor('#60269E')
                }
                .width('100%')
                .padding({ left: 8, right: 8, top: 5, bottom: 5 })
                .backgroundColor('#101010')
                .borderRadius(4)
              }
              .onClick(() => { this.pickPoi(p); })
            }, (p: PoiItem, i: number) => `h${i}-${p.name}-${p.lon}`)
          }
          .width('100%')
          .height(Math.min(this.history.length, 6) * 50)
          .borderRadius(4)
        }

        if (this.destName.length > 0) {
          Column() {
            Text(`目的地: ${this.destName}`).fontSize(12).fontColor('#A6FF00').maxLines(1)
            Text(this.destSub).fontSize(10).fontColor('#8A8A8A').maxLines(1)
          }
          .alignItems(HorizontalAlign.Start)
          .width('100%')
          .padding(6)
          .border({ width: 1, color: '#60269E' })
          .borderRadius(4)
        }

        Text(this.locText).fontSize(10).fontColor('#9A9A9A').maxLines(2)

        // ---------- 两阶段操作 ----------
        Row({ space: 8 }) {
          Button('① 取路线')
            .fontSize(13).height(36).layoutWeight(1)
            .backgroundColor('#60269E').fontColor('#FFFFFF')
            .onClick(() => { this.fetchRoute(); })
          Button('② 下发')
            .fontSize(13).height(36).layoutWeight(1)
            .backgroundColor(this.routeReady ? '#A6FF00' : '#2A2A2A')
            .fontColor(this.routeReady ? '#000000' : '#7A7A7A')
            .onClick(() => {
              if (this.co.pushCachedRoute()) {
                this.summary = this.co.routeSummary();
              }
            })
          Button('停止')
            .fontSize(12).height(36).width(64)
            .backgroundColor('#2A2A2A').fontColor('#FF3333')
            .onClick(() => {
              this.co.stop();
              this.running = false;
            })
        }.width('100%')

        Text('① 取路线用手机自己的网（蜂窝/家用 Wi-Fi）；② 下发走蓝牙 BLE，不用切热点')
          .fontSize(10).fontColor('#7A7A7A').maxLines(2)

        // ---------- 参数 ----------
        Row({ space: 8 }) {
          Text('身高').fontSize(11).fontColor('#9A9A9A').width(64)
          if (this.heightEdit) {
            TextInput({ text: this.heightInput })
              .type(InputType.Number).fontSize(12).height(32)
              .fontColor('#FFFFFF').backgroundColor('#141414')
              .layoutWeight(1)
              .onChange((v: string) => { this.heightInput = v; })
              .onSubmit(() => { this.commitHeight(); })
            Button('确定')
              .fontSize(12).height(32).width(56)
              .backgroundColor('#60269E').fontColor('#FFFFFF')
              .onClick(() => { this.commitHeight(); })
          } else {
            Text(`${this.heightText} cm · ${this.heightSrc}`)
              .fontSize(12).fontColor('#E6E6E6').layoutWeight(1)
            Button('修改')
              .fontSize(12).height(32).width(56)
              .backgroundColor('#2A2A2A').fontColor('#9A9A9A')
              .onClick(() => {
                this.heightInput = this.heightText;
                this.heightEdit = true;
              })
          }
        }

        // ---------- 坐标模式 ----------
        Row({ space: 8 }) {
          Text('坐标').fontSize(11).fontColor('#9A9A9A').width(64)
          Text(this.coordLabel())
            .fontSize(11).fontColor('#8A8A8A').layoutWeight(1).maxLines(2)
          Button('切换')
            .fontSize(12).height(32).width(56)
            .backgroundColor('#2A2A2A').fontColor('#9A9A9A')
            .onClick(() => { this.toggleCoord(); })
        }

        // ---------- 地图（页面最后一块：比例必须 1.5，折线才不会被拉伸错位） ----------
        Stack({ alignContent: Alignment.TopStart }) {
          if (this.mapPix !== null) {
            Image(this.mapPix)
              .width('100%').height('100%')
              .objectFit(ImageFit.Cover)
              .opacity(0.92)
          } else if (!this.hasLine) {
            // 既没底图也没折线时才给占位；恢复离线路线时折线是有的，别让占位文字压在线上
            Column() {
              Text('NO MAP DATA').fontSize(12).fontColor('#3C2A52')
              Text('搜索并取路线后在此显示地图').fontSize(10).fontColor('#3C2A52')
            }
            .width('100%').height('100%')
            .justifyContent(FlexAlign.Center)
            .backgroundColor('#0A0A0A')
          }

          Canvas(this.cctx)
            .width('100%').height('100%')
            .onReady(() => { this.redraw(); })

          Text(this.mapNote)
            .fontSize(9).fontColor('#60269E')
            .padding({ left: 6, right: 6, top: 2, bottom: 2 })
            .backgroundColor('#000000CC')
            .margin({ left: 6, top: 6 })

          // 右下角摘要：Stack 里用 .align() 不生效（会被当成默认左上），
          // 改成铺满尺寸的 Row 再靠右下，避免和左上角标叠在一起
          Row() {
            Blank()
            Text(this.summary)
              .fontSize(10).fontColor('#A6FF00')
              .padding({ left: 6, right: 6, top: 2, bottom: 2 })
              .backgroundColor('#000000CC')
          }
          .width('100%').height('100%')
          .alignItems(VerticalAlign.Bottom)
          .justifyContent(FlexAlign.End)
          .padding(6)
          .hitTestBehavior(HitTestMode.Transparent)
        }
        .width('100%')
        .aspectRatio(1.5)
        .borderRadius(6)
        .borderWidth(1)
        .borderColor('#60269E')
        .clip(true)
      }
      .width('100%')
      .alignItems(HorizontalAlign.Start)
      .padding(14)
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#000000')
    .scrollBar(BarState.Off)
  }

  // ---------------- 搜索 ----------------

  private doSearch(): void {
    const kw = this.keyword.trim();
    if (kw.length === 0) {
      this.status = '请输入目的地关键字';
      return;
    }
    // 搜索中心：优先实时定位；没有就用上次缓存的路线起点（高德不带 city/location 会直接返回 0 条）
    let nearLat: number = 0;
    let nearLon: number = 0;
    const fix: GpsSnapshot | null = this.co.loc.latest;
    if (fix !== null && Math.abs(fix.lat) > 1e-6) {
      nearLat = fix.lat;
      nearLon = fix.lon;
    } else {
      const r = this.co.cachedRoute;
      if (r !== null && r.pts.length > 0) {
        nearLat = r.pts[0].lat;
        nearLon = r.pts[0].lon;
      }
    }
    if (nearLat === 0 && nearLon === 0) {
      this.status = '还没有位置：先点「① 取路线」拿到定位，再搜目的地';
      return;
    }
    const km: number = Math.round(AppCfg.SEARCH_RADIUS_M / 1000);
    this.status = `在附近 ${km} km 内搜“${kw}”…（需外网）`;
    const ps = new PlaceSearch(AppCfg.AMAP_WEB_KEY);
    ps.text(kw, '', nearLat, nearLon).then((items: PoiItem[]) => {
      this.pois = items;
      if (items.length === 0) {
        this.status = `附近 ${km} km 内没搜到，换个关键字（或走远一点再搜）`;
      } else {
        this.status = `附近 ${km} km 内找到 ${items.length} 个地点，点一条选中`;
      }
    }).catch((e: Error) => {
      this.pois = [];
      this.status = `搜索失败: ${e.message}`;
    });
  }

  /** 选中地点 = 定终点，并立刻取路线（失败也能重按「① 取路线」） */
  private pickPoi(p: PoiItem): void {
    this.destName = p.name;
    this.destSub = p.subText();
    this.destLat = p.lat;
    this.destLon = p.lon;
    this.keyword = p.name;
    this.pois = [];
    this.co.setDestName(p.name);     // 板端圆屏"目标地"一行就是这个
    SearchHistory.push(this.ctx(), p).then((list: PoiItem[]) => {
      this.history = list;
    });
    this.fetchRoute();
  }

  private clearHistory(): void {
    SearchHistory.clear(this.ctx()).then(() => {
      this.history = [];
      this.status = '已清空搜索历史';
    });
  }

  private fetchRoute(): void {
    if (this.destName.length === 0) {
      this.status = '先搜索并选中一个目的地';
      return;
    }
    this.co.fetchRouteOnly(this.ctx(), this.destLat, this.destLon).then((ok: boolean) => {
      this.running = this.co.isStarted();
      this.summary = this.co.routeSummary();
      this.routeReady = this.co.hasRoute();
      if (ok) {
        this.routeInfo = '';   // 成功即清掉上一次的失败提示
        this.saveRoute();
      } else {
        this.routeInfo = '取路线失败';
      }
    });
  }

  // ---------------- 身高 / 坐标 --------------

  private commitHeight(): void {
    const h = Number(this.heightInput);
    console.info(`[EVA] 身高提交：输入="${this.heightInput}" 解析=${h} 当前=${this.co.heightCm()}`);
    if (!(h > 100 && h < 230)) {
      this.status = '身高要在 100~230 cm 之间';
      return;
    }
    this.co.setManualHeight(h);
    this.heightText = `${h}`;
    this.heightSrc = this.co.heightSource();
    this.heightEdit = false;
    this.saveNumber(PREF_HEIGHT, h);
    this.status = `身高已更新为 ${h} cm`;
  }

  private coordLabel(): string {
    const d = Math.round(this.coordDelta);
    const head = this.coordDelta > 200 ? ` · 两种模式差 ${d} m` : '';
    return this.gcjDirect
      ? `GCJ 直出（系统已是高德坐标，不再转换）${head}`
      : `WGS → GCJ（系统给 WGS，转换一次）${head}`;
  }

  private toggleCoord(): void {
    this.gcjDirect = !this.co.gcjDirect();
    this.co.setGcjDirect(this.gcjDirect);
    this.saveBool(PREF_COORD, this.gcjDirect);
    this.status = this.gcjDirect
      ? '坐标已切到「GCJ 直出」：位置仍偏就去室内外重取路线看看'
      : '坐标已切到「WGS 转换」：位置仍偏就切回来';
    // 坐标一变，已缓存路线的起点就过期了，有目的地就自动重取一次
    if (this.destName.length > 0) {
      this.fetchRoute();
    }
  }

  // ---------------- 地图绘制 ----------------

  /** 荧光绿路线 + 起终点标记；坐标是归一化的，直接乘以画布尺寸 */
  private redraw(): void {
    const w = this.cctx.width;
    const h = this.cctx.height;
    if (w <= 0 || h <= 0) {
      return;
    }
    this.cctx.clearRect(0, 0, w, h);
    if (this.mfx.length < 2) {
      return;
    }

    // 1) 外发光：宽一点、半透明，模拟荧光溢光
    this.cctx.beginPath();
    this.cctx.lineWidth = 9;
    this.cctx.strokeStyle = '#A6FF0044';
    this.polyline(w, h);
    this.cctx.stroke();

    // 2) 主线
    this.cctx.beginPath();
    this.cctx.lineWidth = 3;
    this.cctx.strokeStyle = '#A6FF00';
    this.polyline(w, h);
    this.cctx.stroke();

    // 3) 起点（紫）、终点（绿）
    const sx = this.mfx[0] * w;
    const sy = this.mfy[0] * h;
    const ex = this.mfx[this.mfx.length - 1] * w;
    const ey = this.mfy[this.mfy.length - 1] * h;

    this.cctx.fillStyle = '#60269E';
    this.cctx.beginPath();
    this.cctx.arc(sx, sy, 7, 0, Math.PI * 2);
    this.cctx.fill();

    this.cctx.strokeStyle = '#A6FF00';
    this.cctx.lineWidth = 2;
    this.cctx.beginPath();
    this.cctx.arc(ex, ey, 8, 0, Math.PI * 2);
    this.cctx.stroke();
    this.cctx.fillStyle = '#A6FF00';
    this.cctx.beginPath();
    this.cctx.arc(ex, ey, 3, 0, Math.PI * 2);
    this.cctx.fill();
  }

  private polyline(w: number, h: number): void {
    for (let i = 0; i < this.mfx.length; i++) {
      const x = this.mfx[i] * w;
      const y = this.mfy[i] * h;
      if (i === 0) {
        this.cctx.moveTo(x, y);
      } else {
        this.cctx.lineTo(x, y);
      }
    }
  }

  /**
   * 路线落盘：取路线（有网）与下发（热点）往往在不同网络环境，中间 App 会被杀，
   * 所以必须持久化，否则"出门前取、到了现场下发"根本走不通。
   */
  private async saveRoute(): Promise<void> {
    const r = this.co.cachedRoute;
    if (r === null) {
      return;
    }
    try {
      const pref = await preferences.getPreferences(this.ctx(), PREF_NAME);
      await pref.put(PREF_ROUTE, r.json);
      await pref.put(PREF_DEST, this.destName);
      await pref.flush();
    } catch (e) {
      const err = e as BusinessError;
      console.error(`[EVA] 路线落盘失败 ${err.code}: ${err.message}`);
    }
  }

  private async loadSavedRoute(): Promise<void> {
    try {
      const pref = await preferences.getPreferences(this.ctx(), PREF_NAME);
      const json = await pref.get(PREF_ROUTE, '') as string;
      const name = await pref.get(PREF_DEST, '') as string;
      if (json.length === 0) {
        return;
      }
      const ok = await this.co.restoreRoute(json);
      if (ok) {
        this.destName = name;
        this.destSub = '上次保存的路线';
        this.summary = this.co.routeSummary();
        this.routeReady = this.co.hasRoute();
      }
    } catch (e) {
      const err = e as BusinessError;
      console.error(`[EVA] 读取缓存路线失败 ${err.code}: ${err.message}`);
    }
  }

  /** 载入记住的身高与坐标模式（在拉健康数据之前，作为兜底基准） */
  private async loadSavedProfile(): Promise<void> {
    try {
      const pref = await preferences.getPreferences(this.ctx(), PREF_NAME);
      const h = await pref.get(PREF_HEIGHT, AppCfg.MANUAL_HEIGHT_CM) as number;
      const gcj = await pref.get(PREF_COORD, AppCfg.LOC_SYS_IS_GCJ02) as boolean;
      if (h > 100 && h < 230) {
        this.co.setManualHeight(h);
        this.heightText = `${h}`;
        this.heightInput = `${h}`;
      }
      this.gcjDirect = gcj;
      this.co.setGcjDirect(gcj);
    } catch (e) {
      const err = e as BusinessError;
      console.error(`[EVA] 读取身高/坐标配置失败 ${err.code}: ${err.message}`);
    }
  }

  private async saveNumber(key: string, v: number): Promise<void> {
    try {
      const pref = await preferences.getPreferences(this.ctx(), PREF_NAME);
      await pref.put(key, v);
      await pref.flush();
    } catch (e) {
      const err = e as BusinessError;
      console.error(`[EVA] 保存 ${key} 失败 ${err.code}: ${err.message}`);
    }
  }

  private async saveBool(key: string, v: boolean): Promise<void> {
    try {
      const pref = await preferences.getPreferences(this.ctx(), PREF_NAME);
      await pref.put(key, v);
      await pref.flush();
    } catch (e) {
      const err = e as BusinessError;
      console.error(`[EVA] 保存 ${key} 失败 ${err.code}: ${err.message}`);
    }
  }

  private ctx(): common.UIAbilityContext {
    return this.getUIContext().getHostContext() as common.UIAbilityContext;
  }
}
