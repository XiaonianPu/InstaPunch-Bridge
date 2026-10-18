# InstaPunch Bridge

开源的蓝牙中转盒固件。初代 ESP32（双模）用经典蓝牙 SPP 连接外业设备，再通过 BLE GATT 把数据转给 iPhone / iPad 上的 [InstaPunch Mapper](https://instapunch.cn/mapper)，App 不需要 MFi。

Open-source Bluetooth bridge firmware: an original ESP32 connects to classic-Bluetooth (SPP) field devices and relays their data to iPhone / iPad over BLE.

| 项 | 内容 |
|---|---|
| 推荐硬件 | [M5Stack Atom Lite](https://shop.m5stack.com/products/atom-lite-esp32-development-kit)（ESP32-PICO-D4），无需焊接，一根 USB-C 线即可 |
| 下游 | 任意经典蓝牙 SPP 设备，2 个槽位；已实测南方银河 G1 RTK，计划测试 TruPulse 360B 激光测距 |
| 上游 | InstaPunch Mapper（BLE 协议见 [docs/protocol.md](docs/protocol.md)） |
| 烧录 | [https://xiaonianpu.github.io/InstaPunch-Bridge/](https://xiaonianpu.github.io/InstaPunch-Bridge/)（桌面版 Chrome / Edge） |
| 手册 | [使用手册](docs/user-guide.md) · [User Guide](docs/user-guide.en.md) · [开发手册](docs/development.md) |

只能用**初代 ESP32**。S2 / S3 / C / H 系列（如 AtomS3、XIAO ESP32C3）没有经典蓝牙。

## 用户：烧录固件（不需要安装任何开发工具）

1. 用 USB-C **数据线**把 Atom Lite 接到电脑。
2. 用桌面版 Chrome 或 Edge 打开[烧录页面](https://xiaonianpu.github.io/InstaPunch-Bridge/)，点击“烧录固件”，选择出现的串口。
3. 打开 InstaPunch Mapper 连接 `InstaPunch-Bridge-XXXX`。第一台配对的手机成为这台中转盒的主人，其他手机连不上。
4. 在 App 的“外部设备”里扫描设备，绑定到槽位 1 或 2，并设置每个槽位是 RTK 还是测距仪。也可以在烧录页面的 “Logs & Console” 中用命令操作：

   ```
   scan
   pair 2 00:80:25:aa:bb:cc 1234
   status
   ```

   命令列表见 [docs/protocol.md](docs/protocol.md#5-控制命令与事件)。换手机时，在串口里输入 `unbind` 清除主人。

不想用浏览器时，可以从 Releases 下载 `instapunch-bridge-merged.bin`，用 esptool 写到地址 0：

```bash
python -m esptool --chip esp32 -b 115200 write_flash 0x0 instapunch-bridge-merged.bin
```

## 开发者：CLion

1. 安装 ESP-IDF v5.4.2（[官方说明](https://docs.espressif.com/projects/esp-idf/en/v5.4.2/esp32/get-started/)），并运行 `install.sh esp32`。macOS 上还需要 `python tools/idf_tools.py install cmake ninja`。
2. **Settings › Build, Execution, Deployment › Toolchains**：新建工具链，*Add environment › From file* 选择 ESP-IDF 的 `export.sh`。如果系统默认的 `python3` 比 ESP-IDF 安装时用的版本新，就改选一个先调整 `PATH` 再加载 `export.sh` 的包装脚本。
3. **CMake** 配置：工具链选上一步新建的，生成器选 Ninja，CMake options 填 `-DIDF_TARGET=esp32`。
4. 烧录与串口：新建 *Shell Script* 运行配置，分别执行 `idf.py -p <串口> -b 115200 flash` 与 `idf.py -p <串口> monitor`；或在 CLion 终端中加载环境后直接运行。

命令行等价操作：

```bash
. $IDF_PATH/export.sh
idf.py build
idf.py merge-bin -o instapunch-bridge-merged.bin   # 网页烧录用的单文件镜像
idf.py -p /dev/cu.usbserial-* -b 115200 flash monitor
```

Atom Lite 在高波特率下烧录偶尔失败，失败时用 115200。Atom Lite 没有引出 JTAG，只能靠串口日志调试。

## 进度

1. [x] 双模控制器、Bluedroid 初始化
2. [x] 两个通用 SPP 槽位（设备类型由 App 决定），后台重连，改绑立即生效
3. [x] 经典蓝牙扫描（`scan`）
4. [x] BLE GATT：下行 / 上行 / 控制三个特征值；只允许第一台绑定的手机连接
5. [x] 协议 v1 帧：`line` / `raw` 双模式、GAP / REPLAY / AGE / PARTIAL 标志、会话号与帧序号、离线暂存与 ack 重发、上行整条记录（`main/frame.c`，主机测试通过）
6. [x] 发送闸门（加密、订阅、拥塞、在途包数）与丢弃事件（`main/pipeline.c`）
7. [x] USB 串口命令（与 BLE 控制命令共用）
8. [x] 网页烧录（ESP Web Tools）与 CI：推送到 `main` 自动编译并发布到 GitHub Pages
9. [x] 上 Atom Lite 实测：与银河 G1、InstaPunch Mapper 连通可用（2026-10-18，[记录](validation/2026-10-18-first-flash/README.md)）；TruPulse 未测
10. [x] InstaPunch Mapper 端的连接、扫描、配对与解析
11. [ ] RGB 灯显示状态：手机未连接慢闪（1 秒亮 / 1 秒灭），连接后每 3 秒快闪；SPP 槽位状态未显示

## 测试

开发板到货前，可用 [mac/](mac/README.md) 中的 Mac 测试转发器联调 App：手机端服务与协议相同，槽位同样连接经典蓝牙 SPP 设备（也可用窗口注入数据的虚拟设备）。

封包逻辑（`main/frame.c`）不依赖 ESP-IDF，可在电脑上测试：

```bash
make -C test
```

## Atom Lite 引脚

| 引脚 | 用途 |
|---|---|
| G39 | 按键 |
| G27 | SK6812 RGB 灯 |
| G26 / G32 | Grove 口（以后可接 RS-232 转 TTL） |

## 参与贡献 / Contributing

本仓库只开源供使用，不接受外部 Pull Request。需要修改请 Fork 后自行维护，见 [CONTRIBUTING.md](CONTRIBUTING.md)。

Published for use only; external pull requests are not accepted. Fork it to make your own changes — see [CONTRIBUTING.md](CONTRIBUTING.md).

## 许可证 / License

本项目代码以 [MIT](LICENSE) 许可证发布。发布的固件镜像还包含 ESP-IDF 及其组件（Apache-2.0 等）和乐鑫蓝牙控制器的二进制库，这些部分按其各自的许可证分发。

Project code is released under the [MIT License](LICENSE). Firmware images also contain ESP-IDF components (Apache-2.0 and others) and Espressif's binary Bluetooth controller libraries, distributed under their own licenses.
