#ifndef AIUS_BOOT_H
#define AIUS_BOOT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ASCII "AIUB", matching the retained marker's human-readable hex form. */
#define AIUS_BOOT_MAGIC 0x41495542u
#define AIUS_BOOT_VERSION 1u

#define AIUS_BOOT_CHECKPOINT_RESET_CAPTURE 0u
#define AIUS_BOOT_CHECKPOINT_USB_RX 1u
#define AIUS_BOOT_CHECKPOINT_USB_TX 2u
#define AIUS_BOOT_CHECKPOINT_METRICS 3u
#define AIUS_BOOT_CHECKPOINT_NETWORK 4u
#define AIUS_BOOT_CHECKPOINT_HTTP 5u
#define AIUS_BOOT_CHECKPOINT_UDP 6u
#define AIUS_BOOT_CHECKPOINT_REPLAY 7u
#define AIUS_BOOT_CHECKPOINT_SD 8u
#define AIUS_BOOT_CHECKPOINT_EPOCH_COMPLETE 9u

typedef enum {
	AIUS_BOOT_MODE_NORMAL = 0,
	AIUS_BOOT_MODE_RECOVERY_SAFE = 1
} AiusBootMode;

typedef enum {
	AIUS_RESET_COLD,
	AIUS_RESET_SOFTWARE,
	AIUS_RESET_WATCHDOG,
	AIUS_RESET_OTHER
} AiusResetKind;

typedef struct {
	uint32_t magic;
	uint16_t version;
	uint16_t last_checkpoint;
	uint32_t build_id;
	uint32_t boot_generation;
	uint32_t reset_source;
	uint8_t early_watchdog_resets;
	uint8_t mode;
	uint16_t reserved;
	uint32_t crc32;
} AiusBootLedger;

typedef struct {
	uint32_t build_id;
	uint32_t raw_reset_source;
	AiusResetKind reset_kind;
} AiusBootInput;

#ifdef __cplusplus
static_assert(sizeof(AiusBootLedger) == 28, "portable boot ledger size");
#else
_Static_assert(sizeof(AiusBootLedger) == 28, "portable boot ledger size");
#endif

void aius_boot_init(AiusBootLedger *ledger,
	            const AiusBootLedger *retained,
	            AiusBootInput input);

void aius_boot_record_checkpoint(AiusBootLedger *ledger,
	                         uint16_t checkpoint);

void aius_boot_mark_healthy(AiusBootLedger *ledger);

bool aius_boot_is_valid(const AiusBootLedger *ledger);

#ifdef __cplusplus
}
#endif

#endif
