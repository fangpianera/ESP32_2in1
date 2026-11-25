#include "f1_api.h"

#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "esp_tls.h"
#include <time.h>

#define F1_API_HOST "api.jolpi.ca"
#define F1_API_IP   "104.21.47.117"   /* 只给 TLS 直连自检用，正常取数仍走域名 */
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_crt_bundle.h"
/* 暂时保留该头文件：将来 SNTP 稳定、要恢复证书校验时直接用回。 */
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "lwip/ip4_addr.h"
#include "lwip/dns.h"          /* dns_setserver：直接改 lwIP 的 DNS 表 */
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

static const char *TAG = "f1_api";

/* cJSON builds a node per JSON token: parsing a 20 KB standings payload costs
 * tens of KB of heap. If that comes from internal RAM the LCD's SPI DMA pool
 * is starved and LVGL starts failing every flush, so put JSON nodes in PSRAM. */
static void *json_malloc(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(size);
}

static void json_free(void *p)
{
    heap_caps_free(p);
}

static void json_hooks_init(void)
{
    static bool done = false;
    if (done) return;
    done = true;
    cJSON_Hooks hooks = {.malloc_fn = json_malloc, .free_fn = json_free};
    cJSON_InitHooks(&hooks);
}

#define WIFI_SSID_MAX   32
#define WIFI_PASS_MAX   64
#define NVS_NAMESPACE   "f1dash"
#define NVS_KEY_SSID    "wifi_ssid"
#define NVS_KEY_PASS    "wifi_pass"

#define WIFI_CONNECTED_BIT  BIT0

static EventGroupHandle_t s_wifi_events;
static volatile bool s_connected = false;
static esp_netif_t *s_sta_netif;

/* ---- 屏幕诊断行 ----
 * 串口在 USB-JTAG 上既会打断板子、重启后句柄又失效，没法稳定观察。
 * 这里维护一条 80 字的最简状态，UI 每秒画到屏幕上，肉眼就能看到卡在哪一步。 */
static char s_diag[80] = "booting";
static int  s_diag_seq;

/* 前面再带一个自增序号：#12 open ... —— 肉眼看一眼就知道有没有在重试、
 * 是不是卡死在同一句上，不用一直盯着等。 */
static void diag_set(const char *fmt, ...)
{
    char body[62];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    snprintf(s_diag, sizeof(s_diag), "#%d %s", ++s_diag_seq, body);
}

const char *f1_diag_line(void)
{
    return s_diag;
}

void f1_diag_note(const char *msg)
{
    diag_set("%s", msg);
}

/* 域名解析自检。结果单独存一份（不写 s_diag），这样每条回传都能带上它，
 * 不会被后面 "fetching" 之类的提示覆盖掉。 */
static char s_dns_stat[32] = "?";
static char s_tls_stat[40] = "?";

void f1_dns_probe(void)
{
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo("api.jolpi.ca", "443", &hints, &res);
    if (rc == 0 && res != NULL) {
        char ip[20] = "?";
        struct sockaddr_in *sa = (struct sockaddr_in *)res->ai_addr;
        uint32_t a = ntohl(sa->sin_addr.s_addr);
        snprintf(ip, sizeof(ip), "%u.%u.%u.%u",
                 (unsigned)((a >> 24) & 0xFF), (unsigned)((a >> 16) & 0xFF),
                 (unsigned)((a >> 8) & 0xFF), (unsigned)(a & 0xFF));
        snprintf(s_dns_stat, sizeof(s_dns_stat), "ok %s", ip);
        freeaddrinfo(res);
    } else {
        snprintf(s_dns_stat, sizeof(s_dns_stat), "FAIL rc=%d", rc);
    }
    diag_set("dns %s", s_dns_stat);
}

/* TLS 直连自检：用 IP 建连接，SNI 用域名（和 PC 上 curl --resolve 完全等价的路径），
 * 把 esp_tls 的真实错误码挖出来。esp_http_client 会把所有连接失败都压成 0x7002，
 * 看不出到底是 TCP 不通、握手失败还是别的原因。 */
void f1_tls_probe(void)
{
    esp_tls_cfg_t cfg = {0};
    cfg.timeout_ms = 12000;
    cfg.common_name = F1_API_HOST;       /* SNI + 校验名（不校验证书，只用来发 SNI） */
    esp_tls_t *tls = esp_tls_init();
    if (!tls) {
        snprintf(s_tls_stat, sizeof(s_tls_stat), "no mem");
        diag_set("tls %s", s_tls_stat);
        return;
    }
    esp_tls_error_handle_t h = NULL;
    esp_tls_get_error_handle(tls, &h);
    /* TCP 已验证能连上（tls=tcp ok），所以这里只做完整握手，把 mbedtls 的真实错误码挖出来：
     * esp_http_client 会把所有连接失败压成 0x7002，看不出是握手被拒、buffer 不够还是别的。 */
    int r = esp_tls_conn_new_sync(F1_API_IP, (int)strlen(F1_API_IP), 443, &cfg, tls);
    if (r == 0) {
        snprintf(s_tls_stat, sizeof(s_tls_stat), "hand ok");
    } else {
        int mbed = 0, espc = 0, sys = 0;
        if (h) {
            esp_tls_get_and_clear_error_type(h, ESP_TLS_ERR_TYPE_MBEDTLS, &mbed);
            esp_tls_get_and_clear_error_type(h, ESP_TLS_ERR_TYPE_ESP, &espc);
            esp_tls_get_and_clear_error_type(h, ESP_TLS_ERR_TYPE_SYSTEM, &sys);
        }
        snprintf(s_tls_stat, sizeof(s_tls_stat), "hb %d m%d e%d s%d", r, mbed, espc, sys);
    }
    esp_tls_conn_destroy(tls);
    diag_set("tls %s", s_tls_stat);
}

/* ---- 局域网回报 ----
 * 串口会打断板子、拍照又看不清，干脆让板子把当前诊断直接 POST 回开发机，
 * 明文 HTTP 到 192.168.3.189:8080，不涉及 TLS，几乎不会失败。 */
#define F1_DIAG_HOST "192.168.3.189"
#define F1_DIAG_PORT 8080

void f1_report(const char *tag)
{
    if (!s_connected) return;
    char body[256];
    struct tm tm_now;
    time_t t_now = time(NULL);
    localtime_r(&t_now, &tm_now);
    snprintf(body, sizeof(body), "%s | %s | dns=%s | tls=%s | t=%02d:%02d | int=%uK hp=%uK ps=%uK",
             tag, s_diag, s_dns_stat, s_tls_stat,
             tm_now.tm_hour, tm_now.tm_min,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) >> 10),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >> 10));

    char url[64];
    snprintf(url, sizeof(url), "http://" F1_DIAG_HOST ":%d/r", F1_DIAG_PORT);
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 3000,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) return;
    esp_http_client_set_method(c, HTTP_METHOD_POST);
    esp_http_client_set_header(c, "Content-Type", "text/plain");
    esp_http_client_set_post_field(c, body, (int)strlen(body));
    esp_http_client_perform(c);
    esp_http_client_cleanup(c);
}

/* ---- 配网 / 重连状态 ----
 * 旧逻辑在 WIFI_EVENT_STA_DISCONNECTED 里直接 esp_wifi_connect()，每秒重试一次。
 * 实测踩到的坑：有旧凭据但目标 AP 不在场时，驱动内部会自己每秒 scan 一次并打
 * "Haven't to connect to a suitable AP now!"，而且【不会】再发 DISCONNECTED 事件，
 * 所以"数失败次数"永远涨不上去、配网永远进不来（板上 t=157~186 s 仍在每秒刷）。
 * 现在改成纯时间判定：每次尝试记起点，超过 WIFI_CONNECT_TIMEOUT_MS 就
 * esp_wifi_disconnect() 掐断驱动内部重试，然后开配网热点。 */
static volatile bool s_portal_up = false;
static volatile TickType_t s_attempt_since = 0;   /* 0 = 当前没有进行中的尝试 */
static httpd_handle_t s_httpd = NULL;
static bool s_ap_netif_created = false;

#define WIFI_CONNECT_TIMEOUT_MS  25000   /* 单轮尝试上限；超时即掐断并换下一个 AP */

/* ---- 固定内嵌 AP 列表（配网已关闭）----
 * 凭据不进仓库：真实值放在同目录 secrets.h（本地文件，.gitignore 已忽略），
 * 仓库里只有 secrets.example.h 的占位模板；没有 secrets.h 时自动退回模板。
 * 顺序即尝试顺序，一轮 25 s 超时后轮到下一组，全部失败再从头循环。 */
#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"
#endif

typedef struct { const char *ssid; const char *pass; } f1_builtin_ap_t;
static const f1_builtin_ap_t s_builtin_aps[] = { F1_WIFI_AP_LIST };
#define F1_AP_COUNT ((int)(sizeof(s_builtin_aps) / sizeof(s_builtin_aps[0])))
static int s_ap_idx = 0;

#define WIFI_PORTAL_SSID         "F1-DASH"

/* ---- NVS credentials ---- */
static bool load_credentials(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
    size_t s = ssid_len, p = pass_len;
    esp_err_t e1 = nvs_get_str(h, NVS_KEY_SSID, ssid, &s);
    esp_err_t e2 = nvs_get_str(h, NVS_KEY_PASS, pass, &p);
    nvs_close(h);
    return (e1 == ESP_OK && e2 == ESP_OK && s > 1);
}

static void save_credentials(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, NVS_KEY_SSID, ssid);
    nvs_set_str(h, NVS_KEY_PASS, pass);
    nvs_commit(h);
    nvs_close(h);
}

/* ---- WiFi event handler ---- */
static void wifi_portal_down(void);
static void wifi_begin_attempt(void);
static void wifi_apply_ap(int idx);

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        /* 不再在这里 esp_wifi_connect()：重连与超时判定都由 supervisor 负责 */
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_connected = true;
        s_attempt_since = 0;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "WiFi connected, got IP");
        {
            wifi_ap_record_t ap;
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                diag_set("IP ok \"%s\" %ddBm", ap.ssid, ap.rssi);
            } else {
                diag_set("IP ok");
            }
        }
        /* 取数已改成固定 IP 直连（见 fetch_url），DNS 完全不参与，这里不再改 DNS。
         * 之前在这里强行写入 223.5.5.5/119.29.29.29 的版本实测会让整机失去响应
         * （ping 都不通、一行回传也没有），所以这段彻底去掉。 */
        /* 收 AP 的动作（set_mode / 重连）放 supervisor 做：
         * 在事件回调里切模式容易引出新的 DISCONNECTED 事件递归 */
    }
}

#if 0   /* ---- 配网（softAP + HTTP 表单）已关闭，保留代码备查 ---- */
/* ---- Captive portal (soft AP + HTTP form) ---- */
static const char *PORTAL_HTML =
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>F1 Dash WiFi</title><style>"
    "body{font-family:sans-serif;background:#111;color:#eee;padding:24px}"
    "input,button{font-size:16px;padding:10px;margin:6px 0;width:100%;box-sizing:border-box}"
    "button{background:#e10600;color:#fff;border:0;border-radius:6px}"
    "</style></head><body><h2>F1 Dashboard WiFi Setup</h2>"
    "<form method='post' action='/save'>"
    "WiFi SSID:<input name='ssid' required>"
    "Password:<input name='pass' type='password'>"
    "<button type='submit'>Connect & Save</button></form></body></html>";

static esp_err_t portal_root_handler(httpd_req_t *req)
{
    httpd_resp_send(req, PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* 取 form 字段并做 URL 解码：SSID 里常有空格 / 中文，
 * 直接 memcpy 会把它存成 "My%20Home" 或 "%E5%AE%B6"，之后永远连不上。 */
static void form_field(const char *buf, const char *key, char *out, size_t out_len)
{
    out[0] = 0;
    const char *k = strstr(buf, key);
    if (k == NULL) return;
    k += strlen(key);
    const char *e = strchr(k, '&');
    size_t n = e ? (size_t)(e - k) : strlen(k);
    if (n >= out_len) n = out_len - 1;

    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < out_len; i++) {
        if (k[i] == '+') {
            out[o++] = ' ';
        } else if (k[i] == '%' && i + 2 < n) {
            char c1 = k[i + 1], c2 = k[i + 2];
            int hi = (c1 >= '0' && c1 <= '9') ? c1 - '0' : ((c1 | 0x20) - 'a' + 10);
            int lo = (c2 >= '0' && c2 <= '9') ? c2 - '0' : ((c2 | 0x20) - 'a' + 10);
            if (hi >= 0 && hi < 16 && lo >= 0 && lo < 16) {
                out[o++] = (char)(hi * 16 + lo);
                i += 2;
            } else {
                out[o++] = k[i];
            }
        } else {
            out[o++] = k[i];
        }
    }
    out[o] = 0;
}

/* 配网保存后不重启：直接把新凭据喂给 STA。
 * 不走 esp_restart()，因为双程序模式下重启会跳回选择器，配网流程被打断。 */
static void apply_creds_task(void *arg)
{
    (void)arg;
    char ssid[WIFI_SSID_MAX] = {0}, pass[WIFI_PASS_MAX] = {0};
    if (load_credentials(ssid, sizeof(ssid), pass, sizeof(pass))) {
        wifi_config_t sta = {0};
        snprintf((char *)sta.sta.ssid, sizeof(sta.sta.ssid), "%s", ssid);
        snprintf((char *)sta.sta.password, sizeof(sta.sta.password), "%s", pass);
        esp_wifi_set_config(WIFI_IF_STA, &sta);
        /* 先 disconnect 掐断上一轮驱动内部每秒 scan 的重试，再发起新的一轮 */
        esp_wifi_disconnect();
        esp_wifi_connect();
        s_attempt_since = xTaskGetTickCount();
        ESP_LOGI(TAG, "Portal: saved \"%s\", connecting", ssid);
    }
    vTaskDelete(NULL);
}

static esp_err_t portal_save_handler(httpd_req_t *req)
{
    char buf[512];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) { httpd_resp_send_500(req); return ESP_FAIL; }
    buf[len] = 0;

    char ssid[WIFI_SSID_MAX] = {0}, pass[WIFI_PASS_MAX] = {0};
    form_field(buf, "ssid=", ssid, sizeof(ssid));
    form_field(buf, "pass=", pass, sizeof(pass));

    if (ssid[0]) {
        save_credentials(ssid, pass);
        char resp[320];
        snprintf(resp, sizeof(resp),
            "<!doctype html><meta name='viewport' content='width=device-width'>"
            "<body style='background:#111;color:#eee;font-family:sans-serif;padding:24px'>"
            "<h2>Saved</h2><p>Connecting to <b>%s</b>...</p>"
            "<p>Once connected, the F1-DASH hotspot closes automatically.</p></body>",
            ssid);
        httpd_resp_sendstr(req, resp);
        xTaskCreate(apply_creds_task, "f1_creds", 3072, NULL, 5, NULL);
    } else {
        httpd_resp_sendstr(req, "<h2>SSID empty</h2><a href='/'>back</a>");
    }
    return ESP_OK;
}

/* 起配网 AP + 网页表单。幂等：已在配网态直接返回，避免重复 httpd_start / 重复建 netif。 */
static void start_portal(void)
{
    if (s_portal_up) return;
    s_portal_up = true;

    if (!s_ap_netif_created) {
        esp_netif_create_default_wifi_ap();
        s_ap_netif_created = true;
    }

    wifi_config_t ap = {0};
    strcpy((char *)ap.ap.ssid, WIFI_PORTAL_SSID);
    ap.ap.ssid_len = strlen(WIFI_PORTAL_SSID);
    ap.ap.channel = 6;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap);

    /* 已经 start 过的驱动会返回 ESP_ERR_WIFI_STATE，这是正常情况，不能 ESP_ERROR_CHECK */
    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_STATE) {
        ESP_LOGE(TAG, "esp_wifi_start 失败: %s", esp_err_to_name(err));
    }

    if (s_httpd == NULL) {
        httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
        cfg.lru_purge_enable = true;
        if (httpd_start(&s_httpd, &cfg) == ESP_OK) {
            httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = portal_root_handler };
            httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = portal_save_handler };
            httpd_register_uri_handler(s_httpd, &root);
            httpd_register_uri_handler(s_httpd, &save);
        } else {
            s_httpd = NULL;
            ESP_LOGE(TAG, "httpd_start 失败，配网页不可用");
        }
    }

    ESP_LOGW(TAG, "配网热点已开: SSID=%s  http://192.168.4.1", WIFI_PORTAL_SSID);
}
#endif  /* ---- 配网已关闭 ---- */

/* 连上以后收掉 AP：电池供电时 AP 常开会持续耗电 */
static void wifi_portal_down(void)
{
    if (s_httpd != NULL) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    if (s_portal_up) {
        s_portal_up = false;
        esp_wifi_set_mode(WIFI_MODE_STA);
        wifi_begin_attempt();    /* 切模式会断开，重新发起一轮 */
        ESP_LOGI(TAG, "配网热点已关闭（STA 已连上）");
    }
}

/* 单轮连接尝试：先 disconnect 掐断驱动内部每秒 scan 的旧重试，再发起并记录起点 */
static void wifi_begin_attempt(void)
{
    esp_wifi_disconnect();
    esp_wifi_connect();
    s_attempt_since = xTaskGetTickCount();
}

/* 切到第 idx 个内嵌 AP（取模轮换）并立刻发起连接。
 * 这是关掉配网后唯一的连接入口。 */
static void wifi_apply_ap(int idx)
{
    s_ap_idx = ((idx % F1_AP_COUNT) + F1_AP_COUNT) % F1_AP_COUNT;
    wifi_config_t sta = {0};
    snprintf((char *)sta.sta.ssid, sizeof(sta.sta.ssid), "%s", s_builtin_aps[s_ap_idx].ssid);
    snprintf((char *)sta.sta.password, sizeof(sta.sta.password), "%s", s_builtin_aps[s_ap_idx].pass);
    /* 默认 WIFI_FAST_SCAN 只盯上一个 AP 的信道，轮换到另一组凭据时经常扫不到
     * （日志里备用 AP 45 秒全程 "Haven't to connect"）。改成全信道扫描 + 按信号排序。 */
    sta.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    sta.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    sta.sta.threshold.rssi = -85;      /* 低于 -85 的弱信号宁可不连，免得连上就掉 */
    esp_wifi_set_config(WIFI_IF_STA, &sta);
    wifi_begin_attempt();
    diag_set("WIFI try %s", s_builtin_aps[s_ap_idx].ssid);
    ESP_LOGI(TAG, "WiFi: 尝试内嵌 AP %d/%d \"%s\"", s_ap_idx + 1, F1_AP_COUNT,
             s_builtin_aps[s_ap_idx].ssid);
}

/* 唯一的重连仲裁者：
 *  - 已连接：什么都不做
 *  - 有进行中的尝试且超过 WIFI_CONNECT_TIMEOUT_MS：disconnect 静音，没在配网就开配网
 *  - 配网态且无进行中的尝试：不自作主张 connect（等用户提交表单或手动触发）
 *  - 其余：发起一轮尝试 */
static void wifi_supervisor_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (s_connected) {
            s_attempt_since = 0;
            if (s_portal_up) {
                /* 配网成功：收掉热点（电池供电时 AP 常开白耗电）。
                 * 切回纯 STA 会断一次，紧接着 wifi_portal_down() 里已重新发起连接。 */
                wifi_portal_down();
                vTaskDelay(pdMS_TO_TICKS(3000));
            }
            continue;
        }

        if (s_attempt_since != 0) {
            uint32_t elapsed_ms =
                (uint32_t)(xTaskGetTickCount() - s_attempt_since) * portTICK_PERIOD_MS;
            if (elapsed_ms >= WIFI_CONNECT_TIMEOUT_MS) {
                esp_wifi_disconnect();     /* 停掉驱动每秒 scan，日志立刻安静 */
                s_attempt_since = 0;
                ESP_LOGW(TAG, "连接超时 %u ms，换下一个内嵌 AP", (unsigned)elapsed_ms);
                /* 配网已关闭：直接轮换到下一个内嵌 AP 继续试 */
                wifi_apply_ap(s_ap_idx + 1);
            }
            continue;
        }

        if (s_portal_up) continue;         /* 配网态：等用户提交，不自动连 */
        wifi_begin_attempt();
    }
}

/* 诊断用：启动时扫一圈，把周围能看到的每个 AP 打到串口。
 * 目的是搞清 "扫不到备用 AP" 到底是信号太弱、名称不对，还是根本没开 2.4GHz。
 * 只做一次阻塞扫描（几秒），之后正常走连接流程。 */
static void wifi_scan_dump(void)
{
    wifi_scan_config_t sc = {
        .ssid = NULL, .bssid = NULL, .channel = 0, .show_hidden = true,
    };
    esp_err_t e = esp_wifi_scan_start(&sc, true);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "scan_start 失败: %s", esp_err_to_name(e));
        return;
    }
    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    if (num == 0) {
        ESP_LOGW(TAG, "扫描完成：附近没有任何 AP");
        return;
    }
    if (num > 32) num = 32;
    wifi_ap_record_t *recs = calloc(num, sizeof(wifi_ap_record_t));
    if (recs == NULL) return;
    uint16_t got = num;
    esp_wifi_scan_get_ap_records(&got, recs);
    ESP_LOGI(TAG, "扫描到 %u 个 AP（authmode 0=OPEN 2=WPA 3=WPA2 4=WPA/WPA2 6=WPA2-ENT）:",
             (unsigned)got);
    for (uint16_t i = 0; i < got; i++) {
        ESP_LOGI(TAG, "  AP[%02u] \"%s\" rssi=%d ch=%d auth=%d",
                 (unsigned)i, recs[i].ssid, recs[i].rssi,
                 recs[i].primary, recs[i].authmode);
    }
    free(recs);
}

void f1_wifi_init(void)
{
    s_wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                    &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                    &wifi_event_handler, NULL, NULL));

    /* 配网已关闭：直接使用内嵌 AP 列表，第一组失败由 supervisor 轮换到第二组 */
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
    wifi_scan_dump();      /* 诊断：打印附近 AP 列表（几秒，判断内嵌 AP 是否可见） */
    wifi_apply_ap(0);
    ESP_LOGI(TAG, "WiFi: 内嵌 AP 列表模式（配网已关闭，共 %d 组）", F1_AP_COUNT);

    xTaskCreate(wifi_supervisor_task, "f1_wifisup", 3072, NULL, 4, NULL);
}

bool f1_wifi_is_connected(void)
{
    return s_connected;
}

/* 把 DNS 指向公共服务器。这里故意不用 esp_netif_set_dns_info：
 * 实测那一句无论放在 GOT_IP 事件回调里还是普通任务里，调用之后整机就再也不发一个包
 * （ping 不通、回传也停），而 dns_setserver 直接操作 lwIP 的 DNS 表，不碰 netif 状态机。 */
void f1_dns_setup(void)
{
    ip_addr_t d;
    IP_ADDR4(&d, 223, 5, 5, 5);
    dns_setserver(0, &d);
    ip_addr_t d2;
    IP_ADDR4(&d2, 119, 29, 29, 29);
    dns_setserver(1, &d2);
    diag_set("dns set");
    ESP_LOGI(TAG, "dns servers set to 223.5.5.5 / 119.29.29.29");
}

/* Ask the STA to (re)associate. Used by the fetch task so a boot that came up
 * before the AP was ready - or a dropped link - recovers without a reboot. */
void f1_wifi_kick(void)
{
    if (!s_connected && s_attempt_since == 0) {
        wifi_begin_attempt();
    }
}

bool f1_wifi_portal_active(void)
{
    return s_portal_up;
}

bool f1_wifi_has_credentials(void)
{
    /* 凭据写死在程序里，永远有 */
    return true;
}

/* 原"手动进配网"入口：配网已关闭，改为立刻换下一个内嵌 AP 重试。 */
void f1_wifi_enter_portal(void)
{
    ESP_LOGW(TAG, "配网已关闭（固定内嵌 AP），改为切换下一个 AP 重试");
    esp_wifi_disconnect();
    s_attempt_since = 0;
    wifi_apply_ap(s_ap_idx + 1);
}

/* ---- Jolpica F1 API ----
 * 必须用域名访问：Cloudflare 对"IP 直连 + Host 头但没有 SNI"的 TLS 握手直接拒绝
 * （PC 上实测 curl -k https://<ip> -H "Host: api.jolpi.ca" 返回 000；80 端口只给 301）。
 * 所以 TLS 的 SNI 只能是域名，这就绕不开 DNS 解析。
 * DNS 走 f1_dns_setup()：在 fetch 任务上下文里设公共 DNS，不在 WiFi 事件回调里动 netif
 * （上一版在 GOT_IP 回调里改 DNS，整机直接失联）。 */

static size_t fetch_url(const char *path, char *buf, size_t bufsize)
{
    char url[192];
    snprintf(url, sizeof(url), "https://" F1_API_HOST "%s", path);
    /* 不挂 CA bundle：certificate_bundle 会同时吃内部 RAM 和校验时间，
     * 而板子的 SNTP 一直没同步上（屏上日期是空的），证书有效期必然校验失败。
     * 不提供 CA 时 esp_tls 走 MBEDTLS_SSL_VERIFY_NONE，绕开时间依赖；
     * 抓的是公开赛事榜单，风险可接受。 */
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 20000,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "client init FAILED (no mem?) for %s", url);
        diag_set("http no mem");
        return 0;
    }
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open FAILED %s for %s (int free=%u psram free=%u)",
                 esp_err_to_name(err), url,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        /* 屏幕上只放得下十几个字符，错误名用数值编码，全名仍进串口日志 */
        diag_set("open %d %uK", (int)err,
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
        esp_http_client_cleanup(client); return 0;
    }
    int len = esp_http_client_fetch_headers(client);
    if (len < 0) {
        ESP_LOGE(TAG, "fetch_headers FAILED for %s (len=%d)", url, len);
        diag_set("hdr %d", len);
        esp_http_client_cleanup(client); return 0;
    }
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGW(TAG, "HTTP status %d for %s", status, url);
        diag_set("HTTP %d", status);
        esp_http_client_cleanup(client); return 0;
    }
    size_t total = esp_http_client_read(client, buf, bufsize - 1);
    buf[total] = 0;
    ESP_LOGI(TAG, "GET ok %u B (content-len %d) int free=%u psram free=%u :: %s",
             (unsigned)total, len,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), url);
    diag_set("GET ok %uB", (unsigned)total);
    esp_http_client_cleanup(client);
    return total;
}

static void parse_standings(const char *json, f1_standings_t *out)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return;
    cJSON *mr = cJSON_GetObjectItem(root, "MRData");
    if (!mr) { cJSON_Delete(root); return; }
    cJSON *table = cJSON_GetObjectItem(mr, "StandingsTable");
    if (table) {
        cJSON *season = cJSON_GetObjectItem(table, "season");
        if (season && season->valuestring)
            snprintf(out->season, sizeof(out->season), "%s", season->valuestring);
    }

    /* Driver standings: MRData.StandingsTable.StandingsLists[0].DriverStandings[] */
    cJSON *lists = table ? cJSON_GetObjectItem(table, "StandingsLists") : NULL;
    cJSON *list0 = (lists && cJSON_IsArray(lists)) ? cJSON_GetArrayItem(lists, 0) : NULL;
    cJSON *ds = list0 ? cJSON_GetObjectItem(list0, "DriverStandings") : NULL;
    if (ds && cJSON_IsArray(ds)) {
        int n = cJSON_GetArraySize(ds);
        if (n > F1_MAX_DRIVERS) n = F1_MAX_DRIVERS;
        for (int i = 0; i < n; i++) {
            cJSON *it = cJSON_GetArrayItem(ds, i);
            f1_driver_t *d = &out->drivers[i];
            cJSON *v;
            if ((v = cJSON_GetObjectItem(it, "position")) && v->valuestring)
                snprintf(d->position, sizeof(d->position), "%s", v->valuestring);
            if ((v = cJSON_GetObjectItem(it, "points")) && v->valuestring)
                snprintf(d->points, sizeof(d->points), "%s", v->valuestring);
            cJSON *drv = cJSON_GetObjectItem(it, "Driver");
            if (drv) {
                if ((v = cJSON_GetObjectItem(drv, "givenName")) && v->valuestring)
                    snprintf(d->given_name, sizeof(d->given_name), "%s", v->valuestring);
                if ((v = cJSON_GetObjectItem(drv, "familyName")) && v->valuestring)
                    snprintf(d->family_name, sizeof(d->family_name), "%s", v->valuestring);
                if ((v = cJSON_GetObjectItem(drv, "code")) && v->valuestring)
                    snprintf(d->code, sizeof(d->code), "%s", v->valuestring);
                if ((v = cJSON_GetObjectItem(drv, "nationality")) && v->valuestring)
                    snprintf(d->nationality, sizeof(d->nationality), "%s", v->valuestring);
            }
            cJSON *cts = cJSON_GetObjectItem(it, "Constructors");
            if (cts && cJSON_IsArray(cts)) {
                cJSON *c0 = cJSON_GetArrayItem(cts, 0);
                cJSON *nm = c0 ? cJSON_GetObjectItem(c0, "name") : NULL;
                if (nm && nm->valuestring)
                    snprintf(d->constructor, sizeof(d->constructor), "%s", nm->valuestring);
            }
        }
        out->driver_count = n;
    }

    /* Constructor standings fetched separately into a second call; see f1_fetch_standings */
    cJSON_Delete(root);
}

static void parse_constructors(const char *json, f1_standings_t *out)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return;
    cJSON *table = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "MRData"), "StandingsTable");
    cJSON *lists = table ? cJSON_GetObjectItem(table, "StandingsLists") : NULL;
    cJSON *list0 = (lists && cJSON_IsArray(lists)) ? cJSON_GetArrayItem(lists, 0) : NULL;
    cJSON *cs = list0 ? cJSON_GetObjectItem(list0, "ConstructorStandings") : NULL;
    if (cs && cJSON_IsArray(cs)) {
        int n = cJSON_GetArraySize(cs);
        if (n > F1_MAX_TEAMS) n = F1_MAX_TEAMS;
        for (int i = 0; i < n; i++) {
            cJSON *it = cJSON_GetArrayItem(cs, i);
            f1_team_t *t = &out->teams[i];
            cJSON *v;
            if ((v = cJSON_GetObjectItem(it, "position")) && v->valuestring)
                snprintf(t->position, sizeof(t->position), "%s", v->valuestring);
            if ((v = cJSON_GetObjectItem(it, "points")) && v->valuestring)
                snprintf(t->points, sizeof(t->points), "%s", v->valuestring);
            cJSON *c = cJSON_GetObjectItem(it, "Constructor");
            if (c) {
                if ((v = cJSON_GetObjectItem(c, "name")) && v->valuestring)
                    snprintf(t->name, sizeof(t->name), "%s", v->valuestring);
                if ((v = cJSON_GetObjectItem(c, "nationality")) && v->valuestring)
                    snprintf(t->nationality, sizeof(t->nationality), "%s", v->valuestring);
            }
        }
        out->team_count = n;
    }
    cJSON_Delete(root);
}

/* circuitId -> 3-letter display code (F1 official style). */
static void circuit_code(const char *id, char *out, size_t n)
{
    static const struct { const char *id; const char *code; } MAP[] = {
        {"albert_park", "ALB"}, {"shanghai", "SHA"}, {"suzuka", "SUZ"},
        {"bahrain", "BAH"}, {"jeddah", "JED"}, {"miami", "MIA"},
        {"monaco", "MON"}, {"catalunya", "CAT"}, {"spain", "MAD"},
        {"silverstone", "SIL"}, {"spa", "SPA"}, {"hungaroring", "HUN"},
        {"zandvoort", "ZAN"}, {"monza", "MON"}, {"baku", "BAK"},
        {"marina_bay", "SGP"}, {"americas", "COT"}, {"rodriguez", "MEX"},
        {"interlagos", "SAO"}, {"vegas", "VGS"}, {"losail", "LOS"},
        {"yas_marina", "YAS"}, {"red_bull_ring", "RBR"}, {"imola", "IMO"},
    };
    for (size_t i = 0; i < sizeof(MAP)/sizeof(MAP[0]); i++) {
        if (strcasecmp(id, MAP[i].id) == 0) {
            snprintf(out, n, "%s", MAP[i].code);
            return;
        }
    }
    /* Fallback: first 3 chars uppercase */
    snprintf(out, n, "%.3s", id ? id : "GP");
    for (char *p = out; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
}

/* Parse ISO-8601 "2026-11-22T04:00:00Z" to epoch (UTC, TZ independent).
 * mktime() would apply the local timezone (we run at CST-8), so convert
 * civil date -> days since epoch by hand instead. */
static time_t parse_iso8601(const char *s)
{
    int Y, M, D, h, m, sec;
    if (!s || sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d", &Y, &M, &D, &h, &m, &sec) != 6)
        return 0;
    int y = Y - (M <= 2 ? 1 : 0);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (M + (M > 2 ? -3 : 9)) + 2) / 5 + D - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = (long)era * 146097 + (long)doe - 719468;
    return (time_t)days * 86400 + h * 3600 + m * 60 + sec;
}

/* circuitId -> POSIX TZ string (newlib parses these directly, DST included). */
static const char *circuit_tz(const char *id)
{
    static const struct { const char *id; const char *tz; } MAP[] = {
        {"albert_park", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
        {"shanghai",    "CST-8"},
        {"suzuka",      "JST-9"},
        {"bahrain",     "<-03>3"},
        {"jeddah",      "<-03>3"},
        {"miami",       "EST5EDT,M3.2.0,M11.1.0"},
        {"monaco",      "CET-1CEST,M3.5.0,M10.5.0/3"},
        {"catalunya",   "CET-1CEST,M3.5.0,M10.5.0/3"},
        {"madrid",      "CET-1CEST,M3.5.0,M10.5.0/3"},
        {"silverstone", "GMT0BST,M3.5.0/1,M10.5.0"},
        {"spa",         "CET-1CEST,M3.5.0,M10.5.0/3"},
        {"hungaroring", "CET-1CEST,M3.5.0,M10.5.0/3"},
        {"zandvoort",   "CET-1CEST,M3.5.0,M10.5.0/3"},
        {"monza",       "CET-1CEST,M3.5.0,M10.5.0/3"},
        {"imola",       "CET-1CEST,M3.5.0,M10.5.0/3"},
        {"red_bull_ring","CET-1CEST,M3.5.0,M10.5.0/3"},
        {"baku",        "<-04>4"},
        {"marina_bay",  "<-08>8"},
        {"americas",    "CST6CDT,M3.2.0,M11.1.0"},
        {"rodriguez",   "CST6"},
        {"interlagos",  "<-03>3"},
        {"vegas",       "PST8PDT,M3.2.0,M11.1.0"},
        {"losail",      "<-03>3"},
        {"yas_marina",  "<-04>4"},
    };
    for (size_t i = 0; i < sizeof(MAP) / sizeof(MAP[0]); i++)
        if (strcasecmp(id, MAP[i].id) == 0) return MAP[i].tz;
    return "UTC0";
}

static void session_push(f1_nextrace_t *nr, const char *label, time_t ts)
{
    if (ts <= 0) return;
    for (int i = 0; i < nr->session_count; i++)
        if (nr->sessions[i].start_ts == ts) return;   /* dedupe SQ aliases */
    if (nr->session_count >= F1_MAX_SESSIONS) return;
    f1_session_t *s = &nr->sessions[nr->session_count++];
    snprintf(s->name, sizeof(s->name), "%s", label);
    s->start_ts = ts;
}

static void session_push_obj(f1_nextrace_t *nr, cJSON *race, const char *key, const char *label)
{
    cJSON *o = cJSON_GetObjectItem(race, key);
    if (!o) return;
    cJSON *d = cJSON_GetObjectItem(o, "date");
    cJSON *t = cJSON_GetObjectItem(o, "time");
    if (!d || !d->valuestring) return;
    char iso[40];
    snprintf(iso, sizeof(iso), "%sT%s", d->valuestring,
             (t && t->valuestring) ? t->valuestring : "00:00:00Z");
    session_push(nr, label, parse_iso8601(iso));
}

/* Parse race calendar: pick next future race + total count. */
static void parse_calendar(const char *json, f1_standings_t *out)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return;
    cJSON *races = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "MRData"), "RaceTable");
    races = races ? cJSON_GetObjectItem(races, "Races") : NULL;
    if (!races || !cJSON_IsArray(races)) { cJSON_Delete(root); return; }
    int total = cJSON_GetArraySize(races);
    out->next_race.total_rounds = total;

    time_t now = time(NULL);
    for (int i = 0; i < total; i++) {
        cJSON *r = cJSON_GetArrayItem(races, i);
        cJSON *date = cJSON_GetObjectItem(r, "date");
        cJSON *timev = cJSON_GetObjectItem(r, "time");
        if (!date || !date->valuestring) continue;
        char isotime[40];
        snprintf(isotime, sizeof(isotime), "%sT%s", date->valuestring,
                 (timev && timev->valuestring) ? timev->valuestring : "00:00:00Z");
        time_t start = parse_iso8601(isotime);
        if (start > now) {
            cJSON *rnd = cJSON_GetObjectItem(r, "round");
            cJSON *name = cJSON_GetObjectItem(r, "raceName");
            cJSON *circ = cJSON_GetObjectItem(r, "Circuit");
            out->next_race.round = rnd ? atoi(rnd->valuestring) : (i + 1);
            if (name && name->valuestring)
                snprintf(out->next_race.name, sizeof(out->next_race.name), "%s", name->valuestring);
            if (circ) {
                cJSON *cid = cJSON_GetObjectItem(circ, "circuitId");
                const char *cid_s = cid ? cid->valuestring : "";
                circuit_code(cid_s, out->next_race.code, sizeof(out->next_race.code));
                snprintf(out->next_race.tz, sizeof(out->next_race.tz), "%s", circuit_tz(cid_s));
            }
            out->next_race.start_ts = start;

            /* Whole weekend, so the dial can count down to the next session. */
            session_push_obj(&out->next_race, r, "FirstPractice", "FP1");
            session_push_obj(&out->next_race, r, "SecondPractice", "FP2");
            session_push_obj(&out->next_race, r, "ThirdPractice", "FP3");
            session_push_obj(&out->next_race, r, "SprintQualifying", "SQ");
            session_push_obj(&out->next_race, r, "SprintShootout", "SQ");
            session_push_obj(&out->next_race, r, "Sprint", "SPR");
            session_push_obj(&out->next_race, r, "Qualifying", "Q");
            session_push(&out->next_race, "RACE", start);

            /* insertion sort by start time */
            f1_nextrace_t *nr = &out->next_race;
            for (int k = 1; k < nr->session_count; k++) {
                f1_session_t key = nr->sessions[k];
                int j = k - 1;
                while (j >= 0 && nr->sessions[j].start_ts > key.start_ts) {
                    nr->sessions[j + 1] = nr->sessions[j];
                    j--;
                }
                nr->sessions[j + 1] = key;
            }
            break;
        }
    }
    cJSON_Delete(root);
}

/* Parse last race results: winner + fastest lap. */
static void parse_last_result(const char *json, f1_standings_t *out)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return;
    cJSON *rt = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "MRData"), "RaceTable");
    cJSON *races = rt ? cJSON_GetObjectItem(rt, "Races") : NULL;
    cJSON *race = (races && cJSON_IsArray(races)) ? cJSON_GetArrayItem(races, 0) : NULL;
    if (!race) { cJSON_Delete(root); return; }
    cJSON *nm = cJSON_GetObjectItem(race, "raceName");
    if (nm && nm->valuestring)
        snprintf(out->last_result.race_name, sizeof(out->last_result.race_name), "%s", nm->valuestring);
    cJSON *results = cJSON_GetObjectItem(race, "Results");
    if (results && cJSON_IsArray(results)) {
        for (int i = 0; i < cJSON_GetArraySize(results); i++) {
            cJSON *it = cJSON_GetArrayItem(results, i);
            cJSON *pos = cJSON_GetObjectItem(it, "position");
            cJSON *drv = cJSON_GetObjectItem(it, "Driver");
            cJSON *code = drv ? cJSON_GetObjectItem(drv, "code") : NULL;
            if (pos && pos->valuestring && strcmp(pos->valuestring, "1") == 0 && code && code->valuestring)
                snprintf(out->last_result.winner, sizeof(out->last_result.winner), "%s", code->valuestring);
            cJSON *fl = cJSON_GetObjectItem(it, "FastestLap");
            cJSON *rank = fl ? cJSON_GetObjectItem(fl, "rank") : NULL;
            if (rank && rank->valuestring && strcmp(rank->valuestring, "1") == 0 && code && code->valuestring)
                snprintf(out->last_result.fastest_lap, sizeof(out->last_result.fastest_lap), "%s", code->valuestring);
        }
    }
    cJSON_Delete(root);
}

uint32_t f1_team_color(const char *name)
{
    if (!name) return 0x9AA0A6;
    struct { const char *key; uint32_t color; } C[] = {
        {"McLaren", 0xFF8700}, {"Red Bull", 0x3671C6}, {"Ferrari", 0xE8002D},
        {"Mercedes", 0x27F4D2}, {"Aston Martin", 0x229971}, {"Alpine", 0xFF87BC},
        {"Williams", 0x64C4FF}, {"Racing Bulls", 0x6692FF}, {"Haas", 0xB6BABD},
        {"Kick Sauber", 0x52E252}, {"Sauber", 0x52E252},
    };
    for (size_t i = 0; i < sizeof(C)/sizeof(C[0]); i++)
        if (strstr(name, C[i].key)) return C[i].color;
    return 0x9AA0A6;
}

/* Each block is fetched separately and one failed request must not wipe the
 * data we already have on screen. Keep a scratch copy of the standings. */
static f1_driver_t s_drv_bak[F1_MAX_DRIVERS];
static f1_team_t   s_team_bak[F1_MAX_TEAMS];

f1_fetch_result_t f1_fetch_standings(f1_standings_t *out)
{
    json_hooks_init();
    if (!s_connected) return F1_FETCH_WIFI_ERROR;

    /* Allocate from PSRAM: a 24KB static buffer in internal DRAM starves the
     * LCD's transient DMA allocations and causes a white screen. */
    const size_t buf_size = 24576;
    char *buf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = malloc(buf_size);
    if (!buf) {
        diag_set("no buf int%uK",
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10));
        return F1_FETCH_HTTP_ERROR;
    }

    /* ---- drivers (required) ---- */
    size_t n = fetch_url("/ergast/f1/current/driverStandings.json?limit=30",
                         buf, buf_size);
    if (n == 0) {
        free(buf);
        return out->driver_count ? F1_FETCH_OK : F1_FETCH_HTTP_ERROR;
    }
    memcpy(s_drv_bak, out->drivers, sizeof(s_drv_bak));
    int dc_bak = out->driver_count;
    char season_bak[8];
    memcpy(season_bak, out->season, sizeof(season_bak));

    out->driver_count = 0;
    parse_standings(buf, out);
    if (out->driver_count == 0) {
        memcpy(out->drivers, s_drv_bak, sizeof(s_drv_bak));
        memcpy(out->season, season_bak, sizeof(season_bak));
        out->driver_count = dc_bak;
        if (!out->driver_count) { free(buf); return F1_FETCH_PARSE_ERROR; }
    }

    /* ---- constructors ---- */
    n = fetch_url("/ergast/f1/current/constructorStandings.json?limit=20",
                  buf, buf_size);
    if (n > 0) {
        memcpy(s_team_bak, out->teams, sizeof(s_team_bak));
        int tc_bak = out->team_count;
        out->team_count = 0;
        parse_constructors(buf, out);
        if (out->team_count == 0) {
            memcpy(out->teams, s_team_bak, sizeof(s_team_bak));
            out->team_count = tc_bak;
        }
    }

    /* ---- calendar: next race + weekend sessions ---- */
    n = fetch_url("/ergast/f1/current.json?limit=100", buf, buf_size);
    if (n > 0) {
        f1_nextrace_t save = out->next_race;
        memset(&out->next_race, 0, sizeof(out->next_race));
        parse_calendar(buf, out);
        if (out->next_race.round == 0 && out->next_race.session_count == 0)
            out->next_race = save;
    }

    /* ---- last race result ---- */
    /* limit=20 so the fastest-lap car is actually in the payload (the FL
     * holder is rarely P1 or P2). */
    n = fetch_url("/ergast/f1/current/last/results.json?limit=20", buf, buf_size);
    if (n > 0) {
        f1_lastresult_t save = out->last_result;
        memset(&out->last_result, 0, sizeof(out->last_result));
        parse_last_result(buf, out);
        if (!out->last_result.race_name[0])
            out->last_result = save;
    }

    free(buf);
    out->synced_at = time(NULL);
    out->valid = true;
    /* 网络通了不等于画面有内容：把三块数据的实际条数写出来，
     * d=车手 t=车队 r=下一站轮次。哪个是 0，就知道是哪一段没解析出来。 */
    diag_set("ok d%d t%d r%d", out->driver_count, out->team_count,
             out->next_race.round);
    return F1_FETCH_OK;
}
