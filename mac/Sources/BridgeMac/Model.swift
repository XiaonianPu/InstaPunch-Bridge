import Foundation
import SwiftUI

@MainActor
final class BridgeModel: ObservableObject {
    static let logMax = 3000

    @Published var snapshot = Snapshot()
    @Published var lines: [LogLine] = []
    @Published var shown: Set<LogKind> = Set(LogKind.allCases)

    private var nextId = 0
    private var pending: [(LogKind, String, Date)] = []
    private var flushScheduled = false
    private(set) var bridge: Bridge!

    init() {
        logSink = { [weak self] kind, text in
            let now = Date()
            DispatchQueue.main.async { self?.append(kind, text, now) }
        }
        bridge = Bridge { [weak self] s in self?.snapshot = s }
    }

    private func append(_ kind: LogKind, _ text: String, _ time: Date) {
        pending.append((kind, text, time))
        guard !flushScheduled else { return }
        flushScheduled = true
        // Batch bursts of frames into one view update.
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) { [weak self] in self?.flush() }
    }

    private func flush() {
        flushScheduled = false
        for (kind, text, time) in pending {
            lines.append(LogLine(id: nextId, time: time, kind: kind, text: text))
            nextId += 1
        }
        pending.removeAll()
        if lines.count > Self.logMax {
            lines.removeFirst(lines.count - Self.logMax)
        }
    }

    func clearLog() {
        lines.removeAll()
    }

    func perform(_ work: @escaping (Bridge) -> Void) {
        bridge.perform(work)
    }
}
