#include "nav_core.h"
#include "app_cfg.h"
#include "board_pins.h"
#include "imu_pdr.h"
#include "ekf.h"
#include "net_link.h"
#include "route_store.h"
#include "ui_eva.h"
#include "power_axp.h"

#include <math.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "nav";
static const float M_PI_F = (float)M_PI;

static ekf_t   s_ekf;
static bool    s_ekf_inited = false;
static SemaphoreHandle_t s_st_mtx;
static nav_state_t s_st;            /* 供 UI 快照 */
static int32_t s_lat0 = 0, s_lon0 = 0;   /* 本地坐标原点（1e7 度） */
static uint32_t s_gps_seq_seen = 0;
static uint32_t s_gps_rx_ms = 0;
static bool     s_gps_valid = false;

#define M_PER_DEG_LAT 110540.0f
#define M_PER_DEG_LON 111320.0f

static void geo_to_local(int32_t lat1e7, int32_t lon1e7, float *x, float *y)
{
    float lat0 = (float)s_lat0 / 1e7f;
    *x = ((float)lon1e7 - (float)s_lon0) / 1e7f * M_PER_DEG_LON
         * cosf(lat0 * M_PI_F / 180.0f);
    *y = ((float)lat1e7 - (float)s_lat0) / 1e7f * M_PER_DEG_LAT;
}

/* 预留：路线段方位角工具（当前 turn_delta 由 walk_bearing 直接相减得到，
 * 暂未调用。加 unused 属性保持 -Wunused-function 干净，避免"看起来像漏调用"。 */
static float __attribute__((unused)) bearing_from_to(float x0, float y0, float x1, float y1)
{
    float b = atan2f(x1 - x0, y1 - y0) * 180.0f / M_PI_F;   /* 0=北，右正 */
    return b < 0 ? b + 360.0f : b;
}

static float wrap180(float d)
{
    while (d >  180.0f) d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    return d;
}

void nav_core_snapshot(nav_state_t *out)
{
    if (xSemaphoreTake(s_st_mtx, pdMS_TO_TICKS(10)) == pdTRUE) {
        *out = s_st;
        xSemaphoreGive(s_st_mtx);
    } else {
        memset(out, 0, sizeof(*out));
    }
}

/* ---------------- 路线投影 → UI 需要的几何量 ---------------- */
static float s_prog_hold = 0.0f;   /* 进度只能增不能减：GPS 抖动会让投影前后跳，环和数字就会来回跳 */
static volatile bool s_walk_reset_req = false; /* 新路线：计步里程与步频必须一起归零，否则 walked_m 会把进度顶回 100% */
static bool  s_along0_valid = false;   /* 进度基准点是否已建立 */
static float s_along0 = 0.0f;          /* 导航起点在路线上的沿程距离（m），进度从这里算 0% */
static uint32_t s_route_fp = 0;        /* 当前路线指纹：同一条路线重复下发时不清零进度 */

/* 路线指纹：点数 + 总长 + 首末点经纬度。同一条路线重复下发（重连后补发）指纹不变，
 * 设备就不会把已经走过的里程和进度一起抹掉——野外"走完全程环还在起点"的一条成因。 */
static uint32_t route_fingerprint(void)
{
    const route_pt_t *p0 = route_point(0);
    size_t n = route_points_count();
    const route_pt_t *pn = (n > 0) ? route_point(n - 1) : NULL;
    if (!p0 || !pn) return 0;
    uint32_t h = 2166136261u;
    uint32_t v[6] = { (uint32_t)n,
                      (uint32_t)llround(route_total_dist_m()),
                      (uint32_t)p0->lat, (uint32_t)p0->lon,
                      (uint32_t)pn->lat, (uint32_t)pn->lon };
    for (int i = 0; i < 6; i++) { h ^= v[i]; h *= 16777619u; }
    return h ? h : 1u;
}

static void refresh_guidance(nav_state_t *st, float x, float y, float psi_deg, float walked_m)
{
    route_loc_t loc;
    if (!route_store_ready() || route_project(x, y, &loc) != ESP_OK) {
        st->route_ok = false;
        return;
    }
    st->route_ok = true;
    st->off_route_m = loc.off_route_m;
    st->off_route_alert = loc.off_route_m > 15.0f;
    st->walk_bearing_deg = loc.seg_bearing;
    st->turn_delta_deg = wrap180(loc.seg_bearing - psi_deg);

    float total_m = route_total_dist_m();
    if (total_m < 1.0f) total_m = loc.remain_m + loc.dist_along;

    /* 是否真的在路线附近：偏出 EVA_PROJECT_OFF_MAX_M 时投影结果没有意义——
     * 最近点必然退化到某个端点，dist_along 会等于 0 或 total，进度被算成 0% 或 100%。
     * 实测证据：从 PC 下发一条杭州坐标的合成路线，板子在两千公里外，
     *   日志出现 prog ratio=1.000 remain=0.0 off=2107099.0 —— 屏上显示"已到达"。*/
    bool on_route = loc.off_route_m <= EVA_PROJECT_OFF_MAX_M;

    /* 剩余里程同理：离线时 loc.remain_m 也退化成一端（要么 0 要么 total），
     * 不能拿它当"还剩多少"。离线就报整条路线的长度，至少数字是真实的。 */
    st->remain_m = on_route ? loc.remain_m : total_m;

    /* 进度基准：路线激活后的第一个有效投影点记为 0%。
     * 不走这一步时，路线原点与当前位置不重合（例如路线是昨天取的）会让一上来就带 8.5% 偏置，
     * 而 s_prog_hold 只增不减，于是"环从头到尾停在一个非零值"。 */
    if (!s_along0_valid && on_route) {
        s_along0 = loc.dist_along;
        if (s_along0 < 0) s_along0 = 0;
        s_along0_valid = true;
        ESP_LOGI(TAG, "进度基准建立 along0=%.1f m / total=%.1f m off=%.1f",
                 (double)s_along0, (double)total_m, (double)loc.off_route_m);
    }
    float span_m = total_m - s_along0;
    if (span_m < 1.0f) { span_m = total_m; s_along0 = 0.0f; }

    /* 两条独立的进度证据，取大者：
     *  ① 投影沿程距离 loc.dist_along（GPS/EKF 位置决定）
     *  ② 计步累计里程 walked_m（PDR 决定，GPS 长时间不来时也不会卡死）
     * 只用 ① 时，GPS 一断位置不动，"走完全程进度纹丝不动"就是这样来的。
     * 但①只在"在路线附近"时才可信：离线时投影退化到端点，会把进度顶到 100%，
     * 这时只认②（计步里程），宁可不动也不要假"已到达"。 */
    float p1 = ((loc.dist_along - s_along0) / span_m);
    float p2 = (walked_m / span_m);
    if (p1 < 0) p1 = 0;
    if (p2 < 0) p2 = 0;
    float prog = (on_route && p1 > p2) ? p1 : p2;
    if (prog > s_prog_hold) s_prog_hold = prog;
    prog = s_prog_hold;
    if (prog < 0) prog = 0;
    if (prog > 1) prog = 1;
    st->progress_ratio = prog;

    /* 剩余里程同样不能回涨：用 span*(1-prog) 与投影剩余两者取小，避免"越走越远" */
    float remain_by_prog = span_m * (1.0f - prog);
    if (remain_by_prog < st->remain_m) st->remain_m = remain_by_prog;
    if (st->remain_m < 0) st->remain_m = 0;

    if (loc.next_step >= 0) {
        /* 转向提示文本：直接取路线记录里的 tip/road */
        st->tip = route_step_tip(loc.next_step);
        st->road = route_step_road(loc.next_step);
        st->next_turn_m = loc.next_dist_m;
    } else {
        st->tip = "沿当前道路直行";
        st->road = "";
        st->next_turn_m = loc.remain_m;
    }
}

/* ---------------- GPS 处理 ---------------- */
static void apply_gps(const gps_fix_t *f)
{
    float x, y;
    int32_t lat = (int32_t)llround(f->lat * 1e7);
    int32_t lon = (int32_t)llround(f->lon * 1e7);

    if (s_lat0 == 0 && s_lon0 == 0) {          /* 首个定位点即原点 */
        s_lat0 = lat; s_lon0 = lon;
    }
    geo_to_local(lat, lon, &x, &y);

    if (!s_ekf_inited) {
        float psi0 = (f->course_deg >= 0) ? f->course_deg : 0.0f;
        ekf_init(&s_ekf, x, y, psi0 * M_PI_F / 180.0f, 1.0f);
        s_ekf_inited = true;
        pdr_anchor_heading(psi0 * M_PI_F / 180.0f);
        s_gps_seq_seen = f->seq;
        ESP_LOGI(TAG, "EKF 初始化于 (%.1f, %.1f) psi=%.0f°", x, y, psi0);
        return;
    }

    bool adopted = ekf_update_pos(&s_ekf, x, y, f->acc_m);
    if (!adopted) {
        ESP_LOGW(TAG, "本次 GPS 位置未采纳（门限/精度） acc=%.1f seq=%u pos=(%.2f,%.2f)",
                 (double)f->acc_m, (unsigned)f->seq, (double)x, (double)y);
    }
    /* [取证] GPS 链路每 5 帧打一条：确认"手机发的点"是否真的进了 EKF。
     * 走完全程进度不动，第一嫌疑就是这里被门限拒了（位置停在原地 → 投影不动）。 */
    if ((f->seq % 5u) == 0u) {
        ESP_LOGI(TAG, "gps seq=%u acc=%.1f spd=%.2f adopted=%d pos=(%.2f,%.2f) sig=%.2f",
                 (unsigned)f->seq, (double)f->acc_m, (double)f->speed_mps, (int)adopted,
                 (double)s_ekf.x[0], (double)s_ekf.x[1], (double)ekf_pos_sigma(&s_ekf));
    }
    /* 航向：静止时手机 course 不可靠，只在有明显位移时使用 */
    if (f->course_deg >= 0 && f->speed_mps > 1.2f && f->course_acc_deg < 30.0f) {
        ekf_update_psi(&s_ekf, f->course_deg * M_PI_F / 180.0f,
                       f->course_acc_deg * M_PI_F / 180.0f);
    }
    pdr_anchor_heading(s_ekf.x[2]);

    s_gps_seq_seen = f->seq;
    s_gps_rx_ms = f->rx_tick_ms;
    s_gps_valid = (f->acc_m <= EVA_GPS_MAX_ACC_M);
}

/* ---------------- 网络事件（net 任务上下文，只做轻量动作）---------------- */
void nav_core_on_net_event(net_event_t e, void *arg)
{
    (void)arg;
    switch (e) {
    case NET_EV_GPS_UPDATED: {
        gps_fix_t f = {0};
        if (net_gps_snapshot(&f, NULL) && f.seq != s_gps_seq_seen) apply_gps(&f);
        break;
    }
    case NET_EV_HEALTH_UPDATED: {
        health_model_t m = {0};
        if (net_health_snapshot(&m, NULL)) {
            pdr_set_base_step(m.base_step_m);
            ESP_LOGI(TAG, "步长基准更新为 %.3f m (conf=%.2f)", m.base_step_m, m.confidence);
        }
        break;
    }
    case NET_EV_ROUTE_UPDATED: {
        /* 新路线：进度与投影必须一起归零，否则沿用上一条路线的 s_prog_hold 会顶死 100% */
        uint32_t fp = route_fingerprint();
        bool same_route = (fp != 0 && fp == s_route_fp);
        s_route_fp = fp;
        if (!same_route) {
            s_prog_hold = 0.0f;
            s_along0_valid = false;
            s_walk_reset_req = true;
        } else {
            /* 同一条路线重复下发（重连补发/重复点②）：保留已走里程与进度，
             * 否则每次重发都把环打回起点，看起来就是"走了一路环不动"。 */
            s_along0_valid = false;   /* 只重建基准，不清进度；旧进度由 s_prog_hold 保住 */
            ESP_LOGI(TAG, "同一条路线重复下发，保留进度 %.0f%%", (double)(s_prog_hold * 100.0f));
        }
        /* 路线更新：把本地原点切到路线原点，避免两套原点偏移 */
        const route_pt_t *p0 = route_point(0);
        if (p0) {
            /* 原点切换必须做坐标平移，不能把 EKF 直接扔到 (0,0)：
             * 原来重置成 (0,0) 等于"瞬移到路线起点"，位置一旦瞬移，
             * 下一帧投影沿程距离就跳变，进度环会出现一次大跳甚至卡值。 */
            float dx = 0.0f, dy = 0.0f;
            if (s_lat0 || s_lon0) {
                route_geo_to_local(s_lat0, s_lon0, &dx, &dy);   /* 旧原点在新原点系下的偏移 */
            }
            s_lat0 = p0->lat; s_lon0 = p0->lon;
            if (s_ekf_inited) {
                float psi = s_ekf.x[2];
                ekf_reset_to(&s_ekf, s_ekf.x[0] + dx, s_ekf.x[1] + dy, psi);
            }
            s_ekf.x[3] = s_ekf.x[3] > 0.1f ? s_ekf.x[3] : 1.0f;
            ESP_LOGI(TAG, "路线已装载，原点重置（平移 %.1f,%.1f m）", (double)dx, (double)dy);
        }
        break;
    }
    default:
        break;
    }
}

/* 把任务本地的 state 整体发布给 UI（单写者 + 互斥量，见 doc §8） */
static void publish(const nav_state_t *ls)
{
    if (xSemaphoreTake(s_st_mtx, pdMS_TO_TICKS(20)) == pdTRUE) {
        /* 只更新可变字段，保留 start() 里设定的 imu_ok 等 */
        s_st.route_ok        = ls->route_ok;
        s_st.x_m             = ls->x_m;
        s_st.y_m             = ls->y_m;
        s_st.remain_m        = ls->remain_m;
        s_st.off_route_m     = ls->off_route_m;
        s_st.walked_m        = ls->walked_m;
        s_st.steps           = ls->steps;
        s_st.step_freq_hz    = ls->step_freq_hz;
        s_st.stride_m        = ls->stride_m;
        s_st.pos_sigma_m     = ls->pos_sigma_m;
        s_st.heading_deg     = ls->heading_deg;
        s_st.walk_bearing_deg= ls->walk_bearing_deg;
        s_st.turn_delta_deg  = ls->turn_delta_deg;
        s_st.gps_valid       = ls->gps_valid;
        s_st.gps_age_s       = ls->gps_age_s;
        s_st.stationary      = ls->stationary;
        s_st.off_route_alert = ls->off_route_alert;
        s_st.tip             = ls->tip;
        s_st.road            = ls->road;
        s_st.next_turn_m     = ls->next_turn_m;
        s_st.progress_ratio  = ls->progress_ratio;
        s_st.speed_mps       = ls->speed_mps;
        xSemaphoreGive(s_st_mtx);
    }
}

/* ---------------- 主任务 ---------------- */
static void nav_task(void *arg)
{
    imu_snap_t s;
    pdr_out_t  p;
    nav_state_t ls = {0};
    uint32_t   tick = 0;
    uint32_t   last_ui_ms = 0;
    uint32_t   last_tick_us = 0;            /* 上一拍时间戳，用于真实 dt */
    float      psi_deg = 0.0f;
    float      stride_hold = 0.70f;         /* 平滑后的步长（m），跨拍保持 */
    float      speed_hold = 0.0f;           /* 平滑后的速度（m/s），给 UI 用 */
    uint32_t   last_step_ms = 0;
    float      s_step_frac = 0.0f;          /* 步数小数部分，避免按步频积分时丢零头 */
    float      s_cad_fb = 0.0f;             /* 兜底步频（峰-谷结算差分），自相关失败时用 */
    uint32_t   s_cad_fb_ms = 0;             /* 兜底步频最后一次刷新时刻 */
    float      s_cad_dbg = 0.0f;            /* diag 用：本拍实际生效的步频 */
    bool       s_cad_dbg_auto = true;       /* diag 用：步频来源（自相关/兜底） */

    ls.tip = "等待路线";
    ls.walked_m = 0.0f;

    while (true) {
        int64_t t_start = esp_timer_get_time();

        if (!imu_read(&s)) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        pdr_apply(&s, &p);
        /* [取证] 主循环实际采样率：步态算法的"100 Hz"假设一旦不成立，
         * 高通/低通的系数、自相关窗口长度、步频换算全部失真。
         * 每秒打一条，用来确认 imu_read 是否真的跑到 100 Hz。 */
        {
            static uint32_t s_rate_cnt = 0;
            static uint32_t s_rate_next = 0;
            uint32_t rnow = (uint32_t)(esp_timer_get_time() / 1000);
            s_rate_cnt++;
            if (rnow >= s_rate_next) {
                s_rate_next = rnow + 1000u;
                ESP_LOGI(TAG, "rate pdr=%u/s", (unsigned)s_rate_cnt);
                s_rate_cnt = 0;
            }
        }
        if (s_walk_reset_req) {
            /* 换路线清零：计步里程/步数/步频保持量都属于"这条路线上的行走" */
            s_walk_reset_req = false;
            ls.walked_m = 0.0f;
            ls.steps = 0;
            s_step_frac = 0.0f;
            speed_hold = 0.0f;
            last_step_ms = (uint32_t)(esp_timer_get_time() / 1000);
        }
        psi_deg = s_ekf_inited ? (s_ekf.x[2] * 180.0f / M_PI_F) : (p.heading_rad * 180.0f / M_PI_F);

        if (s_ekf_inited) {
            /* 航向由 EKF 独持一份，PDR 侧跟随，避免双积分（doc §12 坑 5）
             * 两个坑：
             * 1) 静止时不能积分角速度（PDR 侧本来就冻结），否则人站着不动航向自己转；
             * 2) dt 必须用真实节拍，写死 1/EVA_IMU_HZ 在 UI 刷屏拖慢主循环时会系统性放大积分。 */
            uint32_t now_us = (uint32_t)esp_timer_get_time();
            float dt_s = (last_tick_us == 0) ? 1.0f / EVA_IMU_HZ
                                             : (float)(now_us - last_tick_us) / 1e6f;
            if (dt_s <= 0.0f || dt_s > 0.2f) dt_s = 1.0f / EVA_IMU_HZ;
            last_tick_us = now_us;
            if (!p.stationary) {
                ekf_predict_heading(&s_ekf, p.yaw_rate, dt_s);
            }
            pdr_anchor_heading(s_ekf.x[2]);

            /* 速度/里程改为**连续积分**，不再逐次累加 step_event。
             * 依据（板端实测 log_ui15_march.txt，80 s 插线原地踏步）：
             * 峰-谷逐次结算 198 次 / 70 s ≈ 180 步/分，dt 中位数 334 ms，
             * 同一真实步在全周期内被结算两次，导致速度虚高、walked_m 偏大。
             * cadence_hz 来自 4 s 窗口自相关，看的是整段波形的真实周期，
             * 一步只对应一个周期，与 step_event 是否重复触发无关。 */
            uint32_t now_tick_ms = (uint32_t)(esp_timer_get_time() / 1000);
            float v_walk = 0.0f;

            /* 兜底步频：自相关是主路（看整段波形周期，不受"一步两结算"影响），
             * 但实测常有整秒低于峰值门限而输出 0；此时用峰-谷结算的步频顶上，
             * 半频折扣 EVA_STEP_FB_HALF 对应"同一真实步被结算两次"。
             * 保留 1.2 s 有效期，避免只在有 step_event 的一帧里速度才刷新。 */
            if (p.step_event) {
                float f = (p.step_freq_hz > 0.1f) ? p.step_freq_hz : EVA_CAD_NOMINAL_HZ;
                float cad_fb = EVA_STEP_FB_HALF * f;
                if (cad_fb < 0.60f) cad_fb = 0.60f;
                if (cad_fb > 2.20f) cad_fb = 2.20f;
                s_cad_fb = (s_cad_fb <= 0.0f) ? cad_fb : (0.7f * s_cad_fb + 0.3f * cad_fb);
                s_cad_fb_ms = now_tick_ms;
            } else if (s_cad_fb > 0.0f && (now_tick_ms - s_cad_fb_ms) > 1200u) {
                s_cad_fb = 0.0f;
            }

            float cad_eff = p.cadence_hz;
            bool  cad_auto = true;
            if (cad_eff < 0.45f && s_cad_fb >= 0.45f) {
                cad_eff = s_cad_fb;                 /* 走兜底路 */
                cad_auto = false;
            }
            /* 主路需要幅值过线；兜底路已由 step_event（amp>0.05）把关，不再叠加幅值门限 */
            bool walking = (cad_eff >= 0.45f) && (cad_auto ? (p.amp_rms >= EVA_CAD_AMP_RMS_MIN) : true);
            if (walking) {
                /* 步长按步频温和缩放（快走步幅略大、慢走略小），限幅 ±20% */
                float scl = cad_eff / EVA_CAD_NOMINAL_HZ;
                if (scl < 0.80f) scl = 0.80f;
                if (scl > 1.20f) scl = 1.20f;
                float stride = pdr_base_step() * scl;
                v_walk = cad_eff * stride;
                if (v_walk > 2.20f) v_walk = 2.20f;         /* 正常步行上限 */
                float d = v_walk * dt_s;
                ekf_predict_step(&s_ekf, d);
                ls.walked_m += d * s_ekf.x[3];
                /* 步数按步频积分，小数部分累积进位，避免长距离丢失零头 */
                s_step_frac += cad_eff * dt_s;
                if (s_step_frac >= 1.0f) {
                    uint32_t k = (uint32_t)s_step_frac;
                    ls.steps += k;
                    s_step_frac -= (float)k;
                }
                last_step_ms = now_tick_ms;
                stride_hold = (stride_hold <= 0.0f) ? stride : (0.9f * stride_hold + 0.1f * stride);
            }
            ls.stride_m = stride_hold > 0.0f ? stride_hold : pdr_base_step();

            /* 速度平滑：cadence 每秒才重算一次，直接上屏会跳 0.2 m/s 的台阶。
             * 有效时一阶低通（时间常数 ~0.5 s）；停走 3 s 后按 2 s 时间常数衰减到 0，
             * 不再出现"闪一下数据又变回 0.0"。 */
            if (walking && v_walk > 0.0f) {
                speed_hold = (speed_hold <= 0.0f) ? v_walk : (0.8f * speed_hold + 0.2f * v_walk);
            } else {
                uint32_t idle_ms = now_tick_ms - last_step_ms;
                if (idle_ms > 3000u) speed_hold *= (1.0f - 0.5f * dt_s);
                if (speed_hold < 0.05f) speed_hold = 0.0f;
            }
            if (speed_hold > 2.4f) speed_hold = 2.4f;
            ls.speed_mps = speed_hold;
            ls.step_freq_hz = walking ? cad_eff : 0.0f;
            ls.stationary = (now_tick_ms - last_step_ms > 3000u);
            s_cad_dbg = cad_eff;
            s_cad_dbg_auto = cad_auto;
        }

        /* 5Hz 更新定位/导航量；UI 约 4Hz（230ms）刷一次 */
        if (++tick % (EVA_IMU_HZ / 5) == 0) {
            uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
            ls.pos_sigma_m = s_ekf_inited ? ekf_pos_sigma(&s_ekf) : 0.0f;
            ls.heading_deg = psi_deg;
            ls.gps_age_s = s_gps_rx_ms ? (now_ms - s_gps_rx_ms) / 1000 : 999;
            ls.gps_valid = s_gps_valid && ls.gps_age_s < 45;

            if (s_ekf_inited) {
                refresh_guidance(&ls, s_ekf.x[0], s_ekf.x[1], psi_deg, ls.walked_m);
                ls.x_m = s_ekf.x[0];
                ls.y_m = s_ekf.x[1];
            }
        }
        /* 每 5 秒读一次板载 AXP2101 电量/充电状态（3 个寄存器，~0.3ms，不占实时路径） */
        if (tick % (EVA_IMU_HZ * 5) == 0) {
            power_axp_poll();
            ui_eva_set_power(power_axp_percent(), power_axp_charging());

            /* 静置漂移 / ZUPT 诊断：30s 一条（验收后可再关掉）。
             * psi = EKF 航向，pdr = PDR 侧跟随值（两者应一致，不一致说明锚定被打断）；
             * yaw = 去零偏后的垂直轴角速度（静止时应 ≈0，残差直接体现零偏质量）。 */
            if (tick % (EVA_IMU_HZ * 30) == 0) {
                ESP_LOGI(TAG, "diag t=%us psi=%.1f pdr=%.1f x=%.2f y=%.2f steps=%u walk=%.2f st=%d p_st=%d cad=%.2f src=%s arms=%.2f str=%.2f spd=%.2f yaw=%.4f",
                         (unsigned)(esp_timer_get_time() / 1000000),
                         psi_deg,
                         p.heading_rad * 180.0f / M_PI_F,
                         s_ekf_inited ? s_ekf.x[0] : 0.0f,
                         s_ekf_inited ? s_ekf.x[1] : 0.0f,
                         (unsigned)ls.steps, ls.walked_m,
                         (int)ls.stationary, (int)p.stationary,
                         s_cad_dbg, s_cad_dbg_auto ? "auto" : "fb",
                         p.amp_rms,
                         ls.stride_m, ls.speed_mps, p.yaw_rate);

                /* [取证] 进度链路：ratio/remain 与投影沿程距离、总长、横向偏差、GPS 龄期放一行，
                 * 用来区分"没走路"（steps/walk 不动）、"走偏了"（off 大）、"GPS 断了"（age 大）。 */
                if (ls.route_ok) {
                    route_loc_t loc;
                    float total = route_total_dist_m();
                    if (route_project(s_ekf.x[0], s_ekf.x[1], &loc) == ESP_OK) {
                        ESP_LOGI(TAG, "prog ratio=%.3f remain=%.1f along=%.1f total=%.1f off=%.1f age=%us gpsv=%d",
                                 (double)ls.progress_ratio, (double)ls.remain_m,
                                 (double)loc.dist_along, (double)total,
                                 (double)loc.off_route_m, (unsigned)ls.gps_age_s,
                                 (int)ls.gps_valid);
                    }
                } else {
                    ESP_LOGI(TAG, "prog route_ok=0 age=%us gpsv=%d steps=%u walk=%.2f",
                             (unsigned)ls.gps_age_s, (int)ls.gps_valid,
                             (unsigned)ls.steps, (double)ls.walked_m);
                }
            }
        }

        if (tick % (EVA_IMU_HZ / 4) == 0
            && (uint32_t)(esp_timer_get_time() / 1000) - last_ui_ms >= 230) {
            last_ui_ms = (uint32_t)(esp_timer_get_time() / 1000);
            publish(&ls);
            nav_state_t snap;
            nav_core_snapshot(&snap);
            ui_eva_update(&snap);
        }

        /* 固定 100Hz 节拍（按绝对时间算剩余等待，避免累计漂移） */
        int64_t dt_us = esp_timer_get_time() - t_start;
        int64_t period_us = 1000000 / EVA_IMU_HZ;
        if (dt_us < period_us) vTaskDelay(pdMS_TO_TICKS((period_us - dt_us) / 1000 + 1));
    }
}

void nav_core_recalibrate(void)
{
    ESP_LOGW(TAG, "触发重新校准：请保持静止 4s");
    ui_eva_toast("保持静止：零偏校准中");
    esp_err_t e = imu_static_calibrate(4000);
    if (e == ESP_OK) {
        pdr_reset();
        ui_eva_toast("校准完成");
    } else {
        ui_eva_toast("校准失败：设备未静止");
    }
}

esp_err_t nav_core_start(void)
{
    s_st_mtx = xSemaphoreCreateMutex();
    if (!s_st_mtx) return ESP_ERR_NO_MEM;
    memset(&s_st, 0, sizeof(s_st));
    s_st.imu_ok = imu_cal_get()->valid;
    s_st.tip = "等待路线";

    /* 已缓存路线则先取原点 */
    const route_pt_t *p0 = route_point(0);
    if (p0) { s_lat0 = p0->lat; s_lon0 = p0->lon; }

    xTaskCreatePinnedToCore(nav_task, "nav", 8192, NULL, 5, NULL, 1);
    return ESP_OK;
}
