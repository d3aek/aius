#include "aius_sd_teensy.h"

#include <limits.h>

namespace {

constexpr size_t kMaximumTransfer = 512u;

AiusSdEvent none_event()
{
	return {};
}

AiusSdEvent action_event(AiusSdEventKind kind, uint64_t action_id)
{
	AiusSdEvent event{};
	event.kind = kind;
	event.action_id = action_id;
	return event;
}

bool empty_payload(const AiusSdAction *action)
{
	return action->path == nullptr && action->path_to == nullptr &&
	       action->buffer == nullptr && action->length == 0u &&
	       action->offset == 0u && action->fault == 0u;
}

bool path_payload(const AiusSdAction *action)
{
	return action->path != nullptr && action->path_to == nullptr &&
	       action->buffer == nullptr && action->length == 0u &&
	       action->offset == 0u && action->fault == 0u;
}

bool rename_payload(const AiusSdAction *action)
{
	return action->path != nullptr && action->path_to != nullptr &&
	       action->buffer == nullptr && action->length == 0u &&
	       action->offset == 0u && action->fault == 0u;
}

bool truncate_payload(const AiusSdAction *action)
{
	return empty_payload(action);
}

bool seek_payload(const AiusSdAction *action)
{
	return action->path == nullptr && action->path_to == nullptr &&
	       action->buffer == nullptr && action->length == 0u &&
	       action->offset <= static_cast<uint64_t>(INT64_MAX) &&
	       action->fault == 0u;
}

bool transfer_payload(const AiusSdAction *action)
{
	return action->path == nullptr && action->path_to == nullptr &&
	       action->buffer != nullptr && action->length != 0u &&
	       action->length <= kMaximumTransfer &&
	       action->offset <= static_cast<uint64_t>(INT64_MAX) &&
	       action->fault == 0u;
}

void record_duration(AiusSdTeensy *adapter, uint32_t started, uint32_t ended)
{
	const uint32_t duration = ended - started;
	adapter->last_duration_us = duration;
	if (duration > adapter->longest_duration_us)
		adapter->longest_duration_us = duration;
}

AiusSdEvent protocol_fault(
	AiusSdTeensy *adapter,
	const AiusSdAction *action)
{
	adapter->last_duration_us = 0u;
	if (adapter->protocol_faults != UINT32_MAX)
		adapter->protocol_faults++;
	adapter->last_fault = AIUS_SD_TEENSY_PROTOCOL_FAULT;
	AiusSdEvent event =
		action_event(AIUS_SD_EVENT_FAULT, action->action_id);
	event.fault = AIUS_SD_TEENSY_PROTOCOL_FAULT;
	return event;
}

AiusSdEvent driver_fault(AiusSdTeensy *adapter, uint64_t action_id)
{
	adapter->last_fault = AIUS_SD_TEENSY_DRIVER_FAULT;
	AiusSdEvent event = action_event(AIUS_SD_EVENT_FAULT, action_id);
	event.fault = AIUS_SD_TEENSY_DRIVER_FAULT;
	return event;
}

bool action_id_valid(const AiusSdAction *action)
{
	return action->action_id != 0u;
}

}

void aius_sd_teensy_init(AiusSdTeensy *adapter)
{
	if (adapter == nullptr)
		return;
	adapter->file_open = false;
	adapter->last_duration_us = 0u;
	adapter->longest_duration_us = 0u;
	adapter->protocol_faults = 0u;
	adapter->last_fault = 0u;
}

AiusSdEvent aius_sd_teensy_execute(
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
		const uint32_t started = micros();
		const bool success = SD.begin(BUILTIN_SDCARD);
		const uint32_t ended = micros();
		record_duration(adapter, started, ended);
		return success
			       ? action_event(AIUS_SD_EVENT_OK, action->action_id)
			       : driver_fault(adapter, action->action_id);
	}

	case AIUS_SD_ACTION_MEDIA_PRESENT: {
		if (!action_id_valid(action) || !empty_payload(action))
			return protocol_fault(adapter, action);
		const uint32_t started = micros();
		const bool present = SD.mediaPresent();
		const uint32_t ended = micros();
		record_duration(adapter, started, ended);
		AiusSdEvent event =
			action_event(AIUS_SD_EVENT_BOOL, action->action_id);
		event.value = present;
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
