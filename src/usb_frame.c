/*
 * USB CDC 收图：复用 HTTP /api/frame 的帧契约，避免维护第二套图像格式。
 *
 * 主机 -> 设备：
 *   "MIFR" + 16 字节帧头（A5 5A ...，大端）+ 2bpp 载荷
 *
 * 设备 -> 主机：
 *   "MIFR OK\n" 或 "MIFR ERR ...\n"
 *
 * 注意：USB Serial/JTAG 同时也是控制台。控制台只写不读，所以这里直接读
 * 驱动环形缓冲；收到非帧数据（日志/误输入）时按字节丢弃，直到看到 MIFR。
 */
#include "usb_frame.h"

#include "driver/usb_serial_jtag.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "epd_drv.h"
#include "frame.h"
#include "power.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static const char *TAG = "usb_frame";

#define USB_MAGIC       "MIFR"
#define USB_CHUNK       4096
#define USB_HDR_TIMEOUT 5000
#define USB_RX_TIMEOUT  30000

static uint16_t crc16_update(uint16_t crc, const uint8_t *d, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)d[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                                 : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static void usb_ack(const char *s)
{
    (void)usb_serial_jtag_write_bytes(s, strlen(s), pdMS_TO_TICKS(1000));
}

/* 收满 len 字节；超时返回 -1。途中持续 power_activity()，避免传到一半回睡。 */
static int usb_read_exact(uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    size_t got = 0;
    TickType_t start = xTaskGetTickCount();
    TickType_t limit = pdMS_TO_TICKS(timeout_ms);

    while (got < len) {
        if ((TickType_t)(xTaskGetTickCount() - start) > limit) return -1;
        int n = usb_serial_jtag_read_bytes(buf + got, (uint32_t)(len - got),
                                           pdMS_TO_TICKS(200));
        if (n > 0) {
            got += (size_t)n;
            power_activity();
        }
    }
    return 0;
}

/* 按字节滚动找 "MIFR"；日志/噪声会被丢掉。 */
static void usb_wait_magic(void)
{
    uint8_t win[4] = {0};
    int have = 0;

    for (;;) {
        uint8_t b;
        int n = usb_serial_jtag_read_bytes(&b, 1, pdMS_TO_TICKS(200));
        if (n == 1) {
            if (have < 4) {
                win[have++] = b;
            } else {
                memmove(win, win + 1, 3);
                win[3] = b;
            }
            if (have == 4 && memcmp(win, USB_MAGIC, 4) == 0) return;
        } else if (n < 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

/* 校验 16 字节头；返回 0 表示可收。 */
static int parse_header(const uint8_t *hdr, uint16_t *w, uint16_t *h,
                        uint32_t *plen, uint16_t *crc_hdr)
{
    if (hdr[0] != FRAME_MAGIC0 || hdr[1] != FRAME_MAGIC1) return -1;
    if (hdr[2] != FRAME_FMT_VERSION && hdr[2] != FRAME_FMT_VERSION_NATIVE) return -2;

    uint16_t width  = (uint16_t)((hdr[4] << 8) | hdr[5]);
    uint16_t height = (uint16_t)((hdr[6] << 8) | hdr[7]);
    uint32_t len    = ((uint32_t)hdr[8] << 24) | ((uint32_t)hdr[9] << 16) |
                      ((uint32_t)hdr[10] << 8) | hdr[11];

    if ((width & 3) || len != (uint32_t)(width / 4) * height) return -3;
    if (len > frame_buf_len()) return -4;
    if (!epd_frame_geom_ok(width, height, len)) return -5;

    *w = width;
    *h = height;
    *plen = len;
    *crc_hdr = (uint16_t)((hdr[12] << 8) | hdr[13]);
    return 0;
}

static void usb_frame_task(void *arg)
{
    (void)arg;
    uint8_t hdr[FRAME_HDR_LEN];

    for (;;) {
        usb_wait_magic();

        if (usb_read_exact(hdr, sizeof(hdr), USB_HDR_TIMEOUT) != 0) {
            usb_ack("MIFR ERR header timeout\n");
            continue;
        }

        uint16_t w = 0, h = 0, crc_hdr = 0;
        uint32_t plen = 0;
        if (parse_header(hdr, &w, &h, &plen, &crc_hdr) != 0) {
            usb_ack("MIFR ERR bad header\n");
            continue;
        }

        if (frame_lock(5000) != 0) {
            usb_ack("MIFR ERR busy\n");
            continue;
        }

        uint8_t *fb = frame_buf();
        uint32_t off = 0;
        uint16_t crc = 0xFFFF;
        bool bad = false;

        while (off < plen) {
            size_t take = (size_t)(plen - off);
            if (take > USB_CHUNK) take = USB_CHUNK;
            if (usb_read_exact(fb + off, take, USB_RX_TIMEOUT) != 0) {
                bad = true;
                break;
            }
            crc = crc16_update(crc, fb + off, take);
            off += (uint32_t)take;
        }

        if (bad) {
            frame_abort();
            usb_ack("MIFR ERR rx timeout\n");
            continue;
        }
        if (crc != crc_hdr) {
            frame_abort();
            usb_ack("MIFR ERR crc\n");
            continue;
        }

        frame_commit(w, h);
        power_activity();
        usb_ack("MIFR OK\n");   /* 成功只回执，不写 ERROR 日志，避免误判 */
    }
}

void usb_frame_start(void)
{
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t cfg = {
            .tx_buffer_size = 512,
            .rx_buffer_size = 4096,
        };
        esp_err_t e = usb_serial_jtag_driver_install(&cfg);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "usb serial jtag driver install failed: %s",
                     esp_err_to_name(e));
            return;
        }
    }

    if (xTaskCreate(usb_frame_task, "usb_frame", 4096, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "usb_frame task create failed");
    }
}
