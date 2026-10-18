# 第一次烧录 Atom Lite（2026-10-18）

类型：**开发板实测**（启动与 USB 串口命令）。未连接任何 SPP 设备或手机。

硬件：M5Stack Atom Lite。esptool 识别为 ESP32-PICO-D4 rev v1.1，WiFi + BT，双核，内置 4 MB flash。esptool 提示“检测到晶振 41.01 MHz，与 40 MHz 差别较大”，随后按 40 MHz 工作；启动和串口通信均正常。

环境：macOS，ESP-IDF v5.4.2，串口 `/dev/cu.usbserial-*`，115200。

## 结果

- `idf.py build`：`instapunch_bridge.bin` 0xe4660 字节，3 MB 分区剩余 70%
- `idf.py -b 115200 flash`：bootloader、应用、分区表三段写入并通过哈希校验，应用约 50 秒
- 启动日志：分区表与 `partitions.csv` 一致（nvs / phy_init / factory 3 MB）；`BTDM_INIT` 双模控制器启动，`bridge: dual-mode controller up`；`free heap 118772 bytes, minimum 118308`
- USB 串口命令：`info` → `bridge 0.1.0 proto 1 slots 2`；`status` → `1 none`、`2 none`、`phone none`

## 未验证

SPP 连接与重连、PIN / SSP 配对、`scan`、BLE 广播与扫描响应名称、iOS 配对与订阅、拒绝第二台手机、`unbind`、NVS 重启保留、发送闸门与 ack 重发、网页烧录、GitHub Actions。

## 第 2 次烧录：广播名改为 `InstaPunch-Bridge-XXXX`（同日）

- 编译通过（0xe4660 字节）；镜像中含 `InstaPunch-Bridge-%02X%02X` 格式串
- 烧录三段均通过哈希校验；启动正常，`free heap 117544 bytes`；`info`、`status` 正常
- 启动约 2.5 秒后，之前已配对的手机自动重连并加密（`ble: phone … encrypted`），说明 BLE 绑定在重新烧录后仍保留（NVS 未擦除）
- 因手机已连接、盒子停止广播，未从空中确认新名称；手机已缓存旧名称，可能仍显示 `IPM-Bridge-XXXX`

## 第 3 次烧录：状态灯（同日，用户要求）

`main/status_led.c`：G27 上的 SK6812 用 RMT 字节编码器驱动（0.1 µs 分辨率，0 = 0.3/0.9 µs，1 = 0.6/0.6 µs，GRB），蓝色，亮度 40/255。手机未连接（GATT 未连接）时亮 1 秒灭 1 秒；连接后每 3 秒亮 80 ms。连接 / 断开事件会立即切换，不等当前周期结束。

- 编译通过，`instapunch_bridge.bin` 0xe7620 字节；烧录通过哈希校验
- 启动无 RMT 错误；`free heap 113484 bytes`（比上一版少约 4 KB）；已配对手机约 1.5 秒后自动重连
- 用户目视确认：手机连着时每 3 秒快闪；关掉手机蓝牙后变为亮 1 秒灭 1 秒

## 第 4 次烧录：扫描结果供 App 选择（同日，用户要求）

`scan` 的 `found` 行加上 COD、主设备类和 SPP 提示（EIR 中的 0x1101）；inquiry 结束后对无名设备逐个查询名称（最多 8 台，单台 10 秒超时，`name` 行）；新增 `paired` 命令列出经典蓝牙配对设备。格式见 `docs/protocol.md` §5。Mac 转发器同步（编译通过，未运行验证）。

- 编译通过，无警告，`instapunch_bridge.bin` 0xe7cf0 字节；烧录通过哈希校验
- USB 串口 `scan`：附近只有一台可被发现的经典蓝牙设备（银河 G1），回复 `found 00:80:25:… -62 001f00 uncategorized spp <序列号>` 与 `scan done`。G1 的 EIR 中列出了 SPP，名称也来自 EIR
- `paired`：开发板尚未与任何设备配对，只回复 `paired done`

未验证：名称补查（这次没有无名设备）与其超时、`paired` 有记录时的输出、经 BLE 控制特征值发 `scan`（与 USB 共用命令代码，回复走 notify）、扫描结束后槽位重连。

## 开发板第一次连上 SPP 设备（同日，用户经 Mapper 绑定）

用户在 InstaPunch Mapper 中把银河 G1 绑到槽位 1 并填写 PIN（经 BLE 控制特征值发 `pair`）。读串口时开发板因打开串口复位，复位后：

- 约 1.3 秒开始 SDP（`links: 1: discovering 00:80:25:…`），约 2 秒 `links: 1: connected`；`status` 为 `dialing` → 已连接
- `paired` 回复 `paired 00:80:25:… 1` 与 `paired done`，说明配对信息在复位后保留
- 已配对手机约 2.6 秒后自动重连并加密
- `monitor on` 后 14 秒内收到 42 行 G1 输出（`$GPGGA`、`$GPGST`、`$GPZDA`、`$GPGSA`、`$GPGSV`、`$PTNL,BPQ`、`$GPXXX`），约每秒一组；室内无定位，`$GPZDA` 时间有效

这是开发板与真实 SPP 设备的第一次连通，也是第一次经 BLE 控制特征值配置槽位。未验证：数据经 BLE 到达 Mapper 后的解析、上行写入、断开重连。
