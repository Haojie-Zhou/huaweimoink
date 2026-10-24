# USB 直接传图（不连 Wi-Fi）

设备 USB 是 USB-Serial/JTAG，不是网卡。固件里加了一个 USB CDC 收帧任务，复用 HTTP `/api/frame` 的帧格式；页面负责把当前编辑结果导出成 `frame.bin`，PC 脚本通过 COM 口发过去。

## 前提

- 固件为含 USB 收帧的版本（本分支已加，app 约 1.18MB）。
- 电脑有 Python 3 + pyserial：`pip install pyserial`。
- 设备醒着，设备管理器里能看到 COM 口（深睡时 USB 会消失；按一下 GPIO5/BOOT 或拔插 USB）。

## 流程

1. 页面选图、裁剪、选风格（例如「拍立得」）。
2. 点 **「导出帧（USB）」**，浏览器下载 `frame.bin`（约 106KB）。
3. 在电脑运行：

   ```powershell
   python tools/send_frame_usb.py COM6 frame.bin
   ```

   端口按实际改（Linux 常见 `/dev/ttyACM0`）。波特率默认 921600；USB CDC 实际不受这个波特率限制。

4. 设备回 `MIFR OK`，屏幕约 15–25 秒后刷新。
5. 传输过程中固件每收一块都调用 `power_activity()`，不会传到一半回睡。

## 协议

- 主机 → 设备：`MIFR` + 16 字节帧头（`A5 5A`，大端）+ 2bpp 载荷。
- 设备 → 主机：`MIFR OK\n` 或 `MIFR ERR ...\n`。
- 帧头/载荷与 `POST /api/frame` 完全一致：768×552 → 105,984 字节载荷。
- 固件复用同一块 frame buffer 和同一把锁；USB 与 Wi-Fi 不会并发踩帧。

## USB 连接时保持唤醒

固件检测到 USB 主机在发 SOF（即连着正在工作的电脑）时，`power_should_sleep()` 直接返回 false：不会深睡，COM 口不会消失。接充电宝没有 SOF，不会触发；拔掉 USB 后仍按 `sleep_s` / `wake_s` 正常休眠。

## 常见问题

- **COM 口不存在**：设备深睡。按一下板子按键或拔插 USB，等 COM 口出现再发。
- **`MIFR ERR busy`**：上一帧还在刷（15–25 秒），等屏幕刷完重发。
- **`MIFR ERR crc`**：`frame.bin` 损坏或不是页面导出的，重新导出。
- **打开串口时设备复位一次**：C3 USB Serial/JTAG 的 DTR/RTS 行为；脚本会等 1 秒再发，属正常现象。
- **收不到 `MIFR OK`**：确认没有串口监视器占用 COM 口；确认固件是含 USB 收帧的版本。
- USB 传图只负责“把已经量化好的四色帧发上去”；选图、裁剪、拍立得风格、抖动仍在页面里完成。
