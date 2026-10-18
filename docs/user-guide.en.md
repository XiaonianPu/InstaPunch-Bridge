# InstaPunch Bridge User Guide

[中文](user-guide.md)

InstaPunch Bridge is open-source firmware for a small Bluetooth bridge. Flash it onto an M5Stack Atom Lite and [InstaPunch Mapper](https://instapunch.cn/mapper) on iPhone / iPad can talk to field devices that only support **classic Bluetooth serial (SPP)**, such as RTK receivers and laser rangefinders.

> **Preview:** tested on an M5Stack Atom Lite with a South Galaxy G1 and InstaPunch Mapper; other devices (such as the TruPulse 360B) are not tested yet. Configure it from Mapper's External Devices settings, or from the flasher page's console or a serial terminal.

## 1. What it does

```
 RTK / rangefinder ──classic Bluetooth SPP──▶ Bridge ──BLE──▶ iPhone / iPad (InstaPunch Mapper)
```

- Connects up to **2** classic-Bluetooth SPP devices (slots 1 and 2).
- Passes device data to the phone unchanged, and data from the phone (e.g. RTK corrections) to the device unchanged.
- Serves **one** phone: the first phone that pairs becomes its owner; other phones cannot connect.
- Reconnects devices automatically. Settings are stored on the bridge and survive power loss.

## 2. What you need

| Item | Notes |
|---|---|
| [M5Stack Atom Lite](https://shop.m5stack.com/products/atom-lite-esp32-development-kit) | Recommended, no soldering. Other **original ESP32** boards also work |
| USB-C **data** cable | Charge-only cables cannot flash |
| Computer with desktop Chrome or Edge | Only for flashing and setup |
| iPhone / iPad | With InstaPunch Mapper |
| Field device | Supports classic Bluetooth SPP (serial port profile) |

**Original ESP32 only.** ESP32-S2 / S3 / C3 / C6 / H2 boards (e.g. AtomS3, XIAO ESP32C3) have no classic Bluetooth and will not work.

In the field the bridge is powered over USB-C; a power bank works.

## 3. Flashing the firmware

### Option 1: web flasher (recommended, nothing to install)

1. Connect the Atom Lite to the computer with the USB-C data cable.
2. Open the flasher page in desktop Chrome or Edge: <https://xiaonianpu.github.io/InstaPunch-Bridge/>
3. Click “Install firmware”, pick the serial port (`usbserial-…` on macOS, `COM…` on Windows) and confirm.
4. Wait about one minute until it finishes.

### Option 2: command line

Download `instapunch-bridge-merged.bin` from GitHub Releases, install [esptool](https://docs.espressif.com/projects/esptool/), then run:

```bash
python -m esptool --chip esp32 -b 115200 write_flash 0x0 instapunch-bridge-merged.bin
```

The Atom Lite sometimes fails at higher baud rates; keep 115200.

### Opening the console

Setup is done by typing commands in a console:

- Web flasher page: connect and choose **Logs & Console**.
- Or any serial terminal at 115200 baud; end each command with Enter.

Type `info`. The reply `bridge 0.1.0 proto 1 slots 2` means the firmware is running.

## 4. Connecting your phone the first time

1. When powered, the bridge advertises as `InstaPunch-Bridge-XXXX` (XXXX differs per bridge).
2. Select it in InstaPunch Mapper and connect.
3. When iOS asks to pair, tap “Pair”.

**The first phone that pairs becomes the bridge's owner.** Pairing requests from any other phone are refused. To switch phones, see section 7.

## 5. Adding a field device

1. Put the device in discoverable / pairing mode (see its manual).
2. Make sure it is **not** connected to another phone, data collector or computer: most SPP devices accept only one connection at a time.
3. Type `scan` in the console. Within about 10 seconds nearby devices are listed:

   ```
   found 00:80:25:aa:bb:cc -58 001f00 uncategorized spp device-name
   name 00:80:25:aa:bb:dd another-device
   scan done
   ```

   Each line gives the MAC, signal strength, device class and whether the device announces the serial port service (`spp`; `-` only means it did not announce it, not that it cannot work). Devices without a name are asked for it after the list (`name` lines), so a scan can take longer than 10 seconds. Both slots disconnect during the scan and reconnect afterwards.

   Devices paired before are listed by `paired`; they may not show up in a scan.

4. Bind the device to a slot:

   ```
   pair 1 00:80:25:aa:bb:cc
   ```

   If the device asks for a PIN, add it: `pair 2 00:80:25:aa:bb:cc 1234`. Without one, `0000` is used.

5. Type `status`. `connected` means the link is up. To see the device's raw data, type `monitor on` (`monitor off` to stop).

Tell InstaPunch Mapper what each slot is (RTK, rangefinder, …). The bridge itself does not care about device types.

## 6. Everyday use

- Blue LED: a slow blink (on 1 s, off 1 s) means waiting for the phone; a short flash every 3 s means the phone is connected.
- Just power it: the bridge connects to its bound devices, and the phone connects when Mapper is open.
- If a device is switched off or out of range, the bridge retries every few seconds and reconnects when it is back.
- **Data mode** (per slot):
  - `mode 1 line` (default): for devices that send text lines, such as NMEA or rangefinder readings.
  - `mode 1 raw`: for binary data.
- **Offline hold**: with `hold 1 on`, device data received while the phone is away is kept in the bridge's memory and sent when the phone reconnects; when full, the oldest data is dropped. Off by default (data is discarded while no phone is listening). Held data does not survive power loss.

## 7. Switching phones

For security, the owner can only be cleared over USB:

1. Connect the bridge to a computer, open the console and type `unbind`; it replies `ok`.
2. On the **old phone**, open Settings › Bluetooth, find `InstaPunch-Bridge-XXXX`, tap ⓘ and choose “Forget This Device”.
3. Connect with the new phone; once paired it is the new owner.

Do the same if the same phone was reset or forgot the bridge in its Bluetooth settings.

## 8. Command reference

| Command | Purpose |
|---|---|
| `info` | Firmware and protocol version |
| `status` | Device, link state, mode and dropped bytes per slot, and the phone state |
| `scan` | Search for nearby classic-Bluetooth devices (about 10 s, then name lookups) |
| `paired` | List devices paired with the bridge |
| `pair <slot> <MAC> [PIN]` | Bind a device to slot 1 or 2; takes effect immediately |
| `forget <slot>` | Remove the slot's device |
| `mode <slot> line\|raw` | Data mode |
| `hold <slot> on\|off` | Offline hold |
| `unbind` | Clear the owner phone (USB only) |
| `monitor on\|off` | Show device data in the console (USB only) |

## 9. Troubleshooting

**The device does not show up in `scan`**
It is not discoverable or is connected to another host. Disconnect it from the data collector / phone / computer and put it in pairing mode as its manual describes. If you know its MAC, skip the scan and `pair` directly.

**`status` never shows `connected`**
Check that the device is on, within about 10 m and not used by another host, and that the PIN is right (run `pair` again to overwrite it).

**The phone reports a pairing failure or “Peer removed pairing information”**
The bridge's owner was cleared, or the pairing stored on the phone is stale. Forget `InstaPunch-Bridge-XXXX` in the phone's Bluetooth settings and connect again.

**A second phone cannot connect**
By design: a bridge serves one phone. See section 7.

**How do I recognize it in a generic scanner (e.g. nRF Connect)?**
Its name is `InstaPunch-Bridge-XXXX` and it advertises the service UUID `A7B750D5-8CE4-4F67-B586-8C1AA7800BBF`.

**What does the LED mean?**
Blue, on 1 s / off 1 s: no phone connected, waiting. A short flash every 3 s: phone connected. Completely dark: no power or the firmware is not running. The button has no function yet.

**Which devices are supported?**
In principle any classic-Bluetooth SPP device. Planned tests: South Galaxy G1 RTK and TruPulse 360B rangefinder. BLE-only devices do not need the bridge; Mapper can connect to them directly.

## 10. Security and privacy

- The bridge exchanges data only with its bound phone, and every transfer requires an encrypted link.
- Device MACs and PINs are stored only on the bridge. It has no Wi-Fi and never goes online.
- The bridge neither parses nor stores device data; offline hold lives in memory and is cleared on power loss.

## 11. Open source and license

The firmware is released under the MIT License; the source code is in the project repository. It is published for use only and does not accept external pull requests; fork it to make your own changes. Released firmware images also contain ESP-IDF components and Espressif's Bluetooth controller libraries under their own licenses. Developers: see the [developer guide (Chinese)](development.md).
