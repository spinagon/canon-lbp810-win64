#ifndef USB_COMM_H
#define USB_COMM_H

#include "platform.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef _WIN32
#include <windows.h>
#include <winusb.h>
#else
typedef void* HANDLE;
typedef void* WINUSB_INTERFACE_HANDLE;
typedef unsigned char UCHAR;
#endif

typedef struct {
    HANDLE device_handle;
    WINUSB_INTERFACE_HANDLE winusb;
    UCHAR bulk_out_pipe;
    UCHAR bulk_in_pipe;
    bool connected;
} usb_device_t;

int  usb_open(usb_device_t *dev);
void usb_close(usb_device_t *dev);
int  usb_send_packet(usb_device_t *dev, uint16_t opcode, const uint8_t *payload, uint16_t payload_len);
int  usb_recv_packet(usb_device_t *dev, uint16_t expected_opcode, uint8_t *buf, uint16_t buf_size, uint16_t *actual_size);
int  usb_bulk_write(usb_device_t *dev, const uint8_t *data, uint32_t len);
int  usb_bulk_read(usb_device_t *dev, uint8_t *buf, uint32_t buf_size, uint32_t *actual_read);

#endif
