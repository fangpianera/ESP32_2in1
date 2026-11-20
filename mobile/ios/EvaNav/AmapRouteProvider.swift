//
//  AmapRouteProvider.swift
//  高德步行路线 → 板端精简 JSON
//
//  依赖：AMapFoundationKit / AMapSearchKit（CocoaPods: AMap3DMap + AMapSearch）
//  桥接头：EvaNav-Bridging-Header.h 里 #import <AMapSearchKit/AMapSearchKit.h>
//
//  输出格式（与固件 route_store.c 严格对齐）：
//  {
//    "origin": [lat, lon],
//    "pts":    [[lat, lon, bearing], ...]          // bearing 可为 -1，板端会自算
//    "steps":  [{"i": 转向点索引, "d": 距下一点米, "act": "left|right|left_hard|right_hard|straight|arrive|uturn",
//                "road": "路名", "tip": "中文提示"}, ...]
//  }
//

import Foundation
import CoreLocation

final class AmapRouteProvider: NSObject, AMapSearchDelegate {

    struct RouteResult {
        let json: Data
        let distanceMeters: Double
        let pointsCount: Int
        let stepsCount: Int
    }

    enum RouteError: LocalizedError {
        case noRoute, sdk(String)
        var errorDescription: String? {
            switch self {
            case .noRoute: return "高德未返回可行步行路线"
            case .sdk(let m): return "高德 SDK 错误：\(m)"
            }
        }
    }

    private let search = AMapSearchAPI()
    private var completion: ((Result<RouteResult, Error>) -> Void)?

    override init() {
        super.init()
        search?.delegate = self
    }

    /// 一次性取"整条步行路线"，由调用方整包下发（不做分段导航请求）
    func requestWalkingRoute(from origin: CLLocationCoordinate2D,
                             to destination: CLLocationCoordinate2D,
                             completion: @escaping (Result<RouteResult, Error>) -> Void) {
        self.completion = completion

        let req = AMapWalkingRouteSearchRequest()
        req.origin = AMapGeoPoint.location(withLatitude: CGFloat(origin.latitude),
                                           longitude: CGFloat(origin.longitude))
        req.destination = AMapGeoPoint.location(withLatitude: CGFloat(destination.latitude),
                                                longitude: CGFloat(destination.longitude))
        req.showFields = AMapWalkingRouteShowField.init(rawValue: 0)   // 默认字段即可
        search?.aMapWalkingRouteSearch(req)
    }

    // MARK: - AMapSearchDelegate
    func aMapSearchRequest(_ request: Any!, didFailWithError error: Error!) {
        completion?(.failure(RouteError.sdk(error?.localizedDescription ?? "unknown")))
        completion = nil
    }

    func onWalkingRouteSearchDone(_ request: AMapWalkingRouteSearchRequest!,
                                  response: AMapWalkingRouteSearchResponse!) {
        defer { completion = nil }
        guard let path = response?.route?.paths.first, let steps = path.steps as? [AMapStep], !steps.isEmpty else {
            completion?(.failure(RouteError.noRoute))
            return
        }

        // 1) 把每一步的 polyline（"lon,lat;lon,lat"）拼成整条折线
        var pts: [[Double]] = []
        var stepEndIndex: [Int] = []        // 每个 step 的终点在 pts 中的下标

        for step in steps {
            let raw = (step.polyline ?? "").split(separator: ";")
            for kv in raw {
                let pair = kv.split(separator: ",")
                guard pair.count >= 2,
                      let lon = Double(pair[0]), let lat = Double(pair[1]) else { continue }
                // 高德 polyline 顺序是 lon,lat —— 这里转成 [lat, lon]，别写反（经典坑）
                if let last = pts.last, abs(last[0] - lat) < 1e-7, abs(last[1] - lon) < 1e-7 { continue }
                pts.append([lat, lon, -1.0])
            }
            stepEndIndex.append(max(0, pts.count - 1))
        }

        guard pts.count >= 2, pts.count <= 4000 else {   // 板端 EVA_ROUTE_PTS_MAX 上限
            completion?(.failure(RouteError.sdk("点数 \(pts.count) 超出板端上限(4000)")))
            return
        }

        // 2) 逐点补 bearing（真北，顺时针 0..360），板端会用它在无 GPS 时对准箭头
        for i in 0..<pts.count {
            let j = min(i + 1, pts.count - 1)
            let brg = AmapRouteProvider.bearing(from: pts[i], to: pts[j])
            pts[i][2] = brg
        }

        // 3) 转向指令：高德 action 是中文，映射成板端枚举
        var stepObjs: [[String: Any]] = []
        for (k, step) in steps.enumerated() {
            let idx = stepEndIndex[k]
            let act = AmapRouteProvider.mapAction(step.action, iconType: step.iconType)
            var obj: [String: Any] = [
                "i": idx,
                "d": Double(step.distance ?? 0),
                "act": act,
                "road": step.road ?? "",
                "tip": AmapRouteProvider.tipText(step: step, act: act)
            ]
            if act == "arrive" { obj["d"] = 0.0 }
            stepObjs.append(obj)
            if idx < pts.count { /* 板端会自己打标志位 */ }
        }

        let origin = [pts[0][0], pts[0][1]]
        let payload: [String: Any] = [
            "origin": origin,
            "pts": pts,
            "steps": stepObjs,
            "dist_m": Double(path.distance ?? 0),
            "dur_s": Double(path.duration ?? 0),
            "src": "amap-ios"
        ]

        guard let data = try? JSONSerialization.data(withJSONObject: payload) else {
            completion?(.failure(RouteError.sdk("JSON 序列化失败")))
            return
        }
        completion?(.success(RouteResult(json: data,
                                         distanceMeters: Double(path.distance ?? 0),
                                         pointsCount: pts.count,
                                         stepsCount: steps.count)))
    }

    // MARK: - helpers
    private static func bearing(from a: [Double], to b: [Double]) -> Double {
        let lat1 = a[0] * .pi / 180, lat2 = b[0] * .pi / 180
        let dLon = (b[1] - a[1]) * .pi / 180
        let y = sin(dLon) * cos(lat2)
        let x = cos(lat1) * sin(lat2) - sin(lat1) * cos(lat2) * cos(dLon)
        var deg = atan2(y, x) * 180 / .pi
        if deg < 0 { deg += 360 }
        return (deg * 10).rounded() / 10
    }

    private static func mapAction(_ action: String?, iconType: Int32) -> String {
        let a = action ?? ""
        if a.contains("左") && (a.contains("急") || iconType == 4) { return "left_hard" }
        if a.contains("右") && (a.contains("急") || iconType == 5) { return "right_hard" }
        if a.contains("左") { return "left" }
        if a.contains("右") { return "right" }
        if a.contains("掉头") { return "uturn" }
        if a.contains("到达") || a.contains("终点") { return "arrive" }
        return "straight"
    }

    private static func tipText(step: AMapStep, act: String) -> String {
        // 只保留"动作 + 距离"，板端屏幕小，长句会被截断
        let d = Int(step.distance ?? 0)
        switch act {
        case "left", "left_hard":  return "左转 后 \(d) 米"
        case "right", "right_hard": return "右转 后 \(d) 米"
        case "uturn":              return "掉头 后 \(d) 米"
        case "arrive":             return "到达终点"
        default:
            let road = step.road ?? ""
            return road.isEmpty ? "直行 \(d) 米" : "沿\(road) \(d) 米"
        }
    }
}
