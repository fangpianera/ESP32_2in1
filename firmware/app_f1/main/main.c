#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

#include "lvgl.h"
#include "bsp/esp-bsp.h"

#include "f1_api.h"
#include "status.h"
#include "ui.h"

static const char *TAG = "f1_main";


static f1_standings_t g_standings;
static volatile bool g_refresh_requested = false;

static void do_refresh(void)
{
    if (g_refresh_requested) return;
    g_refresh_requested = true;   /* fetch task picks it up */
}

static void fetch_task(void *arg)
{
    (void)arg;
    TickType_t delay = pdMS_TO_TICKS(2000);
    f1_diag_note("fetch task up");
    while (1) {
        vTaskDelay(delay);
        if (!f1_wifi_is_connected()) {
            /* 重连已交给 f1_api 里的 supervisor（指数退避），这里只管等 */
            f1_diag_note("wait wifi");
            delay = pdMS_TO_TICKS(5000);
            continue;
        }

        /* SNTP 不在这里启动：它要等 netif 完全就绪，放在取数热路径上
         * 一旦失败就会把整个任务卡住（屏上表现就是永远停在 IP ok）。
         * 时间只影响界面上的日期显示，不该拖累取数。 */
        bool forced = g_refresh_requested;
        g_refresh_requested = false;
        f1_diag_note("fetching");
        /* 先回传一条，保证即使后面的 DNS 自检卡住，日志里也能看到"任务跑起来了"。 */
        f1_report("before");
        /* 拿到 IP 后把 DNS 换成公共服务器（用 lwIP 的 dns_setserver，
         * 绝不能用 esp_netif_set_dns_info —— 那句一调，整机就再也不发包）。 */
        static bool dns_ready = false;
        if (!dns_ready) {
            dns_ready = true;
            f1_dns_setup();
            f1_dns_probe();
            f1_tls_probe();
            /* 取数已经稳了，把对时补回来：status.c 里连 ntp.aliyun.com 并设 CST-8，
             * 状态栏的日期/时间只认 status_time_valid()（time(NULL) > 2023-11）这一条判据。 */
            status_time_sync_start();
            f1_report("dns");
        }
        f1_fetch_result_t r = f1_fetch_standings(&g_standings);
        {
            char tag[20];
            snprintf(tag, sizeof(tag), "after r=%d", (int)r);
            f1_report(tag);
        }
        if (r == F1_FETCH_OK) {
            f1_ui_update(&g_standings);
            delay = forced ? pdMS_TO_TICKS(3000) : pdMS_TO_TICKS(5 * 60 * 1000);
        } else {
            ESP_LOGW(TAG, "fetch failed: %d", r);
            delay = pdMS_TO_TICKS(8000);   /* 失败快速重试，屏幕上的序号会一直往上走 */
        }
    }
}

/* 屏幕底部的网络提示：未联网 / 配网中 才显示，联网自动隐藏。
 * 1 秒粒度轮询，只在状态变化时碰 LVGL。 */
static void net_monitor_task(void *arg)
{
    (void)arg;
    while (1) {
        int st = f1_wifi_is_connected() ? 0 : (f1_wifi_portal_active() ? 2 : 1);
        /* 每秒都调用：state 不变时也要把 f1_diag_line() 的文字刷到屏幕上 */
        f1_ui_net_hint(st);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* 双程序启动约定：本固件起来后把 otadata 写回 factory（= app_sel 选择器），
 * 这样下一次断电重启必然回到选择界面，用户永远先看到"选哪个程序"。
 * 只写启动记录、不重启，当前这次运行完全不受影响。
 * 老的单 app 分区表（没有 app_sel）下找不到分区，静默跳过，行为与旧版一致。 */
static void hand_boot_back_to_selector(void)
{
    const esp_partition_t *sel = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, "app_sel");
    if (sel == NULL) {
        return;
    }

    /* 磨损保护：esp_ota_set_boot_partition(factory) 的实现是"擦掉整个 otadata 分区"，
     * 每次启动都擦一遍会白白消耗 flash 擦写寿命；已指向 factory 就跳过。 */
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    if (boot != NULL && boot->address == sel->address) {
        return;
    }

    esp_err_t err = esp_ota_set_boot_partition(sel);
    if (err != ESP_OK) {
        /* 常见原因：选择器槽还没烧固件（镜像校验不通过）。不影响本次运行。 */
        ESP_LOGW(TAG, "写回选择器失败: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "启动记录已写回 app_sel（下次上电进选择界面）");
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    bsp_display_start();
    /* No bsp_display_backlight_on() here: the panel init sequence already sets
     * brightness 0x51 = 0xFF, and calling it now races with the LVGL task's SPI
     * polling transaction ("Cannot acquire bus..."), which wedged app_main.
     * Runtime dimming for screen-off still goes through the BSP. */

    /* 排查 F1 启动崩溃期间临时关掉"交还选择器"：
     * 否则每次崩完 otadata 被擦成空，只能回到选择器，抓不到第二次崩溃现场。 */
    /* hand_boot_back_to_selector(); */

    status_init();   /* timezone + PMU (battery) + I2C node, before any UI read */

    bsp_display_lock(-1);
    f1_ui_create();
    f1_ui_set_refresh_cb(do_refresh);
    bsp_display_unlock();

    /* 顺序很重要：必须先让 Wi-Fi 起来，再建取数任务。
     * 反过来先建 12KB 栈的任务，会把内部 RAM 吃掉，esp_wifi_init() 直接
     * 返回 ESP_ERR_NO_MEM(257)，ESP_ERROR_CHECK 立刻 abort → 复位回选择器，
     * 屏幕上就是"黑一下弹回来"。这条崩溃栈是从 coredump 里读出来的。 */
    f1_wifi_init();
    TaskHandle_t fh = NULL;
    if (xTaskCreate(fetch_task, "f1_fetch", 8192, NULL, 5, &fh) != pdPASS) {
        ESP_LOGE(TAG, "fetch task 8192K failed, retry smaller");
        if (xTaskCreate(fetch_task, "f1_fetch", 6144, NULL, 5, &fh) != pdPASS) {
            ESP_LOGE(TAG, "fetch task create FAILED (no mem)");
        }
    }
    if (xTaskCreate(net_monitor_task, "f1_netmon", 4096, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "netmon task create FAILED");
    }

    ESP_LOGI(TAG, "F1 dashboard running");
}
