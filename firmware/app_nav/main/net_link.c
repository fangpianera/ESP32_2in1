#include "net_link.h"
#include "app_cfg.h"
#include "proto.h"
#include "route_store.h"
#include "ui_eva.h"          /* STATUS 帧里的电量/天气/目标地直接推给界面 */
#include "screen_pm.h"       /* 有效数据到达 = 有人在用，唤醒屏幕（PING 不算） */

#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#include <errno.h>
#include <math.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_err.h"
#if !EVA_LINK_BLE
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_mac.h"
#endif
#include "esp_heap_caps.h"
#include "cJSON.h"
#if !EVA_LINK_BLE
#include "lwip/sockets.h"
#include "lwip/inet.h"
#endif

static const char *TAG = "net";

/* 诊断计数：由 net_task / sync_task 两个任务共同累加。
 * 用 GCC 原子内建（对齐 32bit 字段）而非锁，避免高频路径抢锁；见 doc §8 证据表。 */
#define STAT_INC(field)  __atomic_fetch_add(&(s_stats.field), 1u, __ATOMIC_RELAXED)

/* ---------------- 共享状态 ---------------- */
static SemaphoreHandle_t s_lock;      /* 保护 s_gps / s_health 的整块拷贝 */
static gps_fix_t         s_gps;
static health_model_t    s_health;
static net_stats_t       s_stats;
static net_event_cb_t    s_cb;
static void             *s_cb_arg;

static atomic_bool  s_gps_pending;    /* 30s 调度器置位，session 完成清零 */
#if !EVA_LINK_BLE
static atomic_bool  s_radio_wanted = true;
static bool         s_radio_on = true;
static int          s_listen_fd = -1;
static bool         s_peer_online;
#endif
#if !EVA_LINK_BLE
static uint8_t     *s_iobuf;          /* PSRAM 缓冲，TCP 单写者（net 任务） */
#endif

static void emit(net_event_t ev)
{
    if (s_cb) s_cb(ev, s_cb_arg);      /* 回调在 net 任务上下文，禁止内部阻塞 */
}

/* ---------------- 快照读取（QG §3 快照模式）---------------- */
bool net_gps_snapshot(gps_fix_t *out, uint32_t *last_seq)
{
    bool fresh = false;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        if (s_gps.valid) {
            *out = s_gps;                              /* 入口一次性拷贝 */
            if (last_seq) {
                fresh = (s_gps.seq != *last_seq);
                *last_seq = s_gps.seq;
            } else {
                fresh = true;
            }
        }
        xSemaphoreGive(s_lock);
    }
    return fresh;
}

bool net_health_snapshot(health_model_t *out, uint32_t *last_seq)
{
    bool fresh = false;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        if (s_health.valid) {
            *out = s_health;
            if (last_seq) {
                fresh = (s_health.seq != *last_seq);
                *last_seq = s_health.seq;
            } else {
                fresh = true;
            }
        }
        xSemaphoreGive(s_lock);
    }
    return fresh;
}

void net_stats_get(net_stats_t *out) { if (out) *out = s_stats; }
void net_request_gps_now(void) { atomic_store(&s_gps_pending, true); }

/* ---------------- Wi-Fi SoftAP ---------------- */
#if !EVA_LINK_BLE
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case WIFI_EVENT_AP_STACONNECTED:
        s_peer_online = true;
        STAT_INC(peer_connect_count);
        ESP_LOGI(TAG, "phone connected");
        break;
    case WIFI_EVENT_AP_STADISCONNECTED:
        s_peer_online = false;
        ESP_LOGW(TAG, "phone disconnected");
        emit(NET_EV_PEER_OFFLINE);
        break;
    default:
        break;
    }
}

static void wifi_start_ap_once(void)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(err);

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(err);

    static esp_netif_t *ap_netif = NULL;
    if (!ap_netif) ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&ic));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        wifi_event_handler, NULL, NULL));

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);

    wifi_config_t wc = {0};
    snprintf((char *)wc.ap.ssid, sizeof(wc.ap.ssid), "%s%02X%02X",
             EVA_AP_SSID_PREFIX, mac[4], mac[5]);
    wc.ap.ssid_len = (uint8_t)strlen((char *)wc.ap.ssid);
    /* 开放式热点（无密码）：手机端少一次输密码/密码错误的机会，
     * 风险面仅限于板端 8080 端口，协议层自带 magic + CRC 校验。
     * 需要恢复加密时把下面两行换回 password + WIFI_AUTH_WPA2_PSK 即可。 */
    wc.ap.password[0] = '\0';
    wc.ap.channel = EVA_AP_CHANNEL;
    wc.ap.max_connection = EVA_AP_MAX_CONN;
    wc.ap.authmode = WIFI_AUTH_OPEN;
    wc.ap.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));   /* IDF 5.x 无 WIFI_AP 宏，需 WIFI_MODE_AP */
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* 注：esp_wifi_set_ps() 对 SoftAP 基本无效（AP 必须按 100ms 发 beacon）。
     * 省电只能靠"窗口外关射频"，见 doc §7 功耗预算 / §12 坑 3。 */
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    ESP_LOGI(TAG, "SoftAP up: %s  ch=%d  tcp:%d", (char *)wc.ap.ssid, EVA_AP_CHANNEL, EVA_TCP_PORT);
}

/* ---------------- 监听 socket ---------------- */
static int listen_open(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { ESP_LOGE(TAG, "socket errno=%d", errno); return -1; }

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in a = {
        .sin_family = AF_INET,
        .sin_port = htons(EVA_TCP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        ESP_LOGE(TAG, "bind errno=%d", errno); close(fd); return -1;
    }
    if (listen(fd, 2) != 0) {
        ESP_LOGE(TAG, "listen errno=%d", errno); close(fd); return -1;
    }
    struct timeval tv = { .tv_sec = 0, .tv_usec = 500 * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ESP_LOGI(TAG, "tcp listening :%d", EVA_TCP_PORT);
    return fd;
}
#endif /* !EVA_LINK_BLE */

/* ---------------- 业务：健康 / GPS / 帧分发 ---------------- */

/* 步长基准融合（doc §6.3）
 *   L_height = h * c(sex)   （经验常数，[参考值-非验收线]）
 *   L_meas   = dist_7d / steps_7d
 *   w        = conf * clamp(steps_7d/10000, 0, 1)
 *   L_base   = w * L_meas + (1-w) * L_height
 */
static float health_compute_base(float height_m, const char *sex,
                                 float steps, float dist_m, float conf)
{
    float c = (sex && (sex[0] == 'f' || sex[0] == 'F')) ? 0.413f : 0.415f;
    float lh = height_m * c;
    float lm = (steps > 500.0f) ? (dist_m / steps) : 0.0f;
    bool lm_ok = (lm > 0.30f && lm < 1.20f);

    float w = 0.0f;
    if (lm_ok) {
        float sw = steps / 10000.0f;
        if (sw > 1.0f) sw = 1.0f;
        w = conf * sw;
    }
    float base = w * lm + (1.0f - w) * lh;
    if (base < EVA_STRIDE_BASE_MIN) base = EVA_STRIDE_BASE_MIN;
    if (base > EVA_STRIDE_BASE_MAX) base = EVA_STRIDE_BASE_MAX;

    ESP_LOGI(TAG, "stride: h=%.2f lh=%.3f lm=%.3f(ok=%d) w=%.2f -> base=%.3f",
             height_m, lh, lm, (int)lm_ok, w, base);
    return base;
}

static esp_err_t handle_health(uint8_t *buf, uint32_t len)
{
    cJSON *j = cJSON_ParseWithLength((const char *)buf, len);
    if (!j) return ESP_ERR_INVALID_ARG;

    const cJSON *jh = cJSON_GetObjectItem(j, "height_cm");
    const cJSON *js = cJSON_GetObjectItem(j, "sex");
    const cJSON *ja = cJSON_GetObjectItem(j, "avg_step_m");
    const cJSON *jn = cJSON_GetObjectItem(j, "steps_7d");
    const cJSON *jd = cJSON_GetObjectItem(j, "dist_7d_m");
    const cJSON *jc = cJSON_GetObjectItem(j, "conf");

    float h = cJSON_IsNumber(jh) ? (float)jh->valuedouble / 100.0f : 0.0f;
    float conf = cJSON_IsNumber(jc) ? (float)jc->valuedouble : 0.5f;
    if (conf < 0.0f) conf = 0.0f;
    if (conf > 1.0f) conf = 1.0f;
    float steps = cJSON_IsNumber(jn) ? (float)jn->valuedouble : 0.0f;
    float dist  = cJSON_IsNumber(jd) ? (float)jd->valuedouble : 0.0f;

    if (h < 1.2f || h > 2.2f) {
        float avg = cJSON_IsNumber(ja) ? (float)ja->valuedouble : 0.70f;
        h = avg / 0.415f;
        conf *= 0.5f;
        ESP_LOGW(TAG, "height missing -> fallback avg_step_m=%.3f", avg);
    }

    health_model_t m = {
        .valid = true,
        .height_m = h,
        .base_step_m = health_compute_base(h, cJSON_IsString(js) ? js->valuestring : "m",
                                           steps, dist, conf),
        .steps_7d = steps,
        .dist_7d_m = dist,
        .days = 7,
        .confidence = conf,
    };
    xSemaphoreTake(s_lock, portMAX_DELAY);
    m.seq = s_health.seq + 1;
    s_health = m;
    xSemaphoreGive(s_lock);

    cJSON_Delete(j);
    emit(NET_EV_HEALTH_UPDATED);
    return ESP_OK;
}

/* 手机 → 板：界面读数（电量/今日天气/目标地名）。
 * 板端不联网，天气与地名都只能由手机拉取后随 STATUS 帧下发；
 * 所有字段都是可选的，缺项保持上次显示。 */
static esp_err_t handle_status(uint8_t *buf, uint32_t len)
{
    cJSON *j = cJSON_ParseWithLength((const char *)buf, len);
    if (!j) return ESP_ERR_INVALID_ARG;

    const cJSON *jb = cJSON_GetObjectItem(j, "bat");
    const cJSON *jh = cJSON_GetObjectItem(j, "hi");
    const cJSON *jl = cJSON_GetObjectItem(j, "lo");
    const cJSON *jc = cJSON_GetObjectItem(j, "code");
    const cJSON *jd = cJSON_GetObjectItem(j, "dest");

    if (cJSON_IsNumber(jb)) {
        ui_eva_set_battery((int)jb->valuedouble);
    }
    if (cJSON_IsNumber(jh) && cJSON_IsNumber(jl) && cJSON_IsNumber(jc)) {
        ui_eva_set_weather((int)jh->valuedouble, (int)jl->valuedouble,
                           (int)jc->valuedouble);
    }
    if (cJSON_IsString(jd)) {
        ui_eva_set_dest(jd->valuestring);
    }

    cJSON_Delete(j);
    return ESP_OK;
}

static esp_err_t handle_gps(uint8_t *buf, uint32_t len)
{    cJSON *j = cJSON_ParseWithLength((const char *)buf, len);
    if (!j) return ESP_ERR_INVALID_ARG;

    const cJSON *la = cJSON_GetObjectItem(j, "lat");
    const cJSON *lo = cJSON_GetObjectItem(j, "lon");
    if (!cJSON_IsNumber(la) || !cJSON_IsNumber(lo)) { cJSON_Delete(j); return ESP_ERR_INVALID_ARG; }

    const cJSON *jacc = cJSON_GetObjectItem(j, "acc");
    const cJSON *jcrs = cJSON_GetObjectItem(j, "course");
    const cJSON *jcra = cJSON_GetObjectItem(j, "course_acc");
    const cJSON *jspd = cJSON_GetObjectItem(j, "speed");
    const cJSON *jts  = cJSON_GetObjectItem(j, "ts");

    gps_fix_t f = {
        .valid = true,
        .lat = la->valuedouble,
        .lon = lo->valuedouble,
        .acc_m = cJSON_IsNumber(jacc) ? (float)jacc->valuedouble : 15.0f,
        .course_deg = cJSON_IsNumber(jcrs) ? (float)jcrs->valuedouble : -1.0f,
        .course_acc_deg = cJSON_IsNumber(jcra) ? (float)jcra->valuedouble : 999.0f,
        .speed_mps = cJSON_IsNumber(jspd) ? (float)jspd->valuedouble : 0.0f,
        .phone_ts = cJSON_IsNumber(jts) ? (uint32_t)jts->valuedouble : 0,
        .rx_tick_ms = (uint32_t)(xTaskGetTickCount() * 1000 / configTICK_RATE_HZ),
    };
    xSemaphoreTake(s_lock, portMAX_DELAY);
    f.seq = s_gps.seq + 1;
    s_gps = f;
    xSemaphoreGive(s_lock);

    cJSON_Delete(j);
    ESP_LOGI(TAG, "gps %.6f,%.6f acc=%.1f crs=%.0f", f.lat, f.lon, f.acc_m, f.course_deg);
    emit(NET_EV_GPS_UPDATED);
    return ESP_OK;
}

/* TCP 侧的 reply 实现：直接用 socket 发 */
#if !EVA_LINK_BLE
static esp_err_t tcp_reply(uint8_t type, uint32_t seq, const char *json, void *arg)
{
    int fd = (int)(intptr_t)arg;
    return (proto_send_json(fd, type, seq, json) == PROTO_OK) ? ESP_OK : ESP_FAIL;
}
#endif

/* 处理一帧（传输无关）。返回 false 表示流已不可信（TCP 必须断开重连）。 */
esp_err_t net_handle_frame(uint8_t type, uint32_t seq, uint8_t *buf, uint32_t len,
                           eva_reply_fn reply, void *arg)
{
    char ack[160];

    /* 心跳（PING/PONG）每 10s 一次，不能算"有人在看屏幕"，否则永远不熄屏；
     * 其余帧（路线/STATUS/GPS_RSP/健康）都意味着手机端在操作 → 唤醒。 */
    if (type != EVA_T_PING && type != EVA_T_PONG) {
        screen_pm_activity("ble frame");
    }

    switch (type) {
    case EVA_T_STATUS:
        return handle_status(buf, len);
    case EVA_T_ROUTE_PUSH: {
        uint16_t npts = 0;
        int32_t dist_mm = 0;
        esp_err_t err = route_store_ingest_json((const char *)buf, len, &npts, &dist_mm);
        ESP_LOGI(TAG, "route push len=%u -> npts=%u dist=%dmm ok=%d",
                 (unsigned)len, (unsigned)npts, (int)dist_mm, (int)err);
        snprintf(ack, sizeof(ack), "{\"ok\":%s,\"pts\":%u,\"dist_m\":%.1f}",
                 err == ESP_OK ? "true" : "false", (unsigned)npts, (double)(dist_mm / 1000.0));
        if (reply) reply(EVA_T_ROUTE_ACK, seq, ack, arg);
        if (err == ESP_OK) {
            if (route_store_load()) emit(NET_EV_ROUTE_UPDATED);
            else ESP_LOGE(TAG, "route saved but reload failed");
        }
        return err;
    }
    case EVA_T_GPS_RSP:
        return handle_gps(buf, len);
    case EVA_T_HEALTH_PUSH: {
        handle_health(buf, len);
        health_model_t m = {0};
        net_health_snapshot(&m, NULL);
        snprintf(ack, sizeof(ack), "{\"ok\":true,\"base_step_m\":%.3f}", (double)m.base_step_m);
        if (reply) reply(EVA_T_HEALTH_ACK, seq, ack, arg);
        return ESP_OK;
    }
    case EVA_T_TIME_SYNC:
    case EVA_T_GPS_REQ:
        /* 手机→板的时间同步/反向查询：当前版本仅保留解析点 */
        return ESP_OK;
    case EVA_T_PING:
        if (reply) reply(EVA_T_PONG, seq, "{\"pong\":1}", arg);
        return ESP_OK;
    default:
        ESP_LOGW(TAG, "unhandled type %u", type);
        return ESP_OK;
    }
}

esp_err_t net_ingest_gps(uint8_t *buf, uint32_t len)
{
    return handle_gps(buf, len);
}

/* TCP 侧薄包装 */
#if !EVA_LINK_BLE
static bool handle_frame(int fd, uint8_t type, uint32_t seq, uint8_t *buf, uint32_t len)
{
    net_handle_frame(type, seq, buf, len, tcp_reply, (void *)(intptr_t)fd);
    return true;
}

/* 一次 GPS 往返：发 REQ → 等 RSP，期间其它帧照常处理 */
static bool session_fetch_gps(int fd)
{
    uint8_t  type = 0;
    uint32_t seq = 0, len = 0;

    for (int attempt = 0; attempt <= EVA_GPS_RETRY; attempt++) {
        proto_send_json(fd, EVA_T_GPS_REQ, proto_next_seq(), "{\"want\":\"gps\"}");
        uint32_t t0 = (uint32_t)(xTaskGetTickCount() * 1000 / configTICK_RATE_HZ);

        while ((uint32_t)(xTaskGetTickCount() * 1000 / configTICK_RATE_HZ) - t0 < EVA_GPS_WAIT_MS) {
            proto_err_t e = proto_recv_frame(fd, &type, &seq, s_iobuf, EVA_ROUTE_JSON_MAX, &len);
            if (e == PROTO_ERR_TIMEOUT) continue;
            if (e != PROTO_OK) {
                if (e == PROTO_ERR_CRC) STAT_INC(crc_errors);
                ESP_LOGW(TAG, "session broken while waiting gps, err=%d", e);
                return false;
            }
            if (type == EVA_T_GPS_RSP) {
                if (handle_gps(s_iobuf, len) == ESP_OK) {
                    STAT_INC(gps_rsp_events);
                    return true;
                }
                continue;
            }
            if (!handle_frame(fd, type, seq, s_iobuf, len)) return false;
        }
        ESP_LOGW(TAG, "gps req timeout, attempt %d", attempt + 1);
    }
    return true;                 /* 超时不算断链：保留连接，等下一个窗口 */
}

static void session_run(int fd)
{
    uint8_t  type = 0;
    uint32_t seq = 0, len = 0;
    uint32_t last_active = (uint32_t)(xTaskGetTickCount() * 1000 / configTICK_RATE_HZ);

    struct timeval tv = { .tv_sec = 0, .tv_usec = 500 * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#ifdef TCP_NODELAY
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#endif

    ESP_LOGI(TAG, "session start");
    emit(NET_EV_PEER_ONLINE);

    while (true) {
        uint32_t now = (uint32_t)(xTaskGetTickCount() * 1000 / configTICK_RATE_HZ);

        if (atomic_exchange(&s_gps_pending, false)) {
            if (!session_fetch_gps(fd)) break;
            last_active = now;
        }

        proto_err_t e = proto_recv_frame(fd, &type, &seq, s_iobuf, EVA_ROUTE_JSON_MAX, &len);
        if (e == PROTO_ERR_TIMEOUT) {
            if (now - last_active > EVA_SESSION_IDLE_MS) {
                ESP_LOGI(TAG, "session idle close");
                break;
            }
            continue;
        }
        if (e != PROTO_OK) {
            if (e == PROTO_ERR_CRC) STAT_INC(crc_errors);
            ESP_LOGW(TAG, "session err=%d", e);
            break;
        }
        last_active = now;
        if (!handle_frame(fd, type, seq, s_iobuf, len)) break;
    }

    close(fd);
    emit(NET_EV_PEER_OFFLINE);
    ESP_LOGI(TAG, "session end");
}

/* ---------------- 任务 ---------------- */
static void net_task(void *arg)
{
    while (true) {
        /* 射频开关只有本任务执行（单写者，避免别的任务 close 正在 accept 的 fd） */
        bool want = atomic_load(&s_radio_wanted);
        if (want != s_radio_on) {
            if (!want) {
                if (s_listen_fd >= 0) { close(s_listen_fd); s_listen_fd = -1; }
                esp_wifi_stop();
                s_radio_on = false;
                ESP_LOGI(TAG, "radio OFF");
            } else {
                esp_wifi_start();
                s_radio_on = true;
                ESP_LOGI(TAG, "radio ON");
            }
        }
        if (!s_radio_on) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }

        if (s_listen_fd < 0) {
            s_listen_fd = listen_open();
            if (s_listen_fd < 0) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
        }
        int fd = accept(s_listen_fd, NULL, NULL);
        if (fd < 0) continue;              /* SO_RCVTIMEO 到期（EAGAIN）属正常轮询 */
        session_run(fd);
    }
}

static void sync_task(void *arg)
{
    TickType_t last = xTaskGetTickCount();

    while (true) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(EVA_SYNC_PERIOD_MS));
        STAT_INC(sync_windows);

#if EVA_PS_RADIO_OFF_MODE
        atomic_store(&s_radio_wanted, true);
#endif
        /* 记录本轮开始时的 seq，用来判断"这一轮是否拿到了新定位" */
        {
            gps_fix_t f = {0};
            uint32_t seq0 = 0;
            net_gps_snapshot(&f, NULL);
            seq0 = f.seq;
            atomic_store(&s_gps_pending, true);

            uint32_t t0 = (uint32_t)(xTaskGetTickCount() * 1000 / configTICK_RATE_HZ);
            bool ok = false;
            while ((uint32_t)(xTaskGetTickCount() * 1000 / configTICK_RATE_HZ) - t0
                   < EVA_SYNC_WINDOW_MS) {
                gps_fix_t nf = {0};
                if (net_gps_snapshot(&nf, NULL) && nf.seq > seq0) { ok = true; break; }
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            if (ok) STAT_INC(sync_gps_ok);
            else {
                STAT_INC(sync_gps_timeout);
                ESP_LOGW(TAG, "sync window missed (peer_online=%d)", (int)s_peer_online);
            }
        }
#if EVA_PS_RADIO_OFF_MODE
        atomic_store(&s_radio_wanted, false);
#endif
    }
}
#endif /* !EVA_LINK_BLE */

esp_err_t net_link_start(net_event_cb_t cb, void *arg)
{
    s_cb = cb;
    s_cb_arg = arg;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;


#if EVA_LINK_BLE
    /* BLE 主传输：不起 Wi-Fi、不起 TCP 任务、不占 256KB PSRAM 收包缓冲（ble_link 自带）。
     * 注意 net_link_start 仍然必须被调用 —— 它建立的是"帧处理与事件分发"这套与传输无关的基础设施。 */
    ESP_LOGI(TAG, "BLE transport selected: Wi-Fi SoftAP + TCP disabled");
    return ESP_OK;
#else
    s_iobuf = heap_caps_malloc(EVA_ROUTE_JSON_MAX, MALLOC_CAP_SPIRAM);
    if (!s_iobuf) {
        ESP_LOGW(TAG, "PSRAM 不可用，回退内部 64KB（大路线会失败，属降级）");
        s_iobuf = heap_caps_malloc(64 * 1024, MALLOC_CAP_INTERNAL);
        if (!s_iobuf) return ESP_ERR_NO_MEM;
    }
    wifi_start_ap_once();
    xTaskCreatePinnedToCore(net_task,  "net",  6144, NULL, 4, NULL, 0);
    xTaskCreatePinnedToCore(sync_task, "sync", 3072, NULL, 3, NULL, 1);
    return ESP_OK;
#endif
}

esp_err_t net_inject_route_json(const char *json, uint32_t len)
{
    uint16_t npts = 0;
    int32_t d = 0;
    esp_err_t e = route_store_ingest_json(json, len, &npts, &d);
    if (e == ESP_OK && route_store_load()) emit(NET_EV_ROUTE_UPDATED);
    return e;
}
