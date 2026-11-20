/**
 * screen_pm.h — AMOLED 息屏节能（防烧屏）
 *
 * AMOLED 长时间停在静态画面（本机是紫环 + 荧光绿数字）会留下永久残影，
 * 所以空闲后先降亮度再做一次"黑屏"，触摸或有效数据到来立刻点亮。
 *
 * 状态机（单调，只有 3 态）：
 *   AWAKE --(15s 无活动)--> DIM(25%) --(20s 无活动)--> OFF(亮度 0)
 *   OFF/DIM --(touch | ble frame | key)--> AWAKE(100%)
 *
 * 并发：activity() 只写时间戳（可被 ISR/LVGL 上下文调用），
 * 亮度下发的 QSPI 事务只在 screen_pm_task 里做 —— 不与 LVGL 抢 SPI 总线。
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>

/** 提示级别：只重置计时，不改亮度（由任务统一处理，避免在 LVGL 回调里发 QSPI） */
void screen_pm_activity(const char *why);

/** 是否已息屏（供电/调试用） */
bool screen_pm_is_asleep(void);

/** 启动空闲检测任务；重复调用安全 */
esp_err_t screen_pm_init(void);

/** 调试用：临时改超时（毫秒，<=0 表示恢复默认 20s） */
void screen_pm_set_timeout_ms(int ms);
