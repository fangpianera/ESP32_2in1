/**
 * route_store.h — 路线模型 / 落盘 / 定位投影（纯逻辑，不依赖 Wi-Fi 与 UI）
 *
 * 存储格式（LittleFS: /littlefs/route/current.bin）
 *   route_hdr_t + route_pt_t[npts] + route_step_t[nsteps]
 *   落盘为二进制而非 JSON：解析一次即固化，重启后 O(1) mmap 式读取，且避免 cJSON 大内存。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define ROUTE_MAGIC      0x31525645u   /* 'E','V','R','1' */
#define ROUTE_VER        1

typedef struct __attribute__((packed)) {
    int32_t  lat;          /* ×1e7 定点，1e-7 度 ≈ 1.1cm */
    int32_t  lon;
    int16_t  bearing_deg;  /* 该点航向，-1 表示无效 */
    uint16_t flags;        /* bit0: 转向点；bit1: 途经点 */
} route_pt_t;              /* 12 B */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t npts;
    uint16_t nsteps;
    uint16_t reserved;
    int32_t  total_dist_mm;
    int32_t  origin_lat;
    int32_t  origin_lon;
    uint32_t crc32;        /* 覆盖 pts + steps 区 */
} route_hdr_t;             /* 28 B */

typedef enum {
    ACT_NONE = 0, ACT_LEFT = 1, ACT_RIGHT = 2, ACT_LEFT_HARD = 3,
    ACT_RIGHT_HARD = 4, ACT_STRAIGHT = 5, ACT_ARRIVE = 6, ACT_UTURN = 7,
} route_act_t;

typedef struct __attribute__((packed)) {
    uint16_t pt_index;     /* 对应 route_pt_t 下标（转向发生处） */
    uint8_t  act;          /* route_act_t */
    uint8_t  reserved;
    float    dist_next_m;  /* 到该转向点的沿路线距离 */
    char     road[36];     /* 道路名（UTF-8，按 UTF-8 边界截断） */
    char     tip[52];      /* 动作提示，例如"向左前方行走" */
} route_step_t;            /* 96 B */

typedef struct {
    int32_t  seg_idx;
    float    t;            /* 段内投影比例 [0,1] */
    float    dist_along;   /* 沿路线已行进距离 m */
    float    remain_m;     /* 剩余 m */
    float    off_route_m;  /* 横向偏差 m（>15m 建议强提示"已偏离"） */
    float    seg_bearing;  /* 当前段航向 deg，0=正北，顺时针 */
    int32_t  next_step;    /* 下一转向记录下标，-1 无 */
    float    next_dist_m;  /* 到下一转向点 m */
} route_loc_t;

/* 由 JSON 解析并立即落盘（payload 为 UTF-8 JSON，len 为字节数） */
esp_err_t route_store_ingest_json(const char *json, uint32_t len,
                                  uint16_t *out_npts, int32_t *out_dist_mm);

/* 从 Flash 载入到 PSRAM；返回 false 表示无有效路线 */
bool route_store_load(void);

/* 当前路线是否可用 */
bool route_store_ready(void);

/* 以局部 ENU 米坐标（相对 origin）做投影定位 */
esp_err_t route_project(float x_m, float y_m, route_loc_t *out);

/* 经纬度 → 局部 ENU 米（等距圆柱近似，10km 内误差 <0.1% [代码推算]） */
void route_geo_to_local(int32_t lat1e7, int32_t lon1e7, float *x_m, float *y_m);

/* 取第 idx 个点（供 UI 画“后续路径”） */
const route_pt_t *route_point(size_t idx);
size_t route_points_count(void);

/* 路线全长（米） */
float route_total_dist_m(void);

/* 由 route_step_t 直接给出转向提示文本（供 UI） */
const char *route_step_tip(int32_t idx);
const char *route_step_road(int32_t idx);
