#include "imu_pdr.h"
#include "app_cfg.h"
#include "board_pins.h"

#include <math.h>
#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp/esp-bsp.h"
#include "qmi8658.h"

static const char *TAG = "imu";
static const float DEG2RAD_F = (float)(M_PI / 180.0);
static const float M_PI_F   = (float)M_PI;

static qmi8658_dev_t *s_dev;
static imu_cal_t      s_cal;
/* ZARU 累计修正量（三轴），限幅用；重新标定时清零 */
static float s_zaru_adj[3];

/* ---------------- 初始化 ---------------- */
esp_err_t imu_pdr_init(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) {
        ESP_LOGE(TAG, "i2c bus not ready");
        return ESP_ERR_INVALID_STATE;
    }

    s_dev = calloc(1, sizeof(qmi8658_dev_t));
    if (!s_dev) return ESP_ERR_NO_MEM;

    /* 器件地址以 WHO_AM_I 探测为准：
     * 官方头文件命名反直觉（0x6B 叫 L、0x6A 叫 H），故不在编译期赌一个常量。 */
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < 2; i++) {
        uint8_t addr = (i == 0) ? EVA_ADDR_QMI8658_A : EVA_ADDR_QMI8658_B;
        err = qmi8658_init(s_dev, bus, addr);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "QMI8658 found at 0x%02X", addr);
            break;
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "QMI8658 probe failed (%s)", esp_err_to_name(err));
        return err;
    }

    ESP_RETURN_ON_ERROR(qmi8658_set_accel_range(s_dev, QMI8658_ACCEL_RANGE_8G), TAG, "acc range");
    ESP_RETURN_ON_ERROR(qmi8658_set_accel_odr(s_dev, QMI8658_ACCEL_ODR_500HZ), TAG, "acc odr");
    /* 该驱动此函数返回 void（qmi8658.h L156），不能套 ESP_RETURN_ON_ERROR（编译期实测报错） */
    qmi8658_set_accel_unit_mps2(s_dev, true);
    ESP_RETURN_ON_ERROR(qmi8658_set_gyro_range(s_dev, QMI8658_GYRO_RANGE_2048DPS), TAG, "gyro range");
    ESP_RETURN_ON_ERROR(qmi8658_set_gyro_odr(s_dev, QMI8658_GYRO_ODR_500HZ), TAG, "gyro odr");

    /* CTRL5 = 0x03：打开加速度/陀螺数字低通（与官方 example 04 一致） */
    qmi8658_write_register(s_dev, QMI8658_CTRL5, 0x03);
    /* CTRL7 = 0x03：ACC + GYRO 使能（QMI8658Constants.h ACCEL_GYRO_EN_MASK） */
    qmi8658_write_register(s_dev, QMI8658_CTRL7, 0x03);

    pdr_init(0.70f);
    return ESP_OK;
}

const imu_cal_t *imu_cal_get(void) { return &s_cal; }

bool imu_read(imu_snap_t *out)
{
    if (!s_dev || !out) return false;
    bool ready = false;
    if (qmi8658_is_data_ready(s_dev, &ready) != ESP_OK || !ready) return false;

    qmi8658_data_t d;
    if (qmi8658_read_sensor_data(s_dev, &d) != ESP_OK) return false;

    /* accel 已在 m/s^2（set_accel_unit_mps2(true)）。
     * gyro 单位按 dps 处理 → rad/s；若实测不符（见 doc §9 待核项 U2）改 s_gyro_scale。 */
    const float gyro_scale = DEG2RAD_F;

    /* accel 已在 m/s^2（set_accel_unit_mps2(true)），再乘实测比例因子：
     * 板端静止实测 |a|=9.22 而非 9.80665（多轮一致 [实测]），即器件实际灵敏度比
     * 手册标称 4096 LSB/g 低约 6%。不补偿的话计步阈值、Weinberg 步长会系统性偏小 6%。 */
    float k = s_cal.valid ? s_cal.acc_scale : 1.0f;

    out->ax = d.accelX * k;
    out->ay = d.accelY * k;
    out->az = d.accelZ * k;
    out->gx = d.gyroX * gyro_scale;
    out->gy = d.gyroY * gyro_scale;
    out->gz = d.gyroZ * gyro_scale;
    out->t_ms = (uint32_t)(esp_timer_get_time() / 1000);
    return true;
}

/* ---------------- 静态校准 ---------------- */
esp_err_t imu_static_calibrate(uint32_t duration_ms)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;

    const uint32_t period_ms = 10;
    const uint32_t n = duration_ms / period_ms;
    double sum_g[3] = {0}, sum_a[3] = {0};
    double sum_a2[3] = {0};
    double sum_an = 0;                 /* 逐样本模长之和（见下：不能用"轴均值的模长"） */
    uint32_t got = 0;
    float gmin[3] = {1e9f, 1e9f, 1e9f}, gmax[3] = {-1e9f, -1e9f, -1e9f};

    ESP_LOGI(TAG, "static calibration start (%ums) — 保持静止", (unsigned)duration_ms);

    for (uint32_t i = 0; i < n; i++) {
        imu_snap_t s;
        if (imu_read(&s)) {
            for (int k = 0; k < 3; k++) {
                float g = (k == 0) ? s.gx : (k == 1) ? s.gy : s.gz;
                float a = (k == 0) ? s.ax : (k == 1) ? s.ay : s.az;
                sum_g[k] += g;
                sum_a[k] += a;
                sum_a2[k] += (double)a * a;
                if (g < gmin[k]) gmin[k] = g;
                if (g > gmax[k]) gmax[k] = g;
            }
            sum_an += sqrt((double)s.ax * s.ax + (double)s.ay * s.ay + (double)s.az * s.az);
            got++;
        }
        vTaskDelay(pdMS_TO_TICKS(period_ms));
    }
    if (got < n / 2) {
        ESP_LOGE(TAG, "calibration insufficient samples %u/%u", (unsigned)got, (unsigned)n);
        return ESP_ERR_TIMEOUT;
    }

    /* 稳定性判据：陀螺三轴极差（min-max）。
     * 门限 0.15 rad/s ≈ 8.6 dps。板端实测（平放桌面）极差 0.037~0.093 rad/s [实测]，
     * 手持/挂绳摆动则 35 rad/s 级别。min-max 的弱点：单次尖峰（轻碰桌面、I2C 抖动）
     * 就足以把整窗判成"晃动"。所以下面把「加速度可信」和「陀螺可信」拆开判定：
     * 加速度决定重力轴与比例因子，陀螺只决定零偏。 */
    const float still_thr = 0.15f;
    bool still = true;
    float worst_range = 0;
    int   worst_axis  = 0;
    float bias[3];
    for (int k = 0; k < 3; k++) {
        float rng = gmax[k] - gmin[k];
        if (rng > worst_range) { worst_range = rng; worst_axis = k; }
        if (rng > still_thr) still = false;
        bias[k] = (float)(sum_g[k] / got);
    }

    float ax = (float)(sum_a[0] / got), ay = (float)(sum_a[1] / got), az = (float)(sum_a[2] / got);

    /* |a| 用"逐样本模长的均值"，而不是"各轴均值再求模"。
     * 校准窗口内一旦有转动，各轴均值会因矢量互相抵消而缩小 → 模长偏低
     * （[实测] 手持校准窗口 get 8.14 而非 9.81）。逐样本模长是标量平均，不受抵消影响。 */
    float an = (float)(sum_an / got);

    /* 加速度噪声（各轴方差均值，开方） */
    double var = 0;
    for (int k = 0; k < 3; k++) {
        double m = sum_a[k] / got;
        var += (sum_a2[k] / got) - m * m;
    }
    float acc_noise = (float)sqrtf((float)(var / 3.0));

    /* 加速度窗口是否可信：只看加速度自己的噪声与模长，与陀螺尖峰无关 */
    bool acc_ok = (acc_noise < 0.08f) && (an > 7.0f) && (an < 12.0f);

    /* 重力轴/比例因子只依赖加速度：加速度干净就写。
     * 绝不能再让陀螺尖峰把 g_axis 留成 [0 0 0]——零向量会让 omega_z 恒为 0、
     * 航向彻底不动（2026-11-19 实测三次重试全废、航向冻结就是这么来的）。 */
    if (acc_ok) {
        s_cal.grav_norm = an;
        float len = sqrtf(ax * ax + ay * ay + az * az);
        if (len > 1e-3f) {
            s_cal.grav_axis[0] = ax / len; s_cal.grav_axis[1] = ay / len; s_cal.grav_axis[2] = az / len;
        } else {
            s_cal.grav_axis[0] = 0; s_cal.grav_axis[1] = 0; s_cal.grav_axis[2] = 1;
        }
        s_cal.acc_noise = acc_noise;
        s_cal.samples   = (uint16_t)got;
        /* 自适应比例因子：样本已按上一个 k 缩放过，故用"乘比例"迭代收敛到 1。
         * 首次（k=1.0）实测 |a|=9.22 → k≈1.063；限幅 ±10% 防异常窗口把 k 拉飞。 */
        float cur = s_cal.acc_scale > 0.1f ? s_cal.acc_scale : 1.0f;
        float k_new = cur * (9.80665f / (an > 1e-3f ? an : 9.80665f));
        if (k_new < 0.90f) k_new = 0.90f;
        if (k_new > 1.10f) k_new = 1.10f;
        s_cal.acc_scale = k_new;
    } else if (s_cal.acc_scale < 0.1f) {
        s_cal.acc_scale = 1.0f;
    }

    /* 零偏只依赖陀螺：静止窗口的均值可用（尖峰对 300 样本均值影响 ≈ 0.001 rad/s），
     * 但"是否高置信"由 still 决定。非静止窗口只在从未成功时兜底（worst_range<0.5），
     * 避免把挂绳甩出来的 35 rad/s 固化下来。ZARU 会在后续静止段继续收敛零偏。 */
    bool commit_bias = still || (!s_cal.valid && worst_range < 0.5f);
    if (commit_bias) {
        s_cal.gyro_bias[0] = bias[0]; s_cal.gyro_bias[1] = bias[1]; s_cal.gyro_bias[2] = bias[2];
        /* 新零偏覆盖旧值，ZARU 的累计修正必须一起清零，否则限幅窗口会被旧修正占满 */
        s_zaru_adj[0] = s_zaru_adj[1] = s_zaru_adj[2] = 0.0f;
        s_cal.valid = still;
    }

    ESP_LOGI(TAG, "cal%s: gyro_bias=[%.4f %.4f %.4f] rad/s, g_axis=[%.3f %.3f %.3f], |a|=%.2f, k=%.3f, noise=%.3f",
             still ? " ok" : "(晃动)",
             s_cal.gyro_bias[0], s_cal.gyro_bias[1], s_cal.gyro_bias[2],
             s_cal.grav_axis[0], s_cal.grav_axis[1], s_cal.grav_axis[2],
             an, s_cal.acc_scale, acc_noise);

    if (!still) {
        ESP_LOGW(TAG, "陀螺窗口有尖峰 (axis %d range=%.3f rad/s > %.2f)：零偏按低置信写入（ZARU 会继续收敛），重力轴%s",
                 worst_axis, worst_range, still_thr, acc_ok ? "已由加速度窗口写入" : "未能写入");
        return ESP_ERR_INVALID_STATE;      /* 调用方据此重试 */
    }
    return ESP_OK;
}

/* ---------------- PDR ---------------- */
typedef struct {
    float  base_step;      /* 基准步长 m */
    float  heading;        /* rad */
    float  lp_slow;        /* 重力/漂移的慢分量 */
    float  hp;             /* 高通后波形 */
    float  lp;             /* 低通后波形 */
    float  noise;          /* 自适应噪声估计 */
    float  thr;            /* 当前门限 */
    float  peak, valley;   /* 当前正峰/负谷 */
    bool   in_peak;
    uint32_t t_last_step;
    float  step_freq;
    float  noise_var_acc;  /* 加速度方差（ZUPT 用） */
    float  mean_acc_mag;
    uint32_t still_since;
    bool   stationary;
    bool   primed;
    /* --- 4 s 窗口自相关步频估计（避免"一步检成两次"把速度翻倍） --- */
    float    cad_ring[EVA_CAD_WIN];   /* 高通波形环形缓冲（100 Hz → 4 s） */
    uint16_t cad_idx;                 /* 下一个写入位置 */
    uint16_t cad_fill;                /* 已填充样本数 */
    float    cadence_hz;              /* 最近一次估计结果，0=不可信 */
    float    amp_rms;                 /* 窗口幅值 RMS */
    uint32_t cad_next_ms;             /* 下次重算时刻 */
} pdr_state_t;

static pdr_state_t st;

/* 自相关估算用的临时缓冲（避免在 4 KB 栈上放 1.6 KB 数组） */
static float s_cad_x[EVA_CAD_WIN];
static float s_cad_r[EVA_CAD_LAG_MAX + 2];

void pdr_init(float base_step_m)
{
    memset(&st, 0, sizeof(st));
    s_zaru_adj[0] = s_zaru_adj[1] = s_zaru_adj[2] = 0.0f;
    st.base_step = base_step_m > 0 ? base_step_m : 0.70f;
    st.lp_slow = 9.80665f;
    st.noise = 0.06f;
    st.thr = EVA_STEP_THR_MIN;
    st.t_last_step = 0;
}

void pdr_set_base_step(float base_step_m)
{
    if (base_step_m < EVA_STRIDE_BASE_MIN) base_step_m = EVA_STRIDE_BASE_MIN;
    if (base_step_m > EVA_STRIDE_BASE_MAX) base_step_m = EVA_STRIDE_BASE_MAX;
    st.base_step = base_step_m;
}

float pdr_base_step(void) { return st.base_step; }

void pdr_anchor_heading(float heading_rad)
{
    st.heading = heading_rad;
}

void pdr_reset(void)
{
    float base = st.base_step;
    float hd = st.heading;
    pdr_init(base);
    st.heading = hd;
}

/* 4 s 窗口归一化自相关估步频。
 * 记 x[i] 为去均值后的高通波形，r(lag)=Σx[i]x[i+lag]/Σx[i]^2。
 * 步行时 r 在"一个完整步周期"处出现主峰；取 0.85×主峰以上的**最早**局部峰，
 * 避免误选 2× 周期的次峰（那会把步频算成一半）。
 * 窗口内无明显周期（静止/噪声）时输出 0，调用方据此不积分离里。 */
static void cadence_estimate(void)
{
    uint16_t n = st.cad_fill;
    if (n < EVA_CAD_MIN_SAMPLES) { st.cadence_hz = 0.0f; st.amp_rms = 0.0f; return; }

    uint16_t head_old = (uint16_t)((st.cad_idx + EVA_CAD_WIN - n) % EVA_CAD_WIN);
    float sum = 0.0f;
    for (uint16_t i = 0; i < n; i++) {
        s_cad_x[i] = st.cad_ring[(uint16_t)((head_old + i) % EVA_CAD_WIN)];
        sum += s_cad_x[i];
    }
    float mean = sum / (float)n;
    float e0 = 0.0f;
    for (uint16_t i = 0; i < n; i++) {
        s_cad_x[i] -= mean;
        e0 += s_cad_x[i] * s_cad_x[i];
    }
    if (e0 < 0.05f) { st.cadence_hz = 0.0f; st.amp_rms = 0.0f; return; }
    st.amp_rms = sqrtf(e0 / (float)n);

    uint16_t lag_max = EVA_CAD_LAG_MAX;
    if ((uint32_t)lag_max + EVA_CAD_LAG_MIN > n) lag_max = (uint16_t)(n - EVA_CAD_LAG_MIN);
    float best = 0.0f;
    for (uint16_t lag = EVA_CAD_LAG_MIN; lag <= lag_max; lag++) {
        float acc = 0.0f;
        for (uint16_t i = 0; (uint32_t)i + lag < n; i++) acc += s_cad_x[i] * s_cad_x[i + lag];
        float r = (acc / e0) * ((float)n / (float)(n - lag));   /* 短窗无偏校正 */
        s_cad_r[lag] = r;
        if (r > best) best = r;
    }
    if (best < EVA_CAD_PEAK_MIN) {
        st.cadence_hz = 0.0f;
        ESP_LOGI(TAG, "cad weak best=%.2f arms=%.2f", (double)best, (double)st.amp_rms);
        return;
    }

    float pick = best * 0.85f;
    for (uint16_t lag = EVA_CAD_LAG_MIN + 1; lag < lag_max; lag++) {
        float r = s_cad_r[lag];
        if (r >= pick && r >= s_cad_r[lag - 1] && r >= s_cad_r[lag + 1]) {
            float hz = (float)EVA_IMU_HZ / (float)lag;
            if (hz >= 0.45f && hz <= 2.50f) {
                st.cadence_hz = hz;
                ESP_LOGI(TAG, "cad best=%.2f lag=%u hz=%.2f arms=%.2f",
                         (double)best, (unsigned)lag, (double)hz, (double)st.amp_rms);
                return;
            }
        }
    }
    st.cadence_hz = 0.0f;
    ESP_LOGI(TAG, "cad NO-PEAK best=%.2f arms=%.2f", (double)best, (double)st.amp_rms);
}


void pdr_apply(const imu_snap_t *s, pdr_out_t *out)
{
    memset(out, 0, sizeof(*out));

    /* 1) 去零偏：陀螺减去静态零偏 */
    float gx = s->gx - s_cal.gyro_bias[0];
    float gy = s->gy - s_cal.gyro_bias[1];
    float gz = s->gz - s_cal.gyro_bias[2];

    /* 2) 把陀螺投影到重力轴 → 得到真正的"绕垂直轴"角速度。
     *    板子放在口袋/手持时 z 轴并不等于垂直轴，直接取 gz 会引入 20%~40% 航向误差。 */
    float omega_z = gx * s_cal.grav_axis[0] + gy * s_cal.grav_axis[1] + gz * s_cal.grav_axis[2];

    /* 3) 航向积分（相对航向，由 GPS/罗盘锚定绝对值） */
    static uint32_t t_prev = 0;
    float dt = (t_prev == 0) ? 0.01f : (float)(s->t_ms - t_prev) / 1000.0f;
    t_prev = s->t_ms;
    if (dt <= 0 || dt > 0.2f) dt = 0.01f;
    if (!st.stationary) {
        st.heading += omega_z * dt;
        while (st.heading >  M_PI_F) st.heading -= 2 * M_PI_F;
        while (st.heading < -M_PI_F) st.heading += 2 * M_PI_F;
    } else {
        /* 3b) 零角速率修正（ZARU）：静止窗口内把"残留零偏"也学掉。
         * 静态标定只扣掉启动那一刻的零偏，投影残差会让静止时仍有 -0.05°/s 量级漂移
         * （板上实测航向 -2.9°/min）。这里沿重力轴以 30s 时间常数微调三轴零偏。
         * 符号必须是 +omega_z：omega_z=(raw-bias)·axis，把 bias·axis 加上 alpha*omega_z
         * 才能得到 (1-alpha)*omega_z（收敛）。写反号会变成 (1+alpha) 的正反馈——
         * 上一版就是这么把零偏在 260s 内推到限幅、yaw 涨到 -0.03 rad/s、静止判定失效、
         * 航向一路滑到 -121° 的。限幅 ±0.01 rad/s（≈0.57°/s），远大于任何合理残差。 */
        float kk = dt / 30.0f;
        if (kk > 0.02f) kk = 0.02f;
        if (fabsf(omega_z) < 0.02f) {          /* 只有"确实接近零"才学，防止慢转被误学 */
            for (int i = 0; i < 3; i++) {
                float d = kk * omega_z * s_cal.grav_axis[i];
                if (fabsf(s_zaru_adj[i] + d) <= 0.01f) {
                    s_cal.gyro_bias[i] += d;
                    s_zaru_adj[i] += d;
                }
            }
            gx = s->gx - s_cal.gyro_bias[0];
            gy = s->gy - s_cal.gyro_bias[1];
            gz = s->gz - s_cal.gyro_bias[2];
            omega_z = gx * s_cal.grav_axis[0] + gy * s_cal.grav_axis[1] + gz * s_cal.grav_axis[2];
        }
    }
    out->yaw_rate = omega_z;
    out->heading_rad = st.heading;

    /* 4) 加速度幅值 → 高通去重力 → 低通去抖 */
    float a_mag = sqrtf(s->ax * s->ax + s->ay * s->ay + s->az * s->az);
    if (!st.primed) { st.lp_slow = a_mag; st.primed = true; }

    st.lp_slow += EVA_HP_ALPHA * (a_mag - st.lp_slow);
    st.hp = a_mag - st.lp_slow;
    st.lp += EVA_LP_ALPHA * (st.hp - st.lp);

    /* 5) 自适应噪声：只用"低于当前门限"的样本更新，避免被步态本身抬高 */
    if (fabsf(st.hp) < st.thr) {
        st.noise += 0.02f * (fabsf(st.hp) - st.noise);
    }
    st.thr = EVA_STEP_THR_K * st.noise;
    if (st.thr < EVA_STEP_THR_MIN) st.thr = EVA_STEP_THR_MIN;
    if (st.thr > 0.60f) st.thr = 0.60f;   /* 防跑飞：跑步时也不至于门限过高 */

    /* 6) 峰值检测：正峰上升沿进入，负谷出现时结算一步 */
    uint32_t now = s->t_ms;
    /* [取证] 上电后 120s 内以 4Hz 打一次波形状态：判断"没检到步"到底是
     * ①幅度没到门限（lp 太小）还是 ②只有正峰没负谷（thr 过高/波形不对称）。
     * 120s 后自动停，长期日志不受影响。 */
    {
        static uint32_t s_dbg_next = 0;
        if (now < 120000u && now >= s_dbg_next) {
            s_dbg_next = now + 250u;
            ESP_LOGI(TAG, "sig lp=%.3f thr=%.2f in_peak=%d var=%.4f gain=%.2f",
                     (double)st.lp, (double)st.thr, (int)st.in_peak,
                     (double)st.noise_var_acc, (double)a_mag);
        }
    }
    if (!st.in_peak && st.lp > st.thr) {
        st.in_peak = true;
        st.peak = st.lp;
    } else if (st.in_peak) {
        if (st.lp > st.peak) st.peak = st.lp;
        if (st.lp < -st.thr) {
            st.in_peak = false;
            st.valley = st.lp;

            uint32_t dt_step = now - st.t_last_step;
            if (dt_step >= EVA_STEP_GAP_MIN_MS && dt_step <= EVA_STEP_GAP_MAX_MS) {
                float amp = st.peak - st.valley;
                if (amp > 0.05f) {
                    /* Weinberg：L = K * (a_max - a_min)^(1/4)，[参考值-非验收线] */
                    float lw = EVA_WEINBERG_K * powf(amp, 0.25f);

                    /* 以健康数据推出的基准为锚，Weinberg 只做有限幅度微调 */
                    float ratio = lw / st.base_step;
                    if (ratio < 1.0f - EVA_STRIDE_RATIO_LIMIT) ratio = 1.0f - EVA_STRIDE_RATIO_LIMIT;
                    if (ratio > 1.0f + EVA_STRIDE_RATIO_LIMIT) ratio = 1.0f + EVA_STRIDE_RATIO_LIMIT;
                    float stride = st.base_step * ratio;

                    st.step_freq = st.step_freq <= 0
                                   ? 1000.0f / (float)dt_step
                                   : 0.7f * st.step_freq + 0.3f * (1000.0f / (float)dt_step);

                    out->step_event = true;
                    out->stride_m = stride;
                    out->amp_ms2 = amp;
                    out->step_freq_hz = st.step_freq;
                    st.t_last_step = now;
                    /* [取证] 每次结算都记一条：铃木判据是否真的产出一步 */
                    ESP_LOGI(TAG, "step OK dt=%ums amp=%.2f thr=%.2f noise=%.3f stride=%.2f f=%.2f",
                             (unsigned)dt_step, (double)amp, (double)st.thr, (double)st.noise,
                             (double)stride, (double)st.step_freq);
                } else {
                    ESP_LOGD(TAG, "step REJ amp dt=%ums amp=%.2f thr=%.2f", (unsigned)dt_step,
                             (double)amp, (double)st.thr);
                }
            } else if (dt_step < EVA_STEP_GAP_MIN_MS) {
                /* 抖动/连击，忽略但不更新时间戳，避免漏掉真正的一步 */
                ESP_LOGD(TAG, "step REJ quick dt=%ums", (unsigned)dt_step);
            } else {
                /* 间隔过长（如刚起步），重置计时 */
                ESP_LOGI(TAG, "step REJ long dt=%ums（超过 %ums，重置计时）",
                         (unsigned)dt_step, (unsigned)EVA_STEP_GAP_MAX_MS);
                st.t_last_step = now;
            }
        }
    }

    /* 6.5) 步频自相关估计：每 100 Hz 写一个样本进 4 s 环形窗口，每 1 s 重算。
     * 用整段波形的真实周期代替"峰-谷逐次计数"——后者在一步里会出现两个过零，
     * 板上实测原地踏步 70 s 记了 198 步（≈180 步/分），正是把一步算成两次。 */
    st.cad_ring[st.cad_idx] = st.lp;
    st.cad_idx = (uint16_t)((st.cad_idx + 1u) % EVA_CAD_WIN);
    if (st.cad_fill < EVA_CAD_WIN) st.cad_fill++;
    if (now >= st.cad_next_ms) {
        st.cad_next_ms = now + 1000u;
        cadence_estimate();
    }

    /* 7) ZUPT：陀螺近零 + 加速度方差小，持续 HOLD_MS → 判定静止 */
    st.mean_acc_mag += 0.05f * (a_mag - st.mean_acc_mag);
    st.noise_var_acc += 0.05f * ((a_mag - st.mean_acc_mag) * (a_mag - st.mean_acc_mag)
                                 - st.noise_var_acc);
    float gmag = sqrtf(gx * gx + gy * gy + gz * gz);

    bool quiet = (gmag < EVA_ZUPT_GYRO_THR) && (st.noise_var_acc < EVA_ZUPT_ACC_VAR_THR);
    if (quiet) {
        if (st.still_since == 0) st.still_since = now;
        if ((now - st.still_since) >= EVA_ZUPT_HOLD_MS) st.stationary = true;
    } else {
        st.still_since = 0;
        st.stationary = false;
    }
    out->stationary = st.stationary;
    out->cadence_hz = st.cadence_hz;
    out->amp_rms = st.amp_rms;
}
