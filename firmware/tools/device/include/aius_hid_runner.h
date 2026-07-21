#ifndef AIUS_HID_RUNNER_H
#define AIUS_HID_RUNNER_H

#include "aius_host_runner.h"

#define AIUS_HID_REPORT_BYTES 64u

typedef struct {
    int fd;
    int last_errno;
    uint8_t report[AIUS_HID_REPORT_BYTES];
    AiusHostTransport transport;
} AiusHidTransport;

int aius_hid_transport_open(AiusHidTransport *hid, const char *path);
void aius_hid_transport_close(AiusHidTransport *hid);
const AiusHostTransport *aius_hid_transport_interface(AiusHidTransport *hid);

#endif
