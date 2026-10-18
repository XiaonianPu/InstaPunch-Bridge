import Foundation

// Port of main/commands.c (docs/protocol.md §5). The same text commands work
// from the phone's control characteristic and from the GUI console (`usb`).
// Addition: `pair <slot> virtual` binds a GUI-fed virtual device.

let firmwareVersion = "0.1.0-mac"
let protocolVersion = 1

final class Commands {
    let pipeline: Pipeline
    let phone: PhoneServer
    let links: DeviceLinks

    init(pipeline: Pipeline, phone: PhoneServer, links: DeviceLinks) {
        self.pipeline = pipeline
        self.phone = phone
        self.links = links
    }

    // bridge_command_load_options
    func loadOptions() {
        for i in 0..<slotCount {
            let o = Store.options(i)
            pipeline.configure(slot: i, raw: o.raw, hold: o.hold)
        }
    }

    func setOptions(_ slot: Int, _ options: SlotOptions) {
        Store.setOptions(slot, options)
        pipeline.configure(slot: slot, raw: options.raw, hold: options.hold)
    }

    func status(_ reply: (String) -> Void) {
        for i in 0..<slotCount {
            let (device, state, name) = links.linkState(i)
            let p = pipeline.info(slot: i)
            guard let device else {
                reply("\(i + 1) none")
                continue
            }
            reply("\(i + 1) \(device.mac ?? "virtual") \(state) \(p.raw ? "raw" : "line") hold=\(p.hold ? 1 : 0) "
                  + "epoch=\(p.epoch) lost=\(p.downLost)/\(p.upLost) \(name)")
        }
        reply(phone.status())
    }

    func run(_ line: String, usb: Bool, reply: @escaping (String) -> Void) {
        let args = line.split(whereSeparator: { $0 == " " || $0 == "\t" })
        guard let cmd = args.first else { return }
        let arg = { (n: Int) -> Substring? in args.count > n ? args[n] : nil }

        switch cmd {
        case "info":
            reply("bridge \(firmwareVersion) proto \(protocolVersion) slots \(slotCount)")
        case "status":
            status(reply)
        case "scan":
            if !links.scan(reply: reply) {
                reply("error scan already running")
            }
        case "paired":
            links.paired(reply: reply)
        case "pair":
            let usage = "error usage: pair <slot> <aa:bb:cc:dd:ee:ff> [pin]"
            guard let slot = parseSlot(arg(1)), let target = arg(2) else {
                reply(usage)
                return
            }
            if target == "virtual" {
                links.setDevice(slot, .virtual)
            } else if let mac = normalizeMac(String(target)) {
                let pin = arg(3).map(String.init)
                if let pin, pin.utf8.count > 16 {
                    reply("error pin too long")
                    return
                }
                links.setDevice(slot, SlotDevice(mac: mac, name: links.knownName(mac) ?? "", pin: pin))
            } else {
                reply(usage)
                return
            }
            reply("ok")
        case "forget":
            guard let slot = parseSlot(arg(1)) else {
                reply("error usage: forget <slot>")
                return
            }
            links.setDevice(slot, nil)
            reply("ok")
        case "mode", "hold":
            let isMode = cmd == "mode"
            let value = arg(2)
            guard let slot = parseSlot(arg(1)),
                  let value, isMode ? (value == "line" || value == "raw") : (value == "on" || value == "off") else {
                reply(isMode ? "error usage: mode <slot> line|raw" : "error usage: hold <slot> on|off")
                return
            }
            var o = Store.options(slot)
            if isMode { o.raw = value == "raw" } else { o.hold = value == "on" }
            setOptions(slot, o)
            reply("ok")
        case "ack":
            // Silent on success: acks are frequent and need no answer.
            guard let slot = parseSlot(arg(1)), let e = arg(2).flatMap({ UInt(String($0)) }),
                  let s = arg(3).flatMap({ UInt(String($0)) }) else {
                reply("error usage: ack <slot> <epoch> <seq>")
                return
            }
            pipeline.ack(slot: slot, epoch: UInt8(truncatingIfNeeded: e), seq: UInt8(truncatingIfNeeded: s))
        case "unbind" where usb:
            phone.unbind()
            reply("ok")
        case "monitor" where usb:
            links.monitor = arg(1) == "on"
            reply("ok")
        default:
            reply(usb ? "commands: info | status | scan | paired | pair <slot> <mac> [pin] | forget <slot> | "
                        + "mode <slot> line|raw | hold <slot> on|off | ack <slot> <epoch> <seq> | unbind | monitor on|off"
                  : "error unknown command")
        }
    }
}
