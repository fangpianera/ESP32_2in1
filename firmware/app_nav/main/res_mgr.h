/**
 * res_mgr.h — LittleFS 挂载 + 字体分级加载 + 96x96 图标按需加载
 *
 * 分级策略（doc §8 资源预算）：
 *   T0 常驻：数字/字母 + 导航高频词（约 60 字形，编译进 flash .rodata，零延迟）
 *   T1 按需：完整 CJK 子集 .bin 放 LittleFS（lv_binfont_create），用完即卸
 *   T2 图标：96x96 ARGB8888 .bin 放 LittleFS，界面切换时加载，避免 12 个图标常驻 ~420KB
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "lvgl.h"

esp_err_t res_mgr_init(void);
bool      res_fs_ready(void);

lv_font_t *res_font_nav(void);      /* 中文导航词，可能为 NULL（降级用默认字体） */
lv_font_t *res_font_num(void);      /* 大字号数字 */

/* 创建图标控件；name 不带路径，如 "arrow"、"turn_left"。
 * 找不到 .bin 时回落为 LVGL 内置符号，保证 UI 不空窗。 */
lv_obj_t *res_icon_create(lv_obj_t *parent, const char *name, int size_px);

/* 显式释放（切场景时调用），返回释放字节数估算 */
uint32_t res_cache_flush(void);
