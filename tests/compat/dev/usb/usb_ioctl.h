#pragma once
#include <stdint.h>
struct usb_fs_endpoint {
    void **ppBuffer;
    uint32_t *pLength;
    uint32_t nFrames, aFrames;
    uint16_t flags, timeout, isoc_time_complete;
    int status;
};
struct usb_fs_start { uint8_t ep_index; };
struct usb_fs_complete { uint8_t ep_index; };
struct usb_fs_stop { uint8_t ep_index; };
struct usb_fs_close { uint8_t ep_index; };
#define USB_FS_START 192ul
#define USB_FS_STOP 193ul
#define USB_FS_COMPLETE 194ul
#define USB_FS_CLOSE 198ul
#define USB_FS_FLAG_SINGLE_SHORT_OK 1
#define USB_FS_FLAG_MULTI_SHORT_OK 2
