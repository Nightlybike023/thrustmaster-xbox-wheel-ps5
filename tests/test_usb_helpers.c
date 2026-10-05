#include "usb_helpers.h"
#include <assert.h>
#include <stdarg.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>

static struct usb_fs_endpoint *active;
static int mode, completions, stopped, closed;
int usleep(useconds_t us) { (void)us; return 0; }
int ioctl(int fd, unsigned long request, ...) {
    (void)fd;
    va_list ap; va_start(ap, request); void *arg = va_arg(ap, void *); va_end(ap);
    if (request == USB_FS_START) {
        if (mode == 4) { errno = EBUSY; return -1; }
        return 0;
    }
    if (request == USB_FS_STOP) { stopped++; return 0; }
    if (request == USB_FS_CLOSE) { closed++; return 0; }
    assert(request == USB_FS_COMPLETE);
    completions++;
    if (mode == 2) { errno = EBUSY; return -1; }
    struct usb_fs_complete *complete = arg;
    complete->ep_index = completions == 1 ? 0 : 1;
    if (complete->ep_index == 1) {
        active->status = mode == 1 ? 20 : 0;
        active->aFrames = 1;
        if (mode == 3) active->pLength[0] = 1;
    }
    return 0;
}
int main(void) {
    uint8_t data[5] = {0};
    for (mode = 0; mode < 5; mode++) {
        struct usb_fs_endpoint ep = {0}; active = &ep;
        completions = stopped = closed = 0;
        int ret = usb_send_out(-1, &ep, data, sizeof(data), "test");
        if (mode == 0) { assert(ret == 0 && completions == 2 && !stopped && !closed); }
        else { assert(ret < 0 && stopped == 1 && closed == 1); }
        assert(ep.ppBuffer == NULL && ep.pLength == NULL);
    }
    puts("PASS: USB completion routing, failed status, short writes, timeout and busy cleanup");
}
