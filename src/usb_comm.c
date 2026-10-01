#include "usb_comm.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#include <initguid.h>
#include <setupapi.h>

/* GUID from our INF file */
DEFINE_GUID(GUID_DEVINTERFACE_LBP810,
    0xA5DCBF10, 0x6530, 0x11D2,
    0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED);

#define LBP810_VID  "VID_04A9"
#define LBP810_PID  "PID_260A"

/* Try to open an LBP-810 device using a given interface GUID.
   Enumerates all matching interfaces and checks the device path
   for the Canon VID/PID strings. */
static int try_open_with_guid(usb_device_t *dev, const GUID *guid)
{
    HDEVINFO devs = SetupDiGetClassDevsA(guid, NULL, NULL,
                                          DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devs == INVALID_HANDLE_VALUE) {
        LOG_DEBUG("SetupDiGetClassDevs failed for GUID (err %lu)", GetLastError());
        return -1;
    }

    SP_DEVICE_INTERFACE_DATA iface;
    iface.cbSize = sizeof(iface);

    for (DWORD idx = 0;
         SetupDiEnumDeviceInterfaces(devs, NULL, guid, idx, &iface);
         idx++)
    {
        /* Get required buffer size */
        DWORD required = 0;
        SetupDiGetDeviceInterfaceDetailA(devs, &iface, NULL, 0, &required, NULL);

        PSP_DEVICE_INTERFACE_DETAIL_DATA_A detail =
            (PSP_DEVICE_INTERFACE_DETAIL_DATA_A)malloc(required);
        if (!detail) continue;
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);

        if (!SetupDiGetDeviceInterfaceDetailA(devs, &iface, detail,
                                               required, NULL, NULL)) {
            free(detail);
            continue;
        }

        LOG_DEBUG("USB device [%lu]: %s", (unsigned long)idx, detail->DevicePath);

        /* Check if this is our Canon LBP-810 by VID/PID in the path */
        char *path_upper = _strdup(detail->DevicePath);
        if (path_upper) {
            for (char *p = path_upper; *p; p++) *p = (char)toupper((unsigned char)*p);
        }

        bool match = path_upper && strstr(path_upper, LBP810_VID);
        free(path_upper);

        if (!match) {
            LOG_DEBUG("  -> Not LBP-810, skipping");
            free(detail);
            continue;
        }

        LOG_INFO("Found Canon LBP-810 at: %s", detail->DevicePath);

        /* Open the device */
        dev->device_handle = CreateFileA(
            detail->DevicePath,
            GENERIC_WRITE | GENERIC_READ,
            FILE_SHARE_WRITE | FILE_SHARE_READ,
            NULL, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
            NULL);
        free(detail);

        if (dev->device_handle == INVALID_HANDLE_VALUE) {
            LOG_ERROR("CreateFile failed for LBP-810 (err %lu)", GetLastError());
            continue;
        }

        /* Initialize WinUSB */
        if (!WinUsb_Initialize(dev->device_handle, &dev->winusb)) {
            LOG_ERROR("WinUsb_Initialize failed (err %lu)", GetLastError());
            CloseHandle(dev->device_handle);
            dev->device_handle = INVALID_HANDLE_VALUE;
            continue;
        }

        /* Query pipe endpoints */
        USB_INTERFACE_DESCRIPTOR iface_desc;
        if (WinUsb_QueryInterfaceSettings(dev->winusb, 0, &iface_desc)) {
            LOG_INFO("USB interface: class=0x%02X subclass=0x%02X protocol=0x%02X endpoints=%d",
                     iface_desc.bInterfaceClass, iface_desc.bInterfaceSubClass,
                     iface_desc.bInterfaceProtocol, iface_desc.bNumEndpoints);

            for (int i = 0; i < iface_desc.bNumEndpoints; i++) {
                WINUSB_PIPE_INFORMATION pipe;
                if (WinUsb_QueryPipe(dev->winusb, 0, (UCHAR)i, &pipe)) {
                    if (pipe.PipeType == UsbdPipeTypeBulk) {
                        if (USB_ENDPOINT_DIRECTION_IN(pipe.PipeId)) {
                            dev->bulk_in_pipe = pipe.PipeId;
                            LOG_INFO("  Bulk IN  pipe: 0x%02X", pipe.PipeId);
                        } else {
                            dev->bulk_out_pipe = pipe.PipeId;
                            LOG_INFO("  Bulk OUT pipe: 0x%02X", pipe.PipeId);
                        }
                    }
                }
            }
        }

        /* Set a 5-second read timeout for flow control polling */
        ULONG timeout_ms = 5000;
        WinUsb_SetPipePolicy(dev->winusb, dev->bulk_in_pipe,
                             PIPE_TRANSFER_TIMEOUT, sizeof(timeout_ms), &timeout_ms);

        dev->connected = true;
        SetupDiDestroyDeviceInfoList(devs);
        return 0;
    }

    SetupDiDestroyDeviceInfoList(devs);
    return -1;
}

/* Parse GUID string format "{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}" */
static int parse_guid_string(const char *s, GUID *g) {
    if (!s) return -1;
    const char *p = strchr(s, '{');
    if (!p) p = s;
    unsigned long p0;
    unsigned int p1, p2, p3, p4, p5, p6, p7, p8, p9, p10;
    if (sscanf(p, "{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}",
               &p0, &p1, &p2, &p3, &p4, &p5, &p6, &p7, &p8, &p9, &p10) == 11) {
        g->Data1 = p0;
        g->Data2 = (unsigned short)p1;
        g->Data3 = (unsigned short)p2;
        g->Data4[0] = (unsigned char)p3;
        g->Data4[1] = (unsigned char)p4;
        g->Data4[2] = (unsigned char)p5;
        g->Data4[3] = (unsigned char)p6;
        g->Data4[4] = (unsigned char)p7;
        g->Data4[5] = (unsigned char)p8;
        g->Data4[6] = (unsigned char)p9;
        g->Data4[7] = (unsigned char)p10;
        return 0;
    }
    return -1;
}

int usb_open(usb_device_t *dev) {
    memset(dev, 0, sizeof(*dev));
    dev->connected = false;
    dev->device_handle = INVALID_HANDLE_VALUE;

    LOG_INFO("Searching for Canon CAPT USB Printer (VID=04A9)...");

    /* 1. Scan all USB devices to find any Canon device, report driver status,
          and extract its DeviceInterfaceGUID from the registry */
    HDEVINFO devs = SetupDiGetClassDevsA(NULL, "USB", NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (devs != INVALID_HANDLE_VALUE) {
        SP_DEVINFO_DATA devInfo;
        devInfo.cbSize = sizeof(devInfo);
        for (DWORD i = 0; SetupDiEnumDeviceInfo(devs, i, &devInfo); i++) {
            char hwid[512] = {0};
            char desc[256] = {0};
            char svc[128] = {0};
            SetupDiGetDeviceRegistryPropertyA(devs, &devInfo, SPDRP_HARDWAREID, NULL, (PBYTE)hwid, sizeof(hwid), NULL);
            SetupDiGetDeviceRegistryPropertyA(devs, &devInfo, SPDRP_DEVICEDESC, NULL, (PBYTE)desc, sizeof(desc), NULL);
            SetupDiGetDeviceRegistryPropertyA(devs, &devInfo, SPDRP_SERVICE, NULL, (PBYTE)svc, sizeof(svc), NULL);

            char hwid_upper[512] = {0};
            for (size_t k = 0; hwid[k] && k < sizeof(hwid_upper)-1; k++)
                hwid_upper[k] = (char)toupper((unsigned char)hwid[k]);

            char desc_upper[256] = {0};
            for (size_t k = 0; desc[k] && k < sizeof(desc_upper)-1; k++)
                desc_upper[k] = (char)toupper((unsigned char)desc[k]);

            if (strstr(hwid_upper, "VID_04A9") || strstr(desc_upper, "CANON")) {
                LOG_INFO("Discovered device: '%s'", desc[0] ? desc : "Canon USB Device");
                LOG_INFO("  HardwareID:     %s", hwid);
                LOG_INFO("  Driver Service: %s", svc[0] ? svc : "<none>");

                if (svc[0] && strcasecmp(svc, "WinUSB") != 0) {
                    LOG_WARN("--> Device driver is '%s', NOT 'WinUSB'!", svc);
                    LOG_WARN("--> In Zadig, select 'Canon CAPT USB Printer', target 'WinUSB', click 'Replace Driver'.");
                }

                /* Check device registry for custom DeviceInterfaceGUIDs set by Zadig / INF */
                HKEY hKey = SetupDiOpenDevRegKey(devs, &devInfo, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
                if (hKey != INVALID_HANDLE_VALUE) {
                    char reg_guid[256] = {0};
                    DWORD len = sizeof(reg_guid);
                    DWORD type = 0;
                    if (RegQueryValueExA(hKey, "DeviceInterfaceGUIDs", NULL, &type, (LPBYTE)reg_guid, &len) == ERROR_SUCCESS ||
                        RegQueryValueExA(hKey, "DeviceInterfaceGUID", NULL, &type, (LPBYTE)reg_guid, &len) == ERROR_SUCCESS) {
                        GUID parsed_guid;
                        if (parse_guid_string(reg_guid, &parsed_guid) == 0) {
                            LOG_INFO("  Interface GUID: %s", reg_guid);
                            if (try_open_with_guid(dev, &parsed_guid) == 0) {
                                RegCloseKey(hKey);
                                SetupDiDestroyDeviceInfoList(devs);
                                return 0;
                            }
                        }
                    }
                    RegCloseKey(hKey);
                }
            }
        }
        SetupDiDestroyDeviceInfoList(devs);
    }

    /* 2. Try known GUIDs */
    if (try_open_with_guid(dev, &GUID_DEVINTERFACE_LBP810) == 0)
        return 0;

    static const GUID GUID_USB_DEVICE = {
        0xDEE824EF, 0x729B, 0x4A0E,
        {0x9C, 0x14, 0xB7, 0x11, 0x7D, 0x33, 0xA8, 0x17}
    };
    if (try_open_with_guid(dev, &GUID_USB_DEVICE) == 0)
        return 0;

    static const GUID GUID_WINUSB_DEFAULT = {
        0x88BAE032, 0x5A81, 0x49F0,
        {0xBC, 0x3D, 0xA4, 0xFF, 0x13, 0x82, 0x16, 0xD6}
    };
    if (try_open_with_guid(dev, &GUID_WINUSB_DEFAULT) == 0)
        return 0;

    LOG_ERROR("Canon CAPT USB Printer could not be opened.");
    return -1;
}

void usb_close(usb_device_t *dev) {
    if (dev->connected) {
        WinUsb_Free(dev->winusb);
        CloseHandle(dev->device_handle);
        dev->connected = false;
    }
}

int usb_bulk_write(usb_device_t *dev, const uint8_t *data, uint32_t len) {
    ULONG bytes_written = 0;
    if (!dev->connected) return -1;
    if (!WinUsb_WritePipe(dev->winusb, dev->bulk_out_pipe, (PUCHAR)data, len, &bytes_written, NULL)) {
        return -1;
    }
    return (bytes_written == len) ? 0 : -1;
}

int usb_bulk_read(usb_device_t *dev, uint8_t *buf, uint32_t buf_size, uint32_t *actual_read) {
    ULONG bytes_read = 0;
    if (!dev->connected) return -1;
    if (!WinUsb_ReadPipe(dev->winusb, dev->bulk_in_pipe, buf, buf_size, &bytes_read, NULL)) {
        return -1;
    }
    *actual_read = bytes_read;
    return 0;
}

#else

int usb_open(usb_device_t *dev) {
    LOG_ERROR("Linux USB stubs not fully implemented");
    return -1;
}

void usb_close(usb_device_t *dev) {
    (void)dev;
}

int usb_bulk_write(usb_device_t *dev, const uint8_t *data, uint32_t len) {
    (void)dev; (void)data; (void)len;
    return -1;
}

int usb_bulk_read(usb_device_t *dev, uint8_t *buf, uint32_t buf_size, uint32_t *actual_read) {
    (void)dev; (void)buf; (void)buf_size; (void)actual_read;
    return -1;
}

#endif

static uint16_t bcd_decode(uint16_t bcd) {
    uint8_t hi = (bcd >> 8) & 0xFF;
    uint8_t lo = bcd & 0xFF;
    uint8_t n3 = (hi >> 4) & 0x0F;
    uint8_t n2 = hi & 0x0F;
    uint8_t n1 = (lo >> 4) & 0x0F;
    uint8_t n0 = lo & 0x0F;
    return n3 * 1000 + n2 * 100 + n1 * 10 + n0;
}

int usb_send_packet(usb_device_t *dev, uint16_t opcode, const uint8_t *payload, uint16_t payload_len) {
    uint32_t packet_size = 4 + payload_len;
    uint8_t *buf = (uint8_t*)malloc(packet_size);
    if (!buf) return -1;
    
    buf[0] = opcode & 0xFF;
    buf[1] = (opcode >> 8) & 0xFF;
    buf[2] = packet_size & 0xFF;
    buf[3] = (packet_size >> 8) & 0xFF;
    
    if (payload_len > 0 && payload != NULL) {
        memcpy(buf + 4, payload, payload_len);
    }
    
    int ret = usb_bulk_write(dev, buf, packet_size);
    free(buf);
    return ret;
}

int usb_recv_packet(usb_device_t *dev, uint16_t expected_opcode, uint8_t *buf, uint16_t buf_size, uint16_t *actual_size) {
    uint8_t header[64];
    uint32_t read_bytes = 0;
    
    int ret = usb_bulk_read(dev, header, sizeof(header), &read_bytes);
    if (ret != 0 || read_bytes < 4) {
        return -1;
    }
    
    uint16_t opcode = header[0] | (header[1] << 8);
    uint16_t size = header[2] | (header[3] << 8);
    
    if (bcd_decode(size) == read_bytes) {
        size = bcd_decode(size);
    }
    
    if (opcode != expected_opcode) {
        return -1;
    }
    
    uint16_t payload_size = size - 4;
    if (payload_size > buf_size) {
        payload_size = buf_size; // truncate
    }
    
    memcpy(buf, header + 4, (read_bytes - 4 < payload_size) ? read_bytes - 4 : payload_size);
    uint32_t total_payload_read = read_bytes - 4;
    
    while (total_payload_read < payload_size) {
        uint32_t chunk_read = 0;
        ret = usb_bulk_read(dev, buf + total_payload_read, payload_size - total_payload_read, &chunk_read);
        if (ret != 0) {
            break;
        }
        total_payload_read += chunk_read;
    }
    
    if (actual_size) {
        *actual_size = total_payload_read;
    }
    
    return 0;
}
