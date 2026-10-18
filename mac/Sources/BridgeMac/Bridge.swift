import Foundation

// Counterpart of main/main.c: wires the pipeline, the phone-side GATT server,
// the downstream links and the commands. Everything below runs on `queue`;
// the GUI calls in through `perform` and reads back `Snapshot`s.

struct Snapshot {
    var phone = PhoneSnapshot()
    var central = "未知"
    var slots = Array(repeating: SlotSnapshot(), count: slotCount)
    var scanning = false
    var scanResults: [ScanResult] = []
    var monitor = false
}

final class Bridge {
    let queue = DispatchQueue.main
    private(set) var pipeline: Pipeline!
    private(set) var phone: PhoneServer!
    private(set) var links: DeviceLinks!
    private(set) var commands: Commands!
    private var publisher: DispatchSourceTimer?

    init(publish: @escaping (Snapshot) -> Void) {
        queue.async { [self] in
            pipeline = Pipeline(queue: queue)
            phone = PhoneServer(queue: queue, pipeline: pipeline)
            links = DeviceLinks(queue: queue, pipeline: pipeline)
            commands = Commands(pipeline: pipeline, phone: phone, links: links)
            pipeline.phone = phone
            pipeline.links = links
            links.phone = phone
            phone.onCommand = { [weak self] line, reply in
                self?.commands.run(line, usb: false, reply: reply)
            }
            pipeline.onFrame = { frame, resent in
                let b = [UInt8](frame)
                let flags = b[1]
                var tags: [String] = []
                if flags & 0x01 != 0 { tags.append("GAP") }
                if flags & 0x02 != 0 { tags.append("REPLAY") }
                if flags & 0x04 != 0 { tags.append("AGE") }
                if flags & 0x08 != 0 { tags.append("PARTIAL") }
                if resent { tags.append("重发") }
                let hdr = flags & 0x04 != 0 ? 8 : 4
                log(.down, "ch\(b[0]) e\(b[2]) s\(b[3]) \(tags.joined(separator: " ")) \(frame.count) B  "
                    + frame.dropFirst(hdr).preview)
            }
            commands.loadOptions()
            pipeline.start()

            let t = DispatchSource.makeTimerSource(queue: queue)
            t.schedule(deadline: .now(), repeating: .milliseconds(250))
            t.setEventHandler { [weak self] in
                guard let self else { return }
                let s = self.snapshot()
                DispatchQueue.main.async { publish(s) }
            }
            t.resume()
            publisher = t
            log(.system, "InstaPunch Bridge Mac \(firmwareVersion)，协议 v\(protocolVersion)")
        }
    }

    private func snapshot() -> Snapshot {
        Snapshot(phone: phone.snapshot, central: links.bluetooth,
                 slots: (0..<slotCount).map { links.snapshot($0) },
                 scanning: links.scanning, scanResults: links.scanResults, monitor: links.monitor)
    }

    func perform(_ work: @escaping (Bridge) -> Void) {
        queue.async { [self] in work(self) }
    }

    // GUI console = the firmware's USB console.
    func console(_ line: String) {
        perform { b in
            log(.control, "$ \(line)")
            b.commands.run(line, usb: true) { reply in log(.control, reply) }
        }
    }
}
