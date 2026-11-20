#include "ekf.h"
#include "app_cfg.h"

#include <math.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "ekf";
static const float M_PI_F = (float)M_PI;

float ekf_wrap_pi(float a)
{
    while (a >  M_PI_F) a -= 2.0f * M_PI_F;
    while (a < -M_PI_F) a += 2.0f * M_PI_F;
    return a;
}

void ekf_init(ekf_t *e, float x0, float y0, float psi0, float s0)
{
    memset(e, 0, sizeof(*e));
    e->x[0] = x0;
    e->x[1] = y0;
    e->x[2] = ekf_wrap_pi(psi0);
    e->x[3] = (s0 <= 0.0f) ? 1.0f : s0;

    /* 初始协方差：位置给定 GPS 起点，10m 量级；航向 30°；比例因子较自信(2%) */
    e->P[0][0] = 10.0f * 10.0f;
    e->P[1][1] = 10.0f * 10.0f;
    e->P[2][2] = 0.52f * 0.52f;
    e->P[3][3] = 0.02f * 0.02f;
}

void ekf_predict_step(ekf_t *e, float stride_m)
{
    float psi = e->x[2];
    float s   = e->x[3];
    float d   = stride_m * s;
    float c   = cosf(psi), sn = sinf(psi);

    /* psi 约定以 ekf.h 为准：0 = 正北，顺时针为正。
     * 局部 ENU 里 x 向东、y 向北，所以位移投影是 (sin psi, cos psi)。
     * 用 (cos, sin) 会把"向北走"算成"向东走"，整体航向偏 90°——
     * 这个错在纯推算时看不出来，只有接上 GPS 才会暴露（主机测试 T5/T6 抓到的就是这个）。 */
    e->x[0] += d * sn;            /* 东向分量 */
    e->x[1] += d * c;             /* 北向分量 */

    /* F = I + ∂f/∂x */
    float F[EKF_N][EKF_N];
    memset(F, 0, sizeof(F));
    for (int i = 0; i < EKF_N; i++) F[i][i] = 1.0f;
    F[0][2] =  d * c;                 /* ∂x/∂psi */
    F[0][3] =  stride_m * sn;         /* ∂x/∂s   */
    F[1][2] = -d * sn;                /* ∂y/∂psi */
    F[1][3] =  stride_m * c;          /* ∂y/∂s   */

    /* P = F P F^T + Q */
    float FP[EKF_N][EKF_N] = {0};
    for (int i = 0; i < EKF_N; i++)
        for (int j = 0; j < EKF_N; j++) {
            float acc = 0;
            for (int k = 0; k < EKF_N; k++) acc += F[i][k] * e->P[k][j];
            FP[i][j] = acc;
        }
    for (int i = 0; i < EKF_N; i++)
        for (int j = 0; j < EKF_N; j++) {
            float acc = 0;
            for (int k = 0; k < EKF_N; k++) acc += FP[i][k] * F[j][k];   /* F^T[k][j]=F[j][k] */
            e->P[i][j] = acc;
        }

    /* 过程噪声：与步长成比例的位置不确定性 + 航向 + 比例因子 */
    float qp = (EVA_EKF_SIGMA_POS_STEP * stride_m);
    e->P[0][0] += qp * qp;
    e->P[1][1] += qp * qp;
    e->P[2][2] += EVA_EKF_SIGMA_PSI_STEP * EVA_EKF_SIGMA_PSI_STEP;
    e->P[3][3] += EVA_EKF_SIGMA_SCALE_STEP * EVA_EKF_SIGMA_SCALE_STEP;

    e->n_predict++;
}

void ekf_predict_heading(ekf_t *e, float omega, float dt)
{
    e->x[2] = ekf_wrap_pi(e->x[2] + omega * dt);
    e->P[2][2] += EVA_EKF_SIGMA_GYRO_RATE * EVA_EKF_SIGMA_GYRO_RATE * dt;
}

float ekf_pos_sigma(const ekf_t *e)
{
    return sqrtf(fmaxf(e->P[0][0], e->P[1][1]));
}

/* 2x2 对称阵求逆，返回 false 表示奇异 */
static bool inv2(const float S[2][2], float Si[2][2], float *det_out)
{
    float det = S[0][0] * S[1][1] - S[0][1] * S[1][0];
    if (fabsf(det) < 1e-9f) return false;
    Si[0][0] =  S[1][1] / det;
    Si[0][1] = -S[0][1] / det;
    Si[1][0] = -S[1][0] / det;
    Si[1][1] =  S[0][0] / det;
    if (det_out) *det_out = det;
    return true;
}

bool ekf_update_pos(ekf_t *e, float zx, float zy, float sigma_pos)
{
    if (sigma_pos < EVA_GPS_MIN_ACC_M) sigma_pos = EVA_GPS_MIN_ACC_M;  /* 防过度自信 */
    if (sigma_pos > EVA_GPS_MAX_ACC_M) return false;

    /* y = z - Hx, H = [I2 0] */
    float yv[2] = { zx - e->x[0], zy - e->x[1] };

    /* S = H P H^T + R */
    float S[2][2] = {
        { e->P[0][0] + sigma_pos * sigma_pos, e->P[0][1] },
        { e->P[1][0], e->P[1][1] + sigma_pos * sigma_pos },
    };
    float Si[2][2];
    if (!inv2(S, Si, NULL)) return false;

    /* 马氏距离门限 */
    float d2 = yv[0] * (Si[0][0] * yv[0] + Si[0][1] * yv[1])
             + yv[1] * (Si[1][0] * yv[0] + Si[1][1] * yv[1]);
    e->last_gate_d2 = d2;
    if (d2 > EVA_GATE_CHI2_2DOF_99) {
        e->rej_pos_gate++;
        ESP_LOGW(TAG, "GPS 被门限拒绝 d2=%.1f (>%.2f) 偏差=%.1fm", d2,
                 EVA_GATE_CHI2_2DOF_99, sqrtf(yv[0] * yv[0] + yv[1] * yv[1]));
        return false;
    }

    /* K = P H^T S^-1（H 只取前两行/列） */
    float K[EKF_N][2];
    for (int i = 0; i < EKF_N; i++) {
        K[i][0] = e->P[i][0] * Si[0][0] + e->P[i][1] * Si[1][0];
        K[i][1] = e->P[i][0] * Si[0][1] + e->P[i][1] * Si[1][1];
    }

    /* x += K y */
    for (int i = 0; i < EKF_N; i++) e->x[i] += K[i][0] * yv[0] + K[i][1] * yv[1];
    e->x[2] = ekf_wrap_pi(e->x[2]);
    if (e->x[3] < 0.8f) e->x[3] = 0.8f;          /* 步长比例因子护栏 (±20%) */
    if (e->x[3] > 1.2f) e->x[3] = 1.2f;

    /* Joseph 形式：P = (I-KH) P (I-KH)^T + K R K^T，数值稳定 */
    float IKH[EKF_N][EKF_N];
    for (int i = 0; i < EKF_N; i++)
        for (int j = 0; j < EKF_N; j++)
            IKH[i][j] = (i == j ? 1.0f : 0.0f) - ((j == 0) ? K[i][0] : 0.0f)
                                              - ((j == 1) ? K[i][1] : 0.0f);

    float T[EKF_N][EKF_N];
    for (int i = 0; i < EKF_N; i++)
        for (int j = 0; j < EKF_N; j++) {
            float acc = 0;
            for (int k = 0; k < EKF_N; k++) acc += IKH[i][k] * e->P[k][j];
            T[i][j] = acc;
        }
    for (int i = 0; i < EKF_N; i++)
        for (int j = 0; j < EKF_N; j++) {
            float acc = 0;
            for (int k = 0; k < EKF_N; k++) acc += T[i][k] * IKH[j][k];
            e->P[i][j] = acc;
        }
    float r = sigma_pos * sigma_pos;
    for (int i = 0; i < EKF_N; i++)
        for (int j = 0; j < EKF_N; j++)
            e->P[i][j] += K[i][0] * r * K[j][0] + K[i][1] * r * K[j][1];

    /* 对称化，抑制浮点漂移导致的非对称 */
    for (int i = 0; i < EKF_N; i++)
        for (int j = i + 1; j < EKF_N; j++) {
            float m = 0.5f * (e->P[i][j] + e->P[j][i]);
            e->P[i][j] = e->P[j][i] = m;
        }

    e->n_pos_update++;
    ESP_LOGI(TAG, "GPS 采纳: 修正量 %.2fm / 当前 1σ=%.2fm", sqrtf(yv[0] * yv[0] + yv[1] * yv[1]),
             ekf_pos_sigma(e));
    return true;
}

bool ekf_update_psi(ekf_t *e, float psi_meas, float sigma_psi)
{
    if (sigma_psi > 45.0f * M_PI_F / 180.0f) return false;   /* 观测太差，不用 */

    float y = ekf_wrap_pi(psi_meas - e->x[2]);
    if (fabsf(y) > 0.78f) {          /* >45°：更可能是初始锚定/翻转，而不是噪声 */
        e->x[2] = ekf_wrap_pi(psi_meas);
        e->P[2][2] = sigma_psi * sigma_psi;
        e->n_psi_update++;
        ESP_LOGW(TAG, "航向重锚定 %.1f°", psi_meas * 180.0f / M_PI_F);
        return true;
    }

    float S = e->P[2][2] + sigma_psi * sigma_psi;
    if (S < 1e-9f) return false;
    if ((y * y) / S > 9.0f) {         /* 3σ 门限 */
        e->rej_psi_gate++;
        return false;
    }

    float K[EKF_N];
    for (int i = 0; i < EKF_N; i++) K[i] = e->P[i][2] / S;
    for (int i = 0; i < EKF_N; i++) e->x[i] += K[i] * y;
    e->x[2] = ekf_wrap_pi(e->x[2]);

    float Ptmp[EKF_N][EKF_N];
    memcpy(Ptmp, e->P, sizeof(Ptmp));
    for (int i = 0; i < EKF_N; i++)
        for (int j = 0; j < EKF_N; j++)
            e->P[i][j] = Ptmp[i][j] - K[i] * Ptmp[2][j];
    for (int i = 0; i < EKF_N; i++)
        for (int j = 0; j < EKF_N; j++)
            e->P[i][j] += K[i] * K[j] * sigma_psi * sigma_psi;

    e->n_psi_update++;
    return true;
}

void ekf_reset_to(ekf_t *e, float x, float y, float psi)
{
    e->x[0] = x;
    e->x[1] = y;
    e->x[2] = ekf_wrap_pi(psi);
    e->P[0][0] = e->P[1][1] = 10.0f * 10.0f;
    e->P[2][2] = 0.52f * 0.52f;
    e->P[0][1] = e->P[1][0] = 0.0f;
    e->P[0][2] = e->P[2][0] = 0.0f;
    e->P[1][2] = e->P[2][1] = 0.0f;
    e->P[0][3] = e->P[3][0] = 0.0f;
    e->P[1][3] = e->P[3][1] = 0.0f;
    e->P[2][3] = e->P[3][2] = 0.0f;
    e->P[3][3] = 0.02f * 0.02f;
}
