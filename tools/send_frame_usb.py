#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""通过 USB CDC 把页面导出的 frame.bin 发给 MoInk。

帧格式与 HTTP /api/frame 完全一致：
    16 字节头（A5 5A ...，大端） + 768x552 的 2bpp 载荷（105,984 B）

用法：
    python tools/send_frame_usb.py COM6 frame.bin
    python tools/send_frame_usb.py /dev/ttyACM0 frame.bin

依赖：pyserial（pip install pyserial）。设备需要先唤醒，COM 口可见。
"""
import argparse
import sys
import time

try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

try:
    import serial
except ImportError:
    print("缺少 pyserial：请先运行  pip install pyserial")
    raise SystemExit(2)

MAGIC = b"MIFR"
CHUNK = 4096


def crc16_ccitt_false(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if (crc & 0x8000) else (crc << 1)
            crc &= 0xFFFF
    return crc


def validate(data):
    if len(data) < 16:
        raise ValueError("文件不足 16 字节")
    if data[0:2] != b"\xA5\x5A":
        raise ValueError("帧头魔数不是 A5 5A（不是页面导出的 frame.bin？）")
    ver = data[2]
    if ver not in (1, 2):
        raise ValueError("不支持的帧版本 %d" % ver)
    w = int.from_bytes(data[4:6], "big")
    h = int.from_bytes(data[6:8], "big")
    plen = int.from_bytes(data[8:12], "big")
    crc = int.from_bytes(data[12:14], "big")
    if plen != (w // 4) * h:
        raise ValueError("载荷长度与几何不一致：%dx%d, %d B" % (w, h, plen))
    if len(data) != 16 + plen:
        raise ValueError("文件长度不是 16 + %d，而是 %d" % (plen, len(data)))
    calc = crc16_ccitt_false(data[16:])
    if calc != crc:
        raise ValueError("CRC 不一致：文件 0x%04X，计算 0x%04X" % (crc, calc))
    return w, h, plen


def send(port, baud, data, wait_ack):
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = baud
    ser.timeout = 0.2
    ser.write_timeout = 30
    ser.dsrdtr = False
    ser.rtscts = False
    ser.open()
    try:
        # 避免 DTR/RTS 触发 C3 的复位/下载时序
        try:
            ser.dtr = False
            ser.rts = False
        except Exception:
            pass
        # C3 的 USB Serial/JTAG 在打开串口时可能被 DTR/RTS 复位一次；
        # 等它重新枚举/启动完再发，避免首批字节丢掉。
        time.sleep(1.0)

        ser.write(MAGIC)
        for i in range(0, len(data), CHUNK):
            ser.write(data[i:i + CHUNK])
            ser.flush()
            time.sleep(0.002)

        if not wait_ack:
            return 0

        print("已发送，等待设备回执…")
        buf = b""
        deadline = time.time() + 30
        while time.time() < deadline:
            chunk = ser.read(256)
            if chunk:
                sys.stdout.write(chunk.decode("utf-8", "replace"))
                sys.stdout.flush()
                buf += chunk
                if b"MIFR OK" in buf:
                    return 0
                if b"MIFR ERR" in buf:
                    return 1
        print("\n[超时] 没收到 MIFR OK。请确认：设备醒着、COM 口没被串口监视器占用、"
              "固件已包含 USB 收帧任务。")
        return 1
    finally:
        ser.close()


def main():
    p = argparse.ArgumentParser(description="通过 USB CDC 发送 MoInk 四色帧")
    p.add_argument("port", help="COM6 / /dev/ttyACM0")
    p.add_argument("frame", help="页面导出的 frame.bin")
    p.add_argument("--baud", type=int, default=921600)
    p.add_argument("--no-ack", action="store_true", help="不等设备回执")
    args = p.parse_args()

    try:
        data = open(args.frame, "rb").read()
    except OSError as e:
        print("读文件失败：%s" % e)
        return 2

    try:
        w, h, plen = validate(data)
    except ValueError as e:
        print("帧文件无效：%s" % e)
        return 2

    print("帧 %dx%d，载荷 %d B，文件 %d B -> %s @ %d"
          % (w, h, plen, len(data), args.port, args.baud))
    try:
        return send(args.port, args.baud, data, not args.no_ack)
    except serial.SerialException as e:
        print("串口错误：%s" % e)
        print("提示：设备深睡时 COM 口会消失；按一下板子按键或拔插 USB 再试。")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
