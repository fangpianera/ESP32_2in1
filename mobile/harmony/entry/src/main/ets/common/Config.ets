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
  static readonly AMAP_WEB_KEY: string = 'PASTE_AMAP_WEB_KEY';

  /** 无 GPS 时的手动兜底身高（cm），0 表示不启用 */
  static readonly MANUAL_HEIGHT_CM: number = 172;
  /** 'm' 男 / 'f' 女，只影响身高经验式系数 0.415 / 0.413 */
  static readonly MANUAL_SEX: string = 'm';

  /**
   * 系统定位的坐标系：国内华为/国行机的 geoLocationManager 直出即为 GCJ-02（高德坐标）。
   * 把它再按"WGS-84 → GCJ-02"转一次，会平白多偏一段：海拉尔 574 m / 北京 555 m
   * （离线按同一套公式算得，见 tests/coord_probe.py）。
   * 这就是"App 地图与实际路线整体偏移"的来源。
   *
   * true  = 直出即高德坐标，不再转换（默认，国内机型正确）
   * false = 按 WGS-84 处理，做一次转换（境外机型/纯 GNSS 场景）
   * 运行时可在 App 里一键切换，选择会落盘记住。
   */
  static readonly LOC_SYS_IS_GCJ02: boolean = true;

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
