/**
 * EvaProtocol.ets —— 鸿蒙端协议层（与固件 proto.c / iOS EvaProtocol.swift 完全对称）
 *
 * 帧头 20B（小端）：magic(4) ver(1) type(1) flags(2) seq(4) len(4) crc32(4)
 * 注意 ArkTS 的 Buffer 读写下标是"字节偏移"，别和数组下标混用。
 */
import { buffer } from '@kit.ArkTS';

export enum MsgType {
  ROUTE_PUSH = 1,
  ROUTE_ACK = 2,
  GPS_REQ = 3,
  GPS_RSP = 4,
  HEALTH_PUSH = 5,
  HEALTH_ACK = 6,
  STATUS = 7,
  ERROR = 8,
  TIME_SYNC = 9,
  PING = 10,
  PONG = 11
}

export class EvaFrame {
  type: MsgType = MsgType.PING;
  seq: number = 0;
  payload: buffer.Buffer = buffer.from('');
}

const MAGIC: number = 0x31415645;
const VERSION: number = 1;
const HDR_LEN: number = 20;

const CRC_TABLE: number[] = (() => {
  const t: number[] = [];
  for (let i = 0; i < 256; i++) {
    let c: number = i;
    for (let k = 0; k < 8; k++) {
      c = ((c & 1) !== 0) ? (0xEDB88320 ^ (c >>> 1)) : (c >>> 1);
    }
    t.push(c >>> 0);
  }
  return t;
})();

export function crc32(data: buffer.Buffer): number {
  let crc: number = 0xFFFFFFFF;
  for (let i = 0; i < data.length; i++) {
    const idx: number = (crc ^ data[i]) & 0xFF;
    crc = (CRC_TABLE[idx] ^ (crc >>> 8)) >>> 0;
  }
  return (crc ^ 0xFFFFFFFF) >>> 0;
}

export function encode(type: MsgType, seq: number, payload: buffer.Buffer): buffer.Buffer {
  const out = buffer.alloc(HDR_LEN + payload.length);
  out.writeUInt32LE(MAGIC >>> 0, 0);
  out.writeUInt8(VERSION, 4);
  out.writeUInt8(type as number, 5);
  out.writeUInt16LE(0, 6);
  out.writeUInt32LE(seq >>> 0, 8);
  out.writeUInt32LE(payload.length, 12);
  out.writeUInt32LE(crc32(payload), 16);
  payload.copy(out, HDR_LEN, 0, payload.length);
  return out;
}

/**
 * 从累积缓冲区解一帧；返回 null 表示数据不足。
 * 粘包/半包全部由 len 域决定，和板端同一套规则。
 */
export function decode(acc: buffer.Buffer): EvaFrame | null {
  if (acc.length < HDR_LEN) {
    return null;
  }
  const magic: number = acc.readUInt32LE(0);
  if (magic !== (MAGIC >>> 0)) {
    throw new Error('bad magic, stream desync');
  }
  const len: number = acc.readUInt32LE(12);
  if (acc.length < HDR_LEN + len) {
    return null;
  }
  const payload: buffer.Buffer = buffer.alloc(len);
  acc.copy(payload, 0, HDR_LEN, HDR_LEN + len);
  const crcField: number = acc.readUInt32LE(16);
  const f = new EvaFrame();
  f.type = acc.readUInt8(5) as MsgType;
  f.seq = acc.readUInt32LE(8);
  f.payload = payload;
  if (crc32(payload) !== (crcField >>> 0)) {
    // CRC 错：丢掉这一帧，不重传（板端下一秒会重发 GPS_REQ）
    s_dropCrc++;
    return null;
  }
  return f;
}

/**
 * 说明：ArkTS 里 Buffer 无法直接"删除前 n 字节"，所以调用方维护
 * 一个 consumed 偏移，用 slice 生成新的累积缓冲（见 EvaLink.drain）。
 */
export let s_dropCrc: number = 0;
export function headerLen(): number { return HDR_LEN; }
