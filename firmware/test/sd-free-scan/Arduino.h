#ifndef AIUS_SD_FREE_SCAN_ARDUINO_SHIM_H
#define AIUS_SD_FREE_SCAN_ARDUINO_SHIM_H

/* The smallest Arduino.h the vendored SdFat needs to compile on the host:
 * SdFatConfig.h:49-56 forces ENABLE_ARDUINO_FEATURES and includes Arduino.h,
 * and SysCall.h:40-48 then wants Print and Stream (and -DARDUINO). FLASHMEM is
 * empty here, as on every host build. */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FLASHMEM
#define PSTR(s) (s)
#define PGM_P const char *

#define DEC 10
#define HEX 16
#define OCT 8
#define BIN 2

inline uint32_t millis() { return 0; }
inline uint32_t micros() { return 0; }
inline void yield() {}

class __FlashStringHelper;
#define F(string_literal) (reinterpret_cast<const __FlashStringHelper *>(string_literal))

class String {
public:
	String() {}
	String(const char *s) : s_(s) {}
	const char *c_str() const { return s_; }

private:
	const char *s_ = "";
};

class Print {
public:
	virtual ~Print() {}
	virtual size_t write(uint8_t) = 0;
	virtual size_t write(const uint8_t *buffer, size_t size)
	{
		size_t n = 0;
		while (size--)
			n += write(*buffer++);
		return n;
	}
	size_t write(const char *s) { return write((const uint8_t *)s, strlen(s)); }
	size_t write(const char *buffer, size_t size)
	{
		return write((const uint8_t *)buffer, size);
	}
	size_t print(const char *s) { return write(s); }
	size_t print(char c) { return write((uint8_t)c); }
	size_t print(const __FlashStringHelper *s)
	{
		return write(reinterpret_cast<const char *>(s));
	}
	size_t print(unsigned long v, int = 10) { return printNumber(v); }
	size_t print(long v, int = 10) { return printNumber(v); }
	size_t print(unsigned v, int = 10) { return printNumber(v); }
	size_t print(int v, int = 10) { return printNumber(v); }
	size_t print(unsigned char v, int = 10) { return printNumber(v); }
	size_t println() { return write("\r\n"); }
	size_t println(const char *s) { return print(s) + println(); }
	size_t println(char c) { return print(c) + println(); }
	size_t println(const __FlashStringHelper *s) { return print(s) + println(); }
	size_t println(unsigned long v, int = 10) { return printNumber(v) + println(); }
	size_t println(long v, int = 10) { return printNumber(v) + println(); }
	size_t println(unsigned v, int = 10) { return printNumber(v) + println(); }
	size_t println(int v, int = 10) { return printNumber(v) + println(); }
	size_t println(unsigned char v, int = 10) { return printNumber(v) + println(); }
	virtual void flush() {}

private:
	size_t printNumber(long long v)
	{
		char text[32];
		snprintf(text, sizeof(text), "%lld", v);
		return write(text);
	}
};

class Stream : public Print {
public:
	virtual int available() = 0;
	virtual int read() = 0;
	virtual int peek() = 0;
};

extern Stream &Serial;

#endif
