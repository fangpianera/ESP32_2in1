/**
 * app_main.c — 启动编排（顺序即依赖顺序，不可随意调换）
 *
 * 启动链：
 *   NVS → LittleFS(mount) → 屏幕+LVGL(BSP) → UI 骨架 → 路线缓存载入
 *        → IMU 初始化 → 静置零偏校准 → 网络(SoftAP/TCP) → 融合主循环
 *
 * 断电/异常恢复：任一阶段失败都不 abort 整个系统，
 *   屏幕可用而 IMU 坏了仍能当"路线显示器"，网络坏了仍能靠 PDR 走一段；
 *   这条降级策略是步行导航这类"中途失效即无用"产品的底线（doc §10）。
 */
#include "app_cfg.h"
#include "board_pins.h"
#include "ble_link.h"
#include "imu_pdr.h"
#include "nav_core.h"
#include "net_link.h"
#include "res_mgr.h"
#include "route_store.h"
#include "ui_eva.h"
#include "power_axp.h"
#include "screen_pm.h"

#include <stdio.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp/esp-bsp.h"

static const char *TAG = "app";

/* 任何触摸都算"有人在看屏幕"：只写时间戳，真正的亮度下发改由 screen_pm_task 做，
 * 避免在 LVGL 输入回调里直接发 QSPI 面板命令（会和 flush 抢总线）。 */
static void on_any_touch(lv_event_t *e)
{
    (void)e;
    screen_pm_activity("touch");
}

/* BOOT 键：长按 1.2s → 重新做零偏校准（放口袋走动后精度下降时用） */
static void boot_key_task(void *arg)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << EVA_BOOT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,     /* 轮询而非中断：本任务开销可忽略，且免去去抖中断 */
    };
    gpio_config(&io);

    uint32_t press_ms = 0;
    while (true) {
        if (gpio_get_level(EVA_BOOT_GPIO) == 0) {
            press_ms += 50;
            if (press_ms == 1200) {         /* 只触发一次 */
                screen_pm_activity("boot key");
                nav_core_recalibrate();
            }
        } else {
            press_ms = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
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
    ESP_LOGI(TAG, "EVA-Nav %s boot", EVA_FW_VERSION);

    /* 1) NVS */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* 2) 屏幕 + LVGL（官方 BSP 负责 CO5300 QSPI 初始化、触摸、双缓冲） */
    if (bsp_display_start() == NULL) {
        ESP_LOGE(TAG, "display start failed — 检查 QSPI 引脚/供电（doc §9 U1）");
        /* 不 abort：至少保留串口日志能力，便于现场取证 */
        while (true) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    bsp_display_backlight_on();

    /* 2.5) 把启动指针交还给选择器（双程序模式；老分区表下自动跳过） */
    hand_boot_back_to_selector();

    /* 3) 资源文件系统（必须早于 UI，字体/图标都从 LittleFS 来） */
    if (res_mgr_init() != ESP_OK) {
        ESP_LOGW(TAG, "资源不可用，UI 将使用内置字体/符号（降级模式）");
    }

    /* 4) UI 骨架 —— 失败只降级，不 abort（本机重启循环会把可恢复故障变成不可用）
     *    [实测] 修前：ui_eva_init() 因 LVGL 锁超时返回 0x107，被 ESP_ERROR_CHECK 直接 abort */
    if (ui_eva_init() != ESP_OK) {
        ESP_LOGW(TAG, "UI 初始化失败（LVGL 锁或内存），降级为日志模式启动");
    }

    /* 5) 板载 AXP2101：电量/充电状态（失败只降级为 N/A，不影响导航） */
    if (power_axp_init() != ESP_OK) {
        ESP_LOGW(TAG, "AXP2101 不可用：电量显示 N/A");
        ui_eva_set_power(-1, false);
    } else {
        ui_eva_set_power(power_axp_percent(), power_axp_charging());
    }

    /* 6) 路线缓存（上次下发的路线可能还在，先给用户一个"可导航"的空壳） */
    if (route_store_load()) {
        ESP_LOGI(TAG, "使用缓存路线");
    } else {
        ui_eva_status_line("等待手机下发路线");
    }

    /* 6) IMU + 静置零偏校准
     * 窗口内检测到晃动时 imu_static_calibrate 返回 ESP_ERR_INVALID_STATE（结果会污染
     * 计步与航向），此处自动重试最多 3 次；仍失败则用最后一次结果继续（PDR 会自学习）。 */
    ui_eva_status_line("请保持静止：传感器自检中");
    if (imu_pdr_init() == ESP_OK) {
        esp_err_t cal_err = ESP_FAIL;
        for (int attempt = 0; attempt < 3; attempt++) {
            cal_err = imu_static_calibrate(3000);
            if (cal_err == ESP_OK) break;
            ESP_LOGW(TAG, "静态校准第 %d 次未通过，1s 后重试", attempt + 1);
            ui_eva_status_line("检测到晃动：请静置，正在重试");
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        if (cal_err == ESP_OK) {
            ui_eva_status_line("传感器就绪");
        } else {
            ui_eva_status_line("校准质量差：结果仅作初值");
        }
    } else {
        ui_eva_status_line("IMU 未响应：仅显示路线（无 PDR）");
    }

    /* 7) 网络：帧处理基础设施 + 传输层（默认 BLE；Wi-Fi AP 见 app_cfg.h 开关） */
    if (net_link_start(nav_core_on_net_event, NULL) != ESP_OK) {
        ESP_LOGW(TAG, "网络启动失败：仅 PDR，无法接收路线/校准");
        ui_eva_status_line("网络异常：仅惯导可用");
    }

#if EVA_LINK_BLE
    if (ble_link_start() != ESP_OK) {
        ESP_LOGE(TAG, "BLE 启动失败：改用 Wi-Fi 回退需改 EVA_LINK_BLE=0 重编");
        ui_eva_status_line("蓝牙异常");
    }
#endif

    /* 8) 融合主循环 */
    if (nav_core_start() != ESP_OK) {
        ESP_LOGW(TAG, "融合主循环启动失败");
    }

    /* 9) 息屏节能（防 AMOLED 烧屏）：20s 无触摸/无有效数据 → 熄屏，触摸即亮 */
    if (screen_pm_init() == ESP_OK) {
        lv_indev_t *indev = bsp_display_get_input_dev();
        if (indev != NULL) {
            lv_indev_add_event_cb(indev, on_any_touch, LV_EVENT_PRESSED, NULL);
            lv_indev_add_event_cb(indev, on_any_touch, LV_EVENT_PRESSING, NULL);
        } else {
            ESP_LOGW(TAG, "无触摸设备：只能靠 BLE 帧唤醒");
        }
    } else {
        ESP_LOGW(TAG, "息屏任务启动失败：屏幕常亮（注意烧屏风险）");
    }

    xTaskCreatePinnedToCore(boot_key_task, "boot_key", 3072, NULL, 2, NULL, 0);

    ui_eva_toast("等待手机蓝牙连接");
    ESP_LOGI(TAG, "boot complete, free heap=%u, min=%u",
             (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size());
}
