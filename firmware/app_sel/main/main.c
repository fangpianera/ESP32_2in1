/**
 * main.c — 三程序启动选择器（app_sel / factory 槽）
 *
 * 职责只有一件事：把一次点击变成"下次启动进哪个 app"。
 *   - 界面：整屏一张 466x466 RGB565 底图（上半左 EVA 紫 / 右上 F1 红 / 下半 MotoNav 青），
 *     y<233 且 x<233 → 导航，y<233 且 x>=233 → F1，y>=233 → MotoNav；
 *   - 不写文字：靠三块图形辨识（用户明确要求）；
 *   - 点左上 → otadata 指向 app_nav  → 重启；
 *   - 点右上 → otadata 指向 app_f1   → 重启；
 *   - 点下半 → otadata 指向 app_moto → 重启。
 *
 * 为什么"每次上电都回到这里"能成立（关键）：
 *   第三方 MotoNav 是 Arduino 固件，不会主动把 otadata 写回 factory。
 *   所以不再依赖子程序自觉 —— 选择器自己一启动就把引导指针收回 factory
 *   （pin_boot_to_self），随后点击时才写目标槽。
 *   这样无论哪个程序退出后重启，都必然落回本界面。
 *
 * [坑] esp_ota_set_boot_partition(factory_partition) 在 IDF 里的实现是
 *   "擦掉 otadata 两块扇区"，而不是写入一条 factory 记录。因此：
 *     - 本来就没记录（首上电）时不该重复擦，用 esp_ota_get_boot_partition()
 *       返回的 label 判断，已经是 app_sel 就直接跳过，省一次 8 KB 擦写；
 *     - 擦完再点选目标槽，写入口是干净的，不会被旧 seq 干扰。
 *
 * 安全边界：
 *   esp_ota_set_boot_partition() 会先校验目标镜像，槽里没有可启动固件时返回
 *   ESP_ERR_OTA_VALIDATE_FAILED —— 这里当成"该程序还没烧"，恢复界面并降温提示，
 *   绝不把设备卡在一个空分区上。
 */
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs_flash.h"
#include "screen_pm.h"

#include <string.h>

static const char *TAG = "sel";

#define SEL_W            466
#define SEL_H            466
#define SEL_SPLIT_X      (SEL_W / 2)   /* 233：上半左右分界 */
#define SEL_SPLIT_Y      (SEL_H / 2)   /* 233：上半两格 / 下半整块 分界 */

#define SEL_TARGET_NAV   "app_nav"
#define SEL_TARGET_F1    "app_f1"
#define SEL_TARGET_MOTO  "app_moto"
#define SEL_TARGET_SELF  "app_sel"

/* 底图由 CMake 的 EMBED_FILES 编进固件，符号名取 basename */
extern const uint8_t sel_bg_start[] asm("_binary_sel_bg_bin_start");
extern const uint8_t sel_bg_end[]   asm("_binary_sel_bg_bin_end");

static lv_image_dsc_t s_bg;
static lv_obj_t *s_img;
static volatile bool s_busy;

/* 任何触摸都算"有人在看屏幕"；亮度下发由 screen_pm 任务做，这里不碰总线 */
static void on_touch_activity(lv_event_t *e)
{
    (void)e;
    screen_pm_activity("touch");
}

static const esp_partition_t *find_app(const char *label)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, label);
}

/* 写 otadata 并重启；失败返回 false（不重启，把界面留给用户） */
static bool boot_into(const char *label)
{
    const esp_partition_t *p = find_app(label);
    if (p == NULL) {
        ESP_LOGE(TAG, "分区 %s 不存在：分区表没按 partitions_dual.csv 烧？", label);
        return false;
    }

    esp_err_t err = esp_ota_set_boot_partition(p);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "写启动记录失败 %s: %s", label, esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "启动目标 → %s @0x%06lx (%lu B)", label,
             (unsigned long)p->address, (unsigned long)p->size);
    esp_restart();
    return true;   /* 不可达 */
}

/* 把引导指针收回 factory（自己），保证下一次上电一定回到本界面。
 *
 * 为什么必须由选择器来做：MotoNav 是第三方 Arduino 固件，不会像导航/F1 那样
 * 在启动时写回 factory。只要它自己被 bootloader 选中过，otadata 就停在 ota_2。
 *
 * 这条只是"选择器自己启动时兜底"。真正保证 MotoNav 也回得来的是 bootloader 的
 * rollback：选择器写槽时记录 state=NEW（sdkconfig 里 CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y），
 * 程序启动后变 PENDING_VERIFY，谁都没 mark valid，下次复位 bootloader 就把它标 ABORTED
 * 并回退 factory —— 实测 MotoNav 只能跑一个上电周期，重启必回选择器。
 *
 * 对 factory 分区，esp_ota_set_boot_partition() 的实现是"擦掉 otadata 两块扇区"
 * 而不是写一条 factory 记录，所以先看现有记录指向谁，已经指向自己就跳过，
 * 避免每次上电都白擦 8 KB。 */
static void pin_boot_to_self(void)
{
    const esp_partition_t *self = find_app(SEL_TARGET_SELF);
    if (self == NULL) {
        ESP_LOGE(TAG, "分区 %s 不存在：分区表没按 partitions_dual.csv 烧？", SEL_TARGET_SELF);
        return;
    }

    const esp_partition_t *boot = esp_ota_get_boot_partition();
    if (boot != NULL && strcmp(boot->label, SEL_TARGET_SELF) == 0) {
        return;   /* 已指向 factory，无需擦写 */
    }

    esp_err_t err = esp_ota_set_boot_partition(self);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "收回引导指针失败 %s: %s", SEL_TARGET_SELF, esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "引导指针已收回 %s（原指向 %s）", SEL_TARGET_SELF,
                 boot ? boot->label : "无记录");
    }
}

/* BSP v3 的 bsp_display_lock(ms>0) 是「等到超时」；传 0 等于不等待。
 * [实测 2026-11-19] 第一版这里用 bsp_display_lock(0)，bsp_display_start() 之后 LVGL
 * 渲染任务已经在跑，主任务首帧取锁几乎必然失败 → build_ui 直接 return，
 * 屏幕只剩默认白底，对外表现就是「白屏」。导航工程早先踩过同一个坑，用 200ms×3 重试。
 * 返回 true 表示已持锁（调用方必须 unlock）。 */
static bool lock_with_retry(int tries, int per_try_ms)
{
    for (int i = 0; i < tries; i++) {
        if (bsp_display_lock(per_try_ms) == ESP_OK) {
            return true;
        }
    }
    return false;
}

/* 切换动作放到独立任务：LVGL 回调里不能阻塞，也要保证 QSPI 写不抢 flush */
static void switch_task(void *arg)
{
    const char *target = (const char *)arg;

    /* 200ms：让"变暗"的反馈先被看到，再切 */
    vTaskDelay(pdMS_TO_TICKS(200));

    if (!boot_into(target)) {
        /* 目标槽没有可启动固件：恢复亮度、解锁，留在选择界面 */
        if (lock_with_retry(3, 200)) {
            lv_obj_set_style_image_opa(s_img, LV_OPA_COVER, 0);
            bsp_display_unlock();
        }
        s_busy = false;
    }
    vTaskDelete(NULL);
}

static void on_screen_clicked(lv_event_t *e)
{
    (void)e;
    if (s_busy) {
        return;
    }

    /* 息屏状态下第一下只负责唤醒，避免黑屏误触直接跳进某个程序 */
    if (screen_pm_is_asleep()) {
        screen_pm_activity("wake");
        return;
    }

    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) {
        ESP_LOGW(TAG, "无活动输入设备，忽略本次点击");
        return;
    }

    lv_point_t pt;
    lv_indev_get_point(indev, &pt);

    /* 三分法：下半整块给 MotoNav，上半再左右二分给导航 / F1 */
    const char *target;
    if (pt.y >= SEL_SPLIT_Y) {
        target = SEL_TARGET_MOTO;
    } else if (pt.x < SEL_SPLIT_X) {
        target = SEL_TARGET_NAV;
    } else {
        target = SEL_TARGET_F1;
    }
    ESP_LOGI(TAG, "点击 (%d,%d) → %s", (int)pt.x, (int)pt.y, target);

    s_busy = true;
    lv_obj_set_style_image_opa(s_img, LV_OPA_50, 0);   /* 立即反馈：整屏变暗 */

    if (xTaskCreate(switch_task, "sel_switch", 4096, (void *)target, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "切换任务创建失败");
        lv_obj_set_style_image_opa(s_img, LV_OPA_COVER, 0);
        s_busy = false;
    }
}

static void build_ui(void)
{
    memset(&s_bg, 0, sizeof(s_bg));
    s_bg.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_bg.header.cf    = LV_COLOR_FORMAT_RGB565;
    s_bg.header.w     = SEL_W;
    s_bg.header.h     = SEL_H;
    s_bg.header.stride = SEL_W * 2;
    s_bg.data      = sel_bg_start;
    s_bg.data_size = (uint32_t)(sel_bg_end - sel_bg_start);

    /* 先确认 dsc 能被 LVGL 认可（尺寸/格式/数据长度），否则图不会画出来 */
    lv_image_header_t hdr;
    lv_result_t rc = lv_image_decoder_get_info(&s_bg, &hdr);
    ESP_LOGI(TAG, "底图 rc=%d %dx%d cf=%d stride=%d data=%lu B",
             (int)rc, (int)hdr.w, (int)hdr.h, (int)hdr.cf, (int)hdr.stride,
             (unsigned long)s_bg.data_size);

    if (!lock_with_retry(5, 200)) {
        ESP_LOGE(TAG, "LVGL 锁超时（5×200ms 全失败），界面未建立");
        return;
    }

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    s_img = lv_image_create(scr);
    lv_image_set_src(s_img, &s_bg);
    lv_obj_center(s_img);
    lv_obj_remove_flag(s_img, LV_OBJ_FLAG_CLICKABLE);

    /* 点击挂在 screen 上：图片不拦截，整屏任意点都算 */
    lv_obj_add_event_cb(scr, on_screen_clicked, LV_EVENT_CLICKED, NULL);

    bsp_display_unlock();
    ESP_LOGI(TAG, "界面已建立：三分底图分界 x=%d y=%d（上左导航 / 上右F1 / 下MotoNav）",
             SEL_SPLIT_X, SEL_SPLIT_Y);
}

void app_main(void)
{
    ESP_LOGI(TAG, "EVA-Selector boot");

    /* NVS 只是为了让 bootloader/后续 app 的 store 有归属；失败不影响选择功能 */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需重建：%s", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS 初始化失败 %s（继续运行）", esp_err_to_name(err));
    }

    lv_display_t *disp = bsp_display_start();
    if (disp == NULL) {
        /* 显示起不来时不要继续摸 LVGL：留在串口日志上，白屏原因一眼可见 */
        ESP_LOGE(TAG, "显示初始化失败（CO5300 QSPI 未起来）");
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    bsp_display_backlight_on();

    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    ESP_LOGI(TAG, "运行分区=%s  启动记录=%s",
             run ? run->label : "?", boot ? boot->label : "?");

    /* 先把引导指针收回自己，再画界面 —— 这样即使后面点击失败/重启，
     * 下一次上电也一定还是选择器 */
    pin_boot_to_self();

    build_ui();

    if (screen_pm_init() == ESP_OK) {
        lv_indev_t *indev = bsp_display_get_input_dev();
        if (indev != NULL) {
            lv_indev_add_event_cb(indev, on_touch_activity, LV_EVENT_PRESSED, NULL);
        }
    } else {
        ESP_LOGW(TAG, "息屏策略启动失败：选择界面将常亮");
    }

    ESP_LOGI(TAG, "ready, free heap=%u", (unsigned)esp_get_free_heap_size());
}
