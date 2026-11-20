# iOS 端接入清单（照着填，10 分钟跑通）

## 1. Podfile

```ruby
platform :ios, '15.0'
target 'EvaNav' do
  use_frameworks!
  pod 'AMap3DMap'          # 高德 3D 地图（含定位/坐标转换）
  pod 'AMapSearch'         # 步行路径规划（本项目的核心）
  pod 'AMapFoundation'
end
```

桥接头 `EvaNav-Bridging-Header.h`：

```objc
#import <AMapFoundationKit/AMapFoundationKit.h>
#import <AMapSearchKit/AMapSearchKit.h>
#import <MAMapKit/MAMapKit.h>
```

`AppDelegate` 里注册 Key（Key 必须是 **iOS 平台 + Bundle ID 匹配** 的那一个）：

```swift
AMapServices.shared().apiKey = "你的iOS Key"
AMapServices.shared().enableHTTPS = true
```

## 2. Info.plist（缺一条就必挂）

```xml
<key>NSLocationWhenInUseUsageDescription</key>
<string>用于步行导航时获取当前位置</string>

<key>NSLocationAlwaysAndWhenInUseUsageDescription</key>
<string>锁屏后仍需持续定位，以便每 30 秒回复导航仪的校准请求</string>

<key>NSLocalNetworkUsageDescription</key>
<string>需要连接 EVA-NAV 导航仪的热点以下发路线和同步定位</string>
<!-- iOS 14+ 访问本地网络必须声明；不声明则 TCP 连接直接超时（坑 6） -->

<key>NSHealthShareUsageDescription</key>
<string>读取身高与近 7 天步数/步行距离，用于计算你的平均步长以提升导航精度</string>

<key>NSHealthUpdateUsageDescription</key>
<string>（本项目不写入健康数据，可省略；若 App Store 审核要求写则保留空说明）</string>

<key>UIBackgroundModes</key>
<array>
  <string>location</string>
</array>

<key>NSAppTransportSecurity</key>
<dict>
  <!-- 高德 SDK 部分接口仍走 http，如启用 enableHTTPS 可只保留例外 -->
  <key>NSAllowsArbitraryLoads</key>
  <true/>
</dict>
```

## 3. Signing & Capabilities

- `Background Modes` → 勾 **Location updates**
- `HealthKit` → 勾（会自动写入 entitlement）
- 若要用 `NEHotspotNetwork.fetchCurrent` 判断是否连的是 EVA 热点：
  申请 **Access WiFi Information** capability（否则 SSID 恒为 nil，只能靠"Wi-Fi 已连"近似判断）

## 4. 三条硬性行为约定

| 场景 | 必须的做法 | 不这么做的后果 |
|---|---|---|
| 连接板端 TCP | `NWParameters.requiredInterfaceType = .wifi` | 热点无外网，iOS 走蜂窝 → 连接超时（坑 7） |
| 后台保持应答 | `allowsBackgroundLocationUpdates = true` + `pausesLocationUpdatesAutomatically = false` | 锁屏 1~2 分钟后挂起，30s 应答全断 |
| 路线下发 | 一次性整包（≤256KB），不发分片 | 板端按帧头 len 一次读满，分片会解析失败 |

## 5. 上机顺序（真机，不是模拟器）

1. 手机连 `EVA-NAV-XXXX` 热点（板端上电即广播）
2. 打开 App → 看日志是否打印 `已连上板端 192.168.4.1:8080`
3. 选目的地 → 板端屏幕应出现"路线已下发 N 点"的 toast 与进度弧
4. 走出去 100m，锁屏 → 观察板端每 30s 是否刷新 `GPS xx s 前`

模拟器没有 HealthKit、也没法连热点，**这一步只能在真机上验**。
