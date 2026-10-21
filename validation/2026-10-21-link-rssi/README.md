# 槽位链路实时信号 `rssi=`（2026-10-21）

InstaPunch Mapper 的槽位详情要显示已连接 SPP 设备的实时信号（用户决定改固件，不用搜索时的缓存值）。

## 改动

- `main/links.c`：管理任务每 2 秒对已连接、未在断开中的槽位调用 `esp_bt_gap_read_rssi_delta`；`ESP_BT_GAP_READ_RSSI_DELTA_EVT` 成功且槽位仍连接时记录读数。断开（`mark_idle`）清除读数；10 秒内没有新读数视为未知。搜索期间不读。
- `main/commands.c`：`status` 在每个已绑定槽位行之后输出 `<槽位> rssi=<n>` 或 `<槽位> rssi=-`。
- `mac/`：`DeviceLinks.rssiDelta` 读 `rawRSSI()`（绝对 dBm）并换算为相同含义；`Commands.status` 输出同样的行。
- `docs/protocol.md` §5 与使用手册中英文版说明该行。协议版本不变。

`n` 是 HCI Read RSSI 的值：相对 ESP32 理想接收范围（`ESP_BT_GAP_RSSI_LOW_THRLD` −45 ～ `HIGH_THRLD` −20 dBm）的 dB 差值，0 为范围内。ESP-IDF 对经典蓝牙连接只提供这一差值，没有绝对 dBm。

单独成行而不加在槽位行里：已发布的 Mapper 把 `lost=` 之后的全部内容当作设备名，加字段会让名称带上 `rssi=…`；两个词的槽位行会被旧解析器忽略。

## 验证

| 项 | 结果 | 类型 |
|---|---|---|
| `make -C test` | frame 通过；links/cache 12 个场景通过（新增：未连接槽位不轮询、2 秒节流、10 秒过期、断开后迟到回调被忽略） | 模拟 SDK |
| `idf.py build`（ESP-IDF v5.4.2） | 成功 | 编译 |
| `mac/` `swift build` | 成功 | 编译 |

未做：开发板实测读数、与 Mapper 联调、弱信号环境。
