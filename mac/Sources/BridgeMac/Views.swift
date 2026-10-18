import SwiftUI

@main
struct BridgeMacApp: App {
    @StateObject private var model = BridgeModel()

    var body: some Scene {
        WindowGroup("InstaPunch Bridge（Mac 测试）") {
            ContentView()
                .environmentObject(model)
                .frame(minWidth: 980, minHeight: 640)
        }
    }
}

struct ContentView: View {
    @EnvironmentObject var model: BridgeModel

    var body: some View {
        HSplitView {
            ScrollView {
                VStack(alignment: .leading, spacing: 12) {
                    PhoneCard()
                    ForEach(0..<slotCount, id: \.self) { SlotCard(slot: $0) }
                }
                .padding(12)
            }
            .frame(minWidth: 420, idealWidth: 460, maxWidth: 560)
            LogPane()
                .frame(minWidth: 420)
        }
    }
}

// ---- Phone side ----

struct PhoneCard: View {
    @EnvironmentObject var model: BridgeModel

    var body: some View {
        let p = model.snapshot.phone
        GroupBox {
            VStack(alignment: .leading, spacing: 6) {
                LabeledContent("蓝牙", value: p.bluetooth)
                LabeledContent("广播", value: p.advertising ? PhoneServer.localName : "未广播")
                LabeledContent("主人", value: p.owner?.uuidString ?? "无（第一台订阅的手机成为主人）")
                LabeledContent("订阅", value: "数据 \(p.dataSubscribed ? "✓" : "–")  控制 \(p.controlSubscribed ? "✓" : "–")")
                LabeledContent("MTU", value: p.maxUpdate > 0 ? "\(p.maxUpdate + 3)" : "–")
                HStack {
                    Toggle("特征值要求加密", isOn: Binding(
                        get: { p.requireEncryption },
                        set: { on in model.perform { $0.phone.setRequireEncryption(on) } }))
                    Spacer()
                    Button("解除主人") { model.bridge.console("unbind") }
                        .disabled(p.owner == nil)
                }
            }
            .font(.callout)
        } label: {
            Label("手机端（Bridge 服务）", systemImage: "iphone")
        }
    }
}

// ---- Slots ----

struct SlotCard: View {
    @EnvironmentObject var model: BridgeModel
    let slot: Int
    @State private var picking = false
    @State private var inject = "$PLTIT,HV,0.50,Y,240.40,D,2.90,D,0.50,Y*6E"

    private var s: SlotSnapshot { model.snapshot.slots[slot] }

    private func options(_ change: @escaping (inout SlotOptions) -> Void) {
        var o = SlotOptions(raw: s.info.raw, hold: s.info.hold)
        change(&o)
        let i = slot
        model.perform { $0.commands.setOptions(i, o) }
    }

    var body: some View {
        GroupBox {
            VStack(alignment: .leading, spacing: 8) {
                HStack {
                    if let d = s.device {
                        VStack(alignment: .leading) {
                            Text(d.isVirtual ? "虚拟设备" : (d.name.isEmpty ? "（无名称）" : d.name)).bold()
                            if let mac = d.mac {
                                Text(mac + (d.pin?.isEmpty == false ? "   PIN \(d.pin!)" : ""))
                                    .font(.caption.monospaced()).foregroundStyle(.secondary)
                                    .textSelection(.enabled)
                            }
                        }
                    } else {
                        Text("未绑定").foregroundStyle(.secondary)
                    }
                    Spacer()
                    Button("选择设备…") { picking = true }
                    Button("清除") { model.bridge.console("forget \(slot + 1)") }
                        .disabled(s.device == nil)
                }

                HStack {
                    Picker("模式", selection: Binding(get: { s.info.raw }, set: { v in options { $0.raw = v } })) {
                        Text("line").tag(false)
                        Text("raw").tag(true)
                    }
                    .pickerStyle(.segmented)
                    .frame(width: 180)
                    Toggle("离线暂存 hold", isOn: Binding(get: { s.info.hold }, set: { v in options { $0.hold = v } }))
                }

                Text("epoch \(s.info.epoch)   丢弃 下行 \(s.info.downLost) B / 上行 \(s.info.upLost) B")
                    .font(.caption.monospaced())
                    .foregroundStyle(.secondary)

                HStack {
                    TextField("注入一行（自动加 \\r\\n）", text: $inject)
                        .font(.callout.monospaced())
                        .onSubmit(send)
                    Button("注入", action: send)
                    Toggle("每秒", isOn: Binding(
                        get: { s.repeating != nil },
                        set: { on in
                            let i = slot, text = inject
                            model.perform { $0.links.setRepeat(i, on ? text : nil) }
                        }))
                }
            }
            .font(.callout)
        } label: {
            HStack {
                Label("槽位 \(slot + 1)", systemImage: "antenna.radiowaves.left.and.right")
                StateBadge(state: s.state)
            }
        }
        .sheet(isPresented: $picking) { DevicePicker(slot: slot) }
    }

    private func send() {
        let i = slot, data = Data((inject + "\r\n").utf8)
        model.perform { $0.links.inject(i, data) }
    }

}

struct StateBadge: View {
    let state: String
    var body: some View {
        let (text, color): (String, Color) = switch state {
        case "connected": ("已连接", .green)
        case "dialing": ("连接中", .orange)
        default: ("空闲", .secondary)
        }
        Text(text)
            .font(.caption.bold())
            .padding(.horizontal, 6).padding(.vertical, 1)
            .background(color.opacity(0.18), in: Capsule())
            .foregroundStyle(color)
    }
}

struct DevicePicker: View {
    @EnvironmentObject var model: BridgeModel
    @Environment(\.dismiss) private var dismiss
    let slot: Int
    @State private var mac = ""
    @State private var pin = ""

    private func bind(_ target: String) {
        let p = pin.trimmingCharacters(in: .whitespaces)
        model.bridge.console("pair \(slot + 1) \(target)" + (p.isEmpty ? "" : " \(p)"))
        dismiss()
    }

    var body: some View {
        let results = model.snapshot.scanResults.sorted { ($0.paired ? 1 : 0, $0.rssi) > ($1.paired ? 1 : 0, $1.rssi) }
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Text("槽位 \(slot + 1)：选择经典蓝牙 SPP 设备").font(.headline)
                Spacer()
                Button(model.snapshot.scanning ? "扫描中…" : "扫描") { model.bridge.console("scan") }
                    .disabled(model.snapshot.scanning)
            }
            HStack {
                TextField("MAC，例如 00:80:25:aa:bb:cc", text: $mac).font(.callout.monospaced())
                TextField("PIN（可选，默认 0000）", text: $pin).font(.callout.monospaced()).frame(width: 170)
                Button("绑定") { bind(mac.trimmingCharacters(in: .whitespaces)) }
                    .disabled(normalizeMac(mac.trimmingCharacters(in: .whitespaces)) == nil)
            }
            List {
                Button {
                    bind("virtual")
                } label: {
                    Label("虚拟设备（在窗口里注入数据）", systemImage: "keyboard")
                }
                ForEach(results) { r in
                    Button {
                        bind(r.id)
                    } label: {
                        HStack {
                            Text(r.name.isEmpty ? "（无名称）" : r.name)
                            if r.paired { Text("已配对").font(.caption).foregroundStyle(.secondary) }
                            Spacer()
                            if r.rssi != 0 { Text("\(r.rssi) dBm").foregroundStyle(.secondary) }
                            Text(r.id).font(.caption.monospaced()).foregroundStyle(.secondary)
                        }
                    }
                }
            }
            .buttonStyle(.plain)
            HStack {
                Text("点击列表项即绑定，上面的 PIN 一并保存。扫描约 10 秒，期间所有槽位断开（与固件相同）。")
                    .font(.caption).foregroundStyle(.secondary)
                Spacer()
                Button("关闭") { dismiss() }.keyboardShortcut(.cancelAction)
            }
        }
        .padding(16)
        .frame(width: 600, height: 480)
    }
}

// ---- Log + console ----

struct LogPane: View {
    @EnvironmentObject var model: BridgeModel
    @State private var command = ""
    @State private var follow = true

    private static let timeFormat: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "HH:mm:ss.SSS"
        return f
    }()

    var body: some View {
        VStack(spacing: 0) {
            HStack {
                ForEach(LogKind.allCases) { kind in
                    Toggle(kind.rawValue, isOn: Binding(
                        get: { model.shown.contains(kind) },
                        set: { on in if on { model.shown.insert(kind) } else { model.shown.remove(kind) } }))
                        .toggleStyle(.button)
                }
                Spacer()
                Toggle("设备原始数据", isOn: Binding(
                    get: { model.snapshot.monitor },
                    set: { on in model.bridge.console("monitor \(on ? "on" : "off")") }))
                Toggle("跟随", isOn: $follow)
                Button("清空") { model.clearLog() }
            }
            .controlSize(.small)
            .padding(8)
            Divider()
            let lines = model.lines.filter { model.shown.contains($0.kind) }
            ScrollViewReader { proxy in
                ScrollView {
                    LazyVStack(alignment: .leading, spacing: 1) {
                        ForEach(lines) { line in
                            HStack(alignment: .firstTextBaseline, spacing: 8) {
                                Text(Self.timeFormat.string(from: line.time)).foregroundStyle(.secondary)
                                Text(line.kind.rawValue).foregroundStyle(color(line.kind)).frame(width: 44, alignment: .leading)
                                Text(line.text).textSelection(.enabled)
                            }
                            .id(line.id)
                        }
                    }
                    .font(.caption.monospaced())
                    .padding(8)
                    .frame(maxWidth: .infinity, alignment: .leading)
                }
                .onChange(of: lines.last?.id) { _, id in
                    if follow, let id { proxy.scrollTo(id, anchor: .bottom) }
                }
            }
            Divider()
            HStack {
                Text("$").font(.callout.monospaced())
                TextField("命令（与固件 USB 串口相同，例如 status、mode 1 raw）", text: $command)
                    .textFieldStyle(.plain)
                    .font(.callout.monospaced())
                    .onSubmit {
                        let line = command.trimmingCharacters(in: .whitespaces)
                        guard !line.isEmpty else { return }
                        model.bridge.console(line)
                        command = ""
                    }
            }
            .padding(8)
        }
    }

    private func color(_ kind: LogKind) -> Color {
        switch kind {
        case .system: .secondary
        case .phone: .blue
        case .device: .purple
        case .down: .green
        case .up: .orange
        case .control: .teal
        }
    }
}
