import CoreBluetooth
import Foundation

// Counterpart of main/ble_server.c: the Bridge GATT service of
// docs/protocol.md §1, served by this Mac to the phone.
// macOS limits compared with the firmware:
// - A peripheral cannot see connections or disconnect a central, and the OS
//   owns pairing. "Owner" is the first central that subscribes or writes;
//   other centrals are ignored (writes answered with an auth error) but stay
//   connected. Unbind also needs the pairing removed in System Settings.
// - Advertising stops while the owner is subscribed instead of while connected.

struct PhoneSnapshot {
    var bluetooth = "未知"
    var advertising = false
    var owner: UUID?
    var dataSubscribed = false
    var controlSubscribed = false
    var maxUpdate = 0
    var requireEncryption = true
}

final class PhoneServer: NSObject, CBPeripheralManagerDelegate, PipelinePhone {
    static let serviceUUID = CBUUID(string: "A7B750D5-8CE4-4F67-B586-8C1AA7800BBF")
    static let dataUUID = CBUUID(string: "D36A57C5-8A8E-4C3F-9E65-96E193551BCE")
    static let uplinkUUID = CBUUID(string: "381B0BF7-5C19-4CCB-BF37-BC4214560D5A")
    static let controlUUID = CBUUID(string: "FC838A35-6BDB-4571-A93B-E1D2E538DECD")
    static let localName = "InstaPunch-Bridge-Mac"
    static let controlBacklogMax = 96
    private(set) var controlGeneration = 0
    var canPublishSnapshot: Bool { controlSubscribed && (ownerCentral?.maximumUpdateValueLength ?? 0) >= 128 && controlBacklog.isEmpty }

    let pipeline: Pipeline
    var onCommand: ((String, @escaping (String) -> Void) -> Void)?

    private var manager: CBPeripheralManager!
    private var dataChar: CBMutableCharacteristic?
    private var controlChar: CBMutableCharacteristic?
    private var ownerCentral: CBCentral?
    private var dataSubscribed = false
    private var controlSubscribed = false
    private var ready = false
    private var controlBacklog: [Data] = []
    private var serviceAdded = false

    init(queue: DispatchQueue, pipeline: Pipeline) {
        self.pipeline = pipeline
        super.init()
        manager = CBPeripheralManager(delegate: self, queue: queue)
    }

    var snapshot: PhoneSnapshot {
        PhoneSnapshot(bluetooth: Self.stateName(manager.state), advertising: manager.isAdvertising,
                      owner: Store.owner, dataSubscribed: dataSubscribed, controlSubscribed: controlSubscribed,
                      maxUpdate: ownerCentral?.maximumUpdateValueLength ?? 0,
                      requireEncryption: Store.requireEncryption)
    }

    static func stateName(_ state: CBManagerState) -> String {
        switch state {
        case .poweredOn: return "已开启"
        case .poweredOff: return "已关闭"
        case .unauthorized: return "未授权（系统设置 › 隐私与安全性 › 蓝牙）"
        case .unsupported: return "不支持"
        case .resetting: return "重置中"
        default: return "未知"
        }
    }

    // ---- Service ----

    private func buildService() {
        manager.stopAdvertising()
        manager.removeAllServices()
        resetSubscriptions()
        let enc = Store.requireEncryption
        let data = CBMutableCharacteristic(type: Self.dataUUID, properties: [.notify], value: nil,
                                           permissions: enc ? [.readEncryptionRequired] : [.readable])
        let uplink = CBMutableCharacteristic(type: Self.uplinkUUID, properties: [.writeWithoutResponse], value: nil,
                                             permissions: enc ? [.writeEncryptionRequired] : [.writeable])
        let control = CBMutableCharacteristic(type: Self.controlUUID, properties: [.write, .notify], value: nil,
                                              permissions: enc ? [.readEncryptionRequired, .writeEncryptionRequired]
                                                  : [.readable, .writeable])
        let service = CBMutableService(type: Self.serviceUUID, primary: true)
        service.characteristics = [data, uplink, control]
        dataChar = data
        controlChar = control
        manager.add(service)
    }

    func setRequireEncryption(_ on: Bool) {
        Store.requireEncryption = on
        if manager.state == .poweredOn {
            buildService()
        }
        log(.phone, on ? "特征值要求加密（与固件一致）" : "特征值不要求加密（仅测试用）")
    }

    private func updateAdvertising() {
        guard serviceAdded, manager.state == .poweredOn else { return }
        if ready {
            if manager.isAdvertising {
                manager.stopAdvertising()
                log(.phone, "主人已订阅，停止广播")
            }
        } else if !manager.isAdvertising {
            manager.startAdvertising([CBAdvertisementDataLocalNameKey: Self.localName,
                                      CBAdvertisementDataServiceUUIDsKey: [Self.serviceUUID]])
        }
    }

    private func resetSubscriptions() {
        dataSubscribed = false
        controlSubscribed = false
        ownerCentral = nil
        controlBacklog.removeAll()
        updateReady()
    }

    private func updateReady() {
        let now = dataSubscribed && ownerCentral != nil
        if now != ready {
            ready = now
            pipeline.phoneReady(now, maxUpdate: ownerCentral?.maximumUpdateValueLength ?? 20)
        }
        updateAdvertising()
    }

    // First central to subscribe or write becomes the owner (firmware: first to bond).
    private func accept(_ central: CBCentral) -> Bool {
        if Store.owner == nil {
            Store.owner = central.identifier
            log(.phone, "绑定主人手机 \(central.identifier.uuidString)")
        }
        guard Store.owner == central.identifier else {
            log(.phone, "拒绝非主人手机 \(central.identifier.uuidString)")
            return false
        }
        if ownerCentral !== central {
            ownerCentral = central
            pipeline.setMaxUpdate(central.maximumUpdateValueLength)
        }
        return true
    }

    func unbind() {
        Store.owner = nil
        resetSubscriptions()
        log(.phone, "已清除主人。手机端还需在系统蓝牙设置中忽略这台 Mac 的配对，Mac 端也可在系统设置中移除")
    }

    func status() -> String {
        guard let owner = Store.owner else { return "phone none" }
        return "phone \(owner.uuidString) \(ownerCentral != nil ? "connected" : "idle")"
    }

    // ---- PipelinePhone ----

    func notifyData(_ frame: Data) -> Bool {
        guard let dataChar, let ownerCentral else { return false }
        return manager.updateValue(frame, for: dataChar, onSubscribedCentrals: [ownerCentral])
    }

    func sendControl(_ line: String) {
        log(.control, "→ \(line)")
        guard controlSubscribed, ownerCentral != nil else { return }
        let limit = max(ownerCentral?.maximumUpdateValueLength ?? 20, 20)
        guard line.utf8.count <= limit else {
            log(.phone, "控制消息超过 MTU，拒绝截断")
            return
        }
        let data = Data(line.utf8)
        if !controlBacklog.isEmpty || !pushControl(data) {
            if controlBacklog.count < Self.controlBacklogMax {
                controlBacklog.append(data)
            }
        }
    }

    private func pushControl(_ data: Data) -> Bool {
        guard let controlChar, let ownerCentral else { return false }
        return manager.updateValue(data, for: controlChar, onSubscribedCentrals: [ownerCentral])
    }

    // ---- CBPeripheralManagerDelegate ----

    func peripheralManagerDidUpdateState(_ peripheral: CBPeripheralManager) {
        log(.phone, "蓝牙（外设）\(Self.stateName(peripheral.state))")
        serviceAdded = false
        if peripheral.state == .poweredOn {
            buildService()
        } else {
            resetSubscriptions()
        }
    }

    func peripheralManager(_ peripheral: CBPeripheralManager, didAdd service: CBService, error: Error?) {
        if let error {
            log(.phone, "添加服务失败：\(error.localizedDescription)")
            return
        }
        serviceAdded = true
        updateAdvertising()
    }

    func peripheralManagerDidStartAdvertising(_ peripheral: CBPeripheralManager, error: Error?) {
        if let error {
            log(.phone, "广播失败：\(error.localizedDescription)")
        } else {
            log(.phone, "开始广播 \(Self.localName)")
        }
    }

    func peripheralManager(_ peripheral: CBPeripheralManager, central: CBCentral,
                           didSubscribeTo characteristic: CBCharacteristic) {
        guard accept(central) else { return }
        if characteristic.uuid == Self.dataUUID {
            dataSubscribed = true
            log(.phone, "手机订阅下行数据，MTU \(central.maximumUpdateValueLength + 3)")
        } else if characteristic.uuid == Self.controlUUID {
            if !controlSubscribed { controlGeneration += 1 }
            controlSubscribed = true
            log(.phone, "手机订阅控制")
        }
        updateReady()
    }

    func peripheralManager(_ peripheral: CBPeripheralManager, central: CBCentral,
                           didUnsubscribeFrom characteristic: CBCharacteristic) {
        guard central.identifier == Store.owner else { return }
        if characteristic.uuid == Self.dataUUID {
            dataSubscribed = false
            log(.phone, "手机取消订阅下行数据")
        } else if characteristic.uuid == Self.controlUUID {
            controlSubscribed = false
            controlBacklog.removeAll()
        }
        if !dataSubscribed && !controlSubscribed {
            ownerCentral = nil
        }
        updateReady()
    }

    func peripheralManager(_ peripheral: CBPeripheralManager, didReceiveRead request: CBATTRequest) {
        peripheral.respond(to: request, withResult: .readNotPermitted)
    }

    func peripheralManager(_ peripheral: CBPeripheralManager, didReceiveWrite requests: [CBATTRequest]) {
        guard let first = requests.first else { return }
        let needsResponse = requests.contains { $0.characteristic.uuid == Self.controlUUID }
        guard accept(first.central) else {
            if needsResponse { peripheral.respond(to: first, withResult: .insufficientAuthorization) }
            return
        }
        for request in requests {
            guard let value = request.value else { continue }
            if request.characteristic.uuid == Self.uplinkUUID {
                log(.up, "手机 → ch\(value.first ?? 0) \(value.count) B  \(value.dropFirst(2).preview)")
                pipeline.uplink(value)
            } else if request.characteristic.uuid == Self.controlUUID {
                let text = String(decoding: value, as: UTF8.self)
                for line in text.split(whereSeparator: { $0 == "\n" || $0 == "\r" }) {
                    log(.control, "← \(line)")
                    onCommand?(String(line)) { [weak self] reply in self?.sendControl(reply) }
                }
            }
        }
        if needsResponse { peripheral.respond(to: first, withResult: .success) }
    }

    func peripheralManagerIsReady(toUpdateSubscribers peripheral: CBPeripheralManager) {
        while let data = controlBacklog.first {
            guard pushControl(data) else { return }
            controlBacklog.removeFirst()
        }
        pipeline.transmitReady()
    }
}
