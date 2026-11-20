/**
 * app_cfg.h — 全局可调参数集中处（便于整定，不散落在各 .c）
 * 所有标注 [参考值] 的参数为工程初值，不是验收线；需按 doc 第 13 节实测表整定。
 */
#pragma once

#define EVA_FW_VERSION            "0.9.0-arch"

/* ---------- 传输层选择 ----------
 * 1 = BLE（NimBLE GATT）为主传输，不启动 Wi-Fi AP
 * 0 = Wi-Fi SoftAP + 裸 TCP（旧路径，保留作回退/对比）
 * 选 BLE 的理由：手机连板端热点时没有外网（取路线必失败），且每次要手动切网；
 * BLE 与手机上网互不干扰，功耗也低一个量级。代价是路线大包要走分片 + 流控。
 */
#define EVA_LINK_BLE              1

/* ---------- Wi-Fi / 网络 ---------- */
#define EVA_AP_SSID_PREFIX        "EVA-NAV-"     /* 后接 MAC 末 2 字节，避免多机同名 */
#define EVA_AP_PASS               "eva01navig"   /* WPA2 要求 >=8 字符 */
#define EVA_AP_CHANNEL            6
#define EVA_AP_MAX_CONN           2
#define EVA_TCP_PORT              8080

/* 30s 定期校准窗口 */
#define EVA_SYNC_PERIOD_MS        30000
#define EVA_SYNC_WINDOW_MS        4000     /* 单次窗口内等待手机接入的上限 */
/* [实测 2026-11-19] 手机侧 GPS_RSP 走"排队发送"，若此刻正在发 8 片路线包，
 * 最坏要等 2.5s 才发出；板端原来只等 1.5s → 日志频繁 "gps window missed"。
 * 3.0s 在路线下发与 STATUS 抢占时仍会超时，取 4.0s（与 EVA_SYNC_WINDOW_MS 同量级）。 */
#define EVA_GPS_WAIT_MS           4000     /* 发出 GPS_REQ 后等待 RSP 的上限 */
#define EVA_GPS_RETRY             2
#define EVA_SESSION_IDLE_MS       35000    /* 长连接空闲多久后主动关闭 */

/* 0 = 长连接(推荐, 延迟最小)；1 = 窗口外关射频(最省电, 但每次需重新关联) */
#define EVA_PS_RADIO_OFF_MODE     0

/* ---------- 采样与算法 ---------- */
#define EVA_IMU_HZ               100       /* 融合任务采样率 */
#define EVA_IMU_ODR_HZ           500       /* 器件 ODR（高于采样率，丢样可接受） */

#define EVA_STEP_GAP_MIN_MS      220       /* 220ms ≈ 272 步/分 上限 */
#define EVA_STEP_GAP_MAX_MS      1600      /* 1.6s  ≈ 37 步/分 下限（慢走/老人） */
#define EVA_STEP_THR_MIN         0.12f     /* m/s^2，高通后幅值门限下限 [参考值] */
#define EVA_STEP_THR_K           2.5f      /* 自适应系数：门限 = K * 噪声σ [参考值] */
#define EVA_HP_ALPHA             0.03f     /* 高通（去重力/漂移）EWMA 系数 @100Hz */
#define EVA_LP_ALPHA             0.25f     /* 低通 EWM */

#define EVA_ZUPT_GYRO_THR        0.03f     /* rad/s，静止判据 [参考值] */
#define EVA_ZUPT_ACC_VAR_THR     0.05f     /* m/s^2 方差门限 [参考值] */
#define EVA_ZUPT_HOLD_MS         180       /* 持续静止多久后锁零速 */

#define EVA_WEINBERG_K           0.45f     /* Weinberg 系数 [参考值-非验收线] */
#define EVA_STRIDE_BASE_MIN      0.35f     /* m */

/* ---------- 步频自相关估计（速度/里程按连续积分，避免一步计两次） ---------- */
#define EVA_CAD_WIN              400       /* 4 s @100Hz 环形窗口样本数 */
#define EVA_CAD_MIN_SAMPLES      200       /* 至少 2 s 数据才出估计 */
#define EVA_CAD_LAG_MIN          40        /* 0.40 s → 150 步/分 上限 */
#define EVA_CAD_LAG_MAX          220       /* 2.20 s → 27 步/分 下限 */
#define EVA_CAD_PEAK_MIN         0.30f     /* 归一化自相关峰值下限 [参考值] */
#define EVA_CAD_AMP_RMS_MIN      0.55f     /* 步行判定幅值 RMS 下限 m/s² [参考值] */
#define EVA_CAD_NOMINAL_HZ       1.80f     /* 基准步长对应的名义步频 [参考值] */
#define EVA_STEP_FB_HALF         0.50f     /* 兜底步频半频折扣：同一真实步被结算两次 [实测基线] */

#define EVA_STRIDE_BASE_MAX      1.05f     /* m */
#define EVA_STRIDE_RATIO_LIMIT   0.35f     /* 动态步长相对基准的最大偏离比例 */

/* 投影离线门槛：横向偏差超过此值就判定"不在路线附近"，投影沿程距离不再参与进度。
 * 依据：合成路线压测（杭州坐标 vs 板端两千公里外）实测 off=2,107,099 m 时
 * 投影退化到末点、进度被算成 100%——屏上直接显示"已到达"。[实测]
 * 取 500 m：步行导航偏出 500 m 已属彻底偏离，正常步行横向偏差 < 50 m。 */
#define EVA_PROJECT_OFF_MAX_M    500.0f

/* ---------- EKF ---------- */
#define EVA_EKF_SIGMA_PSI_STEP   0.020f    /* 每步航向过程噪声 rad [参考值] */
#define EVA_EKF_SIGMA_SCALE_STEP 0.010f    /* 每步步长比例因子过程噪声 [参考值] */
#define EVA_EKF_SIGMA_POS_STEP   0.080f    /* 每步位移噪声 m [参考值] */
#define EVA_EKF_SIGMA_GYRO_RATE  0.0035f   /* 陀螺随机游走 rad/s/sqrt(s) [参考值] */
#define EVA_GATE_CHI2_2DOF_99    9.21f     /* 卡方 2 自由度 99% 门限 */
#define EVA_GPS_MIN_ACC_M        3.0f      /* 手机上报精度优于此值才参与更新 */
#define EVA_GPS_MAX_ACC_M        50.0f     /* 差于此值直接丢弃 */

/* ---------- 资源 ---------- */
#define EVA_ROUTE_JSON_MAX       (256 * 1024)  /* 单条路线上行 JSON 上限（PSRAM 缓冲） */
#define EVA_ROUTE_PTS_MAX        8000
#define EVA_ROUTE_STEPS_MAX      256
#define EVA_MPU_PATH             "/littlefs/route/current.bin"
