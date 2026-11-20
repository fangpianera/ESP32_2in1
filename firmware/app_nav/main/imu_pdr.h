/**
 * imu_pdr.h — QMI8658 采集 / 静态零偏校准 / 计步 / ZUPT / 动态步长
 *
 * 位域与寄存器依据：Waveshare SensorLib `REG/QMI8658Constants.h`
 *   WHO_AM_I=0x00(默认值0x05)、CTRL1=0x02、CTRL2=0x03、CTRL3=0x04、CTRL5=0x06、
 *   CTRL7=0x08(ACCEL_GYRO_EN_MASK=0x03)、STATUS0=0x2E、AX_L=0x35..GZ_H=0x40
 * 驱动实现走官方组件 waveshare/qmi8658 (^2.0.0)，不在本工程手写位域，避免猜错。
 *
 * 关键工程判断：该板为 6 轴（无磁力计）→ **没有绝对航向**。
 *   航向只能靠 (a) 陀螺积分（相对）+ (b) 手机罗盘/GPS course 做锚定（见 doc §6.4）。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    float    ax, ay, az;      /* m/s^2，机体系 */
    float    gx, gy, gz;      /* rad/s，机体系 */
    uint32_t t_ms;
} imu_snap_t;

typedef struct {
    float    gyro_bias[3];    /* rad/s */
    float    grav_axis[3];    /* 重力单位向量（机体系），用于把陀螺投影到垂直轴 */
    float    grav_norm;       /* 实测 |a| 均值（未补偿前；正常板子≈9.22，见 acc_scale） */
    float    acc_scale;       /* 加速度比例因子 9.80665/|a|，补偿器件实际灵敏度偏差后 ≈1 */
    float    acc_noise;       /* 静置时加速度标准差 */
    uint16_t samples;
    bool     valid;
} imu_cal_t;

typedef struct {
    bool  step_event;         /* 本次调用是否确认一步（仅用于计数/调试） */
    float step_freq_hz;       /* 近 3 步平均步频 */
    float stride_m;           /* 该步步长（Weinberg 微调后的最终值） */
    float amp_ms2;            /* 该步 a_max-a_min */
    bool  stationary;         /* ZUPT 判定 */
    float heading_rad;        /* 相对航向（弧度，右转为正） */
    float yaw_rate;           /* rad/s，去偏后的垂直轴角速度 */
    /* 窗口自相关估出的步频：不受"一步检成两次"影响，用于速度/里程积分 */
    float cadence_hz;         /* 0 = 当前不可信 */
    float amp_rms;            /* 窗口内高通幅值 RMS（m/s²） */
} pdr_out_t;

esp_err_t imu_pdr_init(void);

/* 静置校准：要求板子静止放置 3~5s。失败返回错误，调用方应提示用户重试。
 * 产出：陀螺零偏、重力轴、噪声水平。 */
esp_err_t imu_static_calibrate(uint32_t duration_ms);

const imu_cal_t *imu_cal_get(void);

/* 读取一次（内部做 data-ready 检查，未就绪返回 false） */
bool imu_read(imu_snap_t *out);

/* ---- PDR ---- */
void pdr_init(float base_step_m);
void pdr_set_base_step(float base_step_m);      /* 健康数据更新后调用 */
void pdr_anchor_heading(float heading_rad);     /* GPS/罗盘锚定 */
void pdr_reset(void);
void pdr_apply(const imu_snap_t *s, pdr_out_t *out);
float pdr_base_step(void);
