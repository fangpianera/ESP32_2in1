/**
 * ble_link.h — BLE（NimBLE GATT）传输层
 *
 * 为什么用 BLE 取代 SoftAP+TCP：
 *   1) 手机连板端热点时没有外网，取路线必然失败（实测 2300006 DNS 解析失败），
 *      被迫"先在家取路线、再切热点下发"，流程极不顺手；
 *   2) BLE 与手机自身的 Wi-Fi/蜂窝互不干扰，可以边走边取路线；
 *   3) 功耗低一个量级，符合"极简步行导航"的产品定位。
 *
 * 与 TCP 完全共用应用层帧协议（proto.h）：板端只换承载，不换语义。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* 128-bit UUID：沿用 Nordic UART Service 布局（手机端生态最省事）
 *   Service 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
 *   RX(write) 6E400002-...  手机 → 板
 *   TX(notify)6E400003-...  板 → 手机
 */
#define EVA_BLE_SVC_UUID_STR   "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define EVA_BLE_RX_UUID_STR    "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define EVA_BLE_TX_UUID_STR    "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

esp_err_t ble_link_start(void);
bool ble_link_is_connected(void);

/**
 * 距下一次 30s 校准（GPS_REQ）的剩余秒数，供环3 倒计时使用。
 * 未连接返回 -1；连接后每次发出请求/收到 GPS_RSP 都重置为 30。
 */
int ble_link_calib_countdown_s(void);
