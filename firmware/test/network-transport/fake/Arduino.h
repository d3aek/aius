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
    int getWriteError() const { return write_error_; }
    void clearWriteError() { write_error_ = 0; }
    void setWriteError(int error = 1) { write_error_ = error; }
    template <typename T> void print(T) {}
    template <typename T> void print(T, int) {}
    template <typename T> void println(T) {}
    void println() {}
private:
    int write_error_ = 0;
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
    /* Real IntervalTimer resolves this to NVIC_SET_PRIORITY(IRQ_PIT, n),
     * because all four PIT channels share one interrupt
     * (cores/teensy4/IntervalTimer.h:96). The fake routes it the same way so a
     * test cannot tell the two spellings apart -- which is the point, since the
     * adapter deliberately uses the NVIC form (_fnet_poll is private). */
    void priority(uint8_t n);
};

/* The NVIC priority register file, modelled as the one thing a host test can
 * observe about it: the last value written per interrupt.
 *
 * Modelled rather than stubbed away because FNET's poll timer preempting the
 * audio software interrupt is a real defect with no other host-visible
 * signature -- stubbing NVIC_SET_PRIORITY to nothing would make the suite
 * report agreement with a firmware that had never set the priority at all. */
enum { IRQ_PIT = 122 };

extern uint8_t fake_nvic_priority[256];

#define NVIC_SET_PRIORITY(irq, prio) \
    (fake_nvic_priority[(unsigned)(irq) & 0xFFu] = (uint8_t)(prio))

inline void IntervalTimer::priority(uint8_t n) { NVIC_SET_PRIORITY(IRQ_PIT, n); }

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
