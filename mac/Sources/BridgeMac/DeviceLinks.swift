import Foundation
import IOBluetooth

// Counterpart of main/links.c: one classic-Bluetooth SPP (RFCOMM) link per
// slot, redialled in the background. Like the firmware, only one slot dials
// at a time (pair if needed → SDP for the SPP channel → open RFCOMM), a failed
// dial is retried after 5 s, and an inquiry scan waits for the dial in
// progress. IOBluetooth delivers its callbacks on the main run loop, which is
// also the bridge queue. A slot can also be a virtual device fed from the GUI.

struct ScanResult: Identifiable, Hashable {
    let id: String // aa:bb:cc:dd:ee:ff
    var name: String
    var rssi: Int
    var paired: Bool
}

struct SlotSnapshot {
    var device: SlotDevice?
    var state = "idle"
    var repeating: String?
    var info = PipeInfo(raw: false, hold: false, epoch: 0, downLost: 0, upLost: 0)
}

private final class Link {
    var device: SlotDevice?
    var state = "idle" // idle | dialing | connected
    var up = false     // link reported to the pipeline
    var nextTry = Date.distantPast
    var bt: IOBluetoothDevice?
    var pairing: IOBluetoothDevicePair?
    var channel: IOBluetoothRFCOMMChannel?
    var closing = false // we closed it ourselves; no retry delay
    var writing: UnsafeMutableRawPointer?
    var writeLen = 0
    var writeOffset = 0
    var repeatText: String?
    var repeatTimer: DispatchSourceTimer?

    func freeWrite() {
        writing?.deallocate()
        writing = nil
        writeLen = 0
        writeOffset = 0
    }
}

func normalizeMac(_ text: String) -> String? {
    let parts = text.lowercased().split(whereSeparator: { $0 == ":" || $0 == "-" })
    guard parts.count == 6, parts.allSatisfy({ $0.count == 2 && UInt8($0, radix: 16) != nil }) else { return nil }
    return parts.joined(separator: ":")
}

final class DeviceLinks: NSObject, PipelineLinks, IOBluetoothRFCOMMChannelDelegate,
    IOBluetoothDeviceInquiryDelegate, IOBluetoothDevicePairDelegate {
    static let retryDelay: TimeInterval = 5
    static let dialTimeout: TimeInterval = 30
    static let scanSeconds: UInt8 = 10
    static let defaultPin = "0000"
    static let sppUUID: IOBluetoothSDPUUID = IOBluetoothSDPUUID(uuid16: 0x1101)!

    let pipeline: Pipeline
    weak var phone: PipelinePhone?
    var monitor = false
    private(set) var scanning = false
    private(set) var scanResults: [ScanResult] = []

    private let queue: DispatchQueue
    private let links = (0..<slotCount).map { _ in Link() }
    private var dialing: Int?
    private var dialStarted = Date()
    private var scanPending = false
    private var scanReply: ((String) -> Void)?
    private var reported: Set<String> = [] // MACs already sent as "found" in this scan
    private var inquiry: IOBluetoothDeviceInquiry?
    private var manager: DispatchSourceTimer?
    private var cached: [String: (line: String, seen: Date)] = [:]

    init(queue: DispatchQueue, pipeline: Pipeline) {
        self.queue = queue
        self.pipeline = pipeline
        super.init()
        for (i, link) in links.enumerated() {
            link.device = Store.device(i)
        }
        let t = DispatchSource.makeTimerSource(queue: queue)
        t.schedule(deadline: .now() + 0.5, repeating: .milliseconds(500))
        t.setEventHandler { [weak self] in self?.tick() }
        t.resume()
        manager = t
    }

    var bluetooth: String {
        guard let host = IOBluetoothHostController.default() else { return "不支持" }
        return host.powerState == kBluetoothHCIPowerStateON ? "已开启" : "已关闭"
    }

    func snapshot(_ i: Int) -> SlotSnapshot {
        let l = links[i]
        return SlotSnapshot(device: l.device, state: l.state, repeating: l.repeatText, info: pipeline.info(slot: i))
    }

    func linkState(_ i: Int) -> (device: SlotDevice?, state: String, name: String) {
        let l = links[i]
        return (l.device, l.state, l.bt?.name ?? l.device?.name ?? "")
    }

    func knownName(_ mac: String) -> String? {
        scanResults.first { $0.id == mac }?.name ?? IOBluetoothDevice(addressString: mac)?.name
    }

    private func slot(of device: IOBluetoothDevice?) -> Int? {
        guard let mac = device?.addressString.flatMap(normalizeMac) else { return nil }
        return links.firstIndex { $0.device?.mac == mac }
    }

    private func slot(of channel: IOBluetoothRFCOMMChannel?) -> Int? {
        links.firstIndex { $0.channel != nil && $0.channel === channel }
    }

    // ---- Manager (manager_task) ----

    private func tick() {
        if let i = dialing, Date().timeIntervalSince(dialStarted) > Self.dialTimeout {
            log(.device, "槽位 \(i + 1)：连接超时")
            failDial(i)
        }
        if dialing == nil, scanPending {
            startScan()
        }
        if dialing == nil, !scanPending, !scanning {
            dialNext()
        }
    }

    private func dialNext() {
        let now = Date()
        for (i, l) in links.enumerated() where l.device != nil && l.state == "idle" && now >= l.nextTry {
            guard let device = l.device else { continue }
            if device.isVirtual {
                l.state = "connected"
                linkUp(i)
                continue
            }
            guard let mac = device.mac, let bt = IOBluetoothDevice(addressString: mac.replacingOccurrences(of: ":", with: "-")) else {
                continue
            }
            l.bt = bt
            l.state = "dialing"
            dialing = i
            dialStarted = now
            if bt.isPaired() {
                log(.device, "槽位 \(i + 1)：查询 \(mac) 的 SPP 服务")
                startSDP(i, bt)
            } else {
                log(.device, "槽位 \(i + 1)：配对 \(mac)")
                let pair = IOBluetoothDevicePair(device: bt)
                pair?.delegate = self
                l.pairing = pair
                if pair?.start() != kIOReturnSuccess {
                    failDial(i)
                }
            }
            return
        }
    }

    private static func sppChannel(_ bt: IOBluetoothDevice) -> BluetoothRFCOMMChannelID? {
        var channelID: BluetoothRFCOMMChannelID = 0
        guard let record = bt.getServiceRecord(for: sppUUID),
              record.getRFCOMMChannelID(&channelID) == kIOReturnSuccess else { return nil }
        return channelID
    }

    // macOS caches the SDP records of paired devices; query only when there is
    // none. The UUID-filtered performSDPQuery(_:uuids:) never called back on
    // macOS 26 (2026-10-17, 银河 G1), so this always queries every record.
    private func startSDP(_ i: Int, _ bt: IOBluetoothDevice) {
        if let channelID = Self.sppChannel(bt) {
            openChannel(i, bt, channelID)
        } else if bt.performSDPQuery(self) != kIOReturnSuccess {
            failDial(i)
        }
    }

    // Called by IOBluetooth when performSDPQuery finishes.
    @objc func sdpQueryComplete(_ device: IOBluetoothDevice!, status: IOReturn) {
        guard let i = slot(of: device), dialing == i else { return }
        guard let channelID = Self.sppChannel(device) else {
            log(.device, "槽位 \(i + 1)：SDP 未找到 SPP（状态 \(String(format: "0x%08x", status))）")
            failDial(i)
            return
        }
        openChannel(i, device, channelID)
    }

    // While the system holds a baseband connection to the device (for
    // example "Connect" in System Settings), the RFCOMM open never completes
    // (sync open: 0xe00002bc). Drop that connection first, then open.
    private func openChannel(_ i: Int, _ bt: IOBluetoothDevice, _ channelID: BluetoothRFCOMMChannelID) {
        if bt.isConnected() {
            log(.device, "槽位 \(i + 1)：设备已被系统连接，先断开再打开 RFCOMM \(channelID)")
            bt.closeConnection()
            queue.asyncAfter(deadline: .now() + 2) { [weak self] in
                guard let self, self.dialing == i, self.links[i].bt === bt else { return }
                self.openRFCOMM(i, bt, channelID)
            }
            return
        }
        openRFCOMM(i, bt, channelID)
    }

    private func openRFCOMM(_ i: Int, _ bt: IOBluetoothDevice, _ channelID: BluetoothRFCOMMChannelID) {
        log(.device, "槽位 \(i + 1)：SPP 在 RFCOMM \(channelID)，连接")
        var channel: IOBluetoothRFCOMMChannel?
        let result = bt.openRFCOMMChannelAsync(&channel, withChannelID: channelID, delegate: self)
        links[i].channel = channel
        if result != kIOReturnSuccess {
            failDial(i)
        }
    }

    private func failDial(_ i: Int) {
        let l = links[i]
        l.channel?.setDelegate(nil)
        l.channel?.close()
        l.channel = nil
        l.pairing = nil
        l.state = "idle"
        l.nextTry = Date() + Self.retryDelay
        if dialing == i { dialing = nil }
    }

    private func linkUp(_ i: Int) {
        let l = links[i]
        guard !l.up else { return }
        l.up = true
        l.state = "connected"
        let epoch = pipeline.linkUp(slot: i)
        let name = linkState(i).name
        log(.device, "槽位 \(i + 1)：已连接 \(name)")
        phone?.sendControl("event link \(i + 1) up \(epoch) \(name)")
    }

    private func linkDown(_ i: Int) {
        let l = links[i]
        l.freeWrite()
        guard l.up else { return }
        l.up = false
        pipeline.linkDown(slot: i)
        phone?.sendControl("event link \(i + 1) down")
    }

    // Drops the link without waiting for the retry delay (rebinding, scanning).
    private func hangUp(_ i: Int) {
        let l = links[i]
        linkDown(i)
        if let c = l.channel {
            l.closing = true
            c.setDelegate(nil)
            c.close()
            l.bt?.closeConnection()
        }
        l.channel = nil
        l.pairing = nil
        l.closing = false
        l.state = "idle"
        l.nextTry = .distantPast
        if dialing == i { dialing = nil }
    }

    // ---- Binding (pair / forget) ----

    // links_set_device: saves the device and applies it immediately.
    func setDevice(_ i: Int, _ device: SlotDevice?) {
        if let mac = device?.mac {
            for (j, other) in links.enumerated() where j != i && other.device?.mac == mac {
                log(.device, "设备已从槽位 \(j + 1) 移到槽位 \(i + 1)")
                setDevice(j, nil)
            }
        }
        hangUp(i)
        links[i].device = device
        links[i].bt = nil
        Store.setDevice(i, device)
        tick()
    }

    // ---- IOBluetoothDevicePairDelegate ----

    func devicePairingPINCodeRequest(_ sender: Any!) {
        guard let pair = sender as? IOBluetoothDevicePair, let i = slot(of: pair.device()) else { return }
        let text = links[i].device?.pin.flatMap { $0.isEmpty ? nil : $0 } ?? Self.defaultPin
        var code = BluetoothPINCode()
        let bytes = Array(text.utf8.prefix(16))
        withUnsafeMutableBytes(of: &code.data) { buf in buf.copyBytes(from: bytes) }
        log(.device, "槽位 \(i + 1)：设备要求 PIN，使用\(links[i].device?.pin?.isEmpty == false ? "设定的" : "默认") PIN")
        pair.replyPINCode(bytes.count, pinCode: &code)
    }

    func devicePairingUserConfirmationRequest(_ sender: Any!, numericValue: BluetoothNumericValue) {
        (sender as? IOBluetoothDevicePair)?.replyUserConfirmation(true) // SSP: confirm automatically
    }

    func devicePairingFinished(_ sender: Any!, error: IOReturn) {
        guard let pair = sender as? IOBluetoothDevicePair, let bt = pair.device(), let i = slot(of: bt),
              dialing == i else { return }
        links[i].pairing = nil
        if error != kIOReturnSuccess {
            log(.device, "槽位 \(i + 1)：配对失败 \(String(format: "0x%08x", error))")
            failDial(i)
            return
        }
        log(.device, "槽位 \(i + 1)：配对成功")
        startSDP(i, bt)
    }

    // ---- IOBluetoothRFCOMMChannelDelegate ----

    func rfcommChannelOpenComplete(_ rfcommChannel: IOBluetoothRFCOMMChannel!, status error: IOReturn) {
        guard let i = slot(of: rfcommChannel) else { return }
        if error != kIOReturnSuccess {
            log(.device, "槽位 \(i + 1)：打开 RFCOMM 失败 \(String(format: "0x%08x", error))")
            failDial(i)
            return
        }
        if dialing == i { dialing = nil }
        linkUp(i)
    }

    func rfcommChannelData(_ rfcommChannel: IOBluetoothRFCOMMChannel!, data dataPointer: UnsafeMutableRawPointer!,
                           length dataLength: Int) {
        guard let i = slot(of: rfcommChannel), dataLength > 0 else { return }
        let data = Data(bytes: dataPointer, count: dataLength)
        if monitor { log(.device, "槽位 \(i + 1) ← \(data.count) B  \(data.preview)") }
        pipeline.deviceData(slot: i, data)
    }

    func rfcommChannelClosed(_ rfcommChannel: IOBluetoothRFCOMMChannel!) {
        guard let i = slot(of: rfcommChannel) else { return }
        log(.device, "槽位 \(i + 1)：SPP 断开")
        linkDown(i)
        failDial(i)
    }

    func rfcommChannelWriteComplete(_ rfcommChannel: IOBluetoothRFCOMMChannel!, refcon: UnsafeMutableRawPointer!,
                                    status error: IOReturn) {
        guard let i = slot(of: rfcommChannel), links[i].writing != nil else { return }
        if error != kIOReturnSuccess {
            finishWrite(i, ok: false)
        } else {
            continueWrite(i)
        }
    }

    // ---- Uplink ----

    func write(slot i: Int, record: Data) -> Bool {
        let l = links[i]
        guard l.up else { return false }
        if l.device?.isVirtual == true {
            log(.up, "→ 虚拟设备 \(i + 1)：\(record.count) B  \(record.preview)")
            queue.async { self.pipeline.uplinkWriteDone(slot: i, ok: true) }
            return true
        }
        guard l.channel?.isOpen() == true, !record.isEmpty else { return false }
        // writeAsync keeps a pointer to the bytes until it completes.
        let buf = UnsafeMutableRawPointer.allocate(byteCount: record.count, alignment: 1)
        record.copyBytes(to: buf.assumingMemoryBound(to: UInt8.self), count: record.count)
        l.writing = buf
        l.writeLen = record.count
        l.writeOffset = 0
        continueWrite(i)
        return true
    }

    private func continueWrite(_ i: Int) {
        let l = links[i]
        guard let buf = l.writing, let c = l.channel else { return }
        if l.writeOffset >= l.writeLen {
            finishWrite(i, ok: true)
            return
        }
        let mtu = max(Int(c.getMTU()), 1)
        let n = min(mtu, l.writeLen - l.writeOffset)
        let result = c.writeAsync(buf + l.writeOffset, length: UInt16(n), refcon: nil)
        l.writeOffset += n
        if result != kIOReturnSuccess {
            finishWrite(i, ok: false)
        }
    }

    private func finishWrite(_ i: Int, ok: Bool) {
        let l = links[i]
        if let buf = l.writing {
            let data = Data(bytes: buf, count: l.writeLen)
            log(.up, "→ 设备 \(i + 1)：\(data.count) B\(ok ? "" : " 失败")  \(data.preview)")
        }
        l.freeWrite()
        pipeline.uplinkWriteDone(slot: i, ok: ok)
    }

    // ---- Virtual / injected data ----

    func inject(_ i: Int, _ data: Data) {
        if monitor { log(.device, "槽位 \(i + 1) 注入 \(data.count) B  \(data.preview)") }
        pipeline.deviceData(slot: i, data)
    }

    func setRepeat(_ i: Int, _ text: String?) {
        let l = links[i]
        l.repeatTimer?.cancel()
        l.repeatTimer = nil
        l.repeatText = text
        guard let text else { return }
        let t = DispatchSource.makeTimerSource(queue: queue)
        t.schedule(deadline: .now(), repeating: .seconds(1))
        t.setEventHandler { [weak self] in self?.inject(i, Data((text + "\r\n").utf8)) }
        t.resume()
        l.repeatTimer = t
    }

    // ---- Scan (links_scan) ----

    // Waits for the dial in progress, then drops every link for the inquiry,
    // as the firmware does. Links redial afterwards.
    func scan(reply: @escaping (String) -> Void) -> Bool {
        guard !scanning, !scanPending else { return false }
        scanReply = reply
        scanPending = true
        tick()
        return true
    }

    private func startScan() {
        scanPending = false
        reported = []
        for i in 0..<slotCount where links[i].device?.isVirtual != true {
            hangUp(i)
        }
        scanResults = IOBluetoothDevice.pairedDevices()?.compactMap { item -> ScanResult? in
            guard let d = item as? IOBluetoothDevice, let mac = d.addressString.flatMap(normalizeMac) else { return nil }
            return ScanResult(id: mac, name: d.name ?? "", rssi: 0, paired: true)
        } ?? []
        let inq = IOBluetoothDeviceInquiry(delegate: self)
        inq?.inquiryLength = Self.scanSeconds
        inq?.updateNewDeviceNames = true
        inquiry = inq
        scanning = true
        if inq?.start() != kIOReturnSuccess {
            scanning = false
            inquiry = nil
            scanReply?("error scan failed")
            scanReply = nil
        }
    }

    func deviceInquiryDeviceFound(_ sender: IOBluetoothDeviceInquiry!, device: IOBluetoothDevice!) {
        guard let mac = device.addressString.flatMap(normalizeMac) else { return }
        let name = device.name ?? ""
        let rssi = Int(device.rawRSSI())
        if let k = scanResults.firstIndex(where: { $0.id == mac }) {
            scanResults[k].rssi = rssi
        } else {
            scanResults.append(ScanResult(id: mac, name: name, rssi: rssi, paired: device.isPaired()))
        }
        let cod = device.classOfDevice & 0xffffff
        // macOS exposes no EIR; only an SDP record it already cached counts as "spp".
        let spp = device.getServiceRecord(for: Self.sppUUID) != nil
        let line = String(format: "found %@ %d %06x %@ %@ %@", mac, rssi, cod, Self.codClass(cod), spp ? "spp" : "-", name)
        if cached[mac] == nil, cached.count >= 24, let oldest = cached.min(by: { $0.value.seen < $1.value.seen })?.key {
            cached.removeValue(forKey: oldest)
        }
        cached[mac] = (line, Date())
        scanReply?(line)
    }

    // Major device class names, as in links.c cod_class().
    static func codClass(_ cod: UInt32) -> String {
        let names = ["misc", "computer", "phone", "lan", "av", "peripheral", "imaging", "wearable", "toy", "health"]
        let major = Int((cod & 0x1f00) >> 8)
        return major < names.count ? names[major] : (major == 31 ? "uncategorized" : "other")
    }

    func deviceInquiryDeviceNameUpdated(_ sender: IOBluetoothDeviceInquiry!, device: IOBluetoothDevice!,
                                        devicesRemaining: UInt32) {
        guard let mac = device.addressString.flatMap(normalizeMac),
              let k = scanResults.firstIndex(where: { $0.id == mac }) else { return }
        let name = device.name ?? ""
        guard !name.isEmpty, name != scanResults[k].name else { return }
        scanResults[k].name = name
        if let entry = cached[mac] {
            let prefix = entry.line.split(separator: " ", maxSplits: 6).prefix(6).joined(separator: " ")
            cached[mac] = (prefix + " " + name, entry.seen)
        }
        scanReply?("name \(mac) \(name)")
    }

    func cachedDevices(reply: (String) -> Void) {
        reply("devices begin \(cached.count)")
        reply(scanning || scanPending ? "scan state running" : "scan state idle")
        for (mac, entry) in cached.sorted(by: { $0.key < $1.key }) {
            reply(entry.line)
            reply("seen \(mac) \(max(0, Int(Date().timeIntervalSince(entry.seen) * 1000)))")
        }
        reply("devices done \(cached.count)")
    }

    // `paired`: on the Mac these are the devices paired with macOS itself.
    func paired(reply: (String) -> Void) {
        for item in IOBluetoothDevice.pairedDevices() ?? [] {
            guard let d = item as? IOBluetoothDevice, let mac = d.addressString.flatMap(normalizeMac) else { continue }
            let slot = links.firstIndex { $0.device?.mac == mac }.map { String($0 + 1) } ?? "-"
            reply("paired \(mac) \(slot) \(d.name ?? "")")
        }
        reply("paired done")
    }

    func deviceInquiryComplete(_ sender: IOBluetoothDeviceInquiry!, error: IOReturn, aborted: Bool) {
        scanning = false
        inquiry = nil
        scanReply?("scan done")
        scanReply = nil
    }
}
