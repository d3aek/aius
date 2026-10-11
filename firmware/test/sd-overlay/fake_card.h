#ifndef AIUS_TEST_FAKE_CARD_H
#define AIUS_TEST_FAKE_CARD_H

/* sd-overlay's stateful fake card for the FR-S6 power-cut model
 * (aius-sample-rate-following.md Phase 4h). It executes lib/aius_sd's
 * primitives against files kept as sectors plus a size, with SdFat's
 * landing rules (FatFile.cpp:1406-1458):
 *
 * - a full, aligned sector WRITE into a sector the file's directory entry
 *   already covers lands on the card at once;
 * - a partial sector, the size and any new allocation (a new file's directory
 *   entry included) land at the next sync, a TRUNCATE or a CLOSE, data before
 *   the directory entry;
 * - a power cut discards everything that has not landed.
 *
 * A cut fires before primitive `cut_after` + 1, or, with `cut_mid_sync` and a
 * sync primitive there, inside it: its data has landed in the sectors the old
 * directory entry covers, the entry itself has not. A fault spec answers the
 * Nth primitive of one kind with a driver fault and no effect; with
 * `fault_card_gone` the card is pulled there (the cache lost, every later
 * primitive faulting) until fake_card_return. RENAME and REMOVE are counted. */

#include <stdbool.h>
#include <stdint.h>

#include "aius_sd.h"

#define FAKE_CARD_SECTOR 512u
#define FAKE_CARD_FILE_MAX 16384u
#define FAKE_CARD_FILES 4u
#define FAKE_CARD_NEVER UINT32_MAX
#define FAKE_CARD_DRIVER_FAULT 0x5344u
#define FAKE_CARD_KINDS ((unsigned)AIUS_SD_ACTION_CACHE_DROP + 1u)

typedef struct {
	char path[AIUS_SD_PATH_STORAGE];
	bool used;
	/* On the card. */
	bool exists;
	uint32_t size;
	uint8_t data[FAKE_CARD_FILE_MAX];
	/* What the mounted volume sees, its cache included. */
	bool live_exists;
	uint32_t live_size;
	uint8_t live[FAKE_CARD_FILE_MAX];
} FakeCardFile;

typedef struct {
	FakeCardFile files[FAKE_CARD_FILES];
	int open;
	bool open_write;
	uint32_t primitives; /* primitives completed (faulted ones included) */
	uint32_t cut_after;  /* FAKE_CARD_NEVER: no cut */
	bool cut_mid_sync;
	bool cut;            /* the power went: execute refuses from here on */
	AiusSdActionKind fault_kind;
	uint32_t fault_occurrence; /* 0: no fault */
	bool fault_card_gone;
	bool gone;
	uint32_t kind_counts[FAKE_CARD_KINDS];
	uint32_t renames;
	uint32_t removes;
} FakeCard;

void fake_card_init(FakeCard *card);

/* Puts a complete file on the card (landed and live). */
void fake_card_store(FakeCard *card, const char *path, const uint8_t *bytes,
                     uint32_t length);

/* Zeroes one sector of a stored file on the card (landed and live). */
void fake_card_zero_sector(FakeCard *card, const char *path, uint32_t sector);

/* Executes one primitive. False when the power is cut before (or inside) it:
 * no event, and the caller abandons the job. */
bool fake_card_execute(FakeCard *card, const AiusSdAction *action,
                       AiusSdEvent *event);

/* The power goes: everything not landed is lost, no file is open. */
void fake_card_power_cut(FakeCard *card);

/* The card is back with its landed state: after a card-gone fault, or the
 * power returning after a cut (the cut disarmed). */
void fake_card_return(FakeCard *card);

/* The file as the next mount reads it; NULL when it does not exist. */
const uint8_t *fake_card_file(const FakeCard *card, const char *path,
                              uint32_t *length);

#endif
