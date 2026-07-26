#include "aius_sd_teensy.h"

#include <limits.h>

/* Cold code -> FLASH. Nothing here runs on the audio block path (this is the
 * one-shot SD driver adapter, stepped once per epoch), so every definition
 * below carries FLASHMEM and lands in FLASH instead of the shared FlexRAM/ITCM
 * pool the audio pipeline needs. FLASHMEM is Teensyduino's
 * __attribute__((section(".flashmem"))) from <avr/pgmspace.h>; TEENSYDUINO is a
 * project-wide compiler -D, so the native host suites fall through to the
 * no-op. The attribute must sit on each DEFINITION: GCC does not merge a
 * `section` attribute from a forward declaration, so a prototype-only FLASHMEM
 * silently leaves the code in ITCM. The SDFile/File/StreamFile members that
 * <SD.h> emits here are framework COMDAT copies, not definitions of this file;
 * they are deduplicated at link time and stay outside this contract. Enforced
 * post-link by test/itcm-placement/verify_target.py. */
#if defined(TEENSYDUINO)
#include <avr/pgmspace.h>
#endif
#ifndef FLASHMEM
#define FLASHMEM
#endif

namespace {

constexpr size_t kMaximumTransfer = 512u;

FLASHMEM AiusSdEvent none_event()
{
	return {};
}

FLASHMEM AiusSdEvent action_event(AiusSdEventKind kind, uint64_t action_id)
{
	AiusSdEvent event{};
	event.kind = kind;
	event.action_id = action_id;
	return event;
}

FLASHMEM bool empty_payload(const AiusSdAction *action)
{
	return action->path == nullptr && action->path_to == nullptr &&
	       action->buffer == nullptr && action->length == 0u &&
	       action->offset == 0u && action->fault == 0u;
}

FLASHMEM bool path_payload(const AiusSdAction *action)
{
	return action->path != nullptr && action->path_to == nullptr &&
	       action->buffer == nullptr && action->length == 0u &&
	       action->offset == 0u && action->fault == 0u;
}

FLASHMEM bool rename_payload(const AiusSdAction *action)
{
	return action->path != nullptr && action->path_to != nullptr &&
	       action->buffer == nullptr && action->length == 0u &&
	       action->offset == 0u && action->fault == 0u;
}

FLASHMEM bool truncate_payload(const AiusSdAction *action)
{
	return empty_payload(action);
}

FLASHMEM bool seek_payload(const AiusSdAction *action)
{
	return action->path == nullptr && action->path_to == nullptr &&
	       action->buffer == nullptr && action->length == 0u &&
	       action->offset <= static_cast<uint64_t>(INT64_MAX) &&
	       action->fault == 0u;
}

FLASHMEM bool transfer_payload(const AiusSdAction *action)
{
	return action->path == nullptr && action->path_to == nullptr &&
	       action->buffer != nullptr && action->length != 0u &&
	       action->length <= kMaximumTransfer &&
	       action->offset <= static_cast<uint64_t>(INT64_MAX) &&
	       action->fault == 0u;
}

FLASHMEM void record_duration(AiusSdTeensy *adapter, uint32_t started, uint32_t ended)
{
	const uint32_t duration = ended - started;
	adapter->last_duration_us = duration;
	if (duration > adapter->longest_duration_us)
		adapter->longest_duration_us = duration;
}

FLASHMEM AiusSdEvent protocol_fault(
	AiusSdTeensy *adapter,
	const AiusSdAction *action)
{
	adapter->last_duration_us = 0u;
	if (adapter->protocol_faults != UINT32_MAX)
		adapter->protocol_faults++;
	adapter->last_fault = AIUS_SD_TEENSY_PROTOCOL_FAULT;
	adapter->last_protocol_action = (uint8_t)action->kind;
	AiusSdEvent event =
		action_event(AIUS_SD_EVENT_FAULT, action->action_id);
	event.fault = AIUS_SD_TEENSY_PROTOCOL_FAULT;
	return event;
}

FLASHMEM AiusSdEvent driver_fault(AiusSdTeensy *adapter, uint64_t action_id)
{
	if (adapter->driver_faults != UINT32_MAX)
		adapter->driver_faults++;
	/* THE CARD SAID NO, SO THE MOUNT IS NO LONGER TRUSTED. Deliberately
	 * blunt: this fires for an open against a missing file as readily as for
	 * a card that stopped answering, and the adapter cannot tell those apart
	 * from a bool. Over-invalidating costs one extra mount on a path that has
	 * already failed; under-invalidating leaves a stale volume descriptor in
	 * front of every later job, which is the failure mode worth avoiding. The
	 * hot path is unaffected -- a replay refill opens a file that exists. */
	adapter->mounted = false;
	adapter->last_fault = AIUS_SD_TEENSY_DRIVER_FAULT;
	AiusSdEvent event = action_event(AIUS_SD_EVENT_FAULT, action_id);
	event.fault = AIUS_SD_TEENSY_DRIVER_FAULT;
	return event;
}

FLASHMEM bool action_id_valid(const AiusSdAction *action)
{
	return action->action_id != 0u;
}

/* The driver's own FAT-type numbering, translated to the portable enum. 12, 16
 * and 32 are the values the status surface has always named; everything else --
 * an unmounted volume (0) and exFAT (64) among them -- is UNKNOWN, which is the
 * answer the legacy switch this restores gave for a type it did not recognise.
 * Widening the vocabulary would be a wire change rather than a fix. */
FLASHMEM AiusSdFilesystem filesystem_of(uint8_t fat_type)
{
	switch (fat_type) {
	case 12u:
		return AIUS_SD_FILESYSTEM_FAT12;
	case 16u:
		return AIUS_SD_FILESYSTEM_FAT16;
	case 32u:
		return AIUS_SD_FILESYSTEM_FAT32;
	default:
		return AIUS_SD_FILESYSTEM_UNKNOWN;
	}
}

}

FLASHMEM void aius_sd_teensy_init(AiusSdTeensy *adapter)
{
	if (adapter == nullptr)
		return;
	adapter->file_open = false;
	adapter->mounted = false;
	adapter->last_duration_us = 0u;
	adapter->longest_duration_us = 0u;
	adapter->protocol_faults = 0u;
	adapter->last_fault = 0u;
	adapter->mounts = 0u;
	adapter->mount_reuses = 0u;
	adapter->driver_faults = 0u;
	adapter->last_protocol_action = (uint8_t)AIUS_SD_ACTION_NONE;
}

FLASHMEM void aius_sd_teensy_invalidate_mount(AiusSdTeensy *adapter)
{
	if (adapter == nullptr)
		return;
	adapter->mounted = false;
}

FLASHMEM AiusSdEvent aius_sd_teensy_execute(
	AiusSdTeensy *adapter,
	const AiusSdAction *action)
{
	if (adapter == nullptr)
		return none_event();
	adapter->last_duration_us = 0u;
	if (action == nullptr)
		return none_event();

	switch (action->kind) {
	case AIUS_SD_ACTION_NONE:
	case AIUS_SD_ACTION_NEED_INPUT:
	case AIUS_SD_ACTION_OUTPUT:
	case AIUS_SD_ACTION_PUBLISH_CONFIG:
	case AIUS_SD_ACTION_COMPLETE:
	case AIUS_SD_ACTION_FAIL:
		return none_event();

	case AIUS_SD_ACTION_BEGIN: {
		if (!action_id_valid(action) || !empty_payload(action) ||
		    adapter->file_open)
			return protocol_fault(adapter, action);
		/* ALREADY MOUNTED IS AN HONEST OK. The portable machine opens
		 * every job with a BEGIN, and SDClass::begin() has no
		 * idempotence of its own -- it re-runs the whole SDIO card
		 * identification and volume mount every time it is called. The
		 * work this skips is therefore real work on the card, not a
		 * formality, and skipping it is what removes both the mount tax
		 * and the mount's failure mode. The flag is only ever true
		 * because a mount succeeded and nothing since has suggested the
		 * card went away; see aius_sd_teensy.h for what clears it. No
		 * card transaction, so no duration to record. */
		if (adapter->mounted) {
			if (adapter->mount_reuses != UINT32_MAX)
				adapter->mount_reuses++;
			return action_event(AIUS_SD_EVENT_OK,
			                    action->action_id);
		}
		if (adapter->mounts != UINT32_MAX)
			adapter->mounts++;
		const uint32_t started = micros();
		const bool success = SD.begin(BUILTIN_SDCARD);
		const uint32_t ended = micros();
		record_duration(adapter, started, ended);
		adapter->mounted = success;
		return success
			       ? action_event(AIUS_SD_EVENT_OK, action->action_id)
			       : driver_fault(adapter, action->action_id);
	}

	case AIUS_SD_ACTION_MEDIA_PRESENT: {
		if (!action_id_valid(action) || !empty_payload(action))
			return protocol_fault(adapter, action);
		const uint32_t started = micros();
		const bool present = SD.mediaPresent();
		/* Not a second SD primitive and not an extra epoch: fatType()
		 * returns the type the mount already recorded in the volume
		 * descriptor -- no card transaction, no wait, no failure mode.
		 * Reading it here, inside the probe the adapter already runs and
		 * while it holds the mounted volume, is the only place the
		 * status surface can learn it without a caller reaching past
		 * this adapter at a moment a job may be mid-mount. */
		const uint8_t fat_type =
			present ? SD.sdfs.vol()->fatType() : 0u;
		const uint32_t ended = micros();
		record_duration(adapter, started, ended);
		/* THE REVALIDATION POINT for the cached mount above, and the
		 * reason caching one is safe at all. This runs on the 1 Hz
		 * probe and asks the card itself (CMD13 on BUILTIN_SDCARD); a
		 * card that does not answer drops the cache, so the next job
		 * mounts for real and the lifecycle sees the absence on exactly
		 * the cadence it always did. */
		if (!present)
			adapter->mounted = false;
		AiusSdEvent event =
			action_event(AIUS_SD_EVENT_BOOL, action->action_id);
		event.value = present;
		event.filesystem = filesystem_of(fat_type);
		return event;
	}

	case AIUS_SD_ACTION_MKDIR: {
		if (!action_id_valid(action) || !path_payload(action))
			return protocol_fault(adapter, action);
		const uint32_t started = micros();
		const bool success = SD.mkdir(action->path);
		const uint32_t ended = micros();
		record_duration(adapter, started, ended);
		return success
			       ? action_event(AIUS_SD_EVENT_OK, action->action_id)
			       : driver_fault(adapter, action->action_id);
	}

	case AIUS_SD_ACTION_OPEN_READ: {
		if (!action_id_valid(action) || !path_payload(action) ||
		    adapter->file_open)
			return protocol_fault(adapter, action);
		const uint32_t started = micros();
		File opened = SD.open(action->path, FILE_READ);
		const uint32_t ended = micros();
		record_duration(adapter, started, ended);
		if (!opened)
			return driver_fault(adapter, action->action_id);
		adapter->file = opened;
		adapter->file_open = true;
		return action_event(AIUS_SD_EVENT_OK, action->action_id);
	}

	case AIUS_SD_ACTION_OPEN_WRITE_BEGIN: {
		if (!action_id_valid(action) || !path_payload(action) ||
		    adapter->file_open)
			return protocol_fault(adapter, action);
		const uint32_t started = micros();
		File opened = SD.open(action->path, FILE_WRITE_BEGIN);
		const uint32_t ended = micros();
		record_duration(adapter, started, ended);
		if (!opened)
			return driver_fault(adapter, action->action_id);
		adapter->file = opened;
		adapter->file_open = true;
		return action_event(AIUS_SD_EVENT_OK, action->action_id);
	}

	case AIUS_SD_ACTION_OPEN_APPEND: {
		if (!action_id_valid(action) || !path_payload(action) ||
		    adapter->file_open)
			return protocol_fault(adapter, action);
		const uint32_t started = micros();
		File opened = SD.open(action->path, FILE_WRITE);
		const uint32_t ended = micros();
		record_duration(adapter, started, ended);
		if (!opened)
			return driver_fault(adapter, action->action_id);
		adapter->file = opened;
		adapter->file_open = true;
		return action_event(AIUS_SD_EVENT_OK, action->action_id);
	}

	case AIUS_SD_ACTION_TRUNCATE: {
		if (!action_id_valid(action) || !truncate_payload(action) ||
		    !adapter->file_open)
			return protocol_fault(adapter, action);
		const uint32_t started = micros();
		const bool success = adapter->file.truncate(0u);
		const uint32_t ended = micros();
		record_duration(adapter, started, ended);
		return success
			       ? action_event(AIUS_SD_EVENT_OK, action->action_id)
			       : driver_fault(adapter, action->action_id);
	}

	case AIUS_SD_ACTION_SEEK: {
		if (!action_id_valid(action) || !seek_payload(action) ||
		    !adapter->file_open)
			return protocol_fault(adapter, action);
		const uint32_t started = micros();
		const bool success = adapter->file.seek(action->offset);
		const uint32_t ended = micros();
		record_duration(adapter, started, ended);
		return success
			       ? action_event(AIUS_SD_EVENT_OK, action->action_id)
			       : driver_fault(adapter, action->action_id);
	}

	case AIUS_SD_ACTION_READ: {
		if (!action_id_valid(action) || !transfer_payload(action) ||
		    !adapter->file_open)
			return protocol_fault(adapter, action);
		const uint32_t started = micros();
		const size_t bytes =
			adapter->file.read(action->buffer, action->length);
		const uint32_t ended = micros();
		record_duration(adapter, started, ended);
		if (bytes > action->length)
			return driver_fault(adapter, action->action_id);
		AiusSdEvent event =
			action_event(AIUS_SD_EVENT_BYTES, action->action_id);
		event.bytes = bytes;
		return event;
	}

	case AIUS_SD_ACTION_WRITE: {
		if (!action_id_valid(action) || !transfer_payload(action) ||
		    !adapter->file_open)
			return protocol_fault(adapter, action);
		const uint32_t started = micros();
		const size_t bytes =
			adapter->file.write(action->buffer, action->length);
		const uint32_t ended = micros();
		record_duration(adapter, started, ended);
		if (bytes > action->length)
			return driver_fault(adapter, action->action_id);
		AiusSdEvent event =
			action_event(AIUS_SD_EVENT_BYTES, action->action_id);
		event.bytes = bytes;
		return event;
	}

	case AIUS_SD_ACTION_FILE_SIZE: {
		if (!action_id_valid(action) || !empty_payload(action) ||
		    !adapter->file_open)
			return protocol_fault(adapter, action);
		const uint32_t started = micros();
		const uint64_t size = adapter->file.size();
		const uint32_t ended = micros();
		record_duration(adapter, started, ended);
		AiusSdEvent event =
			action_event(AIUS_SD_EVENT_SIZE, action->action_id);
		event.size = size;
		return event;
	}

	case AIUS_SD_ACTION_EXISTS: {
		if (!action_id_valid(action) || !path_payload(action))
			return protocol_fault(adapter, action);
		const uint32_t started = micros();
		const bool exists = SD.exists(action->path);
		const uint32_t ended = micros();
		record_duration(adapter, started, ended);
		AiusSdEvent event =
			action_event(AIUS_SD_EVENT_BOOL, action->action_id);
		event.value = exists;
		return event;
	}

	case AIUS_SD_ACTION_RENAME: {
		if (!action_id_valid(action) || !rename_payload(action))
			return protocol_fault(adapter, action);
		const uint32_t started = micros();
		const bool success = SD.rename(action->path, action->path_to);
		const uint32_t ended = micros();
		record_duration(adapter, started, ended);
		return success
			       ? action_event(AIUS_SD_EVENT_OK, action->action_id)
			       : driver_fault(adapter, action->action_id);
	}

	case AIUS_SD_ACTION_REMOVE: {
		if (!action_id_valid(action) || !path_payload(action))
			return protocol_fault(adapter, action);
		const uint32_t started = micros();
		const bool success = SD.remove(action->path);
		const uint32_t ended = micros();
		record_duration(adapter, started, ended);
		return success
			       ? action_event(AIUS_SD_EVENT_OK, action->action_id)
			       : driver_fault(adapter, action->action_id);
	}

	case AIUS_SD_ACTION_CLOSE: {
		if (!action_id_valid(action) || !empty_payload(action) ||
		    !adapter->file_open)
			return protocol_fault(adapter, action);
		const uint32_t started = micros();
		adapter->file.close();
		const uint32_t ended = micros();
		record_duration(adapter, started, ended);
		adapter->file_open = false;
		return action_event(AIUS_SD_EVENT_OK, action->action_id);
	}

	case AIUS_SD_ACTION_TOTAL_SIZE: {
		if (!action_id_valid(action) || !empty_payload(action))
			return protocol_fault(adapter, action);
		const uint32_t started = micros();
		const uint64_t size = SD.totalSize();
		const uint32_t ended = micros();
		record_duration(adapter, started, ended);
		AiusSdEvent event =
			action_event(AIUS_SD_EVENT_SIZE, action->action_id);
		event.size = size;
		return event;
	}

	case AIUS_SD_ACTION_USED_SIZE: {
		if (!action_id_valid(action) || !empty_payload(action))
			return protocol_fault(adapter, action);
		const uint32_t started = micros();
		const uint64_t size = SD.usedSize();
		const uint32_t ended = micros();
		record_duration(adapter, started, ended);
		AiusSdEvent event =
			action_event(AIUS_SD_EVENT_SIZE, action->action_id);
		event.size = size;
		return event;
	}

	default:
		return protocol_fault(adapter, action);
	}
}
