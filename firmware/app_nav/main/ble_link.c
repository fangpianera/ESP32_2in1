/**
 * ble_link.c — NimBLE GATT 服务器：手机 → 板（路线/健康/GPS 回传），板 → 手机（ACK/通知）
 *
 * 数据面约定（与 proto.h 同一套帧，20B 头 + payload + CRC32）：
 *   - 手机把整帧按 MTU 切片，用 write / write-no-rsp 逐片写入 RX 特征；
 *     板端按帧头里的 len 字段重组，凑齐才分发，绝不"边收边解析"。
 *   - 板端回包走 TX 特征 notify，同样按 MTU 切片（ACK 只有百来字节，一片即可）。
 *
 * 关键坑（都按实测踩过）：
 *   1) 默认 MTU 23 → 每包只有 20 字节，路线 JSON 几十 KB 会传很久；
 *      必须让手机主动 requestMtu(517)，板端 CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU 也要跟上。
 *   2) notify 的 mbuf 池很小，连续 notify 会返回 BLE_HS_ENOMEM；
 *      所以每片之间让出 CPU（vTaskDelay），并且限次重试，不能死等。
 *   3) NimBLE 的 GATT 回调跑在 host 任务里，绝不能在里面做 cJSON 解析 + Flash 写；
 *      这里只做"重组 + 投递到队列"，重活交给 net_work 任务。
 */
#include "ble_link.h"
#include "app_cfg.h"
#include "net_link.h"
#include "proto.h"

#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ble";

/* ---------------- 状态 ---------------- */
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_tx_val = 0;                 /* TX 特征句柄，notify 必须用它 */
static uint8_t  s_own_addr_type = 0;

/* 环3 倒计时用：上一次发出 GPS_REQ（或刚连接成功）的时刻，单位 ms */
static volatile uint32_t s_last_sync_ms = 0;

/* 帧重组缓冲（PSRAM，路线 JSON 上限 256KB + 20B 头） */
static uint8_t *s_rx = NULL;
static uint32_t s_rx_len = 0;
static uint32_t s_rx_expect = 0;              /* 0 = 还不知道本帧总长 */

typedef struct {
    uint8_t  type;
    uint32_t seq;
    uint32_t len;
    uint8_t *buf;                             /* 指向 s_rx 的 payload 区（不拷贝） */
} work_item_t;

static QueueHandle_t s_q = NULL;

/* 用静态 UUID128，避免栈上临时对象生命周期问题 */
static const ble_uuid128_t SVC_UUID =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);
static const ble_uuid128_t RX_UUID =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);
static const ble_uuid128_t TX_UUID =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

bool ble_link_is_connected(void)
{
    return s_conn != BLE_HS_CONN_HANDLE_NONE;
}

int ble_link_calib_countdown_s(void)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) {
        return -1;
    }
    uint32_t now = (uint32_t)esp_timer_get_time() / 1000u;
    uint32_t period = (uint32_t)(EVA_SYNC_PERIOD_MS);
    uint32_t el = now - s_last_sync_ms;
    if (el >= period) {
        return 0;
    }
    return (int)((period - el) / 1000u);
}

/* ---------------- 发送（notify 切片） ---------------- */

static esp_err_t ble_reply(uint8_t type, uint32_t seq, const char *json, void *arg)
{
    (void)arg;
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || s_tx_val == 0) {
        ESP_LOGW(TAG, "reply type=%u but no client", type);
        return ESP_FAIL;
    }

    uint32_t plen = (uint32_t)strlen(json);
    uint32_t total = sizeof(eva_hdr_t) + plen;
    uint8_t *frame = heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!frame) {
        frame = malloc(total);
        if (!frame) return ESP_ERR_NO_MEM;
    }
    uint32_t n = proto_build_frame(frame, total, type, seq, json, plen);
    if (n == 0) { free(frame); return ESP_ERR_INVALID_ARG; }

    uint16_t mtu = ble_att_mtu(s_conn);
    uint16_t chunk = (mtu > 4) ? (uint16_t)(mtu - 3) : 20;   /* notify 占 3 字节属性头 */

    uint32_t off = 0;
    while (off < n) {
        uint16_t sub = (n - off > chunk) ? chunk : (uint16_t)(n - off);
        struct os_mbuf *om = ble_hs_mbuf_from_flat(frame + off, sub);
        if (!om) {
            vTaskDelay(pdMS_TO_TICKS(10));      /* mbuf 池紧张，让出 CPU 再试 */
            continue;
        }
        int rc = ble_gatts_notify_custom(s_conn, s_tx_val, om);
        if (rc != 0) {
            ESP_LOGW(TAG, "notify rc=%d off=%u/%u", rc, (unsigned)off, (unsigned)n);
            free(frame);
            return ESP_FAIL;
        }
        off += sub;
        vTaskDelay(pdMS_TO_TICKS(3));           /* 手机侧蓝牙栈消化不过来会丢，慢一点更稳 */
    }
    free(frame);
    ESP_LOGI(TAG, "notified type=%u len=%u mtu=%u", type, (unsigned)n, mtu);
    return ESP_OK;
}

/* ---------------- 接收：帧重组 ---------------- */

/* 把一段写入的数据拼到 s_rx；凑够一帧就投递到工作队列（GATT 回调里不做重活） */
static bool rx_feed(const uint8_t *data, uint32_t len)
{
    if (s_rx_len + len > EVA_ROUTE_JSON_MAX + sizeof(eva_hdr_t)) {
        ESP_LOGE(TAG, "rx overflow: %u + %u", (unsigned)s_rx_len, (unsigned)len);
        s_rx_len = 0;
        s_rx_expect = 0;
        return false;
    }
    memcpy(s_rx + s_rx_len, data, len);
    s_rx_len += len;

    while (true) {
        if (s_rx_expect == 0) {
            if (s_rx_len < sizeof(eva_hdr_t)) return true;
            eva_hdr_t h;
            memcpy(&h, s_rx, sizeof(h));
            if (h.magic != EVA_MAGIC || h.ver != EVA_PROTO_VER) {
                ESP_LOGW(TAG, "bad hdr magic=0x%08x ver=%u (drop %u B)",
                         (unsigned)h.magic, h.ver, (unsigned)s_rx_len);
                s_rx_len = 0;
                return false;
            }
            if (h.len > EVA_ROUTE_JSON_MAX) {
                ESP_LOGE(TAG, "payload %u > cap", (unsigned)h.len);
                s_rx_len = 0;
                return false;
            }
            s_rx_expect = sizeof(eva_hdr_t) + h.len;
            ESP_LOGI(TAG, "frame start type=%u seq=%u len=%u", h.type, (unsigned)h.seq, (unsigned)h.len);
        }
        if (s_rx_len < s_rx_expect) return true;

        eva_hdr_t h;
        memcpy(&h, s_rx, sizeof(h));
        uint8_t *payload = s_rx + sizeof(h);
        if (h.len && eva_crc32(payload, h.len) != h.crc32) {
            ESP_LOGW(TAG, "crc mismatch type=%u len=%u", h.type, (unsigned)h.len);
            s_rx_len = 0;
            s_rx_expect = 0;
            return false;
        }

        work_item_t it = { .type = h.type, .seq = h.seq, .len = h.len, .buf = NULL };
        /* 必须在 GATT 回调里就把 payload 拷走：s_rx 下一次写入即被复用，
         * 而解析/落盘在工作线程里做（NimBLE host 任务里做 Flash 写会堵塞协议栈）。 */
        uint8_t *copy = heap_caps_malloc(h.len ? h.len : 1, MALLOC_CAP_SPIRAM);
        if (!copy) copy = malloc(h.len ? h.len : 1);
        if (!copy) {
            ESP_LOGE(TAG, "oom frame type=%u len=%u", h.type, (unsigned)h.len);
            s_rx_len = 0;
            s_rx_expect = 0;
            return false;
        }
        if (h.len) memcpy(copy, payload, h.len);
        it.buf = copy;

        if (xQueueSend(s_q, &it, 0) != pdTRUE) {
            ESP_LOGW(TAG, "queue full, drop type=%u", it.type);
            free(copy);
            s_rx_len = 0;
            s_rx_expect = 0;
            return false;
        }
        s_rx_len = 0;
        s_rx_expect = 0;
        return true;
    }
}

static int rx_access(uint16_t conn_handle, uint16_t attr_handle,
                     struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;

    uint16_t n = OS_MBUF_PKTLEN(ctxt->om);
    uint8_t *tmp = malloc(n);
    if (!tmp) return BLE_ATT_ERR_INSUFFICIENT_RES;
    os_mbuf_copydata(ctxt->om, 0, n, tmp);
    bool ok = rx_feed(tmp, n);
    free(tmp);
    return ok ? 0 : BLE_ATT_ERR_VALUE_NOT_ALLOWED;
}

static int tx_access(uint16_t conn_handle, uint16_t attr_handle,
                     struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)ctxt; (void)arg;
    return BLE_ATT_ERR_READ_NOT_PERMITTED;   /* TX 只 notify，不允许读 */
}

/* ---------------- GATT / GAP ---------------- */

static const struct ble_gatt_svc_def GATT_SVCS[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &SVC_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &RX_UUID.u,
                .access_cb = rx_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &TX_UUID.u,
                .access_cb = tx_access,
                .flags = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_tx_val,
            },
            { 0 }
        },
    },
    { 0 }
};

static void start_advertise(void);

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    (void)arg;
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            s_conn = ev->connect.conn_handle;
            ESP_LOGI(TAG, "client connected handle=%d", s_conn);
            /* 连接成功即重置 30s 倒计时（环3 从满格开始走） */
            s_last_sync_ms = (uint32_t)esp_timer_get_time() / 1000u;
        } else {
            start_advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "client disconnected reason=0x%x", ev->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_rx_len = 0;
        s_rx_expect = 0;
        start_advertise();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        start_advertise();
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "mtu updated to %d", ev->mtu.value);
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "subscribe attr=%u notify=%d", ev->subscribe.attr_handle,
                 ev->subscribe.cur_notify);
        return 0;

    default:
        return 0;
    }
}

static void start_advertise(void)
{
    struct ble_hs_adv_fields f = {0};
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    char name[32];
    /* 名字缩成 "EVA-01A5"（8 字符）：
     *   广播包 31 字节 = flags(3) + 128bit UUID(18) + 名字(2+8=10) = 31，刚好装满。
     * 手机端按 serviceUuid 过滤扫描（鸿蒙的 ScanFilter 不接受空数组，必须给条件），
     * UUID 留在广播包里才能被过滤命中；完整名字 "EVA-NAV-01A5" 放不下会被 rc=4 拒掉。 */
    snprintf(name, sizeof(name), "EVA-%02X%02X",
             (unsigned)(0x01), (unsigned)(0xA5));

    f.name = (uint8_t *)name;
    f.name_len = (uint8_t)strlen(name);
    f.name_is_complete = 1;
    f.uuids128 = (ble_uuid128_t *)&SVC_UUID;
    f.num_uuids128 = 1;
    f.uuids128_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&f);
    if (rc != 0) { ESP_LOGE(TAG, "adv fields rc=%d", rc); return; }

    struct ble_gap_adv_params p = {0};
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    p.itvl_min = 0x20;                            /* 20ms，连接建立快 */
    p.itvl_max = 0x30;

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "adv start rc=%d", rc);
    } else {
        ESP_LOGI(TAG, "advertising as %s", name);
    }
}

static void on_sync(void)
{
    if (ble_hs_id_infer_auto(0, &s_own_addr_type) != 0) {
        ESP_LOGE(TAG, "id infer failed");
        return;
    }
    start_advertise();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "host reset reason=%d", reason);
    s_conn = BLE_HS_CONN_HANDLE_NONE;
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();                 /* 正常不会返回 */
    nimble_port_freertos_deinit();
}

/* ---------------- 工作线程：真正的解析/落盘在这里 ---------------- */

static void work_task(void *arg)
{
    (void)arg;
    work_item_t it;
    while (true) {
        if (xQueueReceive(s_q, &it, portMAX_DELAY) != pdTRUE) continue;

        esp_err_t r = net_handle_frame(it.type, it.seq, it.buf, it.len, ble_reply, NULL);
        ESP_LOGI(TAG, "frame type=%u len=%u handled=%d", it.type, (unsigned)it.len, (int)r);
        free(it.buf);
    }
}

/* ---------------- GPS 定期回传（30s 窗口，板端主动问） ---------------- */

static void gps_task(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    while (true) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(EVA_SYNC_PERIOD_MS));
        if (!ble_link_is_connected()) continue;

        gps_fix_t f0 = {0};
        net_gps_snapshot(&f0, NULL);
        uint32_t seq0 = f0.seq;

        ble_reply(EVA_T_GPS_REQ, proto_next_seq(), "{\"want\":\"gps\"}", NULL);
        s_last_sync_ms = (uint32_t)esp_timer_get_time() / 1000u;

        uint32_t t0 = (uint32_t)(xTaskGetTickCount() * 1000 / configTICK_RATE_HZ);
        bool ok = false;
        while ((uint32_t)(xTaskGetTickCount() * 1000 / configTICK_RATE_HZ) - t0 < EVA_GPS_WAIT_MS) {
            gps_fix_t nf = {0};
            if (net_gps_snapshot(&nf, NULL) && nf.seq > seq0) { ok = true; break; }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (!ok) ESP_LOGW(TAG, "gps window missed (phone not responding)");
    }
}

/* ---------------- 初始化 ---------------- */

esp_err_t ble_link_start(void)
{
    s_rx = heap_caps_malloc(EVA_ROUTE_JSON_MAX + sizeof(eva_hdr_t), MALLOC_CAP_SPIRAM);
    if (!s_rx) {
        ESP_LOGW(TAG, "PSRAM 不可用，回退内部 96KB（超长路线会失败，属降级）");
        s_rx = malloc(96 * 1024);
        if (!s_rx) return ESP_ERR_NO_MEM;
    }
    s_q = xQueueCreate(6, sizeof(work_item_t));
    if (!s_q) return ESP_ERR_NO_MEM;

    nimble_port_init();
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    /* 不配对、不加密：协议层自带 magic + CRC（与 Wi-Fi 开放热点的风险面一致） */
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set("EVA-NAV");

    int rc = ble_gatts_count_cfg(GATT_SVCS);
    if (rc != 0) { ESP_LOGE(TAG, "count cfg rc=%d", rc); return ESP_FAIL; }
    rc = ble_gatts_add_svcs(GATT_SVCS);
    if (rc != 0) { ESP_LOGE(TAG, "add svcs rc=%d", rc); return ESP_FAIL; }

    xTaskCreatePinnedToCore(work_task, "ble_work", 6144, NULL, 4, NULL, 0);
    xTaskCreatePinnedToCore(gps_task,  "ble_gps",  3072, NULL, 3, NULL, 1);
    nimble_port_freertos_init(host_task);

    ESP_LOGI(TAG, "BLE link started (svc %s)", EVA_BLE_SVC_UUID_STR);
    return ESP_OK;
}
