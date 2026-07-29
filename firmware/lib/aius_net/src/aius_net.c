#include "aius_net.h"

#include <string.h>

#include "aius_boot.h"

/* Cold code -> FLASH. Nothing here runs on the audio block path (this is the
 * UDP control-frame codec and the network supervisor oracle), so every
 * definition below carries FLASHMEM and lands in FLASH instead of the shared
 * FlexRAM/ITCM pool the audio pipeline needs. FLASHMEM is Teensyduino's
 * __attribute__((section(".flashmem"))) from <avr/pgmspace.h>; TEENSYDUINO is a
 * project-wide compiler -D, so the native host suites fall through to the no-op
 * and this file keeps its portable, Arduino-free compile. The attribute must
 * sit on each DEFINITION: GCC does not merge a `section` attribute from a
 * forward declaration, so a prototype-only FLASHMEM silently leaves the code in
 * ITCM. Enforced post-link by test/itcm-placement/verify_target.py. */
#if defined(TEENSYDUINO)
#include <avr/pgmspace.h>
#endif
#ifndef FLASHMEM
#define FLASHMEM
#endif

#define AIUS_NET_MAGIC0 0x41u
#define AIUS_NET_MAGIC1 0x55u
#define AIUS_NET_MAGIC2 0x4Eu
#define AIUS_NET_MAGIC3 0x31u
#define AIUS_NET_LEGACY_STATUS_LEN 71u
#define AIUS_NET_RECOVERY_STATUS_LEN 99u

static FLASHMEM void aius_net_action_none(AiusNetAction *action)
{
	action->kind = AIUS_NET_ACTION_NONE;
	action->generation = 0;
}

static FLASHMEM void aius_net_action_set(AiusNetAction *action, AiusNetActionKind kind,
	                            uint32_t generation)
{
	action->kind = kind;
	action->generation = generation;
}

static FLASHMEM void aius_net_saturating_increment(uint32_t *value)
{
	if (*value != UINT32_MAX)
		(*value)++;
}

static FLASHMEM bool aius_net_deadline_reached(uint32_t now_ms, uint32_t deadline_ms)
{
	return (int32_t)(now_ms - deadline_ms) >= 0;
}

static FLASHMEM void aius_net_begin_teardown(AiusNetSupervisor *supervisor,
	                                AiusNetFault fault,
	                                AiusNetAction *action)
{
	supervisor->state = AIUS_NET_STATE_TEARDOWN;
	supervisor->deadline_ms = 0;
	supervisor->last_fault = fault;
	aius_net_saturating_increment(&supervisor->restarts);
	aius_net_action_set(action, AIUS_NET_ACTION_TEARDOWN,
	                    supervisor->generation);
}

FLASHMEM uint32_t aius_net_crc32(const uint8_t *data, size_t len)
{
	uint32_t crc = 0xffffffffu;
	for (size_t i = 0; i < len; i++) {
		crc ^= data[i];
		for (unsigned bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
	}
	return ~crc;
}

FLASHMEM size_t aius_net_encode(uint8_t *dst, size_t dst_cap, uint8_t type,
                       uint16_t flags, uint32_t request_id,
                       const uint8_t *payload, uint16_t payload_len)
{
	size_t total = AIUS_NET_HEADER_LEN + (size_t)payload_len + AIUS_NET_CRC_LEN;
	if (dst == NULL || total > dst_cap || payload_len > AIUS_NET_MAX_PAYLOAD ||
	    (payload_len != 0 && payload == NULL))
		return 0;
	dst[0] = AIUS_NET_MAGIC0;
	dst[1] = AIUS_NET_MAGIC1;
	dst[2] = AIUS_NET_MAGIC2;
	dst[3] = AIUS_NET_MAGIC3;
	dst[4] = AIUS_NET_VERSION;
	dst[5] = type;
	aius_net_put_u16(dst + 6, flags);
	aius_net_put_u32(dst + 8, request_id);
	aius_net_put_u16(dst + 12, payload_len);
	aius_net_put_u16(dst + 14, 0);
	if (payload_len != 0)
		memcpy(dst + AIUS_NET_HEADER_LEN, payload, payload_len);
	aius_net_put_u32(dst + total - AIUS_NET_CRC_LEN,
	                 aius_net_crc32(dst, total - AIUS_NET_CRC_LEN));
	return total;
}

FLASHMEM size_t aius_net_status_encode(
	uint8_t *output,
	size_t capacity,
	const uint8_t legacy_status[AIUS_NET_LEGACY_STATUS_LEN],
	const AiusRecoveryStatus *recovery)
{
	uint8_t encoded[AIUS_NET_RECOVERY_STATUS_LEN];

	if (output == NULL || legacy_status == NULL || recovery == NULL ||
	    capacity < AIUS_NET_RECOVERY_STATUS_LEN ||
	    legacy_status[0] != AIUS_NET_VERSION ||
	    legacy_status[1] > 1u ||
	    recovery->network_state < AIUS_NET_STATE_STACK_START ||
	    recovery->network_state > AIUS_NET_STATE_RECOVERY_SAFE ||
	    recovery->network_last_fault < AIUS_NET_FAULT_NONE ||
	    recovery->network_last_fault > AIUS_NET_FAULT_DRIVER_PROTOCOL ||
	    recovery->last_checkpoint > AIUS_BOOT_CHECKPOINT_EPOCH_COMPLETE)
		return 0;

	memcpy(encoded, legacy_status, AIUS_NET_LEGACY_STATUS_LEN);
	encoded[71] = (uint8_t)recovery->network_state;
	encoded[72] = recovery->recovery_safe ? 1u : 0u;
	encoded[73] = (uint8_t)recovery->network_last_fault;
	encoded[74] = (uint8_t)recovery->last_checkpoint;
	aius_net_put_u32(encoded + 75, recovery->network_generation);
	aius_net_put_u32(encoded + 79, recovery->boot_generation);
	aius_net_put_u32(encoded + 83, recovery->reset_source);
	encoded[87] = recovery->watchdog_early_resets;
	encoded[88] = 0;
	encoded[89] = 0;
	encoded[90] = 0;
	aius_net_put_u32(encoded + 91, recovery->loop_epoch);
	aius_net_put_u32(encoded + 95, recovery->network_restarts);
	memcpy(output, encoded, sizeof(encoded));
	return sizeof(encoded);
}

FLASHMEM AiusNetError aius_net_decode(const uint8_t *src, size_t len,
                             AiusNetFrame *out)
{
	if (src == NULL || out == NULL)
		return AIUS_NET_ERR_SHORT;
	if (len < AIUS_NET_HEADER_LEN + AIUS_NET_CRC_LEN)
		return AIUS_NET_ERR_SHORT;
	if (src[0] != AIUS_NET_MAGIC0 || src[1] != AIUS_NET_MAGIC1 ||
	    src[2] != AIUS_NET_MAGIC2 || src[3] != AIUS_NET_MAGIC3)
		return AIUS_NET_ERR_MAGIC;
	if (src[4] != AIUS_NET_VERSION)
		return AIUS_NET_ERR_VERSION;
	uint16_t payload_len = aius_net_get_u16(src + 12);
	if (payload_len > AIUS_NET_MAX_PAYLOAD)
		return AIUS_NET_ERR_LENGTH;
	if (aius_net_get_u16(src + 14) != 0)
		return AIUS_NET_ERR_RESERVED;
	size_t total = AIUS_NET_HEADER_LEN + (size_t)payload_len + AIUS_NET_CRC_LEN;
	if (total != len)
		return AIUS_NET_ERR_LENGTH;
	if (aius_net_get_u32(src + total - AIUS_NET_CRC_LEN) !=
	    aius_net_crc32(src, total - AIUS_NET_CRC_LEN))
		return AIUS_NET_ERR_CRC;
	if (src[5] == 0)
		return AIUS_NET_ERR_TYPE;
	out->version = src[4];
	out->type = src[5];
	out->flags = aius_net_get_u16(src + 6);
	out->request_id = aius_net_get_u32(src + 8);
	out->payload = src + AIUS_NET_HEADER_LEN;
	out->payload_len = payload_len;
	return AIUS_NET_OK;
}

FLASHMEM void aius_net_supervisor_init(AiusNetSupervisor *supervisor, uint32_t now_ms,
	                          bool recovery_safe,
	                          AiusNetAction *initial_action)
{
	(void)now_ms;
	if (supervisor == NULL || initial_action == NULL)
		return;

	supervisor->state = recovery_safe ? AIUS_NET_STATE_RECOVERY_SAFE
	                                  : AIUS_NET_STATE_STACK_START;
	supervisor->generation = recovery_safe ? 0u : 1u;
	supervisor->deadline_ms = 0;
	supervisor->restarts = 0;
	supervisor->ignored_events = 0;
	supervisor->cleanup_faults = 0;
	supervisor->last_fault = AIUS_NET_FAULT_NONE;
	aius_net_action_none(initial_action);
	if (!recovery_safe)
		aius_net_action_set(initial_action, AIUS_NET_ACTION_START_STACK,
		                    supervisor->generation);
}

FLASHMEM void aius_net_supervisor_step(AiusNetSupervisor *supervisor, uint32_t now_ms,
	                          AiusNetEvent event, AiusNetAction *action)
{
	if (supervisor == NULL || action == NULL)
		return;

	aius_net_action_none(action);

	if (event.kind != AIUS_NET_EVENT_NONE) {
		if (event.generation != supervisor->generation) {
			aius_net_saturating_increment(&supervisor->ignored_events);
		} else {
			switch (supervisor->state) {
			case AIUS_NET_STATE_STACK_START:
				if (event.kind == AIUS_NET_EVENT_STACK_STARTED) {
					supervisor->state = AIUS_NET_STATE_DHCP_WAIT;
					supervisor->deadline_ms =
						now_ms + AIUS_NET_DHCP_DEADLINE_MS;
					aius_net_action_set(
						action, AIUS_NET_ACTION_START_DHCP,
						supervisor->generation);
					return;
				}
				if (event.kind == AIUS_NET_EVENT_RESOURCE_FAULT) {
					aius_net_begin_teardown(
						supervisor, event.fault, action);
					return;
				}
				aius_net_saturating_increment(
					&supervisor->ignored_events);
				break;
			case AIUS_NET_STATE_DHCP_WAIT:
				if (event.kind == AIUS_NET_EVENT_LEASE_ACQUIRED) {
					supervisor->state = AIUS_NET_STATE_ONLINE;
					supervisor->deadline_ms = 0;
					aius_net_action_set(
						action,
						AIUS_NET_ACTION_PUBLISH_ONLINE,
						supervisor->generation);
					return;
				}
				if (event.kind == AIUS_NET_EVENT_RESOURCE_FAULT) {
					aius_net_begin_teardown(
						supervisor, event.fault, action);
					return;
				}
				aius_net_saturating_increment(
					&supervisor->ignored_events);
				break;
			case AIUS_NET_STATE_ONLINE:
				if (event.kind == AIUS_NET_EVENT_RESOURCE_FAULT) {
					aius_net_begin_teardown(
						supervisor, event.fault, action);
					return;
				}
				aius_net_saturating_increment(
					&supervisor->ignored_events);
				break;
			case AIUS_NET_STATE_TEARDOWN:
				if (event.kind == AIUS_NET_EVENT_TEARDOWN_DONE ||
				    event.kind == AIUS_NET_EVENT_TEARDOWN_FAULT) {
					if (event.kind ==
					    AIUS_NET_EVENT_TEARDOWN_FAULT)
						aius_net_saturating_increment(
							&supervisor
								 ->cleanup_faults);
					supervisor->state =
						AIUS_NET_STATE_BACKOFF;
					supervisor->deadline_ms =
						now_ms + AIUS_NET_BACKOFF_MS;
					return;
				}
				aius_net_saturating_increment(
					&supervisor->ignored_events);
				break;
			case AIUS_NET_STATE_BACKOFF:
			case AIUS_NET_STATE_RECOVERY_SAFE:
			default:
				aius_net_saturating_increment(
					&supervisor->ignored_events);
				break;
			}
		}
	}

	if (supervisor->state == AIUS_NET_STATE_DHCP_WAIT &&
	    aius_net_deadline_reached(now_ms, supervisor->deadline_ms)) {
		aius_net_begin_teardown(supervisor,
		                        AIUS_NET_FAULT_DHCP_TIMEOUT, action);
		return;
	}

	if (supervisor->state == AIUS_NET_STATE_BACKOFF &&
	    aius_net_deadline_reached(now_ms, supervisor->deadline_ms)) {
		supervisor->generation++;
		if (supervisor->generation == 0)
			supervisor->generation = 1;
		supervisor->state = AIUS_NET_STATE_STACK_START;
		supervisor->deadline_ms = 0;
		aius_net_action_set(action, AIUS_NET_ACTION_START_STACK,
		                    supervisor->generation);
	}
}
