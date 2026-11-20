# EVA × F1 — ESP32-S3 三槽双程序工程

微雪 **ESP32-S3-Touch-AMOLED-1.75C**（466×466 圆屏 / 32MB Flash / 8MB PSRAM）上的三个独立应用共用一块板子的完整代码与交付物。上电先看到选择界面，再进入导航或 F1 看板。

本仓库是 2026-11-20 项目冻结时的归档快照，包含全部源码、最终固件、设计文档与运维脚本；构建中间物、历史快照、取证 dump 已剔除。

---

## 一、这套东西在板子上怎么跑

采用 **三槽双程序**方案（factory + 两个 OTA 槽），分区表见 `firmware/*/partitions_dual.csv`：

| 分区 | 槽 | 偏移 | 大小 | 内容 |
|---|---|---|---|---|
| `app_sel` | factory | `0x20000` | 3 MB | 启动选择器（EVA 风格选哪一屏） |
| `app_nav` | ota_0 | `0x320000` | 5 MB | EVA 导航（地图 / 环视 / 步行路线） |
| `app_navbk` | ota_1 | `0x820000` | 5 MB | 导航备份槽（未启用） |
| `app_f1` | ota_2 | `0xD20000` | 5 MB | F1 看板（车手 / 车队 / 下一站） |
| `storage` | spiffs | `0x1220000` | 10 MB | 导航离线资源（label 不可改，littlefs 写死） |
| `storage_f1` | spiffs | `0x1C20000` | 2 MB | F1 资源 |
| `otadata` | — | `0xF000` | 8 KB | 启动槽记录 |

启动约定：三个应用都会在起来后把 `otadata` 写回 `factory`，所以**任何一次断电重启都会回到选择界面**，不存在"进去出不来"。

---

## 二、目录结构

```
EVA_F1_ESP32/
├── firmware/
│   ├── app_sel/          启动选择器（IDF 工程）
│   ├── app_nav/          EVA 导航（IDF 工程，含 littlefs / 字体源）
│   └── app_f1/           F1 看板（IDF 工程，LVGL + HTTPS 取数）
├── mobile/
│   ├── harmony/          鸿蒙手机端（ArkTS，BLE 直连）
│   └── ios/              iOS 端（Swift 源码，尚无 xcodeproj）
├── docs/
│   ├── system-design.md     导航系统设计主文档
│   ├── firmware-lineage.md  固件谱系与板级验收记录
│   ├── dual-boot.md         三槽双程序启动方案
│   ├── mobile-changes.md    手机端四项改动
│   └── f1-design/           F1 三页设计稿（SVG）
├── release/              可直接烧的最终固件 + flash 脚本
│   ├── app_sel/EVA_Selector.bin
│   ├── app_nav/EVA_Nav.bin
│   ├── app_f1/lvgl_demo_v9.bin
│   ├── bootloader.bin / partition-table.bin / ota_data_initial.bin
│   └── flash_all.ps1
├── tools/                取数与诊断辅助脚本
└── CHANGELOG.md          历次变更日志（版本线全在这里）
```

---

## 三、编译

**必须用纯英文路径**——ESP-IDF 的 CMake 在中文路径下会直接失败。本仓库路径已满足。

```powershell
# 1. 激活工具链（pwsh 7 下不要用 export.ps1，它 dot-source 会报错）
. "C:\Espressif\tools\Microsoft.v5.5.6.PowerShell_profile.ps1"

# 2. C 盘紧张时把临时目录指到 D 盘，并关掉 ccache
$env:TMP="D:\nav_tmp"; $env:TEMP="D:\nav_tmp"
$env:CCACHE_DISABLE="1"; $env:IDF_CCACHE_ENABLE="0"

# 3. 编译（首次需联网拉 managed_components，约 233 MB）
cd D:\EVA_F1_ESP32\firmware\app_f1
idf.py -DIDF_TARGET=esp32s3 build
```

三个工程各自独立构建，互不影响。

---

## 四、烧录

`idf.py flash` 的默认偏移是 `0x20000`，**会覆盖选择器**。烧 F1 或导航必须显式指定偏移：

```powershell
$py="C:\Espressif\tools\python\v5.5.6\venv\Scripts\python.exe"

# F1 看板 → ota_2
& $py -m esptool --chip esp32s3 -p COM5 -b 460800 write_flash 0xD20000 release\app_f1\lvgl_demo_v9.bin

# 导航 → ota_0
& $py -m esptool --chip esp32s3 -p COM5 -b 460800 write_flash 0x320000 release\app_nav\EVA_Nav.bin

# 选择器 → factory
& $py -m esptool --chip esp32s3 -p COM5 -b 460800 write_flash 0x20000 release\app_sel\EVA_Selector.bin
```

**强制回选择界面**：擦掉 `otadata` 即可，bootloader 找不到有效记录就会走 factory。

```powershell
& $py -m esptool --chip esp32s3 -p COM5 -b 460800 erase_region 0xF000 0x2000
```

**强制直接进某一槽**（调试用）：用 `tools/mk_otadata.py` 生成 8 KB 记录写到 `0xF000`，`seq=3` 对应 ota_2（F1），`seq=1` 对应 ota_0（导航）。

---

## 五、必须知道的坑

这些都是踩过、写进代码注释的，改代码前先读一遍：

| 坑 | 表现 | 正解 |
|---|---|---|
| BSP `max_transfer_sz` 过大 | 白屏 + `esp_lvgl:bridge Draw bitmap failed` | 改成 `H_RES*16*BPP/8+64`（=14,976），`buffer_height=16` |
| `use_psram=false` 时字横撕裂，`true` 时又缺内部 RAM | 撕裂 / `esp_wifi_init` 返回 257 | 显示缓冲走 PSRAM，Wi-Fi 前留够内部堆；`SPIRAM_MALLOC_ALWAYSINTERNAL=1024` |
| `esp_wifi_init` rc=257 | 点 F1 黑一下弹回选择器 | = `ESP_ERR_NO_MEM`；别把 8KB 栈的 fetch 任务塞在 Wi-Fi 初始化之前 |
| 无 CA 且未开 `SKIP_SERVER_CERT_VERIFY` | 屏幕只显示 `open 28674` | 真错误是 `0x8017`（SSL_SETUP_FAILED）被压成 `ESP_ERR_HTTP_CONNECT`；开 `CONFIG_ESP_TLS_INSECURE` + `SKIP_SERVER_CERT_VERIFY` |
| 只改 `sdkconfig` | 改了不生效 | esp-tls 是预编译库，必须删 `build/esp-idf/esp-tls` 再全量重编 |
| `esp_netif_set_dns_info()` | 一调用整机失联 | 改用 lwIP 原生 `dns_setserver()` |
| Cloudflare 无 SNI | IP 直连 + Host 头 → 000 | 必须域名访问，绕不开握手 |
| 串口抓运行日志 | `LINES=0` 读不到 | 板子主动明文 POST 回传，见 `tools/f1_server.py` |

---

## 六、当前状态

| 项 | 状态 |
|---|---|
| 选择器 → 导航 / F1 双程序切换 | [实测] 正常，上电回选择界面 |
| 导航环2 分段盘（25 段 × 14.4°） | [实测] 板上 ui23 版已验 |
| F1 界面无撕裂、无白屏 | [实测] 16 行条带 + PSRAM 缓冲 |
| F1 取数（api.jolpi.ca，24 车手 / 11 车队 / 赛历） | [实测] 板上跑通，5 分钟一轮 |
| F1 时间同步（ntp.aliyun.com，CST-8） | [实测] 板上 `t=19:00` 正确 |
| F1 20 秒息屏 + 触摸唤醒 | [实测] 已按需求改回 20 秒 |
| Wi-Fi 双 AP 硬编码、连最强信号 | [实测] 落在 -32 dBm 强 AP |
| OpenF1 数据源 | [受限] 需账号 token，未接入 |
| iOS 端 | [待确认] 只有 Swift 源码，无 xcodeproj，从未编译 |
| 鸿蒙签名 / Health Kit | [受限] 商用机签名报 `9568257`，Health Kit `1001502003` |
