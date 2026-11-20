/**
 * net_link.h — Wi-Fi AP + TCP 服务器 + 30s 通联调度 + 手机数据入口
 *
 * 拓扑（默认，server 模式）：
 *   ESP32 = SoftAP(192.168.4.1) + TCP Server(:8080)
 *   手机 = STA 客户端（DHCP 拿 192.168.4.2 等）
 * 板端“主动请求 GPS” = 在已建立的长连接上发 GPS_REQ 帧（不需要板端发起 connect）。
 *
 * 备选（client 模式，见 doc §4.6）：窗口内板端 connect 到手机 :8080，
 *   优点是可彻底关射频，缺点是每次要重新关联（1.5~2.5s）+ 手机 IP 需固定。
 *   v1 固件只实现 server 模式，client 模式留接口 net_client_probe()。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    bool     valid;
    double   lat;             /* 度 */
    double   lon;
    float    acc_m;           /* 手机上报水平精度 (m) */
    float    course_deg;      /* 手机航向（罗盘/行进方向），<0 无效 */
    float    course_acc_deg;
    float    speed_mps;
    uint32_t phone_ts;        /* unix 秒 */
    uint32_t rx_tick_ms;      /* 板端收到时刻（毫秒节拍） */
    uint32_t seq;             /* 单调递增，供消费方判断“是否有新数据” */
} gps_fix_t;

typedef struct {
    bool     valid;
    float    height_m;
    float    base_step_m;     /* 由身高+历史步数/距离推出的基准步长 */
    float    steps_7d;
    float    dist_7d_m;
    uint8_t  days;
    float    confidence;      /* 0~1 */
    uint32_t seq;
} health_model_t;

typedef enum {
    NET_EV_ROUTE_UPDATED = 0,
    NET_EV_GPS_UPDATED,
    NET_EV_HEALTH_UPDATED,
    NET_EV_PEER_ONLINE,
    NET_EV_PEER_OFFLINE,
} net_event_t;

typedef void (*net_event_cb_t)(net_event_t ev, void *arg);

esp_err_t net_link_start(net_event_cb_t cb, void *arg);

/* ---------------- 传输无关的帧处理入口 ----------------
 * TCP 与 BLE 共用同一套语义：谁承载链路，谁提供 reply 回调把 ACK/PONG 送回去。
 * 这样"板端是否收到路线"这件事只有一份实现，不会再出现两条链路行为不一致。
 */
typedef esp_err_t (*eva_reply_fn)(uint8_t type, uint32_t seq, const char *json, void *arg);

esp_err_t net_handle_frame(uint8_t type, uint32_t seq, uint8_t *buf, uint32_t len,
                           eva_reply_fn reply, void *arg);

/* GPS_RSP 的解析入口（BLE 侧收到即调用，与 TCP 侧共用同一份校验/落盘逻辑） */
esp_err_t net_ingest_gps(uint8_t *buf, uint32_t len);

/* 给 BLE 侧用的 JSON 回包构造：把 reply 语义抽象给 ble_reply 实现 */

/* 快照式读取（QG §3：入口一次性拷贝，使用方不再回读共享结构） */
bool net_gps_snapshot(gps_fix_t *out, uint32_t *last_seq);
bool net_health_snapshot(health_model_t *out, uint32_t *last_seq);

/* 一次性把路线 JSON 灌进来（供 USB 串口/HTTP 调试用，正常走 TCP） */
esp_err_t net_inject_route_json(const char *json, uint32_t len);

/* 手动触发一次 GPS 请求（UI 的“立即校准”按钮） */
void net_request_gps_now(void);

/* 统计：窗口命中率等，供 STATUS 上报与现场诊断 */
typedef struct {
    uint32_t sync_windows;
    uint32_t sync_gps_ok;
    uint32_t sync_gps_timeout;   /* 窗口次数 */
    uint32_t gps_rsp_events;     /* 事件数（含重试成功） */
    uint32_t crc_errors;
    uint32_t peer_connect_count;
} net_stats_t;
void net_stats_get(net_stats_t *out);
