/**
 * ekf.h — PDR/GPS 融合的扩展卡尔曼滤波（4 状态）
 *
 * 状态 x = [ x, y, psi, s ]
 *   x,y : 局部 ENU 米（相对路线起点）
 *   psi : 航向 rad（0=正北，顺时针为正，右转为 +）
 *   s   : 步长比例因子（真实步长 = 步态模型步长 × s），吸收"基准步长不准/鞋跟打滑"
 *
 * 观测量：
 *   (1) 每步位移（PDR 的伪观测，在 predict 内部完成）
 *   (2) 手机 GPS 位置（每 30s 一次）
 *   (3) 手机航向/行进方向（可选，速度 >1m/s 且精度足够时）
 *
 * 门限：位置用卡方 2 自由度 99% (9.21)，航向用 3σ；被门限拒绝的观测计入 rejects_*
 * 而不是强行拉偏滤波器——这是城市峡谷里防止 GPS 跳点把航向带飞的关键。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define EKF_N 4

typedef struct {
    float x[EKF_N];              /* 状态 */
    float P[EKF_N][EKF_N];       /* 协方差 */
    uint32_t n_predict;
    uint32_t n_pos_update;
    uint32_t n_psi_update;
    uint32_t rej_pos_gate;
    uint32_t rej_psi_gate;
    float    last_gate_d2;
} ekf_t;

void ekf_init(ekf_t *e, float x0, float y0, float psi0, float s0);

/* 走一步：位移 = stride_m * s，方向 psi */
void ekf_predict_step(ekf_t *e, float stride_m);

/* 陀螺推进航向（每个采样调用）：psi += omega*dt，协方差随之增长 */
void ekf_predict_heading(ekf_t *e, float omega_rad_s, float dt_s);

/* GPS 位置更新。返回 true 表示被采纳，false 表示被门限拒绝或 sigma 不可信 */
bool ekf_update_pos(ekf_t *e, float zx, float zy, float sigma_pos);

/* 航向观测更新（手机罗盘/GPS course）。delta 超过 45° 视为锚定而不是更新 */
bool ekf_update_psi(ekf_t *e, float psi_meas, float sigma_psi);

void ekf_reset_to(ekf_t *e, float x, float y, float psi);

/* 当前位置 1σ 不确定度估计（m），用于 UI 的置信环与 doc 的漂移自评 */
float ekf_pos_sigma(const ekf_t *e);

/* 航向归一化到 [-π, π] 的工具函数 */
float ekf_wrap_pi(float a);
