#include "route_store.h"
#include "app_cfg.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>       /* mkdir */
#include "proto.h"          /* eva_crc32 */
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include <sys/stat.h>
#include <unistd.h>
#include "esp_littlefs.h"

static const char *TAG = "route";

/* ---- 载入后的常驻视图（PSRAM）---- */
static route_hdr_t   *s_hdr  = NULL;
static route_pt_t    *s_pts  = NULL;
static route_step_t  *s_steps = NULL;
static size_t         s_npts = 0, s_nsteps = 0;

/* 等距圆柱：1 度纬度 ≈ 110540 m；1 度经度 ≈ 111320*cos(lat) m */
#define M_PER_DEG_LAT  110540.0f
#define M_PER_DEG_LON  111320.0f

static const char *const k_act_name[] = {
    [ACT_NONE] = "", [ACT_LEFT] = "左转", [ACT_RIGHT] = "右转",
    [ACT_LEFT_HARD] = "向左急转", [ACT_RIGHT_HARD] = "向右急转",
    [ACT_STRAIGHT] = "直行", [ACT_ARRIVE] = "到达终点", [ACT_UTURN] = "调头",
};

static float bearing_between(const route_pt_t *a, const route_pt_t *b)
{
    float lat0 = (float)a->lat / 1e7f;
    float dx = ((float)b->lon - (float)a->lon) / 1e7f * M_PER_DEG_LON * cosf(lat0 * (float)M_PI / 180.0f);
    float dy = ((float)b->lat - (float)a->lat) / 1e7f * M_PER_DEG_LAT;
    float brg = atan2f(dx, dy) * 180.0f / (float)M_PI;
    return brg < 0 ? brg + 360.0f : brg;
}

void route_geo_to_local(int32_t lat1e7, int32_t lon1e7, float *x_m, float *y_m)
{
    float lat0 = (float)s_hdr->origin_lat / 1e7f;
    *x_m = ((float)lon1e7 - (float)s_hdr->origin_lon) / 1e7f * M_PER_DEG_LON
           * cosf(lat0 * (float)M_PI / 180.0f);
    *y_m = ((float)lat1e7 - (float)s_hdr->origin_lat) / 1e7f * M_PER_DEG_LAT;
}

/* ---------------- JSON 解析 ---------------- */
static bool json_num_at(const cJSON *arr, int idx, double *out)
{
    const cJSON *it = cJSON_GetArrayItem(arr, idx);
    if (!cJSON_IsNumber(it)) return false;
    *out = it->valuedouble;
    return true;
}

/* 按 UTF-8 码点边界安全截断，避免 LVGL 渲染半个汉字 */
static void utf8_copy(char *dst, size_t cap, const char *src)
{
    if (!src || cap == 0) return;
    size_t n = strlen(src);
    if (n >= cap) {
        size_t cut = cap - 1;
        while (cut > 0 && ((uint8_t)src[cut] & 0xC0) == 0x80) cut--;
        n = cut;
    }
    memcpy(dst, src, n);
    dst[n] = 0;
}

esp_err_t route_store_ingest_json(const char *json, uint32_t len,
                                  uint16_t *out_npts, int32_t *out_dist_mm)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) {
        ESP_LOGE(TAG, "cJSON parse failed near: %.32s", json);
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ESP_FAIL;

    const cJSON *jpts = cJSON_GetObjectItem(root, "pts");
    const cJSON *jsteps = cJSON_GetObjectItem(root, "steps");
    if (!cJSON_IsArray(jpts)) {
        ESP_LOGE(TAG, "no 'pts' array");
        goto done;
    }
    int npts = cJSON_GetArraySize(jpts);
    if (npts < 2 || npts > EVA_ROUTE_PTS_MAX) {
        ESP_LOGE(TAG, "pts count %d out of range", npts);
        goto done;
    }
    int nsteps = cJSON_IsArray(jsteps) ? cJSON_GetArraySize(jsteps) : 0;
    if (nsteps > EVA_ROUTE_STEPS_MAX) {
        ESP_LOGW(TAG, "steps %d truncated to %d", nsteps, EVA_ROUTE_STEPS_MAX);
        nsteps = EVA_ROUTE_STEPS_MAX;
    }

    uint32_t pts_bytes = (uint32_t)npts * sizeof(route_pt_t);
    uint32_t stp_bytes = (uint32_t)nsteps * sizeof(route_step_t);
    uint32_t total = sizeof(route_hdr_t) + pts_bytes + stp_bytes;

    route_hdr_t *hdr = heap_caps_calloc(1, total, MALLOC_CAP_SPIRAM);
    if (!hdr) { err = ESP_ERR_NO_MEM; goto done; }
    route_pt_t   *pts = (route_pt_t *)(hdr + 1);
    route_step_t *stp = (route_step_t *)((uint8_t *)pts + pts_bytes);

    /* origin：优先取 origin 字段，否则用首点（保证与手机端坐标一致） */
    const cJSON *jorg = cJSON_GetObjectItem(root, "origin");
    double olat = 0, olon = 0;
    if (json_num_at(jorg, 0, &olat) && json_num_at(jorg, 1, &olon)) {
        hdr->origin_lat = (int32_t)llround(olat * 1e7);
        hdr->origin_lon = (int32_t)llround(olon * 1e7);
    }

    for (int i = 0; i < npts; i++) {
        const cJSON *p = cJSON_GetArrayItem(jpts, i);
        double la, lo, br = -1;
        if (!json_num_at(p, 0, &la) || !json_num_at(p, 1, &lo)) {
            ESP_LOGE(TAG, "pt[%d] malformed", i);
            goto cleanup_hdr;
        }
        json_num_at(p, 2, &br);
        pts[i].lat = (int32_t)llround(la * 1e7);
        pts[i].lon = (int32_t)llround(lo * 1e7);
        pts[i].bearing_deg = (br < 0) ? -1 : (int16_t)br;
        pts[i].flags = 0;
        if (i == 0 && !jorg) {
            hdr->origin_lat = pts[0].lat;
            hdr->origin_lon = pts[0].lon;
        }
    }

    /* 转向点：把 step 的 pt_index 回写到点标志，供 UI 打点 */
    for (int i = 0; i < nsteps; i++) {
        const cJSON *s = cJSON_GetArrayItem(jsteps, i);
        const cJSON *ji = cJSON_GetObjectItem(s, "i");
        const cJSON *jd = cJSON_GetObjectItem(s, "d");
        const cJSON *ja = cJSON_GetObjectItem(s, "act");
        const cJSON *jr = cJSON_GetObjectItem(s, "road");
        const cJSON *jt = cJSON_GetObjectItem(s, "tip");

        int idx = cJSON_IsNumber(ji) ? ji->valueint : -1;
        if (idx < 0 || idx >= npts) { nsteps = i; break; }

        stp[i].pt_index = (uint16_t)idx;
        stp[i].dist_next_m = cJSON_IsNumber(jd) ? (float)jd->valuedouble : 0.0f;
        uint8_t act = ACT_NONE;
        if (cJSON_IsString(ja)) {
            const char *a = ja->valuestring;
            if      (!strcmp(a, "left"))        act = ACT_LEFT;
            else if (!strcmp(a, "right"))       act = ACT_RIGHT;
            else if (!strcmp(a, "left_hard"))   act = ACT_LEFT_HARD;
            else if (!strcmp(a, "right_hard"))  act = ACT_RIGHT_HARD;
            else if (!strcmp(a, "straight"))    act = ACT_STRAIGHT;
            else if (!strcmp(a, "arrive"))      act = ACT_ARRIVE;
            else if (!strcmp(a, "uturn"))       act = ACT_UTURN;
        }
        stp[i].act = act;
        utf8_copy(stp[i].road, sizeof(stp[i].road), cJSON_IsString(jr) ? jr->valuestring : "");
        utf8_copy(stp[i].tip,  sizeof(stp[i].tip),
                  cJSON_IsString(jt) ? jt->valuestring : k_act_name[act]);
        pts[idx].flags |= 0x0001;
    }

    /* 距离与航向补全（手机未给 bearing 时本地推算，避免 UI 无航向） */
    float acc = 0.0f;
    for (int i = 0; i < npts; i++) {
        if (pts[i].bearing_deg < 0) {
            pts[i].bearing_deg = (int16_t)bearing_between(&pts[i], &pts[i + 1 < npts ? i + 1 : i]);
        }
        if (i + 1 < npts) {
            float x0, y0, x1, y1;
            /* 以首点为原点临时计算，正式转换在 load 之后 */
            float lat0r = (float)pts[0].lat / 1e7f * (float)M_PI / 180.0f;
            x0 = ((float)pts[i].lon - (float)pts[0].lon) / 1e7f * M_PER_DEG_LON * cosf(lat0r);
            y0 = ((float)pts[i].lat - (float)pts[0].lat) / 1e7f * M_PER_DEG_LAT;
            x1 = ((float)pts[i + 1].lon - (float)pts[0].lon) / 1e7f * M_PER_DEG_LON * cosf(lat0r);
            y1 = ((float)pts[i + 1].lat - (float)pts[0].lat) / 1e7f * M_PER_DEG_LAT;
            acc += hypotf(x1 - x0, y1 - y0);
        }
    }
    hdr->magic = ROUTE_MAGIC;
    hdr->version = ROUTE_VER;
    hdr->npts = (uint16_t)npts;
    hdr->nsteps = (uint16_t)nsteps;
    hdr->total_dist_mm = (int32_t)lroundf(acc * 1000.0f);
    hdr->crc32 = eva_crc32(pts, pts_bytes + stp_bytes);

    /* ---- 落盘（掉电安全：先写 .tmp 再 rename）---- */
    {
        char tmp[64];
        /* 首次上电时 /littlefs/route 目录还不存在，直接 fopen 会 ENOENT（实测：
         * "open /littlefs/route/current.bin.tmp failed" 导致整条路线丢弃并回失败 ACK） */
        mkdir("/littlefs/route", 0777);
        snprintf(tmp, sizeof(tmp), "%s.tmp", EVA_MPU_PATH);
        FILE *f = fopen(tmp, "wb");
        if (!f) {
            ESP_LOGE(TAG, "open %s failed", tmp);
            goto cleanup_hdr;
        }
        size_t w = fwrite(hdr, 1, total, f);
        fflush(f);
        fsync(fileno(f));
        fclose(f);
        if (w != total) {
            ESP_LOGE(TAG, "short write %u/%u", (unsigned)w, (unsigned)total);
            unlink(tmp);
            goto cleanup_hdr;
        }
        unlink(EVA_MPU_PATH);
        if (rename(tmp, EVA_MPU_PATH) != 0) {
            ESP_LOGE(TAG, "rename failed");
            goto cleanup_hdr;
        }
    }

    if (out_npts) *out_npts = (uint16_t)npts;
    if (out_dist_mm) *out_dist_mm = hdr->total_dist_mm;
    ESP_LOGI(TAG, "route saved pts=%d steps=%d dist=%.1fm", npts, nsteps, acc);
    err = ESP_OK;

cleanup_hdr:
    heap_caps_free(hdr);
done:
    cJSON_Delete(root);
    return err;
}

bool route_store_load(void)
{
    FILE *f = fopen(EVA_MPU_PATH, "rb");
    if (!f) {
        ESP_LOGW(TAG, "no cached route");
        return false;
    }
    route_hdr_t h;
    if (fread(&h, 1, sizeof(h), f) != sizeof(h) || h.magic != ROUTE_MAGIC || h.version != ROUTE_VER) {
        ESP_LOGE(TAG, "route header invalid");
        fclose(f);
        return false;
    }
    uint32_t nb = (uint32_t)h.npts * sizeof(route_pt_t) + (uint32_t)h.nsteps * sizeof(route_step_t);
    uint8_t *blob = heap_caps_malloc(nb, MALLOC_CAP_SPIRAM);
    if (!blob) { fclose(f); return false; }
    if (fread(blob, 1, nb, f) != nb) {
        ESP_LOGE(TAG, "route body truncated");
        heap_caps_free(blob); fclose(f);
        return false;
    }
    fclose(f);

    if (eva_crc32(blob, nb) != h.crc32) {
        ESP_LOGE(TAG, "route crc mismatch -> discard");
        heap_caps_free(blob);
        return false;
    }
    s_hdr = heap_caps_malloc(sizeof(route_hdr_t), MALLOC_CAP_SPIRAM);
    if (!s_hdr) { heap_caps_free(blob); return false; }
    memcpy(s_hdr, &h, sizeof(h));
    s_pts = (route_pt_t *)blob;
    s_steps = (route_step_t *)(blob + (uint32_t)h.npts * sizeof(route_pt_t));
    s_npts = h.npts;
    s_nsteps = h.nsteps;
    ESP_LOGI(TAG, "route loaded pts=%u steps=%u", (unsigned)s_npts, (unsigned)s_nsteps);
    return true;
}

bool route_store_ready(void) { return s_hdr && s_npts >= 2; }
const route_pt_t *route_point(size_t i) { return (s_hdr && i < s_npts) ? &s_pts[i] : NULL; }
size_t route_points_count(void) { return s_npts; }
/* 路线全长（米），取自 hdr（落盘时由点列累加得出） */
float route_total_dist_m(void) { return s_hdr ? ((float)s_hdr->total_dist_mm / 1000.0f) : 0.0f; }
const char *route_step_tip(int32_t i)  { return (i >= 0 && i < (int32_t)s_nsteps) ? s_steps[i].tip : ""; }
const char *route_step_road(int32_t i) { return (i >= 0 && i < (int32_t)s_nsteps) ? s_steps[i].road : ""; }

esp_err_t route_project(float x_m, float y_m, route_loc_t *out)
{
    if (!route_store_ready() || !out) return ESP_ERR_INVALID_STATE;

    float best_d2 = 1e30f, best_along = 0.0f, along = 0.0f;
    int best_seg = 0;
    float best_t = 0.0f;

    for (size_t i = 0; i + 1 < s_npts; i++) {
        float x0, y0, x1, y1;
        route_geo_to_local(s_pts[i].lat, s_pts[i].lon, &x0, &y0);
        route_geo_to_local(s_pts[i + 1].lat, s_pts[i + 1].lon, &x1, &y1);
        float dx = x1 - x0, dy = y1 - y0;
        float l2 = dx * dx + dy * dy;
        float seg_len = sqrtf(l2);
        float t = 0.0f;
        if (l2 > 1e-6f) {
            t = ((x_m - x0) * dx + (y_m - y0) * dy) / l2;
            if (t < 0) t = 0;
            if (t > 1) t = 1;
        }
        float px = x0 + t * dx, py = y0 + t * dy;
        float d2 = (x_m - px) * (x_m - px) + (y_m - py) * (y_m - py);
        if (d2 < best_d2) {
            best_d2 = d2;
            best_seg = (int)i;
            best_t = t;
            best_along = along + t * seg_len;
        }
        along += seg_len;
    }

    float total = (float)s_hdr->total_dist_mm / 1000.0f;
    out->seg_idx = best_seg;
    out->t = best_t;
    out->dist_along = best_along;
    out->remain_m = total - best_along;
    out->off_route_m = sqrtf(best_d2);
    out->seg_bearing = bearing_between(&s_pts[best_seg], &s_pts[best_seg + 1]);

    /* 下一个转向点 */
    out->next_step = -1;
    out->next_dist_m = 0.0f;
    for (size_t k = 0; k < s_nsteps; k++) {
        if (s_steps[k].pt_index > (uint16_t)best_seg) {
            float xw, yw, x0, y0;
            route_geo_to_local(s_pts[s_steps[k].pt_index].lat, s_pts[s_steps[k].pt_index].lon, &xw, &yw);
            route_geo_to_local(s_pts[best_seg].lat, s_pts[best_seg].lon, &x0, &y0);
            /* 用沿路线累加而非直线，避免绕路时距离失真 */
            float acc = hypotf(xw - x0, yw - y0);
            out->next_step = (int32_t)k;
            out->next_dist_m = acc;
            break;
        }
    }
    return ESP_OK;
}
