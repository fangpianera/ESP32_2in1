/* secrets.example.h —— 内嵌热点凭据模板
 *
 * 用法：把本文件复制成同目录的 secrets.h，填入自己要连的 2.4 GHz 热点。
 *       secrets.h 已被 .gitignore 忽略，不会进仓库。
 *
 * 没有 secrets.h 时，固件会用下面这组占位值编译（能跑，但连不上任何 AP，
 * 屏幕会停在"无数据"状态）。板载 C6/ESP32-S3 的射频只支持 2.4 GHz。
 */
#pragma once

#define F1_WIFI_AP_COUNT 1
#define F1_WIFI_AP_LIST \
    { "YOUR_SSID_2G", "YOUR_PASSWORD" }
