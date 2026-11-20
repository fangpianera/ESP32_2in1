//
//  LocationService.swift
//  后台持续定位 + 给板端提供"随时可答"的定位快照
//
//  设计要点：
//   1) 板端每 30s 只要一次坐标 → 我们不做"请求时临时定位"（冷启动可能 10s+），
//      而是维持 kCLLocationAccuracyBestForNavigation 持续更新，只维护最新快照。
//   2) allowsBackgroundLocationUpdates = true + location 后台模式，
//      否则锁屏 1~2 分钟后 App 被挂起，30s 应答链路必断（doc 坑 8）。
//

import Foundation
import CoreLocation
import UIKit

struct EvaLocationSnapshot {
    let lat: Double
    let lon: Double
    let horizontalAccuracy: Double
    let course: Double           // 度，真北；无效时 < 0
    let courseAccuracy: Double   // 度；无效时 999
    let speed: Double            // m/s
    let timestamp: Date

    /// 超过 20s 的定位不放给板端：板端 30s 校准一次，陈旧坐标会把漂移"钉死"
    var isFresh: Bool { Date().timeIntervalSince(timestamp) < 20 && horizontalAccuracy > 0 }
}

final class LocationService: NSObject, CLLocationManagerDelegate {

    static let shared = LocationService()

    private let manager = CLLocationManager()
    private(set) var latest: EvaLocationSnapshot?
    private var bgTask: UIBackgroundTaskIdentifier = .invalid

    var onAuthorizationChange: ((CLAuthorizationStatus) -> Void)?

    private override init() {
        super.init()
        manager.delegate = self
        manager.desiredAccuracy = kCLLocationAccuracyBestForNavigation
        manager.distanceFilter = kCLDistanceFilterNone
        manager.activityType = .fitness
        manager.pausesLocationUpdatesAutomatically = false   // 走路停停走走，别让它自己暂停
        if #available(iOS 14.0, *) { manager.showsBackgroundLocationIndicator = true }
    }

    func requestPermission() {
        manager.requestWhenInUseAuthorization()
        // 拿到 When In Use 后再升级 Always，直接 requestAlways 容易被系统忽略
    }

    func start() {
        guard CLLocationManager.locationServicesEnabled() else { return }
        if manager.authorizationStatus == .authorizedWhenInUse ||
           manager.authorizationStatus == .authorizedAlways {
            manager.allowsBackgroundLocationUpdates = true
            manager.startUpdatingLocation()
            beginBackgroundGrace()
        }
    }

    func stop() {
        manager.stopUpdatingLocation()
    }

    // 给"锁屏瞬间"多争取几十秒，用于把最后一帧 GPS 发出去
    private func beginBackgroundGrace() {
        guard bgTask == .invalid else { return }
        bgTask = UIApplication.shared.beginBackgroundTask(withName: "eva.gps") { [weak self] in
            guard let self, self.bgTask != .invalid else { return }
            UIApplication.shared.endBackgroundTask(self.bgTask)
            self.bgTask = .invalid
        }
    }

    // MARK: - CLLocationManagerDelegate
    func locationManagerDidChangeAuthorization(_ manager: CLLocationManager) {
        onAuthorizationChange?(manager.authorizationStatus)
        if manager.authorizationStatus == .authorizedWhenInUse {
            manager.requestAlwaysAuthorization()
        }
        if manager.authorizationStatus == .authorizedAlways { start() }
    }

    func locationManager(_ manager: CLLocationManager, didUpdateLocations locations: [CLLocation]) {
        guard let loc = locations.last else { return }
        // 过滤明显无效点：负精度/超时点会把卡尔曼拉偏
        guard loc.horizontalAccuracy > 0, loc.horizontalAccuracy < 65 else { return }
        latest = EvaLocationSnapshot(lat: loc.coordinate.latitude,
                                     lon: loc.coordinate.longitude,
                                     horizontalAccuracy: loc.horizontalAccuracy,
                                     course: loc.course,
                                     courseAccuracy: loc.courseAccuracy,
                                     speed: max(0, loc.speed),
                                     timestamp: loc.timestamp)
    }

    func locationManager(_ manager: CLLocationManager, didFailWithError error: Error) {
        print("[EVA] location error: \(error.localizedDescription)")
    }
}
