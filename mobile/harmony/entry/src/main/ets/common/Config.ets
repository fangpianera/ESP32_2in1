/**
 * Config.ets —— 鸿蒙端集中配置（唯一改动点）
 *
 * 板端热点固定：SSID EVA-NAV-01A5 / 192.168.4.1:8080（AP MAC 尾 a5）
 * 若换了板子，SSID 尾号和 IP 仍按固件 wifi_ap.c 的规则推导。
 */

export class BoardCfg {
  /** 板端 TCP 服务端口，固件 net_link.c 里是 8080 */
  static readonly PORT: number = 8080;
  /** AP 网关地址：ESP32 SoftAP 默认 192.168.4.1 */
  static readonly HOST: string = '192.168.4.1';
  /** SSID 前缀，用来判断当前 Wi-Fi 是不是板子 */
  static readonly SSID_PREFIX: string = 'EVA-NAV-';
}

export class AppCfg {
  /**
   * 高德「Web 服务 API」Key（注意不是 iOS/鸿蒙 SDK 的 Key，两者不通用）。
   * 申请：open.amap.com → 控制台 → 应用管理 → 创建应用（服务平台选「Web服务」）
   *
   * 仓库里只保留占位符，真实 Key 不要提交。本机用法：
   *   1) 改成你自己的 Key；
   *   2) `git update-index --assume-unchanged mobile/harmony/entry/src/main/ets/common/Config.ets`
   *      让 git 不再跟踪这处改动（要撤销跟踪用 --no-assume-unchanged）。
   * 值为 PASTE_ 开头时，EvaNavCoordinator 会自动跳过在线底图，只画离线折线。
   */
  static readonly AMAP_WEB_KEY: string = 'PASTE_AMAP_WEB_KEY';   /* 本机真值，已 assume-unchanged，勿提交 */

  /** 无 GPS 时的手动兜底身高（cm），0 表示不启用 */
  static readonly MANUAL_HEIGHT_CM: number = 172;
  /** 'm' 男 / 'f' 女，只影响身高经验式系数 0.415 / 0.413 */
  static readonly MANUAL_SEX: string = 'm';

  /**
   * 系统定位的坐标系，决定要不要再做一次 WGS-84 → GCJ-02。
   *
   * 【2026-11-25 本机实测】华为 VOL-AL00（HarmonyOS 7 / API 26）的 geoLocationManager
   * 直出的是 WGS-84：原始 49.196543,119.756550 距纪委监委办公楼 614 m，
   * 按本文件同一套公式转一次得到 49.198487,119.763904，误差降到 41 m。
   * 所以本机默认 false（转一次）。
   *
   * 部分国行机型的直出值本身已是 GCJ-02，那时要设 true（或 App 内点「切换」），
   * 否则会反向偏 500 多米。判定方法：看界面「坐标」行的「两种模式差 XXX m」，
   * 以及切换后定位点是否落在脚下。
   *
   * true  = 直出即高德坐标，不再转换
   * false = 按 WGS-84 处理，做一次转换（默认，本机实测正确）
   * 运行时可在 App 里一键切换，选择会落盘记住。
   */
  static readonly LOC_SYS_IS_GCJ02: boolean = false;

  /** 目的地搜索半径（米）：只在附近搜，省掉每次手打城市名 */
  static readonly SEARCH_RADIUS_M: number = 100000;
  /** 每页候选数（v3/place/text 上限 25），客户端再按半径过滤 */
  static readonly SEARCH_PAGE_SIZE: number = 25;
  /** 搜索历史保留条数 */
  static readonly SEARCH_HISTORY_MAX: number = 12;

  /**
   * 是否尝试读华为健康（Health Service Kit）的身高/步数/距离。
   * 注意：READ_HEALTH_DATA 是受限权限（ACL），个人调试签名拿不到——
   * 未在 AGC 申请到 ACL 前，这里开着只会静默回落（不影响使用）。
   * 数据类型常量已在 SDK 里给出：healthStore.healthDataTypes.HEIGHT / DAILY_ACTIVITIES。
   */
  static readonly HEALTH_ENABLE: boolean = true;
}
