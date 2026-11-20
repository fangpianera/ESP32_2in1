//
//  CoordConv.swift
//  坐标系转换：WGS-84（手机 GNSS / iOS CoreLocation）→ GCJ-02（高德）
//
//  为什么必须转：
//    高德返回的路线点全部是 GCJ-02；iPhone 的 CLLocation 是 WGS-84。
//    直接把两者相减，在城区会差 50~500m —— 板端 EKF 会把"偏移"当成"漂移"，
//    每 30s 校准一次、每次拉偏几百米，走十分钟就彻底跑飞（doc 坑 1，最致命的一个）。
//    统一到 GCJ-02 后，路线与定位才在同一坐标系下可比较。
//

import Foundation
import CoreLocation

enum CoordConv {
    private static let a = 6378245.0
    private static let ee = 0.00669342162296594323

    /// 简化判断：大陆范围外不做偏移（港澳台/境外高德用的是 WGS-84 或本地坐标系）
    private static func outOfChina(_ lat: Double, _ lon: Double) -> Bool {
        lon < 72.004 || lon > 137.8347 || lat < 0.8293 || lat > 55.8271
    }

    static func wgs84ToGcj02(lat: Double, lon: Double) -> CLLocationCoordinate2D {
        if outOfChina(lat, lon) { return CLLocationCoordinate2D(latitude: lat, longitude: lon) }
        var dLat = transformLat(lon - 105.0, lat - 35.0)
        var dLon = transformLon(lon - 105.0, lat - 35.0)
        let radLat = lat / 180.0 * .pi
        var magic = sin(radLat)
        magic = 1 - ee * magic * magic
        let sqrtMagic = sqrt(magic)
        dLat = (dLat * 180.0) / ((a * (1 - ee)) / (magic * sqrtMagic) * .pi)
        dLon = (dLon * 180.0) / (a / sqrtMagic * cos(radLat) * .pi)
        return CLLocationCoordinate2D(latitude: lat + dLat, longitude: lon + dLon)
    }

    static func gcj02ToWgs84(lat: Double, lon: Double) -> CLLocationCoordinate2D {
        if outOfChina(lat, lon) { return CLLocationCoordinate2D(latitude: lat, longitude: lon) }
        // 迭代反解（3 次足够，误差 < 0.5m）
        var wgsLat = lat, wgsLon = lon
        for _ in 0..<3 {
            let g = wgs84ToGcj02(lat: wgsLat, lon: wgsLon)
            wgsLat += lat - g.latitude
            wgsLon += lon - g.longitude
        }
        return CLLocationCoordinate2D(latitude: wgsLat, longitude: wgsLon)
    }

    private static func transformLat(_ x: Double, _ y: Double) -> Double {
        var r = -100.0 + 2.0 * x + 3.0 * y + 0.2 * y * y + 0.1 * x * y + 0.2 * sqrt(abs(x))
        r += (20.0 * sin(6.0 * x * .pi) + 20.0 * sin(2.0 * x * .pi)) * 2.0 / 3.0
        r += (20.0 * sin(y * .pi) + 40.0 * sin(y / 3.0 * .pi)) * 2.0 / 3.0
        r += (160.0 * sin(y / 12.0 * .pi) + 320 * sin(y * .pi / 30.0)) * 2.0 / 3.0
        return r
    }

    private static func transformLon(_ x: Double, _ y: Double) -> Double {
        var r = 300.0 + x + 2.0 * y + 0.1 * x * x + 0.1 * x * y + 0.1 * sqrt(abs(x))
        r += (20.0 * sin(6.0 * x * .pi) + 20.0 * sin(2.0 * x * .pi)) * 2.0 / 3.0
        r += (20.0 * sin(x * .pi) + 40.0 * sin(x / 3.0 * .pi)) * 2.0 / 3.0
        r += (150.0 * sin(x / 12.0 * .pi) + 300.0 * sin(x / 30.0 * .pi)) * 2.0 / 3.0
        return r
    }
}
