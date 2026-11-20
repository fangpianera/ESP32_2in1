/**
 * ui_eva.h — EVA-01 机甲风导航界面（466×466 圆屏，LVGL 9.5）
 *
 * 设计 token（全部集中在 ui_eva.c 顶部，禁止在控件里硬编码）：
 *   BG      #000000  纯黑
 *   PURPLE  #60269E  初号机紫：环骨架、装甲结构线、次级信息
 *   GREEN   #A6FF00  荧光绿：核心数据、导航提示、心跳弧
 *   RED     #FF3333  偏航/低电
 *   ORANGE  #FFA500  中低电（50%→20%）
 *   LILAC   #C0A0E8  环2 已走路段（淡紫）
 *
 * 四区同心圆（等宽 RW=11、等距 RG=11；环1/2/3 + 中心区）：
 *   环1 187–198  左半电量（紫/橙/红三档）｜右半今日天气（红白蓝渐变 + 高/低温 + 天气字符）
 *   环2 165–176  剩余路程比例（紫=未走，淡紫=已走）
 *   环3 143–154  BLE 心跳倒计时（30s 一圈，连接成功即刷新）
 *   中心 r<143   速度(48px 最大) / 目标地 / 当前前进路线 / 剩余里程(最小最淡)
 *
 * 对外 API 冻结：ui_eva_init / ui_eva_update / ui_eva_toast / ui_eva_status_line
 * 的签名不得改变（app_main.c、nav_core.c 直接依赖）。
 */
#pragma once

#include "esp_err.h"
#include "lvgl.h"
#include "nav_core.h"

/* ---- 原有接口（签名冻结） ---- */
esp_err_t ui_eva_init(void);
void ui_eva_update(const nav_state_t *st);
void ui_eva_toast(const char *msg);
void ui_eva_status_line(const char *msg);

/* ---- 界面构建与直接驱动 ---- */

/** 构建同心圆主界面；调用前必须已持 LVGL 锁（由 ui_eva_init 负责）。 */
lv_obj_t *create_main_navigation_ui(void);

/**
 * 一次性写入四项核心数据（内部自行取 LVGL 锁）。
 * @param speed        行走速度 m/s（中心 48px 数字）
 * @param battery      电量 0~100；传 -1 表示改成"无读数"
 * @param heading_angle 历史参数，箭头已移除，保留仅为兼容旧调用方
 * @param progress      已走比例 0~1000（映射到环2 的淡紫段）
 */
void update_navigation_data(float speed, int battery, float heading_angle, int progress);

/** 单独更新电量（0~100）；传 -1 = 未知（未接电池或 AXP2101 探测失败时显示 N/A）。 */
void ui_eva_set_battery(int pct);

/** 电量 + 充电状态（AXP2101 真值）。pct=-1 表示未接电池/未知；charging=true 时
 *  chip 显示 "+xx" 且文字转荧光绿，环底另点一小段绿色提示。 */
void ui_eva_set_power(int pct, bool charging);

/** 今日天气（手机端联网拉取后经 STATUS 帧下发；板端不联网）。
 *  @param hi   今日最高温（℃）
 *  @param lo   今日最低温（℃）
 *  @param code WMO 天气码（0 晴 / 1-2 少云 / 3 阴 / 45-48 雾 / 5x-6x 雨 / 7x 雪 / 8x 阵雨 / 95+ 雷）
 */
void ui_eva_set_weather(int hi, int lo, int code);

/** 目标地名（手机端从路线 JSON 取 name 后下发；NULL/空串 = 恢复"未设终点"）。 */
void ui_eva_set_dest(const char *name);
