#ifndef MOINK_USB_FRAME_H
#define MOINK_USB_FRAME_H

/* 启动 USB CDC 收帧任务：复用 /api/frame 的 16 字节帧头 + 2bpp 载荷。
   主机发送格式： "MIFR" + 16 字节帧头 + 载荷。 */
void usb_frame_start(void);

#endif /* MOINK_USB_FRAME_H */
