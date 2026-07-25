/* Host stand-in for the Teensyduino Arduino.h surface the composed runtime
 * touches. Only the CDC adapter's production primitive table needs it: one
 * usb_serial_class-shaped object plus the software bootloader entry point.
 *
 * FLASHMEM is deliberately NOT defined here -- the target sources fall back to
 * an empty definition on the host, exactly as aius_optical_app.cpp does. */

#ifndef AIUS_RUNTIME_TARGET_FAKE_ARDUINO_H
#define AIUS_RUNTIME_TARGET_FAKE_ARDUINO_H

#include <stddef.h>
#include <stdint.h>

class FakeUsbSerial {
public:
	explicit operator bool() const;
	int read();
	int availableForWrite();
	size_t write(const uint8_t *data, size_t length);
};

extern FakeUsbSerial Serial;

void _reboot_Teensyduino_(void);

uint32_t micros(void);

#endif
