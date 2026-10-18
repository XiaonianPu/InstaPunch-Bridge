import BridgeFrame
import Foundation

// Port of main/pipeline.c. Everything runs on the bridge queue, so there is no
// lock. Differences from the firmware:
// - CoreBluetooth has no per-notification confirmation. updateValue returning
//   false means the transmit queue is full: the frame is kept and sent first
//   when peripheralManagerIsReady fires, so back-pressure never loses data.
// - The downstream write is a GATT write (links write → uplinkWriteDone)
//   instead of esp_spp_write.

protocol PipelinePhone: AnyObject {
    func notifyData(_ frame: Data) -> Bool
    func sendControl(_ line: String)
}

protocol PipelineLinks: AnyObject {
    func write(slot: Int, record: Data) -> Bool
}

final class SlotPipe {
    let down = UnsafeMutablePointer<down_stream_t>.allocate(capacity: 1)
    let window = UnsafeMutablePointer<ack_window_t>.allocate(capacity: 1)
    let up = UnsafeMutablePointer<up_stream_t>.allocate(capacity: 1)
    var hold = false
    var linked = false
    var upBusy = false // a record is being written to the device
    var downLostReported: UInt32 = 0
    var upLostReported: UInt32 = 0

    init(channel: UInt8) {
        down_init(down, channel)
        window_init(window)
        up_init(up)
    }
}

struct PipeInfo {
    var raw: Bool
    var hold: Bool
    var epoch: UInt8
    var downLost: UInt32
    var upLost: UInt32
}

final class Pipeline {
    static let pumpPeriodMs = 10
    static let localMaxFrame = 517 - 3 // firmware LOCAL_MTU 517
    static let lostEventPeriodMs: UInt32 = 1000

    weak var phone: PipelinePhone?
    weak var links: PipelineLinks?
    var onFrame: ((Data, Bool) -> Void)? // (frame, resent) for the log

    private let queue: DispatchQueue
    private let pipes = (0..<slotCount).map { SlotPipe(channel: UInt8($0 + 1)) }
    private let frame = UnsafeMutablePointer<UInt8>.allocate(capacity: Int(FRAME_HDR_LEN + FRAME_AGE_LEN) + 520)
    private var timer: DispatchSourceTimer?
    private var wakeScheduled = false

    private var phoneReady = false
    private var maxUpdate = 20 // MTU 23 - 3
    private var congested = false
    private var blocked: (slot: Int, frame: Data)?
    private var nextSlot = 0
    private var lastLostEventMs: UInt32 = 0

    init(queue: DispatchQueue) {
        self.queue = queue
    }

    func start() {
        let t = DispatchSource.makeTimerSource(queue: queue)
        t.schedule(deadline: .now(), repeating: .milliseconds(Self.pumpPeriodMs))
        t.setEventHandler { [weak self] in self?.pump() }
        t.resume()
        timer = t
    }

    private func wake() {
        guard !wakeScheduled else { return }
        wakeScheduled = true
        queue.async { [weak self] in
            self?.wakeScheduled = false
            self?.pump()
        }
    }

    private func maxFrame(_ pipe: SlotPipe) -> Int {
        var limit = min(maxUpdate, Self.localMaxFrame)
        if pipe.hold { limit = min(limit, Int(FRAME_HOLD_MAX)) }
        return limit
    }

    private static func payloadLen(_ frame: UnsafePointer<UInt8>, _ len: Int) -> UInt32 {
        var view = frame_view_t()
        return frame_parse(frame, len, &view) ? UInt32(view.payload_len) : 0
    }

    private var gateOpen: Bool { phoneReady && !congested && blocked == nil }

    private func send(slot: Int, _ data: Data, resent: Bool) {
        onFrame?(data, resent)
        if phone?.notifyData(data) != true {
            blocked = (slot, data)
            congested = true
        }
    }

    // Sends one frame for the slot if it has one.
    private func pumpSlotDown(_ i: Int, now: UInt32) -> Bool {
        let pipe = pipes[i]
        let limit = maxFrame(pipe)
        var len = 0
        var fresh = false
        var resent = false

        if pipe.hold && pipe.window.pointee.resending {
            var l = 0
            while let old = window_next_resend(pipe.window, &l) {
                if l <= limit {
                    frame.update(from: old, count: l)
                    len = l
                    resent = true
                    break
                }
                pipe.down.pointee.lost &+= Self.payloadLen(old, l) // MTU shrank below the frame
            }
        }
        if len == 0 {
            len = down_pack(pipe.down, now, limit, false, frame)
            fresh = len > 0
        }
        if len == 0 {
            return false
        }
        if fresh && pipe.hold {
            pipe.down.pointee.lost &+= UInt32(window_push(pipe.window, frame, len))
        }
        send(slot: i, Data(bytes: frame, count: len), resent: resent)
        return true
    }

    private func pumpSlotUp(_ i: Int) {
        let pipe = pipes[i]
        var len = 0
        guard let record = up_peek(pipe.up, &len), pipe.linked, !pipe.upBusy else { return }
        if links?.write(slot: i, record: Data(bytes: record, count: len)) == true {
            pipe.upBusy = true // popped in uplinkWriteDone
        } else {
            pipe.up.pointee.lost &+= UInt32(len)
            up_pop(pipe.up)
        }
    }

    private func reportLost(now: UInt32) {
        guard now &- lastLostEventMs >= Self.lostEventPeriodMs else { return }
        lastLostEventMs = now
        for (i, pipe) in pipes.enumerated() {
            let down = pipe.down.pointee.lost, up = pipe.up.pointee.lost
            if down != pipe.downLostReported {
                phone?.sendControl("event lost \(i + 1) down \(down &- pipe.downLostReported)")
                pipe.downLostReported = down
            }
            if up != pipe.upLostReported {
                phone?.sendControl("event lost \(i + 1) up \(up &- pipe.upLostReported)")
                pipe.upLostReported = up
            }
        }
    }

    private func pump() {
        let now = nowMs()
        for i in 0..<slotCount {
            pumpSlotUp(i)
        }
        var progress = true
        while progress && gateOpen {
            progress = false
            for k in 0..<slotCount where gateOpen {
                if pumpSlotDown((nextSlot + k) % slotCount, now: now) {
                    progress = true
                }
            }
            nextSlot = (nextSlot + 1) % slotCount
        }
        reportLost(now: now)
    }

    // ---- Configuration (commands) ----

    func configure(slot: Int, raw: Bool, hold: Bool) {
        let pipe = pipes[slot]
        pipe.down.pointee.mode = raw ? FRAME_MODE_RAW : FRAME_MODE_LINE
        if pipe.hold && !hold {
            window_clear(pipe.window)
        }
        pipe.hold = hold
    }

    func ack(slot: Int, epoch: UInt8, seq: UInt8) {
        window_ack(pipes[slot].window, epoch, seq)
    }

    func info(slot: Int) -> PipeInfo {
        let pipe = pipes[slot]
        let d = pipe.down.pointee
        return PipeInfo(raw: d.mode == FRAME_MODE_RAW, hold: pipe.hold, epoch: d.epoch,
                        downLost: d.lost, upLost: pipe.up.pointee.lost)
    }

    // ---- Device side (links) ----

    func deviceData(slot: Int, _ data: Data) {
        let pipe = pipes[slot]
        if phoneReady || pipe.hold {
            data.withUnsafeBytes { buf in
                down_push(pipe.down, buf.bindMemory(to: UInt8.self).baseAddress, buf.count, nowMs(), !phoneReady)
            }
        } else {
            pipe.down.pointee.gap = true // nobody listening and nothing kept
        }
        wake()
    }

    func linkUp(slot: Int) -> UInt8 {
        let pipe = pipes[slot]
        down_link_up(pipe.down)
        pipe.linked = true
        pipe.upBusy = false
        wake()
        return pipe.down.pointee.epoch
    }

    func linkDown(slot: Int) {
        let pipe = pipes[slot]
        down_link_down(pipe.down)
        up_clear(pipe.up) // stale corrections are worse than none
        pipe.linked = false
        pipe.upBusy = false
        wake()
    }

    func uplinkWriteDone(slot: Int, ok: Bool) {
        let pipe = pipes[slot]
        var len = 0
        if up_peek(pipe.up, &len) != nil {
            if !ok { pipe.up.pointee.lost &+= UInt32(len) }
            up_pop(pipe.up)
        }
        pipe.upBusy = false
        wake()
    }

    // ---- Phone side ----

    func phoneReady(_ ready: Bool, maxUpdate: Int) {
        if ready != phoneReady {
            if !ready, let b = blocked {
                // Never reached the phone. Hold slots resend it from the window.
                let pipe = pipes[b.slot]
                if !pipe.hold {
                    b.frame.withUnsafeBytes { buf in
                        pipe.down.pointee.lost &+= Self.payloadLen(buf.bindMemory(to: UInt8.self).baseAddress!, buf.count)
                    }
                    pipe.down.pointee.gap = true
                }
                blocked = nil
            }
            for pipe in pipes {
                if ready {
                    if pipe.hold { window_start_resend(pipe.window) }
                } else if pipe.hold {
                    pipe.down.pointee.replay_end = pipe.down.pointee.in // unsent bytes now count as offline
                } else {
                    down_discard(pipe.down)
                }
            }
        }
        phoneReady = ready
        self.maxUpdate = maxUpdate
        congested = false
        wake()
    }

    func setMaxUpdate(_ value: Int) {
        maxUpdate = value
        wake()
    }

    // peripheralManagerIsReady: resend the frame the stack refused, then continue.
    func transmitReady() {
        congested = false
        if let b = blocked, phoneReady {
            blocked = nil
            if phone?.notifyData(b.frame) != true {
                blocked = b
                congested = true
            }
        }
        wake()
    }

    func uplink(_ frame: Data) {
        guard frame.count >= Int(UP_HDR_LEN) else { return }
        let bytes = [UInt8](frame)
        guard bytes[0] >= 1, Int(bytes[0]) <= slotCount else { return }
        let pipe = pipes[Int(bytes[0]) - 1]
        let payload = Array(bytes[Int(UP_HDR_LEN)...])
        if pipe.linked {
            payload.withUnsafeBufferPointer { buf in
                up_write(pipe.up, buf.baseAddress, buf.count, (bytes[1] & UInt8(UP_MORE)) != 0)
            }
        } else {
            pipe.up.pointee.lost &+= UInt32(payload.count)
        }
        wake()
    }
}
