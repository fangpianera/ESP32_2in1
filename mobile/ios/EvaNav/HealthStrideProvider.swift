//
//  HealthStrideProvider.swift
//  HealthKit 拉身高 + 近 7 天步数/步行距离 → 生成板端步长基准
//
//  为什么不用 HealthKit 的"步长"字段：系统没有这个量。
//  官方只有 stepCount / distanceWalkingRunning，所以：
//        avg_step = Σ distance / Σ steps        （近 7 天加权，比单日稳）
//        没有步数数据时退回身高经验式 L ≈ 0.415·h（男）/ 0.413·h（女）
//  conf（置信度）用来告诉板端"这个基准有多可信"，板端据此决定
//  健康基准与 Weinberg 实时估计的融合权重（见 net_link.c health_compute_base）。
//

import Foundation
import HealthKit

final class HealthStrideProvider {

    static let shared = HealthStrideProvider()

    private let store = HKHealthStore()
    private var cached: [String: Any]?

    private var readTypes: Set<HKObjectType> {
        var s = Set<HKObjectType>()
        if let h = HKQuantityType.quantityType(forIdentifier: .height) { s.insert(h) }
        if let st = HKQuantityType.quantityType(forIdentifier: .stepCount) { s.insert(st) }
        if let d = HKQuantityType.quantityType(forIdentifier: .distanceWalkingRunning) { s.insert(d) }
        if let bd = HKObjectType.characteristicType(forIdentifier: .dateOfBirth) { s.insert(bd) }
        if let sx = HKObjectType.characteristicType(forIdentifier: .biologicalSex) { s.insert(sx) }
        return s
    }

    func requestAuthorization(completion: @escaping (Bool) -> Void) {
        guard HKHealthStore.isHealthDataAvailable() else { completion(false); return }
        store.requestAuthorization(toShare: [], read: readTypes) { ok, err in
            if let err { print("[EVA] HealthKit auth: \(err.localizedDescription)") }
            completion(ok)
        }
    }

    /// 组装板端 HEALTH_PUSH 的 payload，并缓存一份供连接建立时立即推送
    func refresh(completion: ((Bool) -> Void)? = nil) {
        let group = DispatchGroup()
        var heightCm: Double?
        var sex = "m"
        var steps7d: Double = 0
        var dist7d: Double = 0
        var hasStepData = false

        group.enter()
        readHeight { h in heightCm = h; group.leave() }

        group.enter()
        readSex { s in sex = s; group.leave() }

        group.enter()
        readSum(type: .stepCount, unit: .count()) { v in
            steps7d = v; hasStepData = v > 0; group.leave()
        }

        group.enter()
        readSum(type: .distanceWalkingRunning, unit: .meter()) { v in dist7d = v; group.leave() }

        group.notify(queue: .main) {
            var payload: [String: Any] = [:]
            if let h = heightCm { payload["height_cm"] = h }
            payload["sex"] = sex

            var conf = 0.0
            if hasStepData && dist7d > 0 {
                let avg = dist7d / steps7d
                payload["avg_step_m"] = (avg * 1000).rounded() / 1000
                // 数据量越大越可信；7 天 1 万步以上视为高置信
                conf = min(1.0, steps7d / 10000.0)
            }
            payload["steps_7d"] = steps7d
            payload["dist_7d_m"] = dist7d
            payload["conf"] = (conf * 100).rounded() / 100
            payload["src"] = "healthkit-ios"

            self.cached = payload
            print("[EVA] health payload: \(payload)")
            completion?(true)
        }
    }

    func currentPayload() -> [String: Any]? { cached }

    // MARK: - 私有读取
    private func readHeight(_ done: @escaping (Double?) -> Void) {
        guard let t = HKQuantityType.quantityType(forIdentifier: .height) else { done(nil); return }
        let q = HKSampleQuery(sampleType: t, predicate: nil, limit: 1,
                              sortDescriptors: [NSSortDescriptor(key: HKSampleSortIdentifierStartDate, ascending: false)]) { _, samples, _ in
            guard let s = samples?.first as? HKQuantitySample else { done(nil); return }
            done(s.quantity.doubleValue(for: .meterUnit(with: .centi)))
        }
        store.execute(q)
    }

    private func readSex(_ done: @escaping (String) -> Void) {
        do {
            let s = try store.biologicalSex().biologicalSex
            done(s == .female ? "f" : "m")
        } catch {
            done("m")
        }
    }

    /// 近 7 天累计（HKStatisticsQuery 比逐条 sample 省电）
    private func readSum(type id: HKQuantityTypeIdentifier, unit: HKUnit, done: @escaping (Double) -> Void) {
        guard let t = HKQuantityType.quantityType(forIdentifier: id) else { done(0); return }
        let start = Calendar.current.date(byAdding: .day, value: -7, to: Date())!
        let pred = HKQuery.predicateForSamples(withStart: start, end: Date(), options: .strictStartDate)
        let q = HKStatisticsQuery(quantityType: t, quantitySamplePredicate: pred,
                                  options: .cumulativeSum) { _, stat, _ in
            done(stat?.sumQuantity()?.doubleValue(for: unit) ?? 0)
        }
        store.execute(q)
    }
}
