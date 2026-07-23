#ifndef AIUS_IO_H
#define AIUS_IO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	AIUS_IO_PROGRESS,
	AIUS_IO_WOULD_BLOCK,
	AIUS_IO_CLOSED,
	AIUS_IO_FAULT
} AiusIoResult;

typedef struct {
	AiusIoResult result;
	size_t bytes;
	uint32_t fault;
} AiusIoEvent;

#ifdef __cplusplus
}
#endif

#endif
