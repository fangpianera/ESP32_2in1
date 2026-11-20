//
//  EvaLinkService.swift
//  与 ESP32 的 TCP 长连接（板端是 Server，手机是 Client）
//
//  为什么手机做 Client：
//    板端 SoftAP 的 IP 固定 192.168.4.1，手机侧的 IP 由 DHCP 分配且不固定；
//    板端反向 connect 手机的难度和不确定性都更高，且每 30s 一次的重连
//    会让板端承担 accept 风暴。改成"手机常连、板端发问"，链路状态由手机维护。
//

import Foundation
import Network
import CoreLocation

final class EvaLinkService {

    enum LinkState { case idle, connecting, ready, failed(String) }

    static let host = "192.168.4.1"
    static let port: UInt16 = 8080

    var onStateChange: ((LinkState) -> Void)?
    var onRouteAck: ((Bool, Int, Double) -> Void)?     // ok, pts, dist_m

    private var conn: NWConnection?
    private var rxBuffer = Data()
    private var seq: UInt32 = 1
    private var reconnectDelay: TimeInterval = 0.5
    private let queue = DispatchQueue(label: "eva.link")
    private var lastGpsReqAt: Date = .distantPast
    private var wantConnect = false

    /// 由 LocationService 注入：拿到"当前最新定位"，板端问就答
    var locationProvider: (() -> EvaLocationSnapshot?)?

    // MARK: - 连接管理
    func start() {
        wantConnect = true
        connect()
    }

    func stop() {
        wantConnect = false
        conn?.cancel()
        conn = nil
        rxBuffer.removeAll()
        onStateChange?(.idle)
    }

    private func connect() {
        guard wantConnect else { return }
        guard NetworkMonitor.shared.isOnEvaHotspot else {
            onStateChange?(.failed("未连接到 EVA-NAV-xxxx 热点"))
            scheduleReconnect()
            return
        }

        let tcp = NWProtocolTCP.Options()
        tcp.noDelay = true                 // 30s 通联窗口很短，必须关 Nagle
        tcp.connectionTimeout = 3
        tcp.enableKeepalive = true
        tcp.keepaliveIdle = 5

        let params = NWParameters(tls: nil, tcp: tcp)
        // 关键：强制走 Wi-Fi 接口。热点没有外网，iOS 默认会把 TCP 丢给蜂窝，
        // 表现为"连接超时但网络正常"（最常见的一个坑，doc 坑 7）。
        params.requiredInterfaceType = .wifi
        params.allowLocalEndpointReuse = true

        let c = NWConnection(host: NWEndpoint.Host(Self.host),
                             port: NWEndpoint.Port(rawValue: Self.port)!,
                             using: params)
        conn = c
        onStateChange?(.connecting)

        c.stateUpdateHandler = { [weak self] state in
            guard let self else { return }
            switch state {
            case .ready:
                self.reconnectDelay = 0.5
                self.onStateChange?(.ready)
                self.receiveLoop()
                self.sendTimeSync()
                self.sendHealthIfAvailable()
            case .failed(let err):
                self.onStateChange?(.failed(err.localizedDescription))
                self.teardownAndRetry()
            case .waiting(let err):
                // waiting 往往是热点还没连上，别立刻失败
                self.onStateChange?(.failed("等待网络：\(err.localizedDescription)"))
                self.conn?.cancel()
                self.scheduleReconnect()
            default:
                break
            }
        }
        c.start(queue: queue)
    }

    private func teardownAndRetry() {
        conn?.cancel()
        conn = nil
        rxBuffer.removeAll()
        scheduleReconnect()
    }

    private func scheduleReconnect() {
        guard wantConnect else { return }
        queue.asyncAfter(deadline: .now() + reconnectDelay) { [weak self] in
            self?.reconnectDelay = min(4.0, (self?.reconnectDelay ?? 0.5) * 2)
            self?.connect()
        }
    }

    // MARK: - 接收
    private func receiveLoop() {
        conn?.receive(minimumIncompleteLength: 1, maximumLength: 64 * 1024) { [weak self] data, _, isComplete, error in
            guard let self else { return }
            if let data, !data.isEmpty {
                self.rxBuffer.append(data)
                self.drainFrames()
            }
            if let error {
                self.onStateChange?(.failed(error.localizedDescription))
                self.teardownAndRetry()
                return
            }
            if isComplete {
                self.teardownAndRetry()
                return
            }
            self.receiveLoop()
        }
    }

    private func drainFrames() {
        while let frame = EvaProtocol.decode(from: &rxBuffer) {
            handle(frame)
        }
    }

    private func handle(_ f: EvaFrame) {
        switch f.type {
        case .gpsReq:
            guard Date().timeIntervalSince(lastGpsReqAt) > 3 else { return }   // 去抖，防板端重发
            lastGpsReqAt = Date()
            guard let fix = locationProvider?(), fix.isFresh else {
                sendStatus(ok: false, note: "no-fix")
                return
            }
            // 必须转 GCJ-02：路线点是高德坐标系，WGS-84 直接相减会差几百米（CoordConv.swift 顶部说明）
            let gcj = CoordConv.wgs84ToGcj02(lat: fix.lat, lon: fix.lon)
            let obj: [String: Any] = [
                "lat": gcj.latitude,
                "lon": gcj.longitude,
                "acc": fix.horizontalAccuracy,
                "course": fix.course >= 0 ? fix.course : -1,
                "course_acc": fix.courseAccuracy,
                "speed": fix.speed,
                "ts": Int(Date().timeIntervalSince1970)
            ]
            send(type: .gpsRsp, json: obj)
        case .routeAck:
            if let d = try? JSONSerialization.jsonObject(with: f.payload) as? [String: Any] {
                onRouteAck?(d["ok"] as? Bool ?? false,
                            d["pts"] as? Int ?? 0,
                            d["dist_m"] as? Double ?? 0)
            }
        case .healthAck:
            if let d = try? JSONSerialization.jsonObject(with: f.payload) as? [String: Any] {
                print("[EVA] 板端步长基准 = \(d["base_step_m"] ?? "-")")
            }
        case .status:
            if let s = String(data: f.payload, encoding: .utf8) { print("[EVA] board: \(s)") }
        case .ping:
            send(type: .pong, json: [:])
        default:
            break
        }
    }

    // MARK: - 发送
    private func nextSeq() -> UInt32 { seq &+= 1; return seq }

    func send(type: MsgType, json: [String: Any]) {
        send(type: type, payload: EvaProtocol.jsonData(json))
    }

    func send(type: MsgType, payload: Data) {
        guard let conn, conn.state == .ready else { return }
        let frame = EvaProtocol.encode(type: type, seq: nextSeq(), payload: payload)
        conn.send(content: frame, completion: .contentProcessed { err in
            if let err { print("[EVA] send \(type) failed: \(err)") }
        })
    }

    /// 整包下发路线（一次性，不分片；>200KB 时板端会拒绝，由界面提示缩小范围）
    func pushRoute(_ json: Data) {
        send(type: .routePush, payload: json)
    }

    private func sendHealthIfAvailable() {
        guard let p = HealthStrideProvider.shared.currentPayload() else { return }
        send(type: .healthPush, json: p)
    }

    func pushHealth() { sendHealthIfAvailable() }

    private func sendTimeSync() {
        send(type: .timeSync, json: ["unix": Int(Date().timeIntervalSince1970),
                                     "tz": TimeZone.current.secondsFromGMT() / 60])
    }

    private func sendStatus(ok: Bool, note: String) {
        send(type: .status, json: ["ok": ok, "note": note])
    }
}

/// 热点判定：板端 SSID 前缀 "EVA-NAV-"，配合 Info.plist 的 NSLocalNetworkUsageDescription
final class NetworkMonitor {
    static let shared = NetworkMonitor()
    private(set) var isOnEvaHotspot = false
    private let monitor = NWPathMonitor(requiredInterfaceType: .wifi)

    private init() {
        monitor.pathUpdateHandler = { [weak self] path in
            // NWPath 拿不到 SSID（iOS 14+ 需 entitlement），这里先用"有 Wi-Fi 且无外网"近似，
            // 真正判定见 README：也可用 NEHotspotNetwork.fetchCurrent（需 Access WiFi Info 权限）。
            self?.isOnEvaHotspot = (path.status == .satisfied && path.usesInterfaceType(.wifi))
        }
        monitor.start(queue: DispatchQueue(label: "eva.net.monitor"))
    }
}
