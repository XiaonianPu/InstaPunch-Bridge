# Mac 测试转发器（2026-10-17）

类型：**Mac 本机构建 + 窗口冒烟测试**。未与 InstaPunch Mapper 或任何手机联调，未连接真实下游 BLE 设备。

环境：macOS 26（arm64），Swift 6.4（Swift 5 语言模式），SwiftPM debug 构建，`build-app.sh` 临时签名。

## 结果

- `swift build` 成功，无警告；`main/frame.c` 经符号链接与固件同一份源码编译
- 启动后蓝牙权限已授予：外设与主机两侧均“已开启”，开始广播 `IPM-Bridge-Mac`
- 扫描 10 秒列出周边 BLE 外设（UUID、RSSI、名称），`found …` 行格式与固件一致
- 槽位 1 绑定虚拟设备：推送 `event link 1 up 1 virtual`，epoch 变为 1；开启 hold 后注入 `$PLTIT` 行，无手机时不发帧、丢弃计数为 0

## 未验证

- iPhone 订阅、iOS 配对弹窗、MTU、下行帧到达、上行写入、ack 重发、`event lost`
- 真实 BLE 外设的连接、重连、特征值切换；Terminal I/O 额度流控（银河 G1）
- 非主人手机被拒绝的行为

## 第 2 轮：槽位改为经典蓝牙 SPP（同日，用户要求）

下游由 BLE GATT 改为 IOBluetooth RFCOMM，与固件一致；去掉特征值选择与 Terminal I/O 额度流控。整个转发器改在主队列运行（IOBluetooth 回调在主 run loop）。

- `swift build` 成功，无警告
- 设备选择窗列出 Mac 已配对的经典蓝牙设备；inquiry 扫描约 16 秒后回复 `scan done`
- 槽位 2 绑定已配对的银河 G1（`pair 2 <MAC>` 回复 `ok`），进入 SDP 查询后停在“连接中”，25 秒内没有结果；当时 G1 是否开机、是否在范围内未确认，30 秒超时重试的路径未观察到

未验证：SPP 实际收发、旧式 PIN 配对、断开重连、上行写入分片、扫描期间断开槽位后的重连，以及第 1 轮列出的手机端各项。

## 第 3 轮：SPP 连不上的原因与修复（同日）

用户反馈：在系统蓝牙设置中能连上 G1，但转发器连不上。用独立探测程序（同一 Info.plist 的临时 .app）对 G1（已配对，系统缓存了 3 条 SDP 记录，SPP 在 RFCOMM 1）逐项测试，macOS 26：

| 操作 | 结果 |
|---|---|
| `performSDPQuery(_:uuids:)`（按 SPP UUID 过滤） | 返回成功，20 秒内无回调（第 2 轮卡住的原因） |
| `performSDPQuery(_:)`（不过滤） | 正常回调，status 0 |
| 系统已连接时 `openRFCOMMChannelAsync(1)` | 返回成功，50 秒内无回调 |
| 系统已连接时 `openRFCOMMChannelSync(1)` | 3 秒后返回 `0xe00002bc`（kIOReturnError） |
| `closeConnection()` 后再 `openRFCOMMChannelAsync(1)` | 立即打开，MTU 329，持续收到 NMEA |

结论：macOS 支持 RFCOMM，问题出在两处用法。修复：优先读取系统缓存的 SPP 记录，没有缓存时才做不带过滤的 SDP 查询；设备已有系统连接时先 `closeConnection()`，2 秒后再打开 RFCOMM。

修复后实测（Mac 构建 + 真实设备，未接手机）：
- 槽位 2 自动拨号，约 10 秒后 `event link 2 up 1 <设备名>`（G1 的设备名就是序列号，此处略去）
- `monitor on` 后约每秒收到一组 G1 输出：`$GPGGA`、`$GPGST`、`$GPZDA`、`$GPGSA`、`$GPGSV`，以及 `$PTNL,BPQ`、`$GPXXX`（室内无定位，字段为空）
- 这次拨号时系统没有连着 G1，“先断开系统连接”这一步没有在转发器里走到；该路径只在探测程序中验证过

未验证：上行写入、断开后重连、手机端订阅与收帧。
