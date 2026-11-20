/**
 * nav_core.h — 融合主循环：IMU → PDR → EKF → 路线投影 → UI
 *
 * 单一真值源：航向由 EKF 持有（psi），PDR 每拍以 EKF 航向为基准，
 * 避免"两处各积分一次陀螺"造成的双倍漂移（doc §12 坑 5）。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "net_link.h"   /* net_event_t；net_link.h 不反向包含本文件，无循环依赖 */

typedef struct {
    bool     route_ok;
    float    x_m, y_m;          /* 当前位置（相对路线起点 ENU） */
    float    remain_m;
    float    off_route_m;
    float    walked_m;          /* PDR 累计里程 */
    uint32_t steps;
    float    step_freq_hz;
    float    stride_m;
    float    pos_sigma_m;       /* EKF 位置 1σ */
    float    heading_deg;       /* 0=正北，顺时针 */
    float    walk_bearing_deg;  /* 当前路线段应走方向 */
    float    turn_delta_deg;    /* 箭头应转角度（相对机头，右正） */
    bool     gps_valid;
    uint32_t gps_age_s;
    bool     stationary;
    bool     off_route_alert;
    bool     imu_ok;
    const char *tip;
    const char *road;
    float    next_turn_m;
    float    progress_ratio;    /* 0~1，用于进度弧 */
    float    speed_mps;         /* 平滑后的行走速度，直接给 UI（-1 表示无效） */
} nav_state_t;

esp_err_t nav_core_start(void);
void nav_core_snapshot(nav_state_t *out);
void nav_core_on_net_event(net_event_t ev, void *arg);
void nav_core_recalibrate(void);
