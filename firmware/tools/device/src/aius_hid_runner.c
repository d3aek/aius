/* Linux hidraw transport for the Aius P6 capability runner. */

#include "aius_hid_runner.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>

static size_t hid_write(void *ctx, const uint8_t *data, size_t len)
{
    AiusHidTransport *hid = ctx;
    if (len > AIUS_HID_REPORT_BYTES)
        return 0;
    memset(hid->report, 0, sizeof(hid->report));
    memcpy(hid->report, data, len);
    ssize_t written = write(hid->fd, hid->report, sizeof(hid->report));
    if (written < 0)
        hid->last_errno = errno;
    return written == (ssize_t)sizeof(hid->report) ? len : 0;
}

static size_t hid_read(void *ctx, uint8_t *dst, size_t cap)
{
    AiusHidTransport *hid = ctx;
    if (cap == 0)
        return 0;
    ssize_t n = read(hid->fd, dst, cap < AIUS_HID_REPORT_BYTES
                                  ? cap : AIUS_HID_REPORT_BYTES);
    if (n < 0)
        hid->last_errno = errno;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return 0;
    return n > 0 ? (size_t)n : 0;
}

static size_t hid_available(void *ctx)
{
    AiusHidTransport *hid = ctx;
    struct pollfd pfd = { .fd = hid->fd, .events = POLLIN };
    int ready = poll(&pfd, 1, 0);
    return ready > 0 && (pfd.revents & POLLIN) != 0 ? AIUS_HID_REPORT_BYTES : 0;
}

int aius_hid_transport_open(AiusHidTransport *hid, const char *path)
{
    memset(hid, 0, sizeof(*hid));
    hid->last_errno = 0;
    hid->fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (hid->fd < 0)
        return -1;
    hid->transport.ctx = hid;
    hid->transport.write = hid_write;
    hid->transport.read = hid_read;
    hid->transport.available = hid_available;
    return 0;
}

void aius_hid_transport_close(AiusHidTransport *hid)
{
    if (hid->fd >= 0) {
        close(hid->fd);
        hid->fd = -1;
    }
}

const AiusHostTransport *aius_hid_transport_interface(AiusHidTransport *hid)
{
    return &hid->transport;
}
