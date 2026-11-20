/**
 * ui_eva.c — EVA-01 机甲风导航界面（466×466 圆屏 / CO5300 / LVGL 9.5）
 *
 * ── 设计 token（集中管理，禁止在控件里硬编码颜色）────────────────────────
 *   BG        #000000  纯黑（AMOLED 熄像素，圆屏边界最干净）
 *   PURPLE    #60269E  初号机紫：环骨架、装甲结构线、次级信息
 *   GREEN     #A6FF00  荧光绿：核心数据、导航提示、心跳弧
 *   RED       #FF3333  偏航/低电
 *   ORANGE    #FFA500  中低电（50%→20%）
 *   LILAC     #C0A0E8  淡紫：环2 已走过的路段
 *   WX_R/W/B  天气弧的 红→白→蓝 三锚点
 *
 * ── 四区同心圆（等宽 RW=11px、等间距 RG=11px）──────────────────────────
 *   环1（最外 187..198）左半 = 电量，右半 = 今日天气（红白蓝渐变 + 最高/最低温 + 天气字符）
 *   环2（165..176）      = 剩余路程比例（紫=未走，淡紫=已走）
 *   环3（143..154）      = BLE 心跳倒计时（30s 一圈，连接成功即刷新）
 *   中心（r<143）        = 速度（48px 最大） / 目标地 / 当前前进路线 / 剩余里程（最小最淡）
 *
 * ── lv_conf / sdkconfig 必须项（内存与性能）─────────────────────────────
 *   1) CONFIG_LV_DRAW_SW_COMPLEX=n：不用阴影，开着会给每个弧/圆角多算一层 mask；
 *      本界面每帧有 ~50 段弧，它会让 466×466 整屏刷新 CPU 翻倍。
 *   2) 局部刷新缓冲区走 BSP 的 buffer_height=32（29,824B，64B 对齐），别再调大，
 *      内部 RAM 不够会在 setup_dma_priv_buffer 处失败（坑 20）。
 *   3) 字号全部来自 fonts/eva_tech_48/20/15.c（Chakra Petch）；sdkconfig 里
 *      CONFIG_LV_FONT_MONTSERRAT_* 已全部关闭，省 ~40KB Flash，不要重新打开。
 *   4) 环是"按值变化才重画"：s_dr_* 脏值相同就不 invalidate，10Hz 更新不等于 10Hz 整屏重绘。
 *   5) 不用 lv_arc 控件：LVGL 9.5 在本板把弧控件画成左上角小弧[实测]，
 *      所以三条环全部在 LV_EVENT_DRAW_MAIN 里用 lv_draw_arc 自绘，几何完全可控。
 */
#include "ui_eva.h"
#include "res_mgr.h"
#include "screen_pm.h"       /* toast = 需要人看的事件，唤醒屏幕 */
#include "ble_link.h"
#include "app_cfg.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"             /* lv_refr_now：文本改动后同帧刷完，防刷新分块撕裂 */
#include "src/draw/lv_draw_image.h"
#include "src/draw/lv_image_dsc.h"

static const char *TAG = "ui";

/* EVA 机甲字体（Chakra Petch SemiBold/Medium，OFL，lv_font_conv 生成于 main/fonts/）
 * 只含 ASCII 0x20-0x7E + 度数符号 °，所以中文仍走 res_font_nav() 的 nav18。 */
extern const lv_font_t eva_tech_48;
extern const lv_font_t eva_tech_20;
extern const lv_font_t eva_tech_15;
/* 18px / 24px GB2312 全字库（Noto Sans SC / OFL，由 tools/build_fonts.ps1 生成） */
extern const lv_font_t cjk18;
extern const lv_font_t cjk24;

/* ---------------- 设计 token ---------------- */
#define C_BG       0x000000
#define C_PURPLE   0x60269E
#define C_GREEN    0xA6FF00
#define C_RED      0xFF3333
#define C_ORANGE   0xFFA500
#define C_LILAC    0xC0A0E8        /* 已走路段：淡紫 */
#define C_REMAIN   0xE0D4FA        /* 剩余里程文字：高亮淡紫（0xB79BE0 在户外仍几乎不可见） */
#define C_DIM      0x2E1A44        /* 未激活紫（环比 C_PURPLE 暗约 45%） */
#define C_WX_TOP   0xFF3333        /* 天气弧：顶部红 */
#define C_WX_MID   0xFFFFFF        /*         中部白 */
#define C_WX_BOT   0x2E7BFF        /*         底部蓝 */
#define C_WM       0x100828        /* 水印暗紫（仅作记录；颜色烧在 nerv_bg.bin 像素里，
                                      * 2026-11-19 由 0x080418 提亮到 0x100828） */
#define WM_SIZE    320             /* 水印边长：直径 320 < 可视直径 424 */

#define SCR_W      466
#define SCR_H      466
#define CX         (SCR_W / 2)
#define CY         (SCR_H / 2)

/* ---------------- 同心圆几何（等宽、等距）---------------- */
#define R_MAX      212             /* 可视半径，四周留 21px 防圆屏切角 */
#define RW         16              /* 环宽（px），三条相同（由 11 加粗到 16） */
#define RG         15              /* 环间距（px），两道相同 */
#define R1O        216
#define R1I        (R1O - RW)      /* 200 */
#define R2O        (R1I - RG)      /* 185 */
#define R2I        (R2O - RW)      /* 169 */
/* 原环3（BLE 心跳倒计时，154..138）已移除 */
#define R_CHIP     208             /* 环1 带中线(216+200)/2，chip 压在这里 */

/* 环1 左右半区（顺时针：0°=3点钟，90°=6点钟，180°=9点钟，270°=12点钟） */
#define HALF_LEFT_S   92.0f
#define HALF_LEFT_E   268.0f
#define HALF_RIGHT_S  272.0f
#define HALF_RIGHT_E  448.0f       /* 88°，跨 360° 由 arc_split 处理 */

/* ---------------- 控件句柄 ---------------- */
static lv_obj_t *s_scr;
static lv_obj_t *s_lbl_speed;   /* 中心：速度（48px 绿，最大） */
static lv_obj_t *s_lbl_unit;    /* 中心：m/s */
static lv_obj_t *s_lbl_dest;    /* 中心：目标地（18px 亮紫） */
static lv_obj_t *s_lbl_tip;     /* 中心：当前前进路线（24px 荧光绿） */
static lv_obj_t *s_lbl_remain;  /* 中心：剩余里程（14px 暗紫，最小最淡） */
static lv_obj_t *s_lbl_mode;    /* 中心底部：MODE 状态（14px 紫） */
static lv_obj_t *s_lbl_toast;   /* 顶部临时提示 */

static lv_obj_t *s_lbl_bat;     /* 环1 左侧 chip：电量百分比 */
static lv_obj_t *s_lbl_wx_hi;   /* 环1 顶部 chip：今日最高温 */
static lv_obj_t *s_lbl_wx_ic;   /* 环1 右侧 chip：天气字符（晴/云/雨…） */
static lv_obj_t *s_lbl_wx_lo;   /* 环1 底部 chip：今日最低温 */

static lv_image_dsc_t s_wm_dsc; /* 背景水印描述符（像素常驻 PSRAM） */
static uint8_t       *s_wm_buf;

/* ---------------- 全局样式 ---------------- */
static lv_style_t sty_scr;
static lv_style_t sty_text_green;
static lv_style_t sty_text_purple;
static lv_style_t sty_text_red;
static lv_style_t sty_text_dim;
static lv_style_t sty_chip;      /* 环上读数的"装甲槽"：黑底切断弧线 */

/* ---------------- 运行状态 ---------------- */
static int      s_battery   = -1;   /* -1=未知（未接电池/AXP 探测失败） */
static bool     s_charging  = false;/* AXP2101 STATUS2 充电位 */
static int      s_low_phase = -1;   /* 低电量呼吸相位：-1=不呼吸，0=亮，1=暗 */
static int      s_wx_hi     = -99;  /* -99=未收到天气 */
static int      s_wx_lo     = -99;
static int      s_wx_code   = -1;
static char     s_dest[64];         /* 64B ≈ 21 个汉字：原来 40B 会把长店名截成 13 字 */
static bool     s_dest_set  = false;
static int      s_walk_pct  = 0;    /* 环2：已走比例 0..100 */
static int      s_cnt_s     = 0;    /* 环3：心跳倒计时 0..30 */
static int      s_cnt_max   = 30;

/* 脏值：只有变化才整屏重绘（10Hz 更新不等于 10Hz 重画） */
static int  s_dr_bat = -2, s_dr_wx = -2, s_dr_walk = -1;
static bool s_need_redraw = true;

static int64_t s_toast_until_ms;

/* BSP v3 的 esp_lv_adapter_lock 把 timeout=0 解释为"不等待、立即失败"，
 * 而 bsp_display_start() 之后 LVGL 渲染任务已在跑，主任务此刻取锁必然偶发失败。
 * 统一走"有限等待 + 重试"。 */
static bool ui_lock(uint32_t per_try_ms, int tries)
{
    for (int i = 0; i < tries; i++) {
        if (bsp_display_lock(per_try_ms) == ESP_OK) return true;
    }
    return false;
}

/* ---------------- 样式初始化（一次调用） ---------------- */
static void ui_style_init(void)
{
    lv_style_init(&sty_scr);
    lv_style_set_bg_color(&sty_scr, lv_color_hex(C_BG));
    lv_style_set_bg_opa(&sty_scr, LV_OPA_COVER);
    lv_style_set_text_color(&sty_scr, lv_color_hex(C_GREEN));
    lv_style_set_border_width(&sty_scr, 0);
    lv_style_set_radius(&sty_scr, 0);
    lv_style_set_pad_all(&sty_scr, 0);

    lv_style_init(&sty_text_green);
    lv_style_set_text_color(&sty_text_green, lv_color_hex(C_GREEN));
    lv_style_set_text_font(&sty_text_green, &eva_tech_15);

    lv_style_init(&sty_text_purple);
    lv_style_set_text_color(&sty_text_purple, lv_color_hex(C_PURPLE));
    lv_style_set_text_font(&sty_text_purple, &eva_tech_15);

    lv_style_init(&sty_text_red);
    lv_style_set_text_color(&sty_text_red, lv_color_hex(C_RED));
    lv_style_set_text_font(&sty_text_red, &eva_tech_15);

    lv_style_init(&sty_text_dim);
    lv_style_set_text_color(&sty_text_dim, lv_color_hex(C_DIM));
    lv_style_set_text_font(&sty_text_dim, &eva_tech_15);

    /* 环上读数 chip：黑底把弧线"切断"，形成装甲槽口 */
    lv_style_init(&sty_chip);
    lv_style_set_bg_color(&sty_chip, lv_color_hex(C_BG));
    lv_style_set_bg_opa(&sty_chip, LV_OPA_COVER);
    lv_style_set_radius(&sty_chip, 2);
    lv_style_set_pad_hor(&sty_chip, 3);
    lv_style_set_pad_ver(&sty_chip, 0);
    lv_style_set_border_width(&sty_chip, 0);
}

/* ---------------- 颜色插值（天气弧用） ---------------- */
static lv_color_t col_lerp(lv_color_t a, lv_color_t b, float t)
{
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    lv_color_t r;
    r.red   = (uint8_t)((float)a.red   + ((float)b.red   - (float)a.red)   * t);
    r.green = (uint8_t)((float)a.green + ((float)b.green - (float)a.green) * t);
    r.blue  = (uint8_t)((float)a.blue  + ((float)b.blue  - (float)a.blue)  * t);
    return r;
}

/* ---------------- 自绘弧：radius/width/角度全部由本文件给定 ----------------
 * LVGL 的 0°=3 点钟、顺时针为正；跨越 360° 的段自动拆两笔。 */
static void arc_seg(lv_layer_t *layer, int cx, int cy, int r, int w,
                    lv_color_t col, lv_opa_t opa, float s, float e)
{
    if (e <= s) return;
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.center.x = cx;
    d.center.y = cy;
    d.radius   = r;
    d.width    = w;
    d.color    = col;
    d.opa      = opa;
    d.rounded  = 0;                 /* 平头，机械感 */
    while (e > 360.0f) {
        d.start_angle = (lv_value_precise_t)s;
        d.end_angle   = (lv_value_precise_t)360;
        lv_draw_arc(layer, &d);
        s = 0.0f;
        e -= 360.0f;
    }
    d.start_angle = (lv_value_precise_t)s;
    d.end_angle   = (lv_value_precise_t)e;
    lv_draw_arc(layer, &d);
}

/* 整圈底环（暗紫，40% 不透明） */
static void ring_base(lv_layer_t *layer, int cx, int cy, int r)
{
    arc_seg(layer, cx, cy, r, RW, lv_color_hex(C_DIM), LV_OPA_COVER, 0.0f, 360.0f);
}

/* ---------------- 环1 左半：电量 ---------------- */
static void ring1_battery(lv_layer_t *layer, int cx, int cy)
{
    /* 底槽：整段暗紫，保证"空电"也看得出这是一条仪表带 */
    arc_seg(layer, cx, cy, R1O - RW / 2, RW, lv_color_hex(C_DIM), LV_OPA_COVER,
            HALF_LEFT_S, HALF_LEFT_E);

    if (s_battery < 0) {            /* 无读数：只留底槽 */
        return;
    }
    int p = s_battery > 100 ? 100 : (s_battery < 0 ? 0 : s_battery);
    lv_color_t col = p >= 50 ? lv_color_hex(C_PURPLE)
                   : p >= 20 ? lv_color_hex(C_ORANGE)
                             : lv_color_hex(C_RED);
    /* 从底部 90° 顺时针向上充满（电量越满，紫/橙/红越往上顶） */
    float span = (HALF_LEFT_E - HALF_LEFT_S) * (float)p / 100.0f;
    if (span < 1.5f) span = 1.5f;
    arc_seg(layer, cx, cy, R1O - RW / 2, RW, col, LV_OPA_COVER,
            HALF_LEFT_S, HALF_LEFT_S + span);

    /* 充电中：底部再点一小段荧光绿（"正在进电"的机械提示，不改电量等级色） */
    if (s_charging) {
        arc_seg(layer, cx, cy, R1O + 5, 3, lv_color_hex(C_GREEN), LV_OPA_COVER, 84.0f, 96.0f);
    }
}

/* ---------------- 环1 右半：天气渐变 ---------------- */
static void ring1_weather(lv_layer_t *layer, int cx, int cy)
{
    const float span = HALF_RIGHT_E - HALF_RIGHT_S;   /* 176° */
    const int   segs = 16;                            /* 每段 11°，肉眼看不出台阶 */
    for (int i = 0; i < segs; i++) {
        float t = (float)i / (float)(segs - 1);       /* 0=顶(红) → 1=底(蓝) */
        lv_color_t c = t <= 0.5f
            ? col_lerp(lv_color_hex(C_WX_TOP), lv_color_hex(C_WX_MID), t * 2.0f)
            : col_lerp(lv_color_hex(C_WX_MID), lv_color_hex(C_WX_BOT), (t - 0.5f) * 2.0f);
        float s = HALF_RIGHT_S + span * (float)i / (float)segs;
        float e = HALF_RIGHT_S + span * (float)(i + 1) / (float)segs + 1.0f; /* +1° 消缝 */
        arc_seg(layer, cx, cy, R1O - RW / 2, RW, c, LV_OPA_COVER, s, e);
    }
}

/* ---------------- 环2：剩余路程比例 ----------------
 * 2026-11-19/20 曾采用 ui22「整圈只画两笔」：p≥100 一笔淡紫画满、零交界，
 * 目的是消掉实拍里那串共端点叠色造成的亮纹（25 段 = 25 处）。
 * 2026-11-20 用户步行上班再次实拍，判定「还是回滚到分段的那一版」，
 * 因此恢复为 25 段扇格上色。此处只改环2，其余绘制/链路/门槛逻辑一律未动。 */
static void ring2_remain(lv_layer_t *layer, int cx, int cy)
{
    int p = s_walk_pct;                 /* 已走 % */
    if (p < 0)   p = 0;
    if (p > 100) p = 100;

    /* 2026-11-20 用户回滚：恢复 25 段扇格上色（判据只看段中点是否已走过）。
     * 段宽 360/25 = 14.4°，起点角度先归一到 [0,360) 再交给 arc_seg 拆笔，
     * 否则起始角 >360 时 arc_seg 会画出反向大弧。 */
    const int   segs = 25;
    const float step = 360.0f / (float)segs;
    float travelled  = 360.0f * (float)p / 100.0f;
    int   r_mid      = R2O - RW / 2;

    for (int i = 0; i < segs; i++) {
        float s = 270.0f + step * (float)i;      /* 12 点钟起，顺时针 */
        while (s >= 360.0f) s -= 360.0f;
        bool done = ((float)i + 0.5f) * step <= travelled;
        arc_seg(layer, cx, cy, r_mid, RW,
                lv_color_hex(done ? C_LILAC : C_PURPLE), LV_OPA_COVER,
                s, s + step);
    }
}

/* ---------------- 外部装甲刻度：45° 一道径向短线 ---------------- */
static void ring_ticks(lv_layer_t *layer, int cx, int cy)
{
    lv_draw_line_dsc_t l;
    lv_draw_line_dsc_init(&l);
    l.opa   = LV_OPA_COVER;
    l.width = 2;

    for (int i = 0; i < 8; i++) {
        float deg = 45.0f * (float)i;
        float ang = deg * (float)M_PI / 180.0f;
        float c = cosf(ang), s = sinf(ang);
        l.color = lv_color_hex(C_PURPLE);
        int r1 = R1O + 2, r2 = R1O + 6;
        l.p1.x = (lv_value_precise_t)(cx + c * (float)r1);
        l.p1.y = (lv_value_precise_t)(cy + s * (float)r1);
        l.p2.x = (lv_value_precise_t)(cx + c * (float)r2);
        l.p2.y = (lv_value_precise_t)(cy + s * (float)r2);
        lv_draw_line(layer, &l);
    }
}

/* ---------------- 背景水印：一次性读进 PSRAM ----------------
 * 不走 lv_image_decoder：LVGL 9.5 的 dsc 结构是私有头，且 bin 解码结果未必常驻。
 * 直接 fopen + 读 12 字节头（magic/cf/flags/w/h/stride/reserved_2）校验后把像素
 * 搬进 PSRAM，绘制阶段只剩一次 blit。[实测] 文件头 = magic 0x19, cf 0x12,
 * 320×320, stride 640（=320*2），RGB565。 */
#define WM_DBG() do { ESP_LOGI(TAG, "[heap] int=%u dma=%u psram=%u", \
    (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), \
    (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA), \
    (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM)); } while (0)

static void wm_load(void)
{
    const char *path = "/littlefs/icon/nerv_bg.bin";
    WM_DBG();
    lv_image_header_t hdr;
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "背景水印缺失(%s)，只用纯黑底", path);
        return;
    }
    if (fread(&hdr, 1, sizeof(hdr), f) != sizeof(hdr) ||
        hdr.magic != LV_IMAGE_HEADER_MAGIC || hdr.cf != LV_COLOR_FORMAT_RGB565 ||
        hdr.w != WM_SIZE || hdr.h != WM_SIZE) {
        ESP_LOGW(TAG, "背景水印头非法(magic=%u cf=%u %ux%u)",
                 (unsigned)hdr.magic, (unsigned)hdr.cf, (unsigned)hdr.w, (unsigned)hdr.h);
        fclose(f);
        return;
    }
    size_t sz = (size_t)hdr.stride * hdr.h;
    uint8_t *buf = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGW(TAG, "背景水印 PSRAM 分配失败(%u B)", (unsigned)sz);
        fclose(f);
        return;
    }
    if (fread(buf, 1, sz, f) != sz) {
        ESP_LOGW(TAG, "背景水印读取不完整");
        heap_caps_free(buf);
        fclose(f);
        return;
    }
    fclose(f);
    s_wm_dsc.header    = hdr;
    s_wm_dsc.data_size = (uint32_t)sz;
    s_wm_dsc.data      = buf;
    s_wm_buf           = buf;
    WM_DBG();
    ESP_LOGI(TAG, "背景水印已载入 %ux%u RGB565 (%u B PSRAM)",
             (unsigned)hdr.w, (unsigned)hdr.h, (unsigned)sz);
}

/* ---------------- 整屏自绘：水印 → 环1 → 环2 → 刻度 ----------------
 * 中心区不再画箭头：改为纯文字（速度最大、目标地/前进路线次之、剩余里程最小最淡）。
 * 原「环3 BLE 心跳倒计时」已按用户要求移除（2026-11-19），中心区因此多出 31px 半径余量。 */
static void screen_draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_obj_t   *obj   = lv_event_get_target(e);
    lv_area_t   a;
    lv_obj_get_coords(obj, &a);

    int cx = (a.x1 + a.x2) / 2;
    int cy = (a.y1 + a.y2) / 2;

    /* 水印最先画：DRAW_MAIN 在子对象之前执行，天然位于所有环与文字之下 */
    if (s_wm_dsc.data) {
        lv_draw_image_dsc_t img;
        lv_draw_image_dsc_init(&img);
        img.src = &s_wm_dsc;
        img.opa = LV_OPA_COVER;
        lv_area_t wa;
        wa.x1 = cx - WM_SIZE / 2;
        wa.y1 = cy - WM_SIZE / 2;
        wa.x2 = wa.x1 + WM_SIZE - 1;
        wa.y2 = wa.y1 + WM_SIZE - 1;
        lv_draw_image(layer, &img, &wa);
    }

    ring1_battery(layer, cx, cy);
    ring1_weather(layer, cx, cy);
    ring2_remain(layer, cx, cy);
    ring_ticks(layer, cx, cy);
}

/* UTF-8 安全截断（不切半个汉字），用于目标地名/路名 */
static void utf8_clip(char *dst, size_t cap, const char *src)
{
    if (cap == 0) return;
    size_t n = 0;
    while (src[n] && n < cap - 1) {
        unsigned char c = (unsigned char)src[n];
        int len = (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
        if (n + (size_t)len >= cap) break;
        n += (size_t)len;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* ---------------- chip 控件工厂：环带上的读数槽 ---------------- */
static lv_obj_t *make_chip(lv_obj_t *parent, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_add_style(l, &sty_chip, 0);
    if (font) lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return l;
}

/* ---------------- UI 构建（调用前必须已持 LVGL 锁） ---------------- */
lv_obj_t *create_main_navigation_ui(void)
{
    lv_obj_t *scr = lv_screen_active();
    s_scr = scr;
    lv_obj_add_style(scr, &sty_scr, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    const lv_font_t *f_cjk = res_font_nav();     /* nav18 */

    /* ---- 环1 上的四个读数槽：顶=最高温，右=天气字符，底=最低温，左=电量% ---- */
    s_lbl_wx_hi = make_chip(scr, &eva_tech_20, lv_color_hex(C_WX_TOP));
    lv_label_set_text(s_lbl_wx_hi, "--");
    lv_obj_align(s_lbl_wx_hi, LV_ALIGN_CENTER, 0, -R_CHIP);

    s_lbl_wx_ic = make_chip(scr, f_cjk, lv_color_hex(C_GREEN));
    lv_label_set_text(s_lbl_wx_ic, "--");
    lv_obj_align(s_lbl_wx_ic, LV_ALIGN_CENTER, R_CHIP, 0);

    s_lbl_wx_lo = make_chip(scr, &eva_tech_20, lv_color_hex(C_WX_BOT));
    lv_label_set_text(s_lbl_wx_lo, "--");
    lv_obj_align(s_lbl_wx_lo, LV_ALIGN_CENTER, 0, R_CHIP);

    s_lbl_bat = make_chip(scr, &eva_tech_20, lv_color_hex(C_PURPLE));
    lv_label_set_text(s_lbl_bat, "--");
    lv_obj_align(s_lbl_bat, LV_ALIGN_CENTER, -R_CHIP, 0);

    /* ---- 中心区：速度（最大，48px 荧光绿） ---- */
    s_lbl_speed = lv_label_create(scr);
    lv_label_set_text(s_lbl_speed, "0.0");
    lv_obj_set_style_text_font(s_lbl_speed, &eva_tech_48, 0);
    lv_obj_set_style_text_color(s_lbl_speed, lv_color_hex(C_GREEN), 0);
    lv_obj_align(s_lbl_speed, LV_ALIGN_CENTER, -10, -84);

    s_lbl_unit = lv_label_create(scr);
    lv_label_set_text(s_lbl_unit, "m/s");
    lv_obj_add_style(s_lbl_unit, &sty_text_green, 0);
    lv_obj_align(s_lbl_unit, LV_ALIGN_CENTER, 56, -56);

    /* ---- 中心区：目标地（18px，比主题紫亮一档，中心区才看得清） ----
     * [实测 2026-11-19] 原来是 LV_LABEL_LONG_DOT + 330 宽，板端出现"字叠字"花屏，
     * 且颜色用 C_PURPLE(#60269E) 太暗。改：LONG_CLIP（不改写字符串，只在框外裁掉）
     * + 360 宽 + 固定高度，颜色提到淡紫 C_LILAC。 */
    s_lbl_dest = lv_label_create(scr);
    lv_label_set_text(s_lbl_dest, "未设终点");
    if (f_cjk) lv_obj_set_style_text_font(s_lbl_dest, f_cjk, 0);
    lv_obj_set_style_text_color(s_lbl_dest, lv_color_hex(C_LILAC), 0);
    lv_obj_set_style_pad_all(s_lbl_dest, 0, 0);
    lv_obj_set_width(s_lbl_dest, 336);
    /* 高度必须让 LVGL 按字体行高自己算：写死 24 < cjk18 行高(≈25) 时，
     * 字形会画出控件框外，而失效区域只按框算 → 每次刷新在框外留 1~2px 残影，
     * 累积起来就是板端看到的"字叠字/花屏"。 */
    lv_obj_set_height(s_lbl_dest, LV_SIZE_CONTENT);
    lv_obj_set_style_text_align(s_lbl_dest, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_lbl_dest, LV_LABEL_LONG_CLIP);
    lv_obj_align(s_lbl_dest, LV_ALIGN_CENTER, 0, -40);

    /* ---- 中心区：当前前进路线（24px 荧光绿）----
     * 2026-11-19 板端实拍结论：原来用"同文本 + 偏 1px 的第二个 label"做 faux bold，
     * 只要有第二份文本对象，两个 label 各自持有独立的失效区域，LVGL 分块刷新时
     * 两份字会错开 1~2 帧写入 → 屏幕上就是"字叠字/半边新半边旧"的花屏。
     * 现场三次实拍里所有干净行（目的地 18px、剩 xx 米、速度 48px）都没有副本，
     * 只有这行有副本 → 改为单一 label，粗体交给字体本身（cjk24 用 SimHei 源，
     * 该字面天然比 Noto Regular 更重），不再叠两层。 */
    s_lbl_tip = lv_label_create(scr);
    lv_label_set_text(s_lbl_tip, "等待路线");
    lv_obj_set_style_text_font(s_lbl_tip, &cjk24, 0);
    lv_obj_set_style_text_color(s_lbl_tip, lv_color_hex(C_GREEN), 0);
    lv_obj_set_style_pad_all(s_lbl_tip, 0, 0);
    lv_obj_set_width(s_lbl_tip, 336);
    lv_obj_set_height(s_lbl_tip, LV_SIZE_CONTENT);
    lv_obj_set_style_text_align(s_lbl_tip, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_lbl_tip, LV_LABEL_LONG_CLIP);
    lv_obj_align(s_lbl_tip, LV_ALIGN_CENTER, 0, -2);

    /* ---- 中心区：剩余里程（最小最淡）----
     * 原来是 sty_text_dim 直接套 eva_tech_15（纯 ASCII），"剩"字渲染成方块 □0.12 km。
     * 这里显式把字体换成 cjk18，颜色仍用暗紫。 */
    s_lbl_remain = lv_label_create(scr);
    lv_label_set_text(s_lbl_remain, "NO ROUTE");
    lv_obj_add_style(s_lbl_remain, &sty_text_dim, 0);
    lv_obj_set_style_text_font(s_lbl_remain, &cjk18, 0);
    /* 原来套 sty_text_dim 的暗紫(#3E2A5A 级)在阳光下几乎看不见 → 换成中亮淡紫，
     * 仍比目的地那行暗一档，保持"最小最淡"的层级。 */
    lv_obj_set_style_text_color(s_lbl_remain, lv_color_hex(C_REMAIN), 0);
    lv_obj_set_style_pad_all(s_lbl_remain, 0, 0);
    lv_obj_set_width(s_lbl_remain, 320);
    lv_obj_set_height(s_lbl_remain, LV_SIZE_CONTENT);
    lv_obj_set_style_text_align(s_lbl_remain, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_lbl_remain, LV_LABEL_LONG_CLIP);
    lv_obj_align(s_lbl_remain, LV_ALIGN_CENTER, 0, 40);

    /* ---- 中心底部：MODE 状态 ---- */
    s_lbl_mode = lv_label_create(scr);
    lv_label_set_text(s_lbl_mode, "MODE: STANDBY");
    lv_obj_add_style(s_lbl_mode, &sty_text_purple, 0);
    lv_obj_align(s_lbl_mode, LV_ALIGN_CENTER, 0, 76);

    /* ---- 顶部临时提示（toast）---- */
    s_lbl_toast = lv_label_create(scr);
    lv_label_set_text(s_lbl_toast, "");
    if (f_cjk) lv_obj_set_style_text_font(s_lbl_toast, f_cjk, 0);
    lv_obj_set_style_text_color(s_lbl_toast, lv_color_hex(C_GREEN), 0);
    lv_obj_align(s_lbl_toast, LV_ALIGN_CENTER, 0, -118);
    lv_obj_add_flag(s_lbl_toast, LV_OBJ_FLAG_HIDDEN);

    wm_load();

    lv_obj_add_event_cb(scr, screen_draw_cb, LV_EVENT_DRAW_MAIN, NULL);

    ESP_LOGI(TAG, "EVA UI built (cjk=%s, rings R1=%d..%d R2=%d..%d, R3 removed)",
             f_cjk ? "ok" : "none", R1I, R1O, R2I, R2O);
    return scr;
}

/* ---------------- 数据更新（内部实现，调用方须已持锁） ---------------- */

/* 只有环值真的变了才重绘（水印 320×320 blit + ~50 段弧，不能按 10Hz 无脑刷）
 * 心跳倒计时每秒都在变，但它只占环3 那一圈：只让环3 的包围盒脏，避免整屏每秒重绘，
 * 也就不会因为一整屏刷新占锁太久而把 10Hz 的数值更新挤掉（会刷 "Failed to acquire
 * LVGL lock"）。 */
/* 只让"环带"脏，绝不碰圆心区（三行文字在这里）。
 * 2026-11-19 板端实测两轮教训：
 *  ① 用正方形包围盒 → 每秒一次的心跳倒计时把 308×308（含三行字）整片擦掉重画，
 *     屏幕无独立帧缓存、绘制与扫描并行 → 汉字被横切成条、每秒变一次；
 *  ② 改用 15° 扇段各自取外接矩形 → 弧线在扇段中段向外鼓出，相邻包围盒在缝上
 *     漏 1~2px，漏处保留的是上一帧旧弧（环3 每秒缩短、环2 不断推进）→ 环上出现锯齿/缺口。
 * 最终改法：环带 = 外正方形 减 内正方形，用 上/下/左/右 四个矩形无缝铺满，
 * 中间那块"方孔"完全不失效，既百分百覆盖环带又绝不碰中心文字。 */
static void invalidate_band(int r_in, int r_out)
{
    if (!s_scr || r_out <= 0) return;
    if (r_in < 0) r_in = 0;
    if (r_in >= r_out) return;

    int pad = 2;                                    /* 抗锯齿向外余量 */
    int x1 = CX - r_out - pad, x2 = CX + r_out + pad;
    int y1 = CY - r_out - pad, y2 = CY + r_out + pad;
    int ix1 = CX - r_in, ix2 = CX + r_in;           /* 内方孔边界 */
    int iy1 = CY - r_in, iy2 = CY + r_in;

    lv_area_t a;

    a.x1 = (lv_coord_t)x1; a.x2 = (lv_coord_t)x2;
    a.y1 = (lv_coord_t)y1; a.y2 = (lv_coord_t)(iy1 - 1);
    if (a.y1 <= a.y2) lv_obj_invalidate_area(s_scr, &a);      /* 上 */

    a.y1 = (lv_coord_t)(iy2 + 1); a.y2 = (lv_coord_t)y2;
    if (a.y1 <= a.y2) lv_obj_invalidate_area(s_scr, &a);      /* 下 */

    a.x1 = (lv_coord_t)x1; a.x2 = (lv_coord_t)(ix1 - 1);
    a.y1 = (lv_coord_t)iy1; a.y2 = (lv_coord_t)iy2;
    if (a.x1 <= a.x2) lv_obj_invalidate_area(s_scr, &a);      /* 左 */

    a.x1 = (lv_coord_t)(ix2 + 1); a.x2 = (lv_coord_t)x2;
    if (a.x1 <= a.x2) lv_obj_invalidate_area(s_scr, &a);      /* 右 */
}

static void invalidate_ring_zone(void)
{
    /* 两条环带各脏自己那圈；环1 的外沿多给 8px 以覆盖 45° 装甲刻度线。
     * 环3（原 BLE 倒计时）已移除，不再需要这一带的重绘。 */
    invalidate_band(R1I, R1O + 8);
    invalidate_band(R2I, R2O);
}

static void request_redraw_if_dirty(void)
{
    bool full         = s_need_redraw;
    bool ring_changed = (s_dr_bat != s_battery || s_dr_wx != s_wx_code ||
                         s_dr_walk != s_walk_pct || s_need_redraw);

    /* 环3 移除后，心跳倒计时不再驱动重绘：否则每秒白刷一条环带（无内容却抢总线） */
    if (!ring_changed) {
        return;
    }

    s_dr_bat  = s_battery;
    s_dr_wx   = s_wx_code;
    s_dr_walk = s_walk_pct;
    s_need_redraw = false;

    if (!s_scr) return;

    if (full) {
        lv_obj_invalidate(s_scr);          /* 仅建屏/换主题时整屏重画 */
    } else {
        invalidate_ring_zone();
    }
}

/* 文本缓存：只有内容真的变了才写 label。
 * 板端实测：10Hz 无条件 setText 时，24px 绿字一行跨两个 32px 刷新分块，
 * 分块之间内容被改写 → 上半块是新字、下半块是旧字，肉眼就是"字叠字/花屏"。
 * 现在双保险：① 内容不变不写；② 写完后在同一帧 lv_refr_now() 强制刷完再解锁。 */
static char s_cache_tip[72];
static char s_cache_remain[40];
static char s_cache_mode[32];
static char s_cache_speed[8];
static char s_cache_bat[8];
static char s_cache_dest[80];
static bool s_text_dirty;

static void label_set_cached(lv_obj_t *obj, char *cache, size_t cap, const char *txt)
{
    if (!obj || !cap) return;
    if (strncmp(cache, txt, cap) == 0) return;      /* 没变化：不写、不失效，省一次刷新 */
    snprintf(cache, cap, "%s", txt);
    lv_label_set_text(obj, cache);
    s_text_dirty = true;
}

static void nav_ui_apply(float speed)
{
    char buf[16];
    if (speed < 0.0f) speed = 0.0f;
    snprintf(buf, sizeof(buf), "%.1f", (double)speed);
    label_set_cached(s_lbl_speed, s_cache_speed, sizeof(s_cache_speed), buf);

    snprintf(buf, sizeof(buf), "%d%%", s_battery < 0 ? 0 : s_battery);
    if (s_battery < 0) {
        snprintf(buf, sizeof(buf), "N/A");
    } else if (s_charging) {
        /* 充电中：数字前加 "+"，文字转荧光绿（环的颜色仍按电量等级走规范） */
        snprintf(buf, sizeof(buf), "+%d", s_battery);
    }
    label_set_cached(s_lbl_bat, s_cache_bat, sizeof(s_cache_bat), buf);
    lv_obj_set_style_text_color(s_lbl_bat,
        lv_color_hex(s_battery < 0 ? C_DIM
                    : s_charging   ? C_GREEN
                    : s_battery >= 50 ? C_PURPLE
                    : s_battery >= 20 ? C_ORANGE : C_RED), 0);

    /* 低电量（<20% 且不在充电）：chip 文字 1.2s 周期明暗呼吸，避免只看颜色不够醒目。
     * 只改本 label 的 text_opa，LVGL 只重绘这一个小区域（~70x26 px），不触发整屏。 */
    int phase = -1;
    if (s_battery >= 0 && s_battery < 20 && !s_charging) {
        phase = (int)((esp_timer_get_time() / 1000 / 600) & 1);
    }
    if (phase != s_low_phase) {
        s_low_phase = phase;
        lv_obj_set_style_text_opa(s_lbl_bat, (phase == 1) ? LV_OPA_50 : LV_OPA_COVER, 0);
    }

    request_redraw_if_dirty();
}

/* ---------------- 对外接口（保持原有签名） ---------------- */
esp_err_t ui_eva_init(void)
{
    /* 取锁预算：BSP v3 的 bsp_display_lock(ms>0) 是"等到超时"，200ms×3 曾[实测]
     * 在整屏刷新时失败（"锁 3 次重试仍超时"）。加大到 1000ms×5。 */
    if (!ui_lock(1000, 5)) {
        ESP_LOGE(TAG, "LVGL 锁 5 次重试仍超时，UI 未建立");
        return ESP_ERR_TIMEOUT;
    }
    ui_style_init();
    create_main_navigation_ui();
    bsp_display_unlock();
    return ESP_OK;
}

/* 供外部任务调用：一次写入核心数据（heading_angle 已不再用于箭头，保留兼容） */
void update_navigation_data(float speed, int battery, float heading_angle, int progress)
{
    (void)heading_angle;
    if (!s_scr) return;
    if (!ui_lock(50, 2)) return;
    if (battery >= 0) s_battery = battery > 100 ? 100 : battery;
    s_walk_pct = (progress < 0) ? 0 : (progress > 1000 ? 100 : progress / 10);
    nav_ui_apply(speed);
    bsp_display_unlock();
}

void ui_eva_set_battery(int pct)
{
    if (!s_scr) return;
    if (!ui_lock(50, 2)) return;
    if (pct < 0) {
        s_battery = -1;
    } else {
        s_battery = pct > 100 ? 100 : pct;
    }
    nav_ui_apply(-1.0f);        /* -1 只刷 chip 文本与环，不动速度数字 */
    bsp_display_unlock();
}

/* 电量 + 充电状态（AXP2101 真值）：pct=-1 表示未接电池/未知 */
void ui_eva_set_power(int pct, bool charging)
{
    if (!s_scr) return;
    /* 值没变就不抢锁、不刷 —— 电量每秒轮询，抢锁太勤会把 UI 任务锁死（改前实测 10 次/18s 失败） */
    int nc = (pct < 0) ? -1 : (pct > 100 ? 100 : pct);
    if (nc == s_battery && charging == s_charging) return;
    bool chg_changed = (s_charging != charging);
    if (!ui_lock(50, 2)) return;
    s_charging = charging;
    s_battery = nc;
    if (chg_changed) s_need_redraw = true;      /* 充电提示那一段绿色要重画 */
    nav_ui_apply(-1.0f);                        /* -1 只刷 chip 文本与环，不动速度数字 */
    bsp_display_unlock();
}

/* 天气：手机端拉一次今日预报后经 STATUS 帧下发（板端不联网） */
void ui_eva_set_weather(int hi, int lo, int code)
{
    if (!s_scr) return;
    if (!ui_lock(50, 2)) return;
    s_wx_hi   = hi;
    s_wx_lo   = lo;
    s_wx_code = code;

    char buf[16];
    snprintf(buf, sizeof(buf), "%d°", hi);
    lv_label_set_text(s_lbl_wx_hi, buf);
    snprintf(buf, sizeof(buf), "%d°", lo);
    lv_label_set_text(s_lbl_wx_lo, buf);

    /* WMO 天气码 → 单/双字中文（nav18 字库只含高频字，见 assets/nav18.c 的字表） */
    const char *ic = "云";
    if (code == 0)                         ic = "晴";
    else if (code <= 2)                    ic = "少云";
    else if (code == 3)                    ic = "阴";
    else if (code == 45 || code == 48)     ic = "雾";
    else if (code >= 51 && code <= 67)     ic = "雨";
    else if (code >= 71 && code <= 77)     ic = "雪";
    else if (code >= 80 && code <= 82)     ic = "阵雨";
    else if (code >= 85 && code <= 86)     ic = "阵雪";
    else if (code >= 95)                   ic = "雷";
    lv_label_set_text(s_lbl_wx_ic, ic);

    request_redraw_if_dirty();
    bsp_display_unlock();
}

/* 目标地：手机端从路线 JSON 里取 name 后随 STATUS 下发 */
void ui_eva_set_dest(const char *name)
{
    if (!s_scr) return;
    if (!ui_lock(50, 2)) return;
    if (name && name[0]) {
        utf8_clip(s_dest, sizeof(s_dest), name);
        s_dest_set = true;
        label_set_cached(s_lbl_dest, s_cache_dest, sizeof(s_cache_dest), s_dest);
    } else {
        s_dest_set = false;
        label_set_cached(s_lbl_dest, s_cache_dest, sizeof(s_cache_dest), "未设终点");
    }
    if (s_text_dirty) { s_text_dirty = false; lv_refr_now(NULL); }
    bsp_display_unlock();
}

void ui_eva_update(const nav_state_t *st)
{
    if (!st || !s_scr) return;
    if (!ui_lock(50, 2)) return;

    char buf[64];

    /* 速度直接取融合层的平滑值：原来这里写成"stationary 就置 0"，
     * 一次 ZUPT 抖动就把数字打回 0.0，看起来就是闪一下又归零。 */
    float speed = st->speed_mps;
    if (speed < 0.0f) speed = 0.0f;

    /* 环2：剩余路程比例（progress_ratio 是"已走"，环上画的是"已走=淡紫"）
     * 2026-11-19 修正：原来 route_ok=false（GPS 丢一帧/路线暂不可用）时直接置 0，
     * 环2 会从当前值整体跳回"满圈主题紫"再跳回来，视觉上像整环闪了一下。
     * 现在只在 route_ok 为真时更新；掉线时保持上一值，等 GPS 恢复自然接上。
     * 换路线时新路线 progress 从 0 起步（route_ok=true），环2 会自己归零。 */
    if (st->route_ok) {
        s_walk_pct = (int)(st->progress_ratio * 100.0f);
    }
    if (s_walk_pct < 0)   s_walk_pct = 0;
    if (s_walk_pct > 100) s_walk_pct = 100;

    /* 环3：30s 心跳倒计时（板端自己发 GPS_REQ，连接成功即重置） */
    int cnt = ble_link_calib_countdown_s();
    s_cnt_max = 30;
    s_cnt_s = cnt < 0 ? 0 : (cnt > s_cnt_max ? s_cnt_max : cnt);

    /* 中心：当前前进路线 = 路名 + 转向提示 */
    if (st->route_ok) {
        const char *road = st->road ? st->road : "";
        const char *tip  = st->tip  ? st->tip  : "";
        if (road[0] && tip[0])      snprintf(buf, sizeof(buf), "%s %s", road, tip);
        else if (tip[0])            snprintf(buf, sizeof(buf), "%s", tip);
        else if (road[0])           snprintf(buf, sizeof(buf), "%s", road);
        else                        snprintf(buf, sizeof(buf), "沿路线行走");
    } else {
        snprintf(buf, sizeof(buf), "等待路线");
    }
    label_set_cached(s_lbl_tip, s_cache_tip, sizeof(s_cache_tip), buf);
    /* 粗体副本已移除（见 build 处注释：副本是分块刷新的花屏根因） */

    /* 中心：剩余里程（最小、最淡） */
    if (st->route_ok) {
        if (st->remain_m >= 1000.0f)
            snprintf(buf, sizeof(buf), "剩 %.1f km", (double)(st->remain_m / 1000.0f));
        else
            snprintf(buf, sizeof(buf), "剩 %.0f m", (double)st->remain_m);
    } else {
        snprintf(buf, sizeof(buf), "NO ROUTE");
    }
    label_set_cached(s_lbl_remain, s_cache_remain, sizeof(s_cache_remain), buf);

    /* 中心底部：MODE 状态（偏航整行变红）
     * 野外没有串口时，把"GPS 多久没来"直接暴露在界面上：
     * 超过 90s 没有任何 GPS_REQ 应答 → GPS LOST（红色），此时定位完全靠 PDR 裸推。 */
    const char *mode = "MODE: STANDBY";
    bool gps_lost = st->route_ok && st->gps_age_s > 90;
    if (!st->imu_ok)              mode = "MODE: IMU FAULT";
    else if (st->off_route_alert) mode = "MODE: OFF-ROUTE";
    else if (gps_lost)            mode = "MODE: GPS LOST";
    else if (!st->route_ok)       mode = "MODE: IDLE";
    else if (!st->stationary)     mode = "MODE: WALKING";
    label_set_cached(s_lbl_mode, s_cache_mode, sizeof(s_cache_mode), mode);
    lv_obj_remove_style(s_lbl_mode, NULL, 0);
    lv_obj_add_style(s_lbl_mode,
                     (st->off_route_alert || gps_lost) ? &sty_text_red : &sty_text_purple, 0);

    nav_ui_apply(speed);

    /* [取证] 每 30 s 一条：把 UI 真正拿到的比例/剩余里程/速度打出来。
     * 户外没有串口，只能靠"屏幕看到什么 + 这条日志"对齐判断环2 是否画对。 */
    {
        static uint32_t s_ui_log_next = 0;
        uint32_t ums = (uint32_t)(esp_timer_get_time() / 1000);
        if (ums >= s_ui_log_next) {
            s_ui_log_next = ums + 30000u;
            ESP_LOGI(TAG, "ui walk=%d%% remain=\"%s\" spd=%.2f route_ok=%d",
                     s_walk_pct, s_cache_remain, (double)speed, (int)st->route_ok);
        }
    }

    /* 一次性把本帧所有改动刷完再解锁：避免刷新分块之间被下一次 setText 打断造成撕裂 */
    if (s_text_dirty) {
        s_text_dirty = false;
        lv_refr_now(NULL);
    }

    if (s_toast_until_ms && (int64_t)(esp_timer_get_time() / 1000) > s_toast_until_ms) {
        lv_obj_add_flag(s_lbl_toast, LV_OBJ_FLAG_HIDDEN);
        s_toast_until_ms = 0;
    }

    bsp_display_unlock();
}

void ui_eva_toast(const char *msg)
{
    if (!s_lbl_toast) return;
    /* 提示条都是"要人看"的事件（偏航、路线更新、蓝牙连上）→ 顺手唤醒屏幕 */
    screen_pm_activity("toast");
    if (!ui_lock(50, 2)) return;
    lv_label_set_text(s_lbl_toast, msg ? msg : "");
    lv_obj_clear_flag(s_lbl_toast, LV_OBJ_FLAG_HIDDEN);
    bsp_display_unlock();
    s_toast_until_ms = esp_timer_get_time() / 1000 + 1800;
}

void ui_eva_status_line(const char *msg)
{
    if (!s_lbl_mode) return;
    if (!ui_lock(50, 2)) return;
    lv_label_set_text(s_lbl_mode, msg ? msg : "");
    bsp_display_unlock();
}


