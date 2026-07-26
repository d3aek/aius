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
	/* Non-const, matching usb_serial_class::operator bool() (usb_serial.h:166).
	 * A const conversion here would be MORE permissive than the target: code
	 * testing a const reference would compile on the host and fail to compile on
	 * the board. `explicit` is retained deliberately -- it is stricter than the
	 * real implicit conversion, so it can only reject code the target accepts,
	 * never accept code the target rejects. */
	explicit operator bool();
	int read();
	int availableForWrite();
	size_t write(const uint8_t *data, size_t length);
	void flush();
};

extern FakeUsbSerial Serial;

void _reboot_Teensyduino_(void);

uint32_t micros(void);

uint32_t millis(void);

#endif
