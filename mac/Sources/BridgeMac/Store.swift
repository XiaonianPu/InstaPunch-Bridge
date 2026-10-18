import Foundation

// Counterpart of bridge_config.c: per-slot device and options, persisted in
// UserDefaults instead of NVS.

let slotCount = 2

// A downstream classic SPP device (bridge_device_t), or a virtual device fed
// from the GUI.
struct SlotDevice: Codable, Equatable {
    var mac: String?  // aa:bb:cc:dd:ee:ff; nil = virtual
    var name: String
    var pin: String?  // legacy PIN; "0000" when empty

    var isVirtual: Bool { mac == nil }

    static let virtual = SlotDevice(mac: nil, name: "virtual", pin: nil)
}

struct SlotOptions: Codable, Equatable {
    var raw = false
    var hold = false
}

enum Store {
    private static let defaults = UserDefaults.standard

    private static func load<T: Decodable>(_ key: String) -> T? {
        guard let data = defaults.data(forKey: key) else { return nil }
        return try? JSONDecoder().decode(T.self, from: data)
    }

    private static func save<T: Encodable>(_ value: T?, _ key: String) {
        if let value, let data = try? JSONEncoder().encode(value) {
            defaults.set(data, forKey: key)
        } else {
            defaults.removeObject(forKey: key)
        }
    }

    static func device(_ slot: Int) -> SlotDevice? { load("slot\(slot + 1).device") }
    static func setDevice(_ slot: Int, _ device: SlotDevice?) { save(device, "slot\(slot + 1).device") }

    static func options(_ slot: Int) -> SlotOptions { load("slot\(slot + 1).options") ?? SlotOptions() }
    static func setOptions(_ slot: Int, _ options: SlotOptions) { save(options, "slot\(slot + 1).options") }

    // The owner phone (central identifier as seen by this Mac).
    static var owner: UUID? {
        get { defaults.string(forKey: "owner").flatMap(UUID.init(uuidString:)) }
        set { defaults.set(newValue?.uuidString, forKey: "owner") }
    }

    static var requireEncryption: Bool {
        get { defaults.object(forKey: "requireEncryption") as? Bool ?? true }
        set { defaults.set(newValue, forKey: "requireEncryption") }
    }
}

func parseSlot(_ text: Substring?) -> Int? {
    guard let text, let n = Int(text), n >= 1, n <= slotCount else { return nil }
    return n - 1
}

func nowMs() -> UInt32 {
    UInt32(truncatingIfNeeded: DispatchTime.now().uptimeNanoseconds / 1_000_000)
}

extension Data {
    var hex: String { map { String(format: "%02X", $0) }.joined(separator: " ") }

    // Printable preview: ASCII as is, CR/LF as \r \n, the rest as ·.
    var preview: String {
        String(map { b -> String in
            switch b {
            case 0x0D: return "\\r"
            case 0x0A: return "\\n"
            case 0x20...0x7E: return String(UnicodeScalar(b))
            default: return "·"
            }
        }.joined().prefix(160))
    }
}
