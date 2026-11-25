# 变更日志

记录三个固件从可用到冻结的全部版本线。证据等级沿用项目约定：`[实测]` 板上验证过、`[代码推算]` 由代码/配置推导、`[待确认]` 未验。

---

## 2026-11-21 · 第三程序 MotoNav 并入 + 四槽三程序表

### 板上新固件取证与备份

| 项 | 结论 | 证据 |
|---|---|---|
| 板上分区表已被整包替换 | `nvs 0x9000/0x6000`、`otadata 0xE000/0x2000`、`app0 ota_0 0x10000/3MB`、`app1 ota_1 0x310000/3MB`、`ffat fat 0x610000/9.88MB`、`coredump 0xFF0000/0x60000` | [实测] 读 `0x8000` 起 32KB |
| 新固件身份 | MotoNav，版本串 `v7.12.10-phone.9-zoom.1-cross.1-80M-ota.6-map.6`，启动打印 `MotoNav ready, firmware` | [实测] 镜像字符串扫描 |
| 框架与依赖 | Arduino（`gfx->begin()`）+ NimBLE；资源挂 `/ffat`、`/DIY`；SoftAP 配置页 `192.168.4.1` | [实测] |
| 有效 app 镜像 | 7 段，镜像体结束 `0x17EA30`，4KB 对齐 1,568,768 B；**无 app_desc**（关闭 `APP_DESC_IN_IMAGE`），读不出工程名 | [实测] 段链表解析 |
| `app1` 槽 | 头字节 `0xF0`，非有效镜像；槽内 97% 非 FF 是旧数据残留 | [实测] `image_info` 报 invalid magic |
| ffat 卷 | FAT 卷在 `0x611000`（前 4KB 为 wear-leveling 保留），根目录只有空 `DIY/`，**没有上传过地图包** | [实测] 解析 BPB + 目录项 |
| 原导航 littlefs | `0x1220000` 起 10 MB 完好，`littlefs/` superblock 正常，约 877 KB 数据 | [实测] 新固件没动这一段 |
| 全片备份 | `D:\豆包ESP32项目\motonav_backup\full32m.bin`（32 MB，875 s @306 kbit/s），附 app/分区表/otadata/NVS/ffat/旧 storage 分件与 SHA256 | [实测] |

### 分区表改造（`firmware/*/partitions_dual.csv`，三份同步）

| # | 变更 | 证据 |
|---|---|---|
| 1 | 三槽双程序 → **四槽三程序**：`app_sel 0x20000/2M`、`app_nav 0x220000/5M`、`app_f1 0x720000/2M`、`app_moto 0x920000/3M` | [代码推算] `gen_esp32part --verify` 通过 |
| 2 | 新增 `ffat`（data/fat，`0xC20000/6MB`），给 Arduino FFat 挂 `/ffat`；label 与 subtype 必须匹配否则 `DIY FFat mount failed` | [代码推算] |
| 3 | 拆掉未启用的 `app_navbk`（导航失去 A/B 回滚，出问题重烧） | — |
| 4 | `otadata` 从 MotoNav 的 `0xE000` 收回 `0xF000`；切表前需 `erase_region 0xE000 0x3000` | [代码推算] |
| 5 | `storage` 偏移保持 `0x1220000/10MB` 不动，板上离线资源零迁移 | [实测] |
| 6 | 18 MB app 区末端正好落在 `0x1220000`，总占用 `0x1E30000`，余 1.8 MB | [代码推算] |

### 选择器改造（`app_sel`）

| # | 变更 | 证据 |
|---|---|---|
| 1 | 界面由两半改三分：左上紫（**NERV 徽标**）→ 导航、右上红（**F1 logo**）→ F1、下半青（罗盘）→ MotoNav | [实测] 板上显示正常 |
| 2 | 点击判定改 `pt.y >= 233` → MotoNav，否则按 `pt.x < 233` 分导航/F1 | [代码推算] |
| 3 | 新增 `pin_boot_to_self()`：启动即把 `otadata` 收回 factory，**不再依赖子程序自觉回跳**（第三方 MotoNav 不会） | [实测] 日志 `启动记录=app_sel` |
| 4 | 已指向 factory 时跳过 `esp_ota_set_boot_partition()`，省掉每次上电一次 8 KB 擦写 | [代码推算] |
| 5 | 底图改为程序化生成 `tools/gen_selector_bg.py`（纯标准库，434,312 B RGB565），图与点击分区同源，不会再对不上 | [实测] 板上底图 466×466 cf=18 stride=932 加载成功 |
| 6 | 徽标不再手绘几何图形，改为内置 SVG path 解析器把原 `selector.svg` 的 NERV（`evenodd`，字母+无花果叶）与 F1（`nonzero`）路径展平后光栅化，等比 fit 进各自分区 | [实测] 预览与板上一致 |
| 7 | 分区底色改为**每区独立径向渐变**（中心分别在 (117,117)/(349,117)/(233,352)），否则整圆单渐变会让左上区偏暗、深紫 NERV 压不住 | [实测] |
| 8 | 开 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`，选择器写槽 state=NEW；程序不 mark valid 则下次复位被标 ABORTED 回 factory。**MotoNav 从此只跑一个上电周期** | [实测] NEW→PENDING_VERIFY→`Defaulting to factory` |

### release

- 新增 `release/app_moto/motonav_slot3m.bin`（3 MB 原槽，按原样写入）
- `release/partition-table.bin` 按新表重生成（3,072 B，`--verify` 通过）
- `flash_all.ps1` 改为四槽、四目标；all 模式先擦 `nvs+otadata`（`0x9000 0x8000`）
- `flash_all.ps1` 新增 `-Only reset`：只清 `otadata`，让板子回选择器（MotoNav 当场退出用）
- 修正 `flash_all.ps1` 两处 PowerShell/esptool 语法：函数形参不能叫 `$Args`；子命令与选项用下划线（`write_flash` / `--flash_mode` / `default_reset`）

### 首个三程序上板验证（2026-11-21）

| 项 | 结果 | 证据 |
|---|---|---|
| bootloader 识别新表 | 11 个分区全部列出（`app_sel/app_nav/app_f1/app_moto/ffat/storage/storage_f1/coredump`） | [实测] 串口启动日志 |
| 选择器 | `Loaded app from partition at offset 0x20000`，`EVA-Selector boot`，三分底图建立 | [实测] |
| 导航 | `Loaded app from partition at offset 0x220000`，`EVA_Nav 0.9.0-arch` | [实测] |
| F1 | 段地址 `0x0084CCE8` 落在 0x720000–0x920000，启动后自行回跳 | [实测] |
| MotoNav | `Loaded app from partition at offset 0x920000`、`MNBOARD:WS175`、`DIY FFat ready total=6197248 free=6193152`、`MotoNav ready, firmware v7.12.10…` | [实测] |
| 三程序切换后回选择器 | 复位 → `Defaulting to factory image` → 选择器 | [实测] |
| 手指点击三块区域 | 未测（依赖触摸） | [待确认] |

---

## 2026-11-20 · 项目冻结

### app_f1（F1 看板）收口

| # | 变更 | 证据 |
|---|---|---|
| 41 | `IDLE_OFF_MS` 600000 → **20000**（20 秒无触摸关背光，触摸 `wake_cb` 唤醒） | [实测] 已烧 |
| 40 | 恢复 `hand_boot_back_to_selector()`：F1 起来即把 `otadata` 写回 factory，下次上电必回选择界面 | [实测] |
| 39 | 恢复 `status_time_sync_start()`（SNTP `ntp.aliyun.com` + `CST-8`），回传体加 `t=%02d:%02d` | [实测] 日志 `t=19:00` |
| — | 擦 `otadata` 为全 `FF`，板子回到选择器；导航仍在 `ota_0`（`0x320000`，镜像头 `E9`） | [实测] |
| 38 | `CONFIG_ESP_TLS_INSECURE=y` + `CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y`，删 `build/esp-idf/esp-tls` 强制重编 | [实测] `tls=hb 1 m0 e0 s0` |
| 37 | `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` 16384 → **1024**，内部堆 30K → 39K | [实测] |
| 36 | BSP `.use_psram` 改回 `true`（`max_transfer_sz` 仍 14,976、`buffer_height` 16） | [实测] F1 稳定启动 |
| 35 | 开 `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH`，注释回跳，解析得 `abort()` / `_esp_error_check_failed` / `f1_wifi_init` / **rc=257 = ESP_ERR_NO_MEM** | [实测] |
| 34 | Wi-Fi 改内嵌双 AP（凭据已移到本地 `secrets.h`，仓库只留 `secrets.example.h` 模板），`ALL_CHANNEL_SCAN` + `BY_SIGNAL` + `rssi≥-85`，连上 `-32 dBm` 强 AP | [实测] |
| 33 | 配网门控整段 `#if 0` 关闭 | [实测] |
| 32 | 明文回传取证通道：`f1_report()` POST 到 PC `192.168.3.189:8080` | [实测] |
| 31 | DNS 弃用 `esp_netif_set_dns_info()`（一调用整机失联），改 lwIP `dns_setserver()` | [实测] `dns=ok` |
| 30 | `buffer_height` 8 → **16**、`max_transfer_sz` → 14,976，字符横撕裂消失 | [实测] 用户实拍确认 |
| 29 | 白屏根因定位：`max_transfer_sz=434312` 且 `use_psram=true`，DMA 缓冲落 PSRAM 分配失败 | [实测] |

**当前产物**：`release/app_f1/lvgl_demo_v9.bin`，1,434,512 B，烧 `0xD20000`。

### 回到选择器的办法（无需重新编译）

擦 `otadata`（`0xF000`）即回 factory；写 `tools/mk_otadata.py` 生成的记录则强制进指定槽。这一条替代了过去手工改代码注释的做法。

---

## 2026-11-20 之前 · app_nav（EVA 导航）

版本线以 `release` 内历史 bin 为索引，逐项细节见 `docs/firmware-lineage.md`。

| 版本 | 主题 | 关键改动 |
|---|---|---|
| `EVA_Nav_ui23` | **环2 分段盘（最终）** | `ring2_remain()` 改 `segs=25` / `step=14.4°`，只动环2，其余不碰；3,668,848 B / md5 `919b76676aeefabba7a7cdb71502f8fe` |
| `EVA_Nav_ui22` | 环2 上一版 | 3,668,912 B，回滚基线 |
| `ui10`–`ui21` | 界面重构迭代 | 状态栏、环视、路线行、中文字体逐版调整 |
| `diag1` | 诊断 | 增加运行期自检行 |
| `drift1`–`drift3` | 陀螺零漂 | IMU 漂移补偿标定 |
| `imu1`–`imu3` | PDR | 步行航位推算接入 |
| `field` / `field2` / `field3` | 户外验证 | 实走拍照后的修正版 |
| `routefix` | 路线 | 路线绘制与折线贴图修正 |
| `ringzone` / `ringbig` / `4ring` / `band4` | 环视样式 | 环带宽度、环数、扇格实验 |
| `cjk` / `cjk18` / `cjk24` / `simhei` | 字体 | 中文点阵方案迭代，最终 `cjk24` + `nav18` |
| `scr` / `axp` / `axp2` / `axp3` | 屏幕与电源 | AXP2101 电量读取、亮度控制 |
| `ble` | 手机链路 | BLE 直连替代 TCP 热点 |
| `dbg` / `dump` / `open` | 调试版 | 早期可用性验证 |

**板上结论**：`ui23` 已 `write_flash 0x320000`，`Hash of data verified`。

---

## 平台与架构

| 变更 | 说明 |
|---|---|
| 三槽双程序分区 | factory=`app_sel`、ota_0=`app_nav`、ota_2=`app_f1`，两应用共板互不覆盖 |
| 启动即回跳 | 三个应用均写回 factory，杜绝"进去出不来" |
| BSP 三处补丁 | `max_transfer_sz` / `use_psram` / `buffer_height`，修白屏与撕裂；同源 vendored BSP 需同步 |
| 烧录偏移纪律 | `idf.py flash` 默认 `0x20000` 会覆盖选择器，F1 必须显式 `0xD20000` |
| 取证方式 | 串口运行期不可靠 → 改板端明文 POST 回传 + coredump 落 flash |

---

## 移动端

| 端 | 状态 |
|---|---|
| 鸿蒙 ArkTS | 四项改动完成（`routeInfo` 等）；商用机签名报 `9568257`、Health Kit `1001502003` 未通 |
| iOS Swift | 7 个 `.swift` + `SETUP.md` 就位，无 `.xcodeproj`，从未编译；`EvaLinkService` 仍是 TCP+热点设计，与鸿蒙 BLE 直连不一致 |

---

## 未完成 / 已明确不做

- OpenF1 匿名接口全 `401`，需账号 token；Ergast 官方站已 `404`，现走 `api.jolpi.ca` 镜像。
- 备用热点扫不到，真实 2.4 GHz 名称待确认。
- 导航环2 分段版户外整圈复走未验。
- iOS 链路方案（保留 TCP 还是改 BLE）未拍板。
