#ifndef AIUS_TEST_FAKE_ARDUINO_H
#define AIUS_TEST_FAKE_ARDUINO_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>

#define DMAMEM
#define HEX 16

typedef bool boolean;

class IPAddress {
public:
    IPAddress() : value_(0) {}
    IPAddress(uint32_t value) : value_(value) {}
    IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
        : value_(uint32_t(a) | (uint32_t(b) << 8) |
                 (uint32_t(c) << 16) | (uint32_t(d) << 24)) {}
    uint8_t &operator[](size_t index) {
        return reinterpret_cast<uint8_t *>(&value_)[index];
    }
    uint8_t operator[](size_t index) const {
        return reinterpret_cast<const uint8_t *>(&value_)[index];
    }
    operator uint32_t() const { return value_; }
    bool operator==(const IPAddress &other) const {
        return value_ == other.value_;
    }
    bool operator!=(const IPAddress &other) const {
        return !(*this == other);
    }
    IPAddress &operator=(const uint8_t *bytes) {
        memcpy(&value_, bytes, 4);
        return *this;
    }
private:
    uint32_t value_;
};

inline uint8_t *rawIPAddress(IPAddress &address) {
    return reinterpret_cast<uint8_t *>(&address);
}

class Print {
public:
    virtual ~Print() {}
    virtual size_t write(uint8_t) = 0;
    virtual size_t write(const uint8_t *buffer, size_t size) {
        size_t written = 0;
        while (written < size)
            written += write(buffer[written]);
        return written;
    }
    virtual int availableForWrite() { return 0; }
    virtual void flush() {}
    void setWriteError() { write_error_ = true; }
    template <typename T> void print(T) {}
    template <typename T> void print(T, int) {}
    template <typename T> void println(T) {}
    void println() {}
private:
    bool write_error_ = false;
};

class FakeSerialClass : public Print {
public:
    size_t write(uint8_t) override { return 1; }
    void send_now() {}
};

extern FakeSerialClass Serial;

class IntervalTimer {
public:
    template <typename Callback>
    bool begin(Callback, uint32_t) { return true; }
    void end() {}
};

class elapsedMillis {
public:
    elapsedMillis(uint32_t value = 0) : value_(value) {}
    operator uint32_t() const { return value_; }
private:
    uint32_t value_;
};

unsigned long millis();
unsigned long micros();
void delay(unsigned long);
void yield();

#endif
