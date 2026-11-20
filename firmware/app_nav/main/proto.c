#include "proto.h"
#include "app_cfg.h"

#include <string.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/atomic.h"

static const char *TAG = "proto";
static atomic_uint_least32_t s_seq = 1;   /* 天然原子：32bit 读改写，单写者语义足够 */

uint32_t proto_next_seq(void)
{
    return atomic_fetch_add(&s_seq, 1u) + 1u;
}

/* CRC32 / IEEE 802.3：poly 0xEDB88320 反射，init 0xFFFFFFFF，xorout 0xFFFFFFFF */
uint32_t eva_crc32(const void *data, size_t len)
{
    static uint32_t table[256];
    static bool inited = false;
    if (!inited) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
        inited = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) {
        crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

static int write_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, p + off, len - off, 0);
        if (n <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

static int read_all(int fd, void *buf, size_t len)
{
    uint8_t *p = (uint8_t *)buf;
    size_t off = 0;
    while (off < len) {
        ssize_t n = recv(fd, p + off, len - off, 0);
        if (n == 0) {
            return -1;                     /* 对端关闭 */
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 1;                  /* 超时（由 SO_RCVTIMEO 决定），交调用方决策 */
            }
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

proto_err_t proto_send_frame(int fd, uint8_t type, uint32_t seq,
                             const void *payload, uint32_t len)
{
    eva_hdr_t h = {
        .magic = EVA_MAGIC,
        .ver   = EVA_PROTO_VER,
        .type  = type,
        .flags = 0,
        .seq   = seq,
        .len   = len,
        .crc32 = len ? eva_crc32(payload, len) : 0u,
    };
    if (write_all(fd, &h, sizeof(h)) != 0) {
        ESP_LOGW(TAG, "send hdr failed type=%u", type);
        return PROTO_ERR_IO;
    }
    if (len && write_all(fd, payload, len) != 0) {
        ESP_LOGW(TAG, "send payload failed type=%u len=%u", type, (unsigned)len);
        return PROTO_ERR_IO;
    }
    return PROTO_OK;
}

uint32_t proto_build_frame(uint8_t *out, uint32_t cap, uint8_t type, uint32_t seq,
                           const void *payload, uint32_t len)
{
    if (cap < sizeof(eva_hdr_t) + len) {
        return 0;
    }
    eva_hdr_t h = {
        .magic = EVA_MAGIC,
        .ver   = EVA_PROTO_VER,
        .type  = type,
        .flags = 0,
        .seq   = seq,
        .len   = len,
        .crc32 = len ? eva_crc32(payload, len) : 0u,
    };
    memcpy(out, &h, sizeof(h));                     /* 结构体 packed，字节序即小端 */
    if (len) {
        memcpy(out + sizeof(h), payload, len);
    }
    return (uint32_t)(sizeof(h) + len);
}

proto_err_t proto_send_json(int fd, uint8_t type, uint32_t seq, const char *json)
{
    return proto_send_frame(fd, type, seq, json, (uint32_t)strlen(json));
}

proto_err_t proto_recv_frame(int fd, uint8_t *type, uint32_t *seq,
                             uint8_t *buf, uint32_t buf_cap, uint32_t *out_len)
{
    eva_hdr_t h;
    int rc = read_all(fd, &h, sizeof(h));
    if (rc == 1) return PROTO_ERR_TIMEOUT;
    if (rc != 0) return PROTO_ERR_IO;

    if (h.magic != EVA_MAGIC) {
        ESP_LOGW(TAG, "bad magic 0x%08x", (unsigned)h.magic);
        return PROTO_ERR_MAGIC;            /* 流已错位，必须由上层断开重连 */
    }
    if (h.ver != EVA_PROTO_VER) return PROTO_ERR_VER;
    if (h.len > buf_cap) {
        ESP_LOGE(TAG, "payload %u > cap %u", (unsigned)h.len, (unsigned)buf_cap);
        return PROTO_ERR_TOO_BIG;
    }
    if (h.len) {
        rc = read_all(fd, buf, h.len);
        if (rc == 1) return PROTO_ERR_TIMEOUT;
        if (rc != 0) return PROTO_ERR_IO;
        if (eva_crc32(buf, h.len) != h.crc32) return PROTO_ERR_CRC;
    }
    *type = h.type;
    *seq = h.seq;
    *out_len = h.len;
    return PROTO_OK;
}
