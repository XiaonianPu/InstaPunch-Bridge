import Foundation

enum LogKind: String, CaseIterable, Identifiable {
    case system = "系统"
    case phone = "手机"
    case device = "设备"
    case down = "下行帧"
    case up = "上行"
    case control = "控制"

    var id: String { rawValue }
}

struct LogLine: Identifiable {
    let id: Int
    let time: Date
    let kind: LogKind
    let text: String
}

// Set by BridgeModel; called from the bridge queue.
var logSink: ((LogKind, String) -> Void)?

func log(_ kind: LogKind, _ text: String) {
    logSink?(kind, text)
}
