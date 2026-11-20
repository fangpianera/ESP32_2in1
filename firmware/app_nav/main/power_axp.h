/**
 * power_axp.h — AXP2101 电量与充电状态（板载电源管理芯片，微雪 1.75C 官方硬件）
 *
 * 调用约定：
 *  - init 只探测一次（I2C 0x34 + IC_TYPE==0x4A）；探测失败不代表板子不能用，只是没电量读数。
 *  - poll 由导航任务周期调用（建议 1~5s 一次），内部只读 3 个寄存器，耗时 ~0.3ms。
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

/** 探测 AXP2101 并读一次初始值；返回 ESP_ERR_NOT_FOUND 表示这块板没接/找不到该芯片 */
esp_err_t power_axp_init(void);

/** 读一次 STATUS1/STATUS2/SOC；无电池（只插 USB）时 percent 返回 -1 */
void power_axp_poll(void);

bool power_axp_ready(void);
/** 电量百分比 0..100；-1 = 未知/未接电池 */
int  power_axp_percent(void);
/** 是否正在充电（AXP2101 STATUS2 充电状态位） */
bool power_axp_charging(void);
