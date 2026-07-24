#ifndef AIUS_TEST_FAKE_CLIENT_H
#define AIUS_TEST_FAKE_CLIENT_H

#include "Arduino.h"

class Client : public Print {
public:
    virtual int connect(IPAddress, uint16_t) = 0;
    virtual int connect(const char *, uint16_t) = 0;
    virtual int available() = 0;
    virtual int read() = 0;
    virtual int read(uint8_t *, size_t) = 0;
    virtual int peek() = 0;
    virtual void stop() = 0;
    virtual uint8_t connected() = 0;
    virtual operator bool() = 0;
};

#endif
