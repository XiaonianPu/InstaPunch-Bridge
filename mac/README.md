# Mac 测试转发器 / Mac test relay

开发板到货前在 Mac 上模拟 InstaPunch Bridge，用来联调 InstaPunch Mapper。和固件一样：下游槽位连**经典蓝牙 SPP** 设备（IOBluetooth RFCOMM），向手机提供相同的 BLE 服务、帧格式和控制命令（[docs/protocol.md](../docs/protocol.md) v1）。封包直接编译固件的 `main/frame.c`（符号链接），`Pipeline.swift`、`PhoneServer.swift`、`DeviceLinks.swift`、`Commands.swift` 分别对应 `pipeline.c`、`ble_server.c`、`links.c`、`commands.c`。

A macOS stand-in for the bridge: classic SPP devices downstream (IOBluetooth RFCOMM), the same GATT service, frames and commands towards the phone; it compiles the firmware's `main/frame.c`.

## 构建运行

```bash
./build-app.sh debug --open
```

生成 `build/InstaPunch-Bridge-Mac.app`（临时签名）。第一次运行时 macOS 会请求蓝牙权限。

## 使用

- **槽位**：点“选择设备…”，列表先列出这台 Mac 已配对的经典蓝牙设备，点“扫描”再做约 10 秒 inquiry；点一项即绑定，也可以手填 MAC。PIN 可选，旧式 PIN 配对时使用，未填用 `0000`，SSP 自动确认。也可选“虚拟设备”，在输入框里注入行（自动加 `\r\n`，可每秒重复）。
- **连接流程**与固件相同：一次只拨一个槽位；未配对先配对，再找 SPP（0x1101）的 RFCOMM 通道（优先用系统缓存的 SDP 记录），然后打开通道。设备正被系统连接（例如在系统设置里点了“连接”）时会先断开系统连接，否则 RFCOMM 打不开；失败 5 秒后重试，30 秒超时；断开后自动重连。扫描会等当前拨号结束，并在扫描期间断开所有槽位。
- **模式 / 离线暂存**：与 `mode`、`hold` 命令相同，保存在 UserDefaults。
- **控制台**：相当于固件的 USB 串口，命令与固件一致（`pair <槽位> <MAC> [PIN]` 等），另加 `pair <槽位> virtual`；`unbind`、`monitor on|off` 只能在这里用。
- **手机端**：广播 `InstaPunch-Bridge-Mac`。默认特征值要求加密（与固件相同，iOS 会弹配对框）；联调时可关掉。

## 与固件的差异

| 项 | 固件 | Mac 测试版 |
|---|---|---|
| `info` | `bridge 0.1.0 …` | `bridge 0.1.0-mac proto 1 slots 2` |
| 槽位设备 | — | 另有虚拟设备（`status` 中 MAC 一栏为 `virtual`） |
| 主人 | 第一台完成配对的手机；拒绝其他配对，30 秒未加密断开 | 第一台订阅或写入的手机；其他手机写入回授权错误、通知不发给它，但 macOS 无法断开它 |
| `unbind` | 删除绑定 | 只清除记录的主人；系统里的配对需在手机和 Mac 的蓝牙设置中手动移除。Mac 端配对没了而手机还留着时，连接报 “Peer removed pairing information”，在手机“设置 › 蓝牙”里忽略这台 Mac 后重连 |
| 广播 | 已连接时停止；名称在扫描响应里 | 主人订阅数据后停止（外设端看不到连接）；CoreBluetooth 放不下名称，扫描软件显示 N/A，按服务 UUID 能找到；macOS 同一地址还会轮流发系统自己的广播，扫描软件里服务 UUID 会时有时无 |
| 发送闸门 | 在途 4 包 + 拥塞事件 | `updateValue` 返回 false 时保留该帧，`peripheralManagerIsReady` 后先补发，不丢帧 |
| 上行写入 | `esp_spp_write` + 拥塞事件 | RFCOMM `writeAsync`，按通道 MTU 分片，写完一整条再取下一条 |
| 设备配对 | 由固件完成，配对信息存在 ESP32 | 由 macOS 完成，设备会出现在 Mac 的蓝牙设置里；`paired` 列出的是 Mac 的已配对设备 |
| `scan` 的 `spp` 标记 | 来自设备 EIR | macOS 不提供 EIR，只有系统已缓存该设备的 SDP 记录时才标 `spp`；名称由 inquiry 自动补查 |
