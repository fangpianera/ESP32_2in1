//
//  EvaNavCoordinator.swift
//  把四个模块串起来的最小可运行闭环
//
//  时序：
//   启动 → HealthKit 授权 + 拉取 → 定位授权 + 开始更新 → 检测热点 → 连 TCP → 推健康/时间
//   连上后：板端发 GPS_REQ → 这里取 latest 快照回 GPS_RSP
//   用户选目的地 → 高德取整条步行路线 → 整包 pushRoute（一次）
//

import UIKit
import CoreLocation

final class EvaNavCoordinator {

    static let shared = EvaNavCoordinator()

    private let link = EvaLinkService()
    private let amap = AmapRouteProvider()
    private(set) var lastRouteDistance: Double = 0

    var onLog: ((String) -> Void)?

    func bootstrap() {
        // 1) 健康数据（先拿基准，板端一连上就有 step base）
        HealthStrideProvider.shared.requestAuthorization { ok in
            guard ok else { self.log("HealthKit 未授权：步长将退回身高经验式"); return }
            HealthStrideProvider.shared.refresh { _ in }
        }

        // 2) 定位
        LocationService.shared.onAuthorizationChange = { status in
            self.log("定位权限 = \(status.rawValue)")
        }
        LocationService.shared.requestPermission()
        LocationService.shared.start()

        // 3) 链路：注入"取快照"闭包（板端问就答）
        link.locationProvider = { LocationService.shared.latest }
        link.onStateChange = { [weak self] st in
            switch st {
            case .ready:      self?.log("已连上板端 192.168.4.1:8080")
            case .connecting: self?.log("连接中…")
            case .failed(let m): self?.log("链路异常：\(m)")
            case .idle:       break
            }
        }
        link.onRouteAck = { [weak self] ok, pts, dist in
            self?.log(ok ? "路线已下发：\(pts) 点 / \(Int(dist)) m" : "板端拒收路线")
        }
        link.start()
    }

    /// 导航入口：目的地高德坐标 → 取路线 → 下发
    func startNavigation(to destination: CLLocationCoordinate2D,
                         name: String,
                         completion: @escaping (Bool) -> Void) {
        guard let here = LocationService.shared.latest else {
            log("还没有定位，无法规划"); completion(false); return
        }
        amap.requestWalkingRoute(from: CLLocationCoordinate2D(latitude: here.lat, longitude: here.lon),
                                 to: destination) { [weak self] result in
            guard let self else { return }
            switch result {
            case .success(let r):
                self.lastRouteDistance = r.distanceMeters
                self.log("高德路线 \(r.pointsCount) 点 / \(r.stepsCount) 段 / \(Int(r.distanceMeters)) m")
                if r.json.count > 256 * 1024 {
                    self.log("路线 JSON \(r.json.count/1024)KB 超过板端 256KB 上限，请缩短起终点")
                    completion(false); return
                }
                self.link.pushRoute(r.json)
                HealthStrideProvider.shared.refresh { _ in self.link.pushHealth() }
                completion(true)
            case .failure(let e):
                self.log("取路线失败：\(e.localizedDescription)")
                completion(false)
            }
        }
    }

    func stopNavigation() { link.stop() }

    private func log(_ s: String) { print("[EVA] \(s)"); onLog?(s) }
}
