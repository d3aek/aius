#ifndef AIUS_TEST_FAKE_SERVER_H
#define AIUS_TEST_FAKE_SERVER_H

#include "Arduino.h"

class Server : public Print {
public:
    virtual void begin() = 0;
};

#endif
