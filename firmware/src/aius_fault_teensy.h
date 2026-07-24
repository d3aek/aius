#ifndef AIUS_FAULT_TEENSY_H
#define AIUS_FAULT_TEENSY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void aius_fault_checkpoint(uint16_t checkpoint);

#ifdef __cplusplus
}
#endif

#endif
