# InstaPunch Bridge 项目约定

本文件适用于整个仓库，写给在本仓库工作的维护者与 AI 编码助手。本仓库是 iOS App [InstaPunch Mapper](https://instapunch.cn/mapper) 的蓝牙中转盒固件，独立构建、独立发布，以 MIT 许可证开源。

本仓库只开源供使用，不接受外部 Pull Request（见 [CONTRIBUTING.md](CONTRIBUTING.md)）。不要为外部贡献者添加贡献流程、PR 模板或合并说明。

## 面向用户：简单易用

- 目标用户只买设备、插线、点一下就能烧录，不安装开发工具，不改源码，不跑 menuconfig。
- 发布物是一个合并镜像 `instapunch-bridge-merged.bin`（写到地址 0），通过 `web/` 的 ESP Web Tools 页面或 esptool 烧录。CI（`.github/workflows/build.yml`）负责构建。
- 用户相关的配置（下游设备 MAC、PIN 等）一律运行时写入 NVS，经 BLE 控制特征值或 USB 串口命令修改（同一套命令，`main/commands.c`）。不要把设备信息做成编译期选项。
- 推荐硬件是 M5Stack Atom Lite（不需焊接）。支持其他板子时按板子区分引脚，不要求用户改代码。
- 用户可见的文档和网页同时提供中文与英文。

## 技术约束

- 只用初代 ESP32（双模 BR/EDR + BLE），ESP-IDF v5.4.2，Bluedroid（NimBLE 不支持经典蓝牙），控制器模式 BTDM，不开 Wi-Fi。
- 开发环境用 CLion；`CMakeLists.txt` 保持标准 ESP-IDF 结构，不加 IDE 专属文件（`.idea/`、`cmake-build-*/` 不提交，已列入 `.gitignore`）。
- 自己的符号加 `bridge_` 前缀或设为 `static`：Bluedroid 导出了 `config_save` 等通用名字。
- 固件只做 SPP ↔ BLE 透传（设计决定，2026-10-17）：槽位只有编号，不绑定设备类型；槽位对应 RTK、激光测距还是其他设备由 Mapper 设置。数据（NMEA、RTCM、`$PLTIT` 等）原样转发，解析放在 App 端的 MapperCore。不在固件里猜测南方 `@SIC` 设置指令。
- 手册：[开发手册](docs/development.md)、[使用手册](docs/user-guide.md)（英文版 `user-guide.en.md`，两版同步修改）。改命令、行为或烧录流程时一并更新。
- BLE 协议的事实来源是 [docs/protocol.md](docs/protocol.md)。改 UUID、帧格式或特征值时提升协议版本，并由维护者同步 InstaPunch Mapper 端；发布后保持旧版本 App 可连接。
- 分区表单 factory 3 MB（不带 OTA）；需要 OTA 时再调整，并确认固件体积。
- 广播包只放标志和 128 位服务 UUID，名称放扫描响应（两者合计超过 31 字节）。App 按服务 UUID 扫描，不靠名称。

## Mac 测试转发器（`mac/`）

没有开发板时用来联调 Mapper（2026-10-17 建立，已与银河 G1 和 iPhone 连通）。见 [mac/README.md](mac/README.md) 与 `validation/2026-10-17-mac-relay/`。

- 结构跟固件一一对应：SwiftPM 的 `BridgeFrame` 目标通过符号链接直接编译 `main/frame.c`；`Pipeline.swift`、`PhoneServer.swift`、`DeviceLinks.swift`、`Commands.swift`、`Store.swift` 分别对应 `pipeline.c`、`ble_server.c`、`links.c`、`commands.c`、`bridge_config.c`。改协议、调度或命令时两边同步，不可避免的差异写进 `mac/README.md` 的差异表。
- 槽位下游必须是经典蓝牙 SPP（与固件一致；最初的 BLE GATT 版本已放弃），走 IOBluetooth RFCOMM；手机端是 CoreBluetooth `CBPeripheralManager`。除了窗口注入数据的虚拟设备，不加其他下游类型。
- 构建用 `mac/build-app.sh`，必须打成带 `NSBluetoothAlwaysUsageDescription` 的 `.app`：裸可执行文件一碰蓝牙就会被 TCC 杀掉（退出码 134）。写独立探测程序时也一样，用 `open -W -n --stdout <文件> X.app --args …` 运行才能看到输出。
- 整个转发器跑在主队列：IOBluetooth 的回调只在主 run loop 上触发。

### macOS 蓝牙踩过的坑（macOS 26，2026-10-17 实测）

- `performSDPQuery(_:uuids:)`（按 UUID 过滤）返回成功但永远不回调；不带过滤的 `performSDPQuery(_:)` 正常。已配对设备优先用系统缓存的记录（`getServiceRecord(for:)` → `getRFCOMMChannelID`）。
- 系统已连着设备时（例如用户在系统设置里点了“连接”），`openRFCOMMChannelAsync` 永远不回调，同步版返回 `0xe00002bc`。要先 `closeConnection()`，等约 2 秒再打开。
- 系统设置能连上 SPP 设备，不代表 App 能打开 RFCOMM；排查时先写独立探测程序逐项试（缓存记录、SDP、异步 / 同步打开、断开后打开），不要凭猜测改代码。
- 外设端看不到手机的连接和断开，也不能断开手机、不能管理配对；“主人”只能按第一台订阅或写入的手机记录。
- CoreBluetooth 自己安排广播内容：名称放不下就不带，扫描软件显示 N/A；macOS 同一地址还会轮流发系统广播，服务 UUID 时有时无。都属正常，按服务 UUID 能找到。
- 手机连接报 “Peer removed pairing information”：手机还留着旧配对、Mac 端已没有。让用户在 iPhone“设置 › 蓝牙”里忽略这台 Mac（显示为电脑名）后重连。固件 `unbind` 后也会出现，属预期。

### 银河 G1 的 SPP 输出

- SPP 在 RFCOMM 1，MTU 329，连上后不用发命令就约每秒输出一组 `$GPGGA`、`$GPGST`、`$GPZDA`、`$GPGSA`、`$GPGSV`，另有 `$PTNL,BPQ` 和 `$GPXXX`（`$GPXXX` 含设备序列号，不要写进仓库）。没有定位时字段为空。

## 开源与验证

- 不提交设备序列号、真实 MAC、PIN、个人信息或密钥。示例用 `00:80:25:aa:bb:cc` 这类占位值。
- 封包逻辑放在不依赖 ESP-IDF 的 `main/frame.c`，改动后运行 `make -C test`；测试数据用真实设备输出（如 TruPulse 的 `$PLTIT` 行）。
- 没有真机验证的功能不能写成“已完成”。验证记录放在 `validation/<日期>-<主题>/`，区分编译、开发板实测和与 App 联调。
- 安全模型是“只认已绑定的手机”（设计决定，2026-10-17）：第一台完成 BLE 配对的手机成为主人，其他手机的配对一律拒绝。清除主人（2026-10-22 用户决定）：USB `unbind`；主人手机在加密链路上发送 `unbind`（App 中转盒详情的“解除绑定”）；长按 Atom Lite 按键（G39）10 秒，同时清除两个槽位与所有经典蓝牙配对（`main/reset_button.c`）。不要加免加密的特征值或其他人可触发的解绑。
