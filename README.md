# EVA × F1 × MotoNav — ESP32-S3 四槽三程序工程

微雪 **ESP32-S3-Touch-AMOLED-1.75C**（466×466 圆屏 / 32MB Flash / 8MB PSRAM）上的三个独立应用共用一块板子的完整代码与交付物。上电先看到选择界面，再进入 EVA 导航、F1 看板或 MotoNav。

本仓库是 2026-11-20 项目冻结时的归档快照，包含全部源码、最终固件、设计文档与运维脚本；构建中间物、历史快照、取证 dump 已剔除。2026-11-21 追加第三程序（MotoNav）与四槽分区表。

---

## 一、这套东西在板子上怎么跑

采用 **四槽三程序**方案（factory + 三个 OTA 槽），外加 MotoNav 的 FFat 资源卷，分区表见 `firmware/*/partitions_dual.csv`（文件名沿用旧名，内容已是三程序表）：

| 分区 | 槽 | 偏移 | 大小 | 内容 |
|---|---|---|---|---|
| `app_sel` | factory | `0x20000` | 2 MB | 启动选择器（三分底图：紫/红/青） |
| `app_nav` | ota_0 | `0x220000` | 5 MB | EVA 导航（地图 / 环视 / 步行路线） |
| `app_f1` | ota_1 | `0x720000` | 2 MB | F1 看板（车手 / 车队 / 下一站） |
| `app_moto` | ota_2 | `0x920000` | 3 MB | MotoNav（摩托导航 + 天气时钟，第三方固件） |
| `ffat` | fat | `0xC20000` | 6 MB | MotoNav 资源卷（label 必须 `ffat`，subtype 必须 `fat`） |
| `storage` | spiffs | `0x1220000` | 10 MB | 导航离线资源（label 不可改，littlefs 写死；偏移保持原样） |
| `storage_f1` | spiffs | `0x1C20000` | 2 MB | F1 资源 |
| `otadata` | — | `0xF000` | 8 KB | 启动槽记录 |

启动约定：**选择器自己一启动就把 `otadata` 收回 `factory`**（调用 `esp_ota_set_boot_partition(app_sel)`，对 factory 而言等于擦掉两块 otadata 扇区）。导航与 F1 里原有的回跳逻辑保留，双保险。

第三方 MotoNav 不会回跳，靠的是 bootloader 的 rollback 兜底：选择器写槽时记录 `state=NEW`（`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`），程序启动后变 `PENDING_VERIFY`；只要它不调用 `esp_ota_mark_app_valid_cancel_rollback()`，下次复位 bootloader 就把它标 `ABORTED` 并回退到 factory。实测 MotoNav 只跑一个上电周期，**重启必回选择器**。

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
│   ├── app_moto/motonav_slot3m.bin
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
#    ESP-IDF 工具链与源码均已迁到 D 盘（原 C:\Espressif 已清空）：
#      IDF_TOOLS_PATH = D:\Espressif\tools
#      IDF_PATH       = D:\Espressif\esp\v5.5.6\esp-idf
. "D:\Espressif\tools\Microsoft.v5.5.6.PowerShell_profile.ps1"

# 2. C 盘紧张时把临时目录指到 D 盘，并关掉 ccache
$env:TMP="D:\nav_tmp2"; $env:TEMP="D:\nav_tmp2"
$env:CCACHE_DISABLE="1"; $env:IDF_CCACHE_ENABLE="0"

# 3. 编译（首次需联网拉 managed_components，约 233 MB）
cd D:\EVA_F1_ESP32\firmware\app_f1
idf.py -DIDF_TARGET=esp32s3 build
```

三个工程各自独立构建，互不影响。

### Wi-Fi 凭据怎么给（F1 看板）

仓库里**不含任何真实凭据**。`firmware/app_f1/main/` 下只有模板 `secrets.example.h`：

```powershell
cd firmware\app_f1\main
copy secrets.example.h secrets.h      # 然后编辑 secrets.h，填自己的 2.4 GHz 热点
```

`secrets.h` 已在 `.gitignore` 里，不会被提交。没有这个文件时，固件用模板的
`YOUR_SSID_2G` 占位值编译——能烧能跑，但连不上 Wi-Fi，屏幕停在"无数据"。
仓库 `release/app_f1/lvgl_demo_v9.bin` 就是用占位值编的，所以**想让它连网必须自己重编**。

鸿蒙端的高德 Key同理：`Config.ets` 里只有 `PASTE_AMAP_WEB_KEY` 占位，
本机填真值后 `git update-index --assume-unchanged <该文件>` 再提交其他改动。

---

## 四、烧录

`idf.py flash` 的默认偏移是 `0x20000`，**会覆盖选择器**。烧 F1 或导航必须显式指定偏移：

```powershell
$py="D:\Espressif\tools\python\v5.5.6\venv\Scripts\python.exe"

# 选择器 → factory
& $py -m esptool --chip esp32s3 -p COM5 -b 460800 write_flash 0x20000 release\app_sel\EVA_Selector.bin

# 导航 → ota_0
& $py -m esptool --chip esp32s3 -p COM5 -b 460800 write_flash 0x220000 release\app_nav\EVA_Nav.bin

# F1 看板 → ota_1
& $py -m esptool --chip esp32s3 -p COM5 -b 460800 write_flash 0x720000 release\app_f1\lvgl_demo_v9.bin

# MotoNav → ota_2（3 MB 整槽写入）
& $py -m esptool --chip esp32s3 -p COM5 -b 460800 write_flash 0x920000 release\app_moto\motonav_slot3m.bin
```

切换分区表时（从旧三槽表换成四槽三程序表）必须把 bootloader/分区表一起烧，并清掉 MotoNav 时代留下的 nvs / otadata 区（旧表的 `otadata` 在 `0xE000`，正压在新表 nvs 里）：

```powershell
& $py -m esptool --chip esp32s3 -p COM5 -b 460800 erase_region 0x9000 0x8000
```

`release\flash_all.ps1` 已经把这三步串好，直接跑即可。

**强制回选择界面**：擦掉 `otadata` 即可，bootloader 找不到有效记录就会走 factory。

```powershell
# 推荐：脚本里的应急模式（擦完自动复位）
.\release\flash_all.ps1 -Only reset
# 等价的手工命令
& $py -m esptool --chip esp32s3 -p COM5 -b 460800 erase_region 0xF000 0x2000
```

**强制直接进某一槽**（调试用）：用 `tools/mk_otadata.py` 生成 8 KB 记录写到 `0xF000`。注意 crc **只覆盖前 4 字节 `ota_seq`**（`esp_rom_crc32_le(0xFFFFFFFF, &ota_seq, 4)`，等价 `binascii.crc32(struct.pack('<I',seq), 0xFFFFFFFF)`），`ota_state` 字段不参与 crc，随便写；写错 crc 会被 bootloader 判 `ota data partition invalid` 并回 factory。`seq=1` → ota_0（导航），`seq=2` → ota_1（F1），`seq=3` → ota_2（MotoNav）。想要"只跑一次"，把 `ota_state` 写成 0（`ESP_OTA_IMG_NEW`）。

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
| MotoNav 整包烧录把 `otadata` 挪到 `0xE000` | 换成我们的表后旧记录残留、引导乱跳 | 切表时先 `erase_region 0x9000 0x8000`（旧 otadata 压在新 nvs 里） |
| 手工写 `otadata` | bootloader 报 `ota data partition invalid, falling back to factory` | crc 只覆盖前 4 字节 `ota_seq`，且要用 `binascii.crc32(pack, 0xFFFFFFFF)`；不要按"覆盖前 28 字节"算 |
| 第三方程序不回跳 | 重启后仍停在它里面 | 开 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`，点选时 state=NEW，不 mark valid 则在下次复位自动回 factory |
| 改 `sdkconfig` 里 bootloader 选项 | 改了不生效 | 已有的 `sdkconfig` 不会吸收 `sdkconfig.defaults` 的新增项，得直接改 `sdkconfig` 那行再 `idf.py build` |
| `flash_all.ps1` 传参 | esptool 打印 usage 就退出 | 函数形参不能叫 `$Args`（PowerShell 保留变量），且 esptool 子命令/选项一律用下划线（`write_flash`/`--flash_mode`） |
| Arduino FFat 找不到卷 | `DIY FFat mount failed` | 分区必须 label=`ffat`、subtype=`fat`(0x81)，大小给够 |
| 在 MotoNav 里做固件 OTA | 可能写入 `ota_0`（导航槽） | 别用它更新固件；真更了就把导航重烧一遍 |

---

## 六、当前状态

| 项 | 状态 |
|---|---|
| 选择器 → 导航 / F1 双程序切换（旧三槽表） | [实测] 正常，上电回选择界面 |
| 四槽三程序分区表 + 选择器三分界面（NERV 徽标 / F1 logo / 罗盘） | [实测] 2026-11-21 全量烧录，bootloader 列出 11 个分区，选择器正常起界面 |
| 选择器启动即收回引导指针（不依赖子程序回跳） | [实测] 日志 `运行分区=app_sel  启动记录=app_sel` |
| 导航从 `ota_0`(0x220000) 启动 | [实测] `Loaded app from partition at offset 0x220000` |
| F1 从 `ota_1`(0x720000) 启动 | [实测] 段地址落在 0x720000–0x920000，启动后自行回跳 |
| MotoNav 在 `ota_2`(0x920000) 启动、FFat 卷挂载 | [实测] `MotoNav ready, firmware v7.12.10…`、`DIY FFat ready total=6197248` |
| MotoNav 不回跳时自动回选择器（rollback 兜底） | [实测] state NEW→PENDING_VERIFY→ABORTED，下次复位 `Defaulting to factory image` |
| 三个分区的手势点击判定（左/右/下） | [待确认] 需手指点按确认 |
| 导航环2 分段盘（25 段 × 14.4°） | [实测] 板上 ui23 版已验 |
| F1 界面无撕裂、无白屏 | [实测] 16 行条带 + PSRAM 缓冲 |
| F1 取数（api.jolpi.ca，24 车手 / 11 车队 / 赛历） | [实测] 板上跑通，5 分钟一轮 |
| F1 时间同步（ntp.aliyun.com，CST-8） | [实测] 板上 `t=19:00` 正确 |
| F1 20 秒息屏 + 触摸唤醒 | [实测] 已按需求改回 20 秒 |
| Wi-Fi 多 AP 轮换、连最强信号（凭据走本地 `secrets.h`） | [实测] 落在 -32 dBm 强 AP |
| OpenF1 数据源 | [受限] 需账号 token，未接入 |
| iOS 端 | [待确认] 只有 Swift 源码，无 xcodeproj，从未编译 |
| 鸿蒙签名 / Health Kit | [受限] 商用机签名报 `9568257`，Health Kit `1001502003` |
