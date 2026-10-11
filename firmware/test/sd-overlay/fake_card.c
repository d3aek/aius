/* sd-overlay's stateful fake card (FR-S6); the model is in fake_card.h. */
#include "fake_card.h"

#include <string.h>

static bool path_equal(const char *a, const char *b)
{
	for (;; a++, b++) {
		char x = *a;
		char y = *b;

		if (x >= 'A' && x <= 'Z')
			x = (char)(x - 'A' + 'a');
		if (y >= 'A' && y <= 'Z')
			y = (char)(y - 'A' + 'a');
		if (x != y)
			return false;
		if (x == '\0')
			return true;
	}
}

static FakeCardFile *find(FakeCard *card, const char *path)
{
	for (unsigned i = 0u; i < FAKE_CARD_FILES; i++)
		if (card->files[i].used && path_equal(card->files[i].path, path))
			return &card->files[i];
	return NULL;
}

static FakeCardFile *find_or_add(FakeCard *card, const char *path)
{
	FakeCardFile *file = find(card, path);

	if (file != NULL)
		return file;
	for (unsigned i = 0u; i < FAKE_CARD_FILES; i++) {
		file = &card->files[i];
		if (!file->used && strlen(path) < sizeof(file->path)) {
			memset(file, 0, sizeof(*file));
			file->used = true;
			strcpy(file->path, path);
			return file;
		}
	}
	return NULL;
}

static uint32_t sectors(uint32_t size)
{
	return (size + FAKE_CARD_SECTOR - 1u) / FAKE_CARD_SECTOR;
}

/* Sectors the card's directory entry covers. */
static uint32_t allocated(const FakeCardFile *file)
{
	return file->exists ? sectors(file->size) : 0u;
}

/* Data first, into the sectors already allocated; then, unless the cut lands
 * inside this sync, the new allocation and the directory entry. */
static void sync_file(FakeCardFile *file, bool cut_inside)
{
	uint32_t old = allocated(file);
	uint32_t live = sectors(file->live_size);

	for (uint32_t s = 0u; s < old && s < live; s++)
		memcpy(file->data + s * FAKE_CARD_SECTOR,
		       file->live + s * FAKE_CARD_SECTOR, FAKE_CARD_SECTOR);
	if (cut_inside)
		return;
	if (live > old)
		memcpy(file->data + old * FAKE_CARD_SECTOR,
		       file->live + old * FAKE_CARD_SECTOR,
		       (live - old) * FAKE_CARD_SECTOR);
	file->exists = file->live_exists;
	file->size = file->live_size;
}

static void lose_cache(FakeCard *card)
{
	for (unsigned i = 0u; i < FAKE_CARD_FILES; i++) {
		FakeCardFile *file = &card->files[i];

		if (!file->used)
			continue;
		file->live_exists = file->exists;
		file->live_size = file->size;
		memcpy(file->live, file->data, sizeof(file->live));
	}
	card->open = -1;
	card->open_write = false;
}

void fake_card_init(FakeCard *card)
{
	memset(card, 0, sizeof(*card));
	card->open = -1;
	card->cut_after = FAKE_CARD_NEVER;
}

void fake_card_store(FakeCard *card, const char *path, const uint8_t *bytes,
                     uint32_t length)
{
	FakeCardFile *file = find_or_add(card, path);

	if (file == NULL || length > FAKE_CARD_FILE_MAX)
		return;
	memset(file->data, 0, sizeof(file->data));
	memcpy(file->data, bytes, length);
	file->exists = true;
	file->size = length;
	memcpy(file->live, file->data, sizeof(file->live));
	file->live_exists = true;
	file->live_size = length;
}

void fake_card_zero_sector(FakeCard *card, const char *path, uint32_t sector)
{
	FakeCardFile *file = find(card, path);

	if (file == NULL || sector >= FAKE_CARD_FILE_MAX / FAKE_CARD_SECTOR)
		return;
	memset(file->data + sector * FAKE_CARD_SECTOR, 0, FAKE_CARD_SECTOR);
	memset(file->live + sector * FAKE_CARD_SECTOR, 0, FAKE_CARD_SECTOR);
}

void fake_card_power_cut(FakeCard *card)
{
	lose_cache(card);
	card->cut = true;
}

void fake_card_return(FakeCard *card)
{
	card->gone = false;
	card->cut = false;
	card->cut_after = FAKE_CARD_NEVER;
	card->cut_mid_sync = false;
}

const uint8_t *fake_card_file(const FakeCard *card, const char *path,
                              uint32_t *length)
{
	FakeCardFile *file = find((FakeCard *)card, path);

	*length = 0u;
	if (file == NULL || !file->live_exists)
		return NULL;
	*length = file->live_size;
	return file->live;
}

static void answer(AiusSdEvent *event, const AiusSdAction *action,
                   AiusSdEventKind kind)
{
	memset(event, 0, sizeof(*event));
	event->kind = kind;
	event->action_id = action->action_id;
}

static void fault(AiusSdEvent *event, const AiusSdAction *action)
{
	answer(event, action, AIUS_SD_EVENT_FAULT);
	event->fault = FAKE_CARD_DRIVER_FAULT;
}

static bool is_sync(AiusSdActionKind kind)
{
	return kind == AIUS_SD_ACTION_TRUNCATE || kind == AIUS_SD_ACTION_CLOSE;
}

bool fake_card_execute(FakeCard *card, const AiusSdAction *action,
                       AiusSdEvent *event)
{
	FakeCardFile *file = card->open >= 0 ? &card->files[card->open] : NULL;

	if (card->cut)
		return false;
	if (card->primitives == card->cut_after) {
		if (card->cut_mid_sync && is_sync(action->kind) && file != NULL &&
		    card->open_write) {
			if (action->kind == AIUS_SD_ACTION_TRUNCATE &&
			    action->offset <= FAKE_CARD_FILE_MAX)
				file->live_size = (uint32_t)action->offset;
			sync_file(file, true);
		}
		fake_card_power_cut(card);
		return false;
	}
	card->primitives++;
	if ((unsigned)action->kind < FAKE_CARD_KINDS)
		card->kind_counts[action->kind]++;
	if (card->gone) {
		if (action->kind == AIUS_SD_ACTION_CLOSE) {
			card->open = -1;
			card->open_write = false;
		}
		fault(event, action);
		return true;
	}
	if (card->fault_occurrence != 0u && action->kind == card->fault_kind &&
	    card->kind_counts[action->kind] == card->fault_occurrence) {
		if (card->fault_card_gone) {
			lose_cache(card);
			card->gone = true;
		} else if (action->kind == AIUS_SD_ACTION_CLOSE) {
			card->open = -1;
			card->open_write = false;
		}
		fault(event, action);
		return true;
	}

	switch (action->kind) {
	case AIUS_SD_ACTION_BEGIN:
	case AIUS_SD_ACTION_MKDIR:
	case AIUS_SD_ACTION_CACHE_DROP:
		answer(event, action, AIUS_SD_EVENT_OK);
		return true;
	case AIUS_SD_ACTION_EXISTS: {
		FakeCardFile *named = find(card, action->path);

		answer(event, action, AIUS_SD_EVENT_BOOL);
		event->value = path_equal(action->path, "/aius") ||
		               (named != NULL && named->live_exists);
		return true;
	}
	case AIUS_SD_ACTION_OPEN_WRITE_BEGIN:
	case AIUS_SD_ACTION_OPEN_READ: {
		bool write = action->kind == AIUS_SD_ACTION_OPEN_WRITE_BEGIN;
		FakeCardFile *named = write ? find_or_add(card, action->path)
		                            : find(card, action->path);

		if (file != NULL || named == NULL ||
		    (!write && !named->live_exists)) {
			fault(event, action);
			return true;
		}
		if (write && !named->live_exists) {
			/* O_CREAT: an empty file, its entry landing at the next sync. */
			named->live_exists = true;
			named->live_size = 0u;
		}
		card->open = (int)(named - card->files);
		card->open_write = write;
		answer(event, action, AIUS_SD_EVENT_OK);
		return true;
	}
	case AIUS_SD_ACTION_WRITE: {
		if (file == NULL || !card->open_write ||
		    action->offset + action->length > FAKE_CARD_FILE_MAX) {
			fault(event, action);
			return true;
		}
		uint32_t offset = (uint32_t)action->offset;
		uint32_t end = offset + (uint32_t)action->length;

		memcpy(file->live + offset, action->buffer, action->length);
		if (end > file->live_size)
			file->live_size = end;
		/* A full aligned sector inside the landed allocation goes straight
		 * to the card. */
		for (uint32_t s = offset / FAKE_CARD_SECTOR;
		     s * FAKE_CARD_SECTOR < end; s++) {
			uint32_t start = s * FAKE_CARD_SECTOR;

			if (start >= offset && start + FAKE_CARD_SECTOR <= end &&
			    s < allocated(file))
				memcpy(file->data + start, file->live + start,
				       FAKE_CARD_SECTOR);
		}
		answer(event, action, AIUS_SD_EVENT_BYTES);
		event->bytes = action->length;
		return true;
	}
	case AIUS_SD_ACTION_TRUNCATE:
		if (file == NULL || !card->open_write ||
		    action->offset > FAKE_CARD_FILE_MAX) {
			fault(event, action);
			return true;
		}
		if (action->offset > file->live_size)
			memset(file->live + file->live_size, 0,
			       (size_t)action->offset - file->live_size);
		file->live_size = (uint32_t)action->offset;
		sync_file(file, false);
		answer(event, action, AIUS_SD_EVENT_OK);
		return true;
	case AIUS_SD_ACTION_SEEK:
		if (file == NULL) {
			fault(event, action);
			return true;
		}
		answer(event, action, AIUS_SD_EVENT_OK);
		return true;
	case AIUS_SD_ACTION_CLOSE:
		if (file == NULL) {
			fault(event, action);
			return true;
		}
		if (card->open_write)
			sync_file(file, false);
		card->open = -1;
		card->open_write = false;
		answer(event, action, AIUS_SD_EVENT_OK);
		return true;
	case AIUS_SD_ACTION_FILE_SIZE:
		if (file == NULL) {
			fault(event, action);
			return true;
		}
		answer(event, action, AIUS_SD_EVENT_SIZE);
		event->size = file->live_size;
		return true;
	case AIUS_SD_ACTION_READ: {
		if (file == NULL || action->offset > file->live_size) {
			fault(event, action);
			return true;
		}
		size_t n = file->live_size - (uint32_t)action->offset;

		if (n > action->length)
			n = action->length;
		memcpy(action->buffer, file->live + action->offset, n);
		answer(event, action, AIUS_SD_EVENT_BYTES);
		event->bytes = n;
		return true;
	}
	case AIUS_SD_ACTION_REMOVE: {
		FakeCardFile *named = find(card, action->path);

		card->removes++;
		if (named != NULL) {
			named->exists = false;
			named->live_exists = false;
			named->size = 0u;
			named->live_size = 0u;
		}
		answer(event, action, AIUS_SD_EVENT_OK);
		return true;
	}
	case AIUS_SD_ACTION_RENAME:
		card->renames++;
		fault(event, action);
		return true;
	default:
		fault(event, action);
		return true;
	}
}
