#ifndef AIUS_TEST_FAKE_UDP_H
#define AIUS_TEST_FAKE_UDP_H

#include "Arduino.h"

class UDP : public Print {
public:
    virtual uint8_t begin(uint16_t) = 0;
    virtual uint8_t beginMulticast(IPAddress, uint16_t) = 0;
    virtual void stop() = 0;
    virtual int beginPacket(IPAddress, uint16_t) = 0;
    virtual int beginPacket(const char *, uint16_t) = 0;
    virtual int endPacket() = 0;
    virtual int parsePacket() = 0;
    virtual int available() = 0;
    virtual int read() = 0;
    virtual int read(unsigned char *, size_t) = 0;
    virtual int peek() = 0;
};

#endif
