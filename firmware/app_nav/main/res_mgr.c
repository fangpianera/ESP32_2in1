#include "res_mgr.h"
#include "app_cfg.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include "esp_log.h"
#include "esp_littlefs.h"
#include "esp_heap_caps.h"
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "res";
static bool       s_mounted = false;
static lv_font_t *s_font_nav = NULL;
static lv_font_t *s_font_num = NULL;

/* 内嵌中文字体：GB2312 全字库（7614 字 + ASCII），由 tools/build_fonts.ps1 用
 * lv_font_conv 从 Noto Sans SC(OFL) 生成，18px/4bpp。
 * 早期版本只收了 ~500 个高频字，路名/店名一出现生僻字就是方块（实测 2026-11-19），
 * 所以改成整库内嵌 —— 不再依赖 LittleFS 里的 nav18.bin 子集。 */
extern const lv_font_t cjk18;
/* UI 同款机甲数字字体（Chakra Petch SemiBold 48，fonts/eva_tech_48.c），作 num48 兜底 */
extern const lv_font_t eva_tech_48;

/* ---------------- 资源镜像（EMBED_FILES 编进固件的二进制）----------------
 * assets/nerv64.bin → 链接器符号 _binary_nerv64_bin_start/_end。 */
extern const uint8_t _binary_nerv64_bin_start[] asm("_binary_nerv64_bin_start");
extern const uint8_t _binary_nerv64_bin_end[]   asm("_binary_nerv64_bin_end");
extern const uint8_t _binary_num48_bin_start[]  asm("_binary_num48_bin_start");
extern const uint8_t _binary_num48_bin_end[]    asm("_binary_num48_bin_end");
extern const uint8_t _binary_nerv_bg_bin_start[] asm("_binary_nerv_bg_bin_start");
extern const uint8_t _binary_nerv_bg_bin_end[]   asm("_binary_nerv_bg_bin_end");

/* FNV-1a 32bit：用于判断 flash 上的资源是否与固件内嵌版本一致。
 * 只比长度会在"新版资源与旧版等长"时误判为已就绪（图标就是这种情况），
 * 因此这里做内容校验；三份资源合计 ~130KB，开机多读一次代价可忽略。 */
static uint32_t res_fnv1a(const uint8_t *p, size_t n, uint32_t h)
{
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static uint32_t res_file_hash(const char *path, size_t need, bool *ok)
{
    FILE *r = fopen(path, "rb");
    if (!r) { *ok = false; return 0; }
    uint32_t h = 2166136261u;
    uint8_t  buf[512];
    size_t   n, total = 0;
    while ((n = fread(buf, 1, sizeof(buf), r)) > 0) { h = res_fnv1a(buf, n, h); total += n; }
    fclose(r);
    *ok = (total == need);
    return h;
}

/* 把编进固件的资源落到 LittleFS 资源分区：已存在且内容一致就跳过。
 * 只写文件，不格式化分区；分区被擦或首次挂载后自动重建。 */
static esp_err_t res_provision_one(const char *dst, const uint8_t *start, const uint8_t *end)
{
    size_t need = (size_t)(end - start);
    uint32_t want = res_fnv1a(start, need, 2166136261u);
    struct stat st;

    if (stat(dst, &st) == 0 && (size_t)st.st_size == need) {
        bool ok = false;
        uint32_t got = res_file_hash(dst, need, &ok);
        if (ok && got == want) {
            ESP_LOGI(TAG, "资源就绪 %s (%u B, hash %08x)", dst, (unsigned)need, (unsigned)want);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "资源校验不符 %s (hash %08x != %08x)，重写", dst,
                 (unsigned)got, (unsigned)want);
    }
    FILE *f = fopen(dst, "wb");
    if (!f) {
        ESP_LOGW(TAG, "资源写入失败 %s", dst);
        return ESP_FAIL;
    }
    size_t wr = fwrite(start, 1, need, f);
    fclose(f);
    if (wr != need) {
        ESP_LOGW(TAG, "资源写入不完整 %s (%u/%u)", dst, (unsigned)wr, (unsigned)need);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "资源自举 %s (%u B, hash %08x)", dst, (unsigned)need, (unsigned)want);
    return ESP_OK;
}

static void res_provision(void)
{
    mkdir("/littlefs/fonts", 0777);
    mkdir("/littlefs/icon", 0777);
    res_provision_one("/littlefs/icon/nerv64.bin",
                      _binary_nerv64_bin_start, _binary_nerv64_bin_end);
    res_provision_one("/littlefs/icon/nerv_bg.bin",
                      _binary_nerv_bg_bin_start, _binary_nerv_bg_bin_end);
    res_provision_one("/littlefs/fonts/num48.bin",
                      _binary_num48_bin_start, _binary_num48_bin_end);
}

/* ---------------- LVGL 文件系统驱动（POSIX 直通 LittleFS）---------------- */
static void *fs_open(lv_fs_drv_t *drv, const char *path, lv_fs_mode_t mode)
{
    char full[160];
    snprintf(full, sizeof(full), "/littlefs%s", path);
    const char *m = (mode == LV_FS_MODE_WR) ? "wb"
                  : (mode == LV_FS_MODE_RD) ? "rb" : "rb+";
    FILE *f = fopen(full, m);
    if (!f) {
        ESP_LOGW(TAG, "open %s failed (%s)", full, strerror(errno));
        return NULL;
    }
    return f;
}

static lv_fs_res_t fs_close(lv_fs_drv_t *drv, void *fp)
{
    if (fp) fclose((FILE *)fp);
    return LV_FS_RES_OK;
}

static lv_fs_res_t fs_read(lv_fs_drv_t *drv, void *fp, void *buf, uint32_t btr, uint32_t *br)
{
    size_t n = fread(buf, 1, btr, (FILE *)fp);
    if (br) *br = (uint32_t)n;
    return LV_FS_RES_OK;
}

static lv_fs_res_t fs_seek(lv_fs_drv_t *drv, void *fp, uint32_t pos, lv_fs_whence_t whence)
{
    int w = (whence == LV_FS_SEEK_CUR) ? SEEK_CUR
          : (whence == LV_FS_SEEK_END) ? SEEK_END : SEEK_SET;
    return (fseek((FILE *)fp, (long)pos, w) == 0) ? LV_FS_RES_OK : LV_FS_RES_UNKNOWN;
}

static lv_fs_res_t fs_tell(lv_fs_drv_t *drv, void *fp, uint32_t *pos)
{
    long p = ftell((FILE *)fp);
    if (p < 0) return LV_FS_RES_UNKNOWN;
    *pos = (uint32_t)p;
    return LV_FS_RES_OK;
}

static void lv_fs_posix_register(void)
{
    static lv_fs_drv_t drv;
    lv_fs_drv_init(&drv);
    drv.letter = 'F';
    drv.cache_size = 0;
    drv.open_cb = fs_open;
    drv.close_cb = fs_close;
    drv.read_cb = fs_read;
    drv.seek_cb = fs_seek;
    drv.tell_cb = fs_tell;
    lv_fs_drv_register(&drv);
}

/* ---------------- 初始化 ---------------- */
esp_err_t res_mgr_init(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = "/littlefs",
        .partition_label = "storage",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "littlefs mount failed: %s", esp_err_to_name(err));
        return err;
    }
    s_mounted = true;

    size_t total = 0, used = 0;
    esp_littlefs_info("storage", &total, &used);
    ESP_LOGI(TAG, "littlefs %u/%u KB used", (unsigned)(used / 1024), (unsigned)(total / 1024));

    lv_fs_posix_register();
    res_provision();

    /* 中文：直接用内嵌 GB2312 全字库（flash 常驻，字形按需取，不占额外 RAM）。
     * 早期试过 lv_binfont_create 加载 LittleFS 里的子集 bin，但子集缺字就是方块，
     * 且 binfont 会把整个文件读进堆里，反而不划算。 */
    s_font_nav = (lv_font_t *)&cjk18;

#if LVGL_VERSION_MAJOR >= 9
    s_font_num = lv_binfont_create("F:/fonts/num48.bin");
#else
    s_font_num = lv_font_load("F:/fonts/num48.bin");
#endif
    if (!s_font_num) ESP_LOGW(TAG, "num font missing -> 回退内嵌 eva_tech_48");

    ESP_LOGI(TAG, "字体来源: cjk=%s(%d px, GB2312 7614 字) num48=%s",
             s_font_nav ? "embedded-GB2312" : "none",
             s_font_nav ? s_font_nav->line_height : 0,
             s_font_num ? "flash-bin" : "embedded-eva_tech_48");
    return ESP_OK;
}

bool res_fs_ready(void) { return s_mounted; }
lv_font_t *res_font_nav(void) { return s_font_nav; }
/* 兜底用 UI 同款机甲字体（Chakra Petch 48），不再为它保留 vg_lite Montserrat_48
 * —— 这里改完后 sdkconfig 可把 CONFIG_LV_FONT_MONTSERRAT_14/48 全部关掉，省 ~40KB Flash。 */
lv_font_t *res_font_num(void) { return s_font_num ? s_font_num : (lv_font_t *)&eva_tech_48; }

lv_obj_t *res_icon_create(lv_obj_t *parent, const char *name, int size_px)
{
    lv_obj_t *img = lv_image_create(parent);
    char path[96];
    snprintf(path, sizeof(path), "F:/icon/%s%d.bin", name, size_px);

    /* .bin 图标由 LVGL 的 bin 解码器按头部元信息解析，无需预知格式 */
    lv_image_set_src(img, path);

    const void *src = lv_image_get_src(img);
    ESP_LOGI(TAG, "icon %s -> %s", path, src ? "bin-image" : "符号兜底");
    if (!src) {
        /* 资源缺失：删掉空控件，回落符号，避免 UI 出现空洞 */
        lv_obj_delete(img);
        lv_obj_t *sym = lv_label_create(parent);
        lv_label_set_text(sym, LV_SYMBOL_RIGHT);
        return sym;
    }
    return img;
}

uint32_t res_cache_flush(void)
{
    /* 估算：96x96 ARGB8888 = 36,864 B/个 */
    return 0;
}
