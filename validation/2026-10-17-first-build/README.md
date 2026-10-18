# 编译记录（2026-10-17）

类型：**仅编译**。未烧录、未上 Atom Lite、未连接任何蓝牙设备或手机。

环境：macOS arm64，ESP-IDF v5.4.2，xtensa-esp-elf GCC 14.2.0，Python 3.9.6（系统自带）。

## 第 1 轮：单设备 SPP 骨架

- `idf.py build` 成功；`instapunch_bridge.bin` 919,632 字节
- `idf.py merge-bin` 生成 `instapunch-bridge-merged.bin` 985,168 字节，从地址 0 写入

## 第 2 轮：双槽位 + BLE GATT + 绑定主人（同日改为通用编号槽位，重新编译通过，0xe2c30 字节）

- `idf.py build` 成功，本项目代码无警告；`instapunch_bridge.bin` 0xe2c00 字节（928,768），3 MB factory 分区剩余 70%
- 未验证：SPP 连接与重连、PIN / SSP 配对、扫描结果、改绑立即生效、GATT 读写与通知、iOS 配对弹窗、拒绝第二台手机、`unbind`、NVS 持久化、网页烧录、GitHub Actions

## 第 3 轮：协议 v1 帧与调度层

- 主机测试 `make -C test`（Apple clang，`-Wall -Wextra -Werror`，ASan + UBSan）：14 项全部通过。用例使用 2026-10-17 实测的 TruPulse 360B `$PLTIT` 输出，覆盖协议文档示例帧逐字节一致、分段行合并、整行装帧、长行 PARTIAL、无结束符超时、raw 模式、溢出 GAP、离线 REPLAY + AGE、断链截断半行、确认窗口与重发、上行整条记录、32 位位置回绕
- `idf.py build` 成功，本项目代码无警告；`instapunch_bridge.bin` 0xe4540 字节（935,232），3 MB 分区剩余 70%
- `idf.py size`：静态 DRAM 89,152 / 124,580 字节（.bss 70,424，其中两个槽位缓冲约 37 KB）；运行时空闲堆需上机读取启动日志 `free heap`
- 未验证：发送闸门与 `CONF_EVT` 回收额度的实际时机、拥塞时的行为、ack 重发、iOS 端订阅与 MTU、`event` 推送，以及前几轮列出的全部上机项
