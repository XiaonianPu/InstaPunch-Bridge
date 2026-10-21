# 解除绑定：App 内解除与长按按键（2026-10-22）

用户要求：已绑定的手机可在 App 的中转盒详情中解除绑定；长按 Bridge 按键 10 秒解除当前绑定（包括 SPP 设备）。

## 改动

- `unbind` 不再限于 USB：主人手机经控制特征值发送也可执行（控制特征值要求已绑定的加密链路，只有主人能写）。回复 `ok` 约 1 秒后断开手机。
- `main/reset_button.c`：G39 按键按住 10 秒，清除主人手机（`ble_server_unbind_all`）、两个槽位与所有经典蓝牙配对（`links_forget_all`），指示灯快闪 6 下琥珀色。数据模式与离线暂存设置保留。
- 固件版本 0.1.2，协议版本仍为 1（只新增 BLE 上可用的命令；旧固件经 BLE 回复 `error unknown command`，App 提示更新固件）。
- Mac 转发器同步 BLE `unbind`（没有按键，见 `mac/README.md` 差异表）。
- InstaPunch Mapper：中转盒详情新增“解除绑定”（仅连接时可用），成功后在本机移除该中转盒并提示在系统蓝牙设置中忽略它。

## 结果

类型：**编译** + **开发板启动**。

- `idf.py build` 无警告，`instapunch_bridge.bin` 0xea9e0 字节，3 MB 分区剩余 69%
- `make -C test`：frame 与 links/cache 12 个场景通过（macOS）
- 烧录三段通过哈希校验；启动正常，G39 配置为输入；`info` → `bridge 0.1.2 proto 1 slots 2`；`status` 正常
- 启动时 `free heap 81668 bytes`，上一版 0.1.1 为 90840（按键任务栈 3 KB 等）
- Mac 转发器 `swift build` 通过；Mapper 模拟器构建通过，i18n 键检查 OK

## 未验证

为保留用户现有的手机与 G1 绑定，未在开发板上实际执行 `unbind` 或长按按键。待测：长按 10 秒后灯效、`status` 显示 `phone none` 与两个槽位 `none`；App 内解除绑定后 `ok` 送达再断开、App 移除中转盒、iPhone 忽略后重新配对；第二台设备在解绑后成为主人。
