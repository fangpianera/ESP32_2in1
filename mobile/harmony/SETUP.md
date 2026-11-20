# HarmonyOS 端接入清单

> 目标：HarmonyOS NEXT（API 12+）。DevEco Studio 5.x。

## 1. 目录结构（本仓库已给出）

```
entry/src/main/ets/
├── common/
│   ├── EvaProtocol.ets      # 20B 帧头 + CRC32，与固件 proto.c 对称
│   └── CoordConv.ets        # WGS-84 → GCJ-02
└── service/
    ├── EvaLink.ets          # TCP 长连接 + GPS_REQ 应答 + 路线下发
    ├── LocationService.ets  # 持续定位 + 后台长时任务
    ├── HealthStride.ets     # 健康数据 → 步长基准（含手动兜底）
    └── AmapRoute.ets        # 高德 Web 服务 API 取步行路线
```

## 2. module.json5 权限（`entry/src/main/module.json5`）

```json5
{
  "module": {
    "requestPermissions": [
      { "name": "ohos.permission.INTERNET" },
      { "name": "ohos.permission.GET_NETWORK_INFO" },
      { "name": "ohos.permission.LOCATION",
        "reason": "$string:reason_location",
        "usedScene": { "abilities": ["EntryAbility"], "when": "inuse" } },
      { "name": "ohos.permission.APPROXIMATELY_LOCATION",
        "reason": "$string:reason_location",
        "usedScene": { "abilities": ["EntryAbility"], "when": "inuse" } },
      { "name": "ohos.permission.LOCATION_IN_BACKGROUND",
        "reason": "$string:reason_location_bg",
        "usedScene": { "abilities": ["EntryAbility"], "when": "always" } },
      { "name": "ohos.permission.ACTIVITY_MOTION" },
      { "name": "ohos.permission.KEEP_BACKGROUND_RUNNING" },
      { "name": "ohos.permission.READ_HEALTH_DATA" }
    ]
  }
}
```

- `LOCATION_IN_BACKGROUND` 需要 **ACL（受限权限）**：个人调试可用 DevEco 的自动签名，
  上架必须申请权限 ACL，否则审核拒绝。
- `READ_HEALTH_DATA` 同理，属于受限权限，需在 AGC 后台申请。

## 3. 连接方式

鸿蒙手机连 `EVA-NAV-XXXX` 热点时，系统会提示"该 WLAN 无法访问互联网，是否保持连接"，
必须点 **保持连接**；若系统自动切回蜂窝，TCP 连接会一直 timeout（与 iOS 坑 7 同类）。

连接前建议先判断当前 WLAN 是否为 EVA 热点：

```ts
import { wifiManager } from '@kit.ConnectivityKit';

wifiManager.getLinkedInfo().then((info) => {
  const ssid = info.ssid.replace('"', '').replace('"', '');
  if (ssid.startsWith('EVA-NAV-')) { /* 可以连接板端 */ }
});
```

## 4. [待确认] 需要你本机核对的接口

| 接口 | 风险 | 核对方式 |
|---|---|---|
| `healthStore.getStepData()` / `getDistanceData()` / `getLatestHeight()` | 方法名与入参随 API 版本变化，且需受限权限 | 打开 `@kit.HealthServiceKit` 的 `d.ts` 核对；失败时自动回落手动身高 |
| `geoLocationManager.LocationRequest` 字段 | `scenario` / `timeInterval` 名称有版本差异 | 同上 |
| 高德 HarmonyOS SDK | 仍在迭代，包名不固定 | 优先用已给出的 **Web 服务 API** 路径，零 SDK 依赖 |
| `socket.TCPSocket.send` 的 data 类型 | 要求 ArrayBuffer | 已按 `frame.buffer as ArrayBuffer` 处理 |

## 5. 行为验收

1. 连热点 → App 日志出现 `READY`
2. 选目的地 → `pushRouteJson()` → 板端 toast 显示"路线已下发 N 点"
3. 锁屏走动 → 板端每 30s 刷新 `GPS xxs 前`；若停止刷新，先查 `KEEP_BACKGROUND_RUNNING` 与长时任务是否真的起来了
