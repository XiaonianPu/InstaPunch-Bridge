# InstaPunch Bridge 开发手册

本仓库只开源供使用，不接受外部 Pull Request（见 [CONTRIBUTING.md](../CONTRIBUTING.md)）。本手册写给维护者，以及 Fork 后自行修改的开发者。

项目约定见 [AGENTS.md](../AGENTS.md)，BLE 协议见 [protocol.md](protocol.md)，给最终用户的说明见 [使用手册](user-guide.md)。

状态（2026-10-18）：已在 Atom Lite 上与银河 G1、InstaPunch Mapper 实测可用；TruPulse 360B 未测。Mac 测试转发器已与银河 G1、iPhone 连通。

## 1. 它做什么

Bridge 是一个蓝牙中转盒：初代 ESP32 用经典蓝牙 SPP 连接外业设备（RTK、激光测距等），再通过 BLE GATT 把字节流转给 iPhone / iPad 上的 InstaPunch Mapper。iOS 不允许普通 App 直接用经典蓝牙 SPP（需要 MFi），这个盒子把它换成 BLE。

固件只做透传：

- 两个编号槽位（1、2），每个绑定一台 SPP 设备；槽位不知道设备类型，由 Mapper 决定按什么格式解析。
- 数据原样转发，不解析 NMEA、RTCM、`$PLTIT`，也不发设备设置指令。
- 只认一台手机（第一台完成配对的“主人”）。

## 2. 架构

```
 SPP 设备 1 ─┐                                      ┌─ 下行 notify（帧）
             ├─ links.c ─ pipeline.c ─ ble_server.c ┼─ 上行 write（帧）
 SPP 设备 2 ─┘     ↑        (frame.c)        ↑       └─ 控制 write + notify（文本）
                   └──────── commands.c ─────┘
                                ↑
                  console.c（USB 串口）/ 控制特征值
```

| 文件 | 职责 |
|---|---|
| `main/main.c` | 初始化 NVS、BTDM 双模控制器与 Bluedroid，依次启动各模块 |
| `main/links.c` | 经典蓝牙侧：每槽位一条 SPP 链路，后台重拨；inquiry 扫描；PIN / SSP 配对 |
| `main/pipeline.c` | 调度：每槽位的下行缓冲、确认窗口、上行队列；发送闸门；丢弃统计与 `event lost` |
| `main/frame.c` | 封包与解析，不依赖 ESP-IDF，可在电脑上测试 |
| `main/ble_server.c` | BLE 侧：GATT 服务、广播、主人绑定、MTU、通知 |
| `main/commands.c` | 文本命令，USB 串口与 BLE 控制特征值共用 |
| `main/console.c` | USB 串口读行，交给 `commands.c` |
| `main/bridge_config.c` | NVS 读写：槽位设备、PIN、模式、暂存开关 |
| `main/status_led.c` | 状态灯（G27 SK6812，RMT 驱动）：未连接手机时亮 1 秒灭 1 秒，连接后每 3 秒快闪 80 ms |

### 线程模型

- `pipeline.c` 有一个泵任务（10 ms 周期，或被事件唤醒），独占所有槽位缓冲。蓝牙回调只投递事件，并持锁调用 `pipeline_*`。
- `pipeline.c` 不会在持有自己锁的同时等待 `links.c` 的锁；调用方也不要在持有 links 锁时调用 pipeline。
- `links.c` 一次只做一个 SDP / 连接（`ESP_SPP_DISCOVERY_COMP_EVT` 不带对端地址）；有扫描请求时等当前拨号结束，扫描期间断开所有槽位。失败 5 秒后重试；30 秒报告拨号缓慢，继续保留请求归属直到协议栈终结回调，避免迟到结果污染下一次拨号。

### 关键参数

| 参数 | 值 | 位置 |
|---|---|---|
| 下行缓冲 / 槽位 | 8 KB | `frame.h` `DOWN_CAP` |
| 确认窗口 / 槽位 | 2 KB | `WINDOW_CAP` |
| 上行队列 / 槽位，单条上限 | 4 KB，1100 字节 | `UP_QUEUE_CAP`、`UP_RECORD_MAX` |
| raw 模式攒包 / line 模式等行尾 | 15 ms / 200 ms | `FRAME_FLUSH_MS`、`FRAME_LINE_TIMEOUT_MS` |
| hold 槽位帧长上限 | 182 字节 | `FRAME_HOLD_MAX` |
| 本地 MTU | 517 | `ble_server.c` |
| 在途通知上限 | 4 | `pipeline.c` `MAX_IN_FLIGHT` |
| 未加密断开 | 30 秒 | `PAIRING_TIMEOUT` |

### NVS（命名空间 `bridge`）

| 键 | 内容 |
|---|---|
| `mac0` / `mac1` | 槽位 1 / 2 的设备 MAC（6 字节） |
| `pin0` / `pin1` | 旧式 PIN（可无） |
| `mode0` / `mode1` | 0 = line，1 = raw |
| `hold0` / `hold1` | 离线暂存开关 |

BLE 绑定（主人）由 Bluedroid 自己存在 NVS 中。`forget` 只清设备，保留模式与暂存设置。

## 3. 开发环境

只能用初代 ESP32（双模）。S2 / S3 / C / H 系列没有经典蓝牙。推荐 [M5Stack Atom Lite](https://shop.m5stack.com/products/atom-lite-esp32-development-kit)（ESP32-PICO-D4，4 MB flash，无 PSRAM）。

1. 安装 ESP-IDF v5.4.2（[官方说明](https://docs.espressif.com/projects/esp-idf/en/v5.4.2/esp32/get-started/)），运行 `install.sh esp32`。macOS 上还需 `python tools/idf_tools.py install cmake ninja`。
2. 如果系统默认 `python3` 比安装 ESP-IDF 时用的新，写一个包装脚本：先把指向旧版 Python 的目录放到 `PATH` 前面，再加载 `export.sh`。之后终端和 CLion 都加载这个脚本。
3. CLion：
   - *Settings › Build, Execution, Deployment › Toolchains*：新建工具链，*Add environment › From file* 选上一步的脚本（或 `export.sh`）。
   - *CMake*：选这个工具链，生成器 Ninja，CMake options 填 `-DIDF_TARGET=esp32`。
   - 烧录 / 串口：新建 *Shell Script* 运行配置，分别执行下一节的 `flash` 与 `monitor`。
   - `.idea/` 不提交，`CMakeLists.txt` 保持标准 ESP-IDF 结构。

## 4. 编译、烧录、看日志

```bash
. $IDF_PATH/export.sh            # 或自己的包装脚本
idf.py build
idf.py -p /dev/cu.usbserial-* -b 115200 flash
idf.py -p /dev/cu.usbserial-* monitor   # Ctrl+] 退出
```

- Atom Lite 用高波特率烧录偶尔失败，统一用 115200（应用约 50 秒）。它没有引出 JTAG，只能看串口日志。
- esptool 可能提示 Atom Lite 晶振约 41 MHz 与 40 MHz 不符，随后按 40 MHz 工作，不影响使用。
- 正常启动日志末尾应有 `bridge: dual-mode controller up` 和 `free heap …`（首次实测约 118 KB）。
- 在 `monitor` 里直接输入命令（`info`、`status`、`scan`、`pair 1 <MAC> [PIN]`、`monitor on` 等，见 [protocol.md §5](protocol.md#5-控制命令与事件)）。
- 生成给网页烧录用的单文件镜像：`idf.py merge-bin -o instapunch-bridge-merged.bin`（写到地址 0）。

## 5. 测试

### 主机测试（封包逻辑）

```bash
make -C test
```

Apple clang / gcc，`-Wall -Wextra -Werror`，ASan + UBSan。改 `main/frame.c` 后必须运行。测试数据用真实设备输出（如 TruPulse 的 `$PLTIT` 行）；协议文档里的示例帧要与测试逐字节一致。

### 开发板测试

建议顺序：

1. 启动日志、`info`、`status`。
2. `scan` 找设备，`pair 1 <MAC> [PIN]`，`monitor on` 看设备数据是否进来。
3. 手机用 nRF Connect 连 `InstaPunch-Bridge-XXXX`：订阅数据特征值，确认帧头（槽位、标志、会话号、帧序号）与负载；写控制特征值 `status`。
4. 第二台手机应被拒绝配对；USB 或 BLE `unbind`、长按按键 10 秒后可换手机（按键还应清空两个槽位）。
5. `hold 1 on` 后取消订阅再订阅，应收到带 `REPLAY` / `AGE` 的补发帧；写 `ack` 后不再重发已确认的帧。
6. 设备关机再开机应自动重连，`epoch` 加 1。

测之前确认设备没被别的主机占着：经典蓝牙 SPP 设备通常同时只接受一条连接（例如 Mac 的系统蓝牙或 Mac 测试转发器连着它时，开发板连不上）。

### Mac 测试转发器

没有开发板或想先测 App 时，用 [mac/](../mac/README.md)：同样的 BLE 服务、帧格式和命令，槽位经 IOBluetooth 连接经典蓝牙 SPP 设备，也可以选“虚拟设备”手动注入数据。

```bash
mac/build-app.sh debug --open
```

结构与固件一一对应（`Pipeline.swift` ↔ `pipeline.c` 等），`frame.c` 是同一份源码。macOS 蓝牙的已知坑（SDP 过滤查询不回调、系统连着设备时 RFCOMM 打不开、广播名显示 N/A、“Peer removed pairing information”）见 [AGENTS.md](../AGENTS.md) 的“macOS 蓝牙踩过的坑”。

## 6. 修改规则

- **协议**：事实来源是 [protocol.md](protocol.md)。改 UUID、帧格式、特征值或命令语义时提升协议版本（`info` 返回的 `proto`），由维护者确认 InstaPunch Mapper 端兼容；发布后旧版本 App 仍要能连。
- **用户配置**一律运行时写 NVS，经命令修改；不要做成编译期选项或 menuconfig 项。
- **安全**：只认已绑定的手机；清除主人只经 USB `unbind`、主人手机在加密链路上发送的 `unbind`、或长按按键 10 秒（2026-10-22 用户决定）；不加免加密特征值。
- **命名**：自己的符号加 `bridge_` 前缀或设为 `static`（Bluedroid 导出了 `config_save` 等通用名字）。
- **Mac 转发器**：改协议、调度或命令时两边同步；无法一致的地方写进 `mac/README.md` 的差异表。
- **开源**：不提交设备序列号、真实 MAC、PIN、个人信息或密钥。示例用 `00:80:25:aa:bb:cc`。注意有的设备名就是序列号（如银河 G1），`$GPXXX` 语句里也带序列号。
- **用户文档**：用户可见的文档和网页同时提供中文与英文。

## 7. 发布

1. 推送到 `main` 或提 PR 时，GitHub Actions（`.github/workflows/build.yml`）用 ESP-IDF v5.4.2 编译，生成 `instapunch-bridge-merged.bin`，连同 `web/` 打包为构件。
2. 打 `v*` 标签时，合并镜像附到 GitHub Release。
3. 网页烧录页 `web/index.html` + `manifest.json`（ESP Web Tools）需要 HTTPS：推送到 `main` 时 CI 的 `pages` 任务把它们与合并镜像一起发布到 GitHub Pages（<https://xiaonianpu.github.io/InstaPunch-Bridge/>）。仓库 Settings › Pages 的 Source 须设为 “GitHub Actions”。`manifest.json` 的 `version` 与 `commands.h` 的 `BRIDGE_FIRMWARE_VERSION` 一起更新。

分区表是单个 3 MB factory 分区，不带 OTA；当前固件约 0.9 MB。需要 OTA 时重新规划分区并确认体积。

CI 与网页烧录页尚未实际运行过。

## 8. 验证记录

放在 `validation/<日期>-<主题>/`，写明类型（主机测试 / 编译 / 开发板实测 / 与 App 联调 / Mac 转发器），列出已验证和未验证的项目。没有真机验证的功能不能写成“已完成”。

| 记录 | 内容 |
|---|---|
| [2026-10-17-first-build](../validation/2026-10-17-first-build/README.md) | 编译、主机测试 14 项 |
| [2026-10-17-mac-relay](../validation/2026-10-17-mac-relay/README.md) | Mac 转发器；macOS RFCOMM 排查；与 G1 连通 |
| [2026-10-18-first-flash](../validation/2026-10-18-first-flash/README.md) | 第一次烧录 Atom Lite，启动与 USB 命令 |

## 9. 待办

- 开发板上验证 SPP、配对、扫描、BLE 订阅与收帧、拒绝第二台手机、`unbind`、NVS 重启保留、ack 重发。
- InstaPunch Mapper 端：连接、扫描、配对与解析。
- 状态灯目前只区分手机是否连接，SPP 槽位状态尚未显示；按键（G39）用途待定。
- 确认 CI 首次运行与 GitHub Pages 发布成功。

## 2026-10-19：缓存推送与重连 / Cache and reconnect

`device_cache.c` 保存 24 条 RAM 发现记录；`commands.c` 独立任务每 5 秒推送 `devices` / `status` / `paired`，由 `ble_server.c` 有界控制队列发送。`links.c` 保留 SDP 请求归属直到终结回调，缓存 RFCOMM 通道并轮换重试槽位。`make -C test` 包含真实 links.c 回调的模拟 SDK 场景，不代表射频测试。验证见 `validation/2026-10-19-spp-cache/`。

The bounded control queue sends periodic RAM snapshots without inquiry. Callback tests exercise production link code against mocked IDF calls; they do not establish radio reliability.
