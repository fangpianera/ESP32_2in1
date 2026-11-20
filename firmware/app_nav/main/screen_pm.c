/**
 * screen_pm.c — AMOLED 息屏节能（防烧屏）
 *
 * 设计取舍：
 *  - 计时基准用 esp_timer_get_time()（微秒，单调），不受 tick 漂移影响；
 *  - activity() 只写一个 uint64 时间戳 + 打印，不加锁：64 位单次写入在 LX7 上是
 *    单字节对齐读写不会撕裂到"半新半旧"，最坏也只是多亮/少亮 200ms；
 *  - 亮度下发（QSPI 面板命令 0x51）只在本任务里做，LVGL 回调里绝不动总线。
 *  - 只是"屏幕"休眠：BLE 连接、PDR 融合、路线缓存全部照常跑，唤醒即续画面。
 */
#include "screen_pm.h"

#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp/esp-bsp.h"

static const char *TAG = "scr";

#define SCR_DIM_MS      15000   /* 15s 无活动 → 降到 25%（先给用户一个"要黑了"的提示） */
#define SCR_OFF_MS      20000   /* 20s 无活动 → 亮度 0（AMOLED 近似断电，抑制残影） */
#define SCR_DIM_PCT     25
#define SCR_AWAKE_PCT   100
#define SCR_POLL_MS     200     /* 200ms 粒度：息屏延迟误差不超过 0.2s */

typedef enum {
    SCR_ST_AWAKE = 0,
    SCR_ST_DIM,
    SCR_ST_OFF,
} scr_state_t;

static volatile uint64_t s_last_act_us;
static volatile scr_state_t s_state = SCR_ST_AWAKE;
static int s_off_ms = SCR_OFF_MS;
static TaskHandle_t s_task = NULL;
static const char *s_last_why = "boot";

void screen_pm_activity(const char *why)
{
    s_last_act_us = (uint64_t)esp_timer_get_time();
    if (why != NULL) {
        s_last_why = why;
    }
}

bool screen_pm_is_asleep(void)
{
    return s_state == SCR_ST_OFF;
}

void screen_pm_set_timeout_ms(int ms)
{
    s_off_ms = (ms > 0) ? ms : SCR_OFF_MS;
}

static void apply_brightness(int pct, scr_state_t st, const char *why)
{
    esp_err_t err = bsp_display_brightness_set(pct);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "亮度 %d%% 下发失败 0x%x", pct, err);
    }
    s_state = st;
    ESP_LOGI(TAG, "屏幕 %s (%d%%) 空闲原因=%s", st == SCR_ST_OFF ? "OFF" :
             (st == SCR_ST_DIM ? "DIM" : "ON"), pct, why);
}

static void screen_pm_task(void *arg)
{
    (void)arg;
    while (true) {
        uint64_t now = (uint64_t)esp_timer_get_time();
        uint32_t idle_ms = (uint32_t)((now - s_last_act_us) / 1000ULL);
        scr_state_t st = s_state;

        if (idle_ms >= (uint32_t)s_off_ms) {
            if (st != SCR_ST_OFF) {
                apply_brightness(0, SCR_ST_OFF, s_last_why);
            }
        } else if (idle_ms >= SCR_DIM_MS) {
            if (st == SCR_ST_OFF || st == SCR_ST_AWAKE) {
                /* 从 OFF 直接被触摸唤醒时 idle_ms 必然很小，这里只处理 AWAKE→DIM */
                if (st == SCR_ST_AWAKE) {
                    apply_brightness(SCR_DIM_PCT, SCR_ST_DIM, s_last_why);
                }
            }
        } else {
            if (st != SCR_ST_AWAKE) {
                apply_brightness(SCR_AWAKE_PCT, SCR_ST_AWAKE, s_last_why);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(SCR_POLL_MS));
    }
}

esp_err_t screen_pm_init(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }
    screen_pm_activity("init");
    /* 优先级 3、栈 3KB：只做一次 uint64 比较 + 偶尔一条面板命令 */
    if (xTaskCreate(screen_pm_task, "screen_pm", 3072, NULL, 3, &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "息屏策略：%dms 降亮 %d%% → %dms 熄屏；触摸/BLE 帧即唤醒",
             SCR_DIM_MS, SCR_DIM_PCT, s_off_ms);
    return ESP_OK;
}
