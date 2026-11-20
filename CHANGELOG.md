# 变更日志

记录三个固件从可用到冻结的全部版本线。证据等级沿用项目约定：`[实测]` 板上验证过、`[代码推算]` 由代码/配置推导、`[待确认]` 未验。

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
| 34 | Wi-Fi 改内嵌双 AP：`814/***REDACTED***`、`REDACTED_SSID/***REDACTED***`，`ALL_CHANNEL_SCAN` + `BY_SIGNAL` + `rssi≥-85`，连上 `-32 dBm` 强 AP | [实测] |
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
- `REDACTED_SSID` 热点扫不到，真实 2.4 GHz 名称待确认。
- 导航环2 分段版户外整圈复走未验。
- iOS 链路方案（保留 TCP 还是改 BLE）未拍板。
