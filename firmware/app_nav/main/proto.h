/**
 * proto.h — ESP32 ⇄ 手机 应用层帧协议（v1）
 *
 * 设计取舍：
 *  - 为什么不直接用 HTTP：30s 一次的通联要"一问一答 + 极短窗口"，裸 TCP + 定长头 的开销
 *    比 HTTP 头小一个数量级，且便于在同一连接上混合"路线大包"和"GPS 小包"。
 *  - 为什么 payload 用 JSON：路线数据来自高德 SDK，天然是 JSON/对象；手机端先做
 *    "降维"（只保留 lat/lon/bearing/转向）再上送，板端 cJSON 解析后立刻落盘为二进制。
 *  - 帧头定长 20B + CRC32，杜绝 TCP 粘包/半包歧义（长度域唯一权威）。
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#define EVA_MAGIC        0x31415645u   /* 线序 'E','V','A','1' 的小端读法 */
#define EVA_PROTO_VER    1

typedef enum {
    EVA_T_ROUTE_PUSH   = 1,   /* 手机 → 板：完整步行路线 JSON */
    EVA_T_ROUTE_ACK    = 2,   /* 板 → 手机 */
    EVA_T_GPS_REQ      = 3,   /* 板 → 手机：请立即回传当前定位 */
    EVA_T_GPS_RSP      = 4,   /* 手机 → 板 */
    EVA_T_HEALTH_PUSH  = 5,   /* 手机 → 板：身高/步数/距离 → 步长基准 */
    EVA_T_HEALTH_ACK   = 6,
    EVA_T_STATUS       = 7,   /* 板 → 手机：电量/PDR 状态/漂移自评 */
    EVA_T_ERROR        = 8,
    EVA_T_TIME_SYNC    = 9,   /* 手机 → 板：unix 时间，用于 RTC/日志对齐 */
    EVA_T_PING         = 10,
    EVA_T_PONG         = 11,
} eva_msg_type_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;      /* EVA_MAGIC */
    uint8_t  ver;        /* EVA_PROTO_VER */
    uint8_t  type;       /* eva_msg_type_t */
    uint16_t flags;      /* 预留：bit0=payload 为分片, bit1=最后一片 */
    uint32_t seq;        /* 会话内自增，用于丢帧检测 */
    uint32_t len;        /* payload 字节数 */
    uint32_t crc32;      /* 仅对 payload 计算（IEEE 802.3 反射算法） */
} eva_hdr_t;             /* 20 字节 */

typedef enum {
    PROTO_OK = 0,
    PROTO_ERR_TIMEOUT,
    PROTO_ERR_IO,
    PROTO_ERR_MAGIC,
    PROTO_ERR_VER,
    PROTO_ERR_CRC,
    PROTO_ERR_TOO_BIG,
    PROTO_ERR_NOMEM,
} proto_err_t;

uint32_t eva_crc32(const void *data, size_t len);

/* 发送一帧；payload 可为 NULL(len=0)。返回 proto_err_t。 */
proto_err_t proto_send_frame(int fd, uint8_t type, uint32_t seq,
                             const void *payload, uint32_t len);

/* 把一帧序列化到一个连续缓冲（BLE 分片发送用）。
 * 返回总字节数（20 + len），cap 不足返回 0。 */
uint32_t proto_build_frame(uint8_t *out, uint32_t cap, uint8_t type, uint32_t seq,
                           const void *payload, uint32_t len);

/* 接收一帧：buf 由调用方提供（建议 PSRAM），收到后 *out_len 为实际 payload 长度。 */
proto_err_t proto_recv_frame(int fd, uint8_t *type, uint32_t *seq,
                             uint8_t *buf, uint32_t buf_cap, uint32_t *out_len);

/* 便捷：以 0 结尾的 JSON 文本发送 */
proto_err_t proto_send_json(int fd, uint8_t type, uint32_t seq, const char *json);

/* 会话级 seq 生成 */
uint32_t proto_next_seq(void);
