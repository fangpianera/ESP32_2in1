//
//  EvaProtocol.swift
//  EVA-Nav iOS / 协议层
//
//  与固件 proto.c 完全对称：20 字节帧头 + JSON payload + CRC32(IEEE)
//  帧头（小端）：magic(4) ver(1) type(1) flags(2) seq(4) len(4) crc32(4)
//

import Foundation

enum MsgType: UInt8 {
    case routePush  = 1
    case routeAck   = 2
    case gpsReq     = 3
    case gpsRsp     = 4
    case healthPush = 5
    case healthAck  = 6
    case status     = 7
    case error      = 8
    case timeSync   = 9
    case ping       = 10
    case pong       = 11
}

struct EvaFrame {
    let type: MsgType
    let seq: UInt32
    let payload: Data
}

enum EvaProtocol {
    static let magic: UInt32 = 0x3141_5645   // 'E','V','A','1'
    static let version: UInt8 = 1
    static let headerLen = 20

    private static let crcTable: [UInt32] = {
        (0..<256).map { i -> UInt32 in
            var c = UInt32(i)
            for _ in 0..<8 { c = (c & 1) != 0 ? (0xEDB8_8320 ^ (c >> 1)) : (c >> 1) }
            return c
        }
    }()

    static func crc32(_ data: Data) -> UInt32 {
        var crc: UInt32 = 0xFFFF_FFFF
        for b in data {
            crc = crcTable[Int((crc ^ UInt32(b)) & 0xFF)] ^ (crc >> 8)
        }
        return crc ^ 0xFFFF_FFFF
    }

    static func encode(type: MsgType, seq: UInt32, payload: Data) -> Data {
        var out = Data(capacity: headerLen + payload.count)
        func le<T: FixedWidthInteger>(_ v: T) { withUnsafeBytes(of: v.littleEndian) { out.append(contentsOf: $0) } }
        le(magic)
        out.append(version)
        out.append(type.rawValue)
        le(UInt16(0))               // flags
        le(seq)
        le(UInt32(payload.count))
        le(crc32(payload))
        out.append(payload)
        return out
    }

    /// 从缓冲区解析一帧；返回 nil 表示数据还不够（半包），caller 继续 receive
    static func decode(from buffer: inout Data) -> EvaFrame? {
        guard buffer.count >= headerLen else { return nil }
        let d = [UInt8](buffer.prefix(headerLen))
        let magicV = UInt32(d[0]) | (UInt32(d[1]) << 8) | (UInt32(d[2]) << 16) | (UInt32(d[3]) << 24)
        guard magicV == magic else {
            // 流错位：只能整条连接废弃，交给上层重连
            buffer.removeAll()
            return nil
        }
        let len = UInt32(d[12]) | (UInt32(d[13]) << 8) | (UInt32(d[14]) << 16) | (UInt32(d[15]) << 24)
        let total = headerLen + Int(len)
        guard buffer.count >= total else { return nil }

        let payload = buffer.subdata(in: headerLen..<total)
        let crcField = UInt32(d[16]) | (UInt32(d[17]) << 8) | (UInt32(d[18]) << 16) | (UInt32(d[19]) << 24)
        guard crc32(payload) == crcField else {
            buffer.removeFirst(total)
            return nil                     // CRC 错：丢这一帧，等下一帧
        }
        let typeRaw = d[5]
        let seq = UInt32(d[8]) | (UInt32(d[9]) << 8) | (UInt32(d[10]) << 16) | (UInt32(d[11]) << 24)
        buffer.removeFirst(total)
        guard let type = MsgType(rawValue: typeRaw) else { return nil }
        return EvaFrame(type: type, seq: seq, payload: payload)
    }

    /// 便捷：把 JSON 字典编码为 payload
    static func jsonData(_ obj: [String: Any]) -> Data {
        (try? JSONSerialization.data(withJSONObject: obj, options: [])) ?? Data("{}".utf8)
    }
}
