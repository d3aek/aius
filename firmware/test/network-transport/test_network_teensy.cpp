#include "aius_network_teensy.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace {

enum Operation {
	OP_CALLBACKS_INSTALL,
	OP_CALLBACKS_CLEAR,
	OP_STACK_START,
	OP_DHCP_START,
	OP_LISTENER_OPEN,
	OP_UDP_OPEN,
	OP_LISTENER_CLOSE,
	OP_UDP_CLOSE,
	OP_STACK_CLOSE,
	OP_LINK_STATUS,
	OP_ACCEPT,
	OP_READ,
	OP_WRITE,
	OP_CLOSE,
	OP_UDP_RECEIVE,
	OP_UDP_SEND,
	OP_COUNT
};

struct Fake {
	Operation log[64];
	size_t log_count;
	bool succeeds[OP_COUNT];
	AiusIoEvent io[6];
	AiusNetworkTeensyLink link;
	AiusNetworkTeensy *callback_adapter;
	AiusNetworkTeensy *cleared_adapter;
	uint32_t callback_generation;
	bool queue_was_clear_at_first_close;
	const uint8_t *last_const_buffer;
	uint8_t *last_mutable_buffer;
	size_t last_length;
};

Fake g_fake;

void record(Operation operation)
{
	assert(g_fake.log_count < sizeof(g_fake.log) / sizeof(g_fake.log[0]));
	g_fake.log[g_fake.log_count++] = operation;
}

void callbacks_install(AiusNetworkTeensy *adapter, uint32_t generation)
{
	record(OP_CALLBACKS_INSTALL);
	g_fake.callback_adapter = adapter;
	g_fake.callback_generation = generation;
}

void callbacks_clear()
{
	record(OP_CALLBACKS_CLEAR);
	g_fake.cleared_adapter = g_fake.callback_adapter;
	g_fake.callback_adapter = nullptr;
}

bool operation(Operation operation)
{
	record(operation);
	if (operation == OP_LISTENER_CLOSE && g_fake.log_count >= 2u) {
		g_fake.queue_was_clear_at_first_close =
			g_fake.callback_adapter == nullptr &&
			g_fake.cleared_adapter != nullptr &&
			g_fake.cleared_adapter->pending_count == 0u;
	}
	return g_fake.succeeds[operation];
}

/* The diagnostic readers are a separate fake, matching the separate table the
 * adapter exposes them through. Every other case in this file leaves that table
 * uninstalled, which is what proves the measurements are optional: the whole
 * lifecycle, callback, deadline and I/O suite runs with no diagnostic reader
 * wired at all.
 */
struct DiagFake {
	bool netif_initialized;
	AiusNetworkTeensyLink link_live;
	uint8_t dhcp_state;
	bool frames_ok;
	uint32_t tx;
	uint32_t rx;
	uint32_t uptime_ms;
	/* Wall time the fake stack_start() consumes, so the adapter's
	 * measurement of a blocking beginNoWait() has something to measure. */
	uint32_t stack_start_cost_ms;
	unsigned netif_initialized_calls;
	unsigned link_live_calls;
	unsigned dhcp_state_calls;
	unsigned frames_calls;
	unsigned uptime_calls;
};

DiagFake g_diag;

bool stack_start()
{
	const bool started = operation(OP_STACK_START);
	g_diag.uptime_ms += g_diag.stack_start_cost_ms;
	return started;
}

bool diag_netif_initialized()
{
	g_diag.netif_initialized_calls++;
	return g_diag.netif_initialized;
}

AiusNetworkTeensyLink diag_link_live()
{
	g_diag.link_live_calls++;
	return g_diag.link_live;
}

uint8_t diag_dhcp_state()
{
	g_diag.dhcp_state_calls++;
	return g_diag.dhcp_state;
}

bool diag_frames(uint32_t *tx, uint32_t *rx)
{
	g_diag.frames_calls++;
	*tx = g_diag.tx;
	*rx = g_diag.rx;
	return g_diag.frames_ok;
}

uint32_t diag_uptime_ms()
{
	g_diag.uptime_calls++;
	return g_diag.uptime_ms;
}

bool dhcp_start()
{
	return operation(OP_DHCP_START);
}

bool listener_open()
{
	return operation(OP_LISTENER_OPEN);
}

bool udp_open()
{
	return operation(OP_UDP_OPEN);
}

bool listener_close()
{
	return operation(OP_LISTENER_CLOSE);
}

bool udp_close()
{
	return operation(OP_UDP_CLOSE);
}

bool stack_close()
{
	return operation(OP_STACK_CLOSE);
}

AiusNetworkTeensyLink link_status()
{
	record(OP_LINK_STATUS);
	return g_fake.link;
}

AiusIoEvent accept_once()
{
	record(OP_ACCEPT);
	return g_fake.io[0];
}

AiusIoEvent read_once(uint8_t *buffer, size_t capacity)
{
	record(OP_READ);
	g_fake.last_mutable_buffer = buffer;
	g_fake.last_length = capacity;
	return g_fake.io[1];
}

AiusIoEvent write_once(const uint8_t *buffer, size_t length)
{
	record(OP_WRITE);
	g_fake.last_const_buffer = buffer;
	g_fake.last_length = length;
	return g_fake.io[2];
}

AiusIoEvent udp_receive_once(uint8_t *buffer, size_t capacity)
{
	record(OP_UDP_RECEIVE);
	g_fake.last_mutable_buffer = buffer;
	g_fake.last_length = capacity;
	return g_fake.io[4];
}

AiusIoEvent udp_send_once(const uint8_t *buffer, size_t length)
{
	record(OP_UDP_SEND);
	g_fake.last_const_buffer = buffer;
	g_fake.last_length = length;
	return g_fake.io[5];
}

AiusIoEvent close_once()
{
	record(OP_CLOSE);
	return g_fake.io[3];
}

uint32_t g_stack_polls;
uint32_t g_stack_poll_last_ms;

void stack_poll(uint32_t now_ms)
{
	g_stack_polls++;
	g_stack_poll_last_ms = now_ms;
}

const AiusNetworkTeensyPrimitives kPrimitives = {
	callbacks_install,
	callbacks_clear,
	stack_start,
	dhcp_start,
	listener_open,
	udp_open,
	listener_close,
	udp_close,
	stack_close,
	link_status,
	accept_once,
	read_once,
	write_once,
	close_once,
	udp_receive_once,
	udp_send_once,
	stack_poll,
};

const AiusNetworkTeensyDiagPrimitives kDiagPrimitives = {
	diag_netif_initialized,
	diag_link_live,
	diag_dhcp_state,
	diag_frames,
	diag_uptime_ms,
};

AiusNetAction action(AiusNetActionKind kind, uint32_t generation = 7u)
{
	return {kind, generation};
}

void reset_fake()
{
	memset(&g_fake, 0, sizeof(g_fake));
	memset(&g_diag, 0, sizeof(g_diag));
	for (size_t index = 0u; index < OP_COUNT; index++) {
		g_fake.succeeds[index] = true;
	}
	g_fake.link = AIUS_NETWORK_TEENSY_LINK_UP;
	g_fake.io[0] = {AIUS_IO_WOULD_BLOCK, 0u, 0u};
	g_fake.io[1] = {AIUS_IO_WOULD_BLOCK, 0u, 0u};
	g_fake.io[2] = {AIUS_IO_WOULD_BLOCK, 0u, 0u};
	g_fake.io[3] = {AIUS_IO_WOULD_BLOCK, 0u, 0u};
	g_fake.io[4] = {AIUS_IO_WOULD_BLOCK, 0u, 0u};
	g_fake.io[5] = {AIUS_IO_WOULD_BLOCK, 0u, 0u};
	aius_network_teensy_test_set_primitives(&kPrimitives);
}

void reset_adapter(AiusNetworkTeensy *adapter)
{
	reset_fake();
	memset(adapter, 0xa5, sizeof(*adapter));
	aius_network_teensy_init(adapter);
	assert(adapter->generation == 0u);
	assert(adapter->active_action.kind == AIUS_NET_ACTION_NONE);
	assert(!adapter->stack_started);
	assert(!adapter->dhcp_started);
	assert(!adapter->online);
	assert(!adapter->listener_open);
	assert(!adapter->udp_open);
	assert(adapter->pending_count == 0u);
}

void expect_event(const AiusNetEvent &event, AiusNetEventKind kind,
	              uint32_t generation, AiusNetFault fault)
{
	assert(event.kind == kind);
	assert(event.generation == generation);
	assert(event.fault == fault);
}

void make_stack_ready(AiusNetworkTeensy *adapter)
{
	AiusNetEvent event = {};
	aius_network_teensy_apply(adapter, 1u,
	                         action(AIUS_NET_ACTION_START_STACK), &event);
	expect_event(event, AIUS_NET_EVENT_STACK_STARTED, 7u,
	             AIUS_NET_FAULT_NONE);
	aius_network_teensy_apply(adapter, 2u,
	                         action(AIUS_NET_ACTION_START_DHCP), &event);
	expect_event(event, AIUS_NET_EVENT_NONE, 0u, AIUS_NET_FAULT_NONE);
}

void make_online(AiusNetworkTeensy *adapter)
{
	make_stack_ready(adapter);
	AiusNetEvent event = {};
	aius_network_teensy_apply(adapter, 3u,
	                         action(AIUS_NET_ACTION_PUBLISH_ONLINE), &event);
	expect_event(event, AIUS_NET_EVENT_NONE, 0u, AIUS_NET_FAULT_NONE);
	assert(adapter->listener_open);
	assert(!adapter->udp_open);
	assert(!adapter->online);
	aius_network_teensy_apply(adapter, 4u,
	                         action(AIUS_NET_ACTION_NONE, 0u), &event);
	expect_event(event, AIUS_NET_EVENT_NONE, 0u, AIUS_NET_FAULT_NONE);
	assert(adapter->online);
	assert(adapter->udp_open);
}

void test_init_and_action_matrix()
{
	AiusNetworkTeensy adapter;
	reset_adapter(&adapter);
	AiusNetEvent event = {AIUS_NET_EVENT_RESOURCE_FAULT, 99u,
	                      AIUS_NET_FAULT_SOCKET};

	aius_network_teensy_apply(
		nullptr, 0u, action(AIUS_NET_ACTION_START_STACK), &event);
	expect_event(event, AIUS_NET_EVENT_NONE, 0u, AIUS_NET_FAULT_NONE);
	event = {AIUS_NET_EVENT_RESOURCE_FAULT, 99u, AIUS_NET_FAULT_SOCKET};
	aius_network_teensy_poll(nullptr, 0u, &event);
	expect_event(event, AIUS_NET_EVENT_NONE, 0u, AIUS_NET_FAULT_NONE);

	aius_network_teensy_apply(&adapter, 0u,
	                         action(AIUS_NET_ACTION_NONE, 0u), &event);
	expect_event(event, AIUS_NET_EVENT_NONE, 0u, AIUS_NET_FAULT_NONE);
	assert(g_fake.log_count == 0u);

	/* The link is up in the fake, so the stack-start link read is the
	 * observation that releases STACK_STARTED in the same call. */
	aius_network_teensy_apply(
		&adapter, 1u, action(AIUS_NET_ACTION_START_STACK), &event);
	expect_event(event, AIUS_NET_EVENT_STACK_STARTED, 7u,
	             AIUS_NET_FAULT_NONE);
	assert(adapter.stack_started);
	assert(adapter.link_up_reported);
	assert(adapter.stack_started_ms == 1u);
	assert(g_fake.log_count == 3u);
	assert(g_fake.log[0] == OP_CALLBACKS_INSTALL);
	assert(g_fake.log[1] == OP_STACK_START);
	assert(g_fake.log[2] == OP_LINK_STATUS);

	aius_network_teensy_apply(
		&adapter, 2u, action(AIUS_NET_ACTION_START_DHCP), &event);
	expect_event(event, AIUS_NET_EVENT_NONE, 0u, AIUS_NET_FAULT_NONE);
	assert(adapter.dhcp_started);
	assert(g_fake.log[g_fake.log_count - 1u] == OP_DHCP_START);

	aius_network_teensy_apply(
		&adapter, 3u, action(AIUS_NET_ACTION_PUBLISH_ONLINE), &event);
	assert(adapter.active_action.kind == AIUS_NET_ACTION_PUBLISH_ONLINE);
	assert(adapter.listener_open);
	assert(!adapter.udp_open);
	assert(!adapter.online);
	assert(g_fake.log[g_fake.log_count - 1u] == OP_LISTENER_OPEN);

	const size_t before = g_fake.log_count;
	aius_network_teensy_apply(
		&adapter, 4u, action(AIUS_NET_ACTION_PUBLISH_ONLINE), &event);
	assert(g_fake.log_count == before + 1u);
	assert(g_fake.log[g_fake.log_count - 1u] == OP_UDP_OPEN);
	assert(adapter.active_action.kind == AIUS_NET_ACTION_NONE);
	assert(adapter.online);
}

void test_action_resource_fault_matrix()
{
	struct FaultCase {
		Operation operation;
		AiusNetFault fault;
	};
	const FaultCase cases[] = {
		{OP_STACK_START, AIUS_NET_FAULT_STACK_START},
		{OP_DHCP_START, AIUS_NET_FAULT_DHCP_START},
		{OP_LISTENER_OPEN, AIUS_NET_FAULT_LISTENER},
		{OP_UDP_OPEN, AIUS_NET_FAULT_UDP},
	};

	for (const FaultCase &fault_case : cases) {
		AiusNetworkTeensy adapter;
		reset_adapter(&adapter);
		g_fake.succeeds[fault_case.operation] = false;
		AiusNetEvent event = {};
		if (fault_case.operation != OP_STACK_START) {
			aius_network_teensy_apply(
				&adapter, 1u, action(AIUS_NET_ACTION_START_STACK), &event);
		}
		if (fault_case.operation == OP_LISTENER_OPEN ||
		    fault_case.operation == OP_UDP_OPEN) {
			aius_network_teensy_apply(
				&adapter, 2u, action(AIUS_NET_ACTION_START_DHCP), &event);
		}
		if (fault_case.operation == OP_UDP_OPEN) {
			aius_network_teensy_apply(
				&adapter, 3u, action(AIUS_NET_ACTION_PUBLISH_ONLINE), &event);
			aius_network_teensy_apply(
				&adapter, 4u, action(AIUS_NET_ACTION_NONE, 0u), &event);
		} else {
			const AiusNetActionKind kind =
				fault_case.operation == OP_STACK_START
					? AIUS_NET_ACTION_START_STACK
				: fault_case.operation == OP_DHCP_START
					? AIUS_NET_ACTION_START_DHCP
					: AIUS_NET_ACTION_PUBLISH_ONLINE;
			aius_network_teensy_apply(&adapter, 4u, action(kind), &event);
		}
		expect_event(event, AIUS_NET_EVENT_RESOURCE_FAULT, 7u,
		             fault_case.fault);
	}
}

void test_action_protocol_and_preemption()
{
	AiusNetworkTeensy adapter;
	reset_adapter(&adapter);
	make_stack_ready(&adapter);
	AiusNetEvent event = {};
	aius_network_teensy_apply(
		&adapter, 1u, action(AIUS_NET_ACTION_PUBLISH_ONLINE), &event);
	aius_network_teensy_apply(
		&adapter, 2u, action(AIUS_NET_ACTION_START_DHCP), &event);
	expect_event(event, AIUS_NET_EVENT_RESOURCE_FAULT, 7u,
	             AIUS_NET_FAULT_DRIVER_PROTOCOL);
	assert(adapter.active_action.kind == AIUS_NET_ACTION_NONE);

	reset_adapter(&adapter);
	make_stack_ready(&adapter);
	aius_network_teensy_apply(
		&adapter, 1u, action(AIUS_NET_ACTION_PUBLISH_ONLINE), &event);
	aius_network_teensy_apply(
		&adapter, 2u, action(AIUS_NET_ACTION_TEARDOWN), &event);
	assert(adapter.active_action.kind == AIUS_NET_ACTION_TEARDOWN);
	assert(g_fake.log[g_fake.log_count - 1u] == OP_LISTENER_CLOSE);
	assert(g_fake.queue_was_clear_at_first_close);
	assert(adapter.pending_count == 0u);
}

void test_teardown_order_and_fault_matrix()
{
	const Operation close_operations[] = {
		OP_LISTENER_CLOSE,
		OP_UDP_CLOSE,
		OP_STACK_CLOSE,
	};
	for (Operation failed_operation : close_operations) {
		AiusNetworkTeensy adapter;
		reset_adapter(&adapter);
		make_online(&adapter);
		g_fake.succeeds[failed_operation] = false;
		g_fake.log_count = 0u;
		aius_network_teensy_test_callback(
			&adapter, 7u, AIUS_NET_EVENT_LEASE_ACQUIRED,
			AIUS_NET_FAULT_NONE);

		AiusNetEvent event = {};
		aius_network_teensy_apply(
			&adapter, 10u, action(AIUS_NET_ACTION_TEARDOWN), &event);
		expect_event(event, AIUS_NET_EVENT_NONE, 0u, AIUS_NET_FAULT_NONE);
		assert(g_fake.log_count == 2u);
		assert(g_fake.log[0] == OP_CALLBACKS_CLEAR);
		assert(g_fake.log[1] == OP_LISTENER_CLOSE);
		assert(adapter.pending_count == 0u);

		aius_network_teensy_apply(
			&adapter, 11u, action(AIUS_NET_ACTION_NONE, 0u), &event);
		expect_event(event, AIUS_NET_EVENT_NONE, 0u, AIUS_NET_FAULT_NONE);
		assert(g_fake.log[g_fake.log_count - 1u] == OP_UDP_CLOSE);

		aius_network_teensy_apply(
			&adapter, 12u, action(AIUS_NET_ACTION_NONE, 0u), &event);
		expect_event(event, AIUS_NET_EVENT_TEARDOWN_FAULT, 7u,
		             AIUS_NET_FAULT_TEARDOWN);
		assert(g_fake.log[g_fake.log_count - 1u] == OP_STACK_CLOSE);
		assert(!adapter.stack_started);
		assert(!adapter.dhcp_started);
		assert(!adapter.listener_open);
		assert(!adapter.udp_open);
		assert(!adapter.online);
	}

	AiusNetworkTeensy clean;
	reset_adapter(&clean);
	aius_network_teensy_test_callback(
		&clean, 7u, AIUS_NET_EVENT_LEASE_ACQUIRED, AIUS_NET_FAULT_NONE);
	assert(clean.pending_count == 1u);
	AiusNetEvent event = {};
	aius_network_teensy_apply(
		&clean, 0u, action(AIUS_NET_ACTION_TEARDOWN), &event);
	expect_event(event, AIUS_NET_EVENT_TEARDOWN_DONE, 7u,
	             AIUS_NET_FAULT_NONE);
	assert(clean.pending_count == 0u);
	assert(g_fake.log_count == 0u);
}

void test_identical_teardown_reapplication_preserves_fault()
{
	AiusNetworkTeensy adapter;
	reset_adapter(&adapter);
	make_online(&adapter);
	g_fake.succeeds[OP_LISTENER_CLOSE] = false;
	g_fake.log_count = 0u;
	const AiusNetAction teardown =
		action(AIUS_NET_ACTION_TEARDOWN);
	AiusNetEvent event = {};

	aius_network_teensy_apply(&adapter, 10u, teardown, &event);
	expect_event(event, AIUS_NET_EVENT_NONE, 0u, AIUS_NET_FAULT_NONE);
	assert(adapter.active_action.kind == AIUS_NET_ACTION_TEARDOWN);
	assert(adapter.action_step == 2u);
	assert(adapter.action_fault);
	assert(g_fake.log_count == 2u);
	assert(g_fake.log[0] == OP_CALLBACKS_CLEAR);
	assert(g_fake.log[1] == OP_LISTENER_CLOSE);

	aius_network_teensy_apply(&adapter, 11u, teardown, &event);
	expect_event(event, AIUS_NET_EVENT_NONE, 0u, AIUS_NET_FAULT_NONE);
	assert(adapter.active_action.kind == AIUS_NET_ACTION_TEARDOWN);
	assert(adapter.action_step == 3u);
	assert(adapter.action_fault);
	assert(g_fake.log_count == 3u);
	assert(g_fake.log[2] == OP_UDP_CLOSE);

	aius_network_teensy_apply(&adapter, 12u, teardown, &event);
	expect_event(event, AIUS_NET_EVENT_TEARDOWN_FAULT, 7u,
	             AIUS_NET_FAULT_TEARDOWN);
	assert(adapter.active_action.kind == AIUS_NET_ACTION_NONE);
	assert(g_fake.log_count == 4u);
	assert(g_fake.log[3] == OP_STACK_CLOSE);
}

void test_callback_fifo_stale_duplicate_overflow_and_health()
{
	AiusNetworkTeensy adapter;
	reset_adapter(&adapter);
	AiusNetEvent event = {};
	aius_network_teensy_apply(
		&adapter, 0u, action(AIUS_NET_ACTION_START_STACK), &event);
	g_fake.log_count = 0u;

	/* The stack-start read already observed link-up for generation 7, so
	 * both late link callbacks are the same event arriving again: they are
	 * dropped rather than delivered as duplicate STACK_STARTEDs, while a
	 * stale-generation callback is still discarded and counted. */
	assert(adapter.link_up_reported);
	aius_network_teensy_test_callback(
		&adapter, 6u, AIUS_NET_EVENT_LEASE_ACQUIRED, AIUS_NET_FAULT_NONE);
	aius_network_teensy_test_callback(
		&adapter, 7u, AIUS_NET_EVENT_STACK_STARTED, AIUS_NET_FAULT_NONE);
	aius_network_teensy_test_callback(
		&adapter, 7u, AIUS_NET_EVENT_STACK_STARTED, AIUS_NET_FAULT_NONE);
	const uint32_t polls_before = g_stack_polls;
	aius_network_teensy_poll(&adapter, 1u, &event);
	expect_event(event, AIUS_NET_EVENT_NONE, 0u, AIUS_NET_FAULT_NONE);
	/* The optional stack_poll primitive runs once per adapter poll, with
	 * the caller's clock. */
	assert(g_stack_polls == polls_before + 1u);
	assert(g_stack_poll_last_ms == 1u);
	assert(aius_network_teensy_test_stale_callbacks() == 1u);
	assert(adapter.pending_count == 0u);
	assert(g_fake.log_count == 1u);
	assert(g_fake.log[0] == OP_LINK_STATUS);

	/* A current-generation lease still comes through the same FIFO. */
	aius_network_teensy_test_callback(
		&adapter, 7u, AIUS_NET_EVENT_LEASE_ACQUIRED, AIUS_NET_FAULT_NONE);
	aius_network_teensy_poll(&adapter, 2u, &event);
	expect_event(event, AIUS_NET_EVENT_LEASE_ACQUIRED, 7u,
	             AIUS_NET_FAULT_NONE);
	assert(g_fake.log_count == 1u);

	aius_network_teensy_poll(&adapter, 3u, &event);
	expect_event(event, AIUS_NET_EVENT_NONE, 0u, AIUS_NET_FAULT_NONE);
	assert(g_fake.log_count == 2u);
	assert(g_fake.log[1] == OP_LINK_STATUS);

	g_fake.link = AIUS_NETWORK_TEENSY_LINK_DOWN;
	aius_network_teensy_poll(&adapter, 4u, &event);
	expect_event(event, AIUS_NET_EVENT_RESOURCE_FAULT, 7u,
	             AIUS_NET_FAULT_LINK_LOST);
	assert(g_fake.log_count == 3u);

	for (size_t index = 0u; index < 5u; index++) {
		aius_network_teensy_test_callback(
			&adapter, 7u, AIUS_NET_EVENT_LEASE_ACQUIRED,
			AIUS_NET_FAULT_NONE);
	}
	assert(adapter.pending_count == 0u);
	assert(adapter.callback_overflow);
	const size_t before = g_fake.log_count;
	aius_network_teensy_poll(&adapter, 5u, &event);
	expect_event(event, AIUS_NET_EVENT_RESOURCE_FAULT, 7u,
	             AIUS_NET_FAULT_CALLBACK_OVERFLOW);
	assert(!adapter.callback_overflow);
	assert(g_fake.log_count == before);
}

/* Defined below with the other lifecycle helpers. */
void drive_teardown(AiusNetworkTeensy *adapter, uint32_t generation);

/* The link gate. STACK_STARTED is the supervisor's cue to start DHCP, so it may
 * only be emitted once the link has actually been observed up -- Ethernet's
 * beginNoWait() returns before the PHY has negotiated, and a DISCOVER sent onto
 * a dead link costs the whole 5 s DHCP deadline. The three observers (the
 * stack-start read, the link callback, the poll read) must produce exactly one
 * STACK_STARTED per generation between them, and a link that never comes up
 * must still make bounded progress.
 */
void test_link_gate_before_dhcp()
{
	AiusNetworkTeensy adapter;
	AiusNetEvent event = {};

	/* 1. Link already up when the stack starts. NativeEthernet reports link
	 * changes, so no callback will ever arrive for a link that was already
	 * up: the stack-start read is the observation, and it happens once.
	 */
	reset_adapter(&adapter);
	g_fake.link = AIUS_NETWORK_TEENSY_LINK_UP;
	aius_network_teensy_apply(&adapter, 100u,
	                         action(AIUS_NET_ACTION_START_STACK), &event);
	expect_event(event, AIUS_NET_EVENT_STACK_STARTED, 7u,
	             AIUS_NET_FAULT_NONE);
	assert(adapter.link_up_reported);
	for (uint32_t tick = 0u; tick < 4u; tick++) {
		aius_network_teensy_poll(&adapter, 101u + tick, &event);
		expect_event(event, AIUS_NET_EVENT_NONE, 0u,
		             AIUS_NET_FAULT_NONE);
	}
	/* Well past the link deadline, an up link is never a fault. */
	aius_network_teensy_poll(
		&adapter, 100u + AIUS_NETWORK_TEENSY_LINK_DEADLINE_MS + 500u,
		&event);
	expect_event(event, AIUS_NET_EVENT_NONE, 0u, AIUS_NET_FAULT_NONE);
	assert(!adapter.dhcp_started);

	/* 2. Link down at stack start, up later. Nothing is emitted while the
	 * link is down or merely unknown, so the supervisor stays in
	 * STACK_START and never issues START_DHCP; the first UP read releases
	 * STACK_STARTED, and only once.
	 */
	reset_adapter(&adapter);
	g_fake.link = AIUS_NETWORK_TEENSY_LINK_DOWN;
	aius_network_teensy_apply(&adapter, 1000u,
	                         action(AIUS_NET_ACTION_START_STACK), &event);
	expect_event(event, AIUS_NET_EVENT_NONE, 0u, AIUS_NET_FAULT_NONE);
	assert(adapter.stack_started);
	assert(!adapter.link_up_reported);
	aius_network_teensy_poll(&adapter, 1100u, &event);
	expect_event(event, AIUS_NET_EVENT_NONE, 0u, AIUS_NET_FAULT_NONE);
	g_fake.link = AIUS_NETWORK_TEENSY_LINK_UNKNOWN;
	aius_network_teensy_poll(&adapter, 1200u, &event);
	expect_event(event, AIUS_NET_EVENT_NONE, 0u, AIUS_NET_FAULT_NONE);
	assert(!adapter.link_up_reported);
	g_fake.link = AIUS_NETWORK_TEENSY_LINK_UP;
	aius_network_teensy_poll(&adapter, 1300u, &event);
	expect_event(event, AIUS_NET_EVENT_STACK_STARTED, 7u,
	             AIUS_NET_FAULT_NONE);
	assert(adapter.link_up_reported);
	aius_network_teensy_poll(&adapter, 1400u, &event);
	expect_event(event, AIUS_NET_EVENT_NONE, 0u, AIUS_NET_FAULT_NONE);

	/* 3. Both observers see the same link-up. The callback is queued while
	 * the poll read is what first observes UP; exactly one STACK_STARTED
	 * leaves the adapter either way.
	 */
	reset_adapter(&adapter);
	g_fake.link = AIUS_NETWORK_TEENSY_LINK_DOWN;
	aius_network_teensy_apply(&adapter, 2000u,
	                         action(AIUS_NET_ACTION_START_STACK), &event);
	expect_event(event, AIUS_NET_EVENT_NONE, 0u, AIUS_NET_FAULT_NONE);
	aius_network_teensy_test_callback(&adapter, 7u,
	                                  AIUS_NET_EVENT_STACK_STARTED,
	                                  AIUS_NET_FAULT_NONE);
	g_fake.link = AIUS_NETWORK_TEENSY_LINK_UP;
	aius_network_teensy_poll(&adapter, 2100u, &event);
	expect_event(event, AIUS_NET_EVENT_STACK_STARTED, 7u,
	             AIUS_NET_FAULT_NONE);
	assert(adapter.pending_count == 0u);
	aius_network_teensy_poll(&adapter, 2200u, &event);
	expect_event(event, AIUS_NET_EVENT_NONE, 0u, AIUS_NET_FAULT_NONE);
	/* ... and in the other order: the callback wins, the poll read that
	 * follows adds nothing. */
	reset_adapter(&adapter);
	g_fake.link = AIUS_NETWORK_TEENSY_LINK_DOWN;
	aius_network_teensy_apply(&adapter, 3000u,
	                         action(AIUS_NET_ACTION_START_STACK), &event);
	aius_network_teensy_test_callback(&adapter, 7u,
	                                  AIUS_NET_EVENT_STACK_STARTED,
	                                  AIUS_NET_FAULT_NONE);
	aius_network_teensy_poll(&adapter, 3100u, &event);
	expect_event(event, AIUS_NET_EVENT_STACK_STARTED, 7u,
	             AIUS_NET_FAULT_NONE);
	g_fake.link = AIUS_NETWORK_TEENSY_LINK_UP;
	aius_network_teensy_poll(&adapter, 3200u, &event);
	expect_event(event, AIUS_NET_EVENT_NONE, 0u, AIUS_NET_FAULT_NONE);

	/* 4. Link never comes up. The wait is bounded by the adapter's link
	 * deadline -- the portable supervisor has no deadline in STACK_START --
	 * so the generation ends in a resource fault rather than a stall, and
	 * the next generation starts its own bounded wait.
	 */
	reset_adapter(&adapter);
	g_fake.link = AIUS_NETWORK_TEENSY_LINK_DOWN;
	aius_network_teensy_apply(&adapter, 4000u,
	                         action(AIUS_NET_ACTION_START_STACK), &event);
	expect_event(event, AIUS_NET_EVENT_NONE, 0u, AIUS_NET_FAULT_NONE);
	aius_network_teensy_poll(
		&adapter, 4000u + AIUS_NETWORK_TEENSY_LINK_DEADLINE_MS - 1u,
		&event);
	expect_event(event, AIUS_NET_EVENT_NONE, 0u, AIUS_NET_FAULT_NONE);
	aius_network_teensy_poll(
		&adapter, 4000u + AIUS_NETWORK_TEENSY_LINK_DEADLINE_MS, &event);
	expect_event(event, AIUS_NET_EVENT_RESOURCE_FAULT, 7u,
	             AIUS_NET_FAULT_LINK_LOST);
	assert(!adapter.dhcp_started);
	drive_teardown(&adapter, 7u);
	assert(!adapter.link_up_reported);
	aius_network_teensy_apply(&adapter, 9000u,
	                         action(AIUS_NET_ACTION_START_STACK, 8u),
	                         &event);
	expect_event(event, AIUS_NET_EVENT_NONE, 0u, AIUS_NET_FAULT_NONE);
	assert(adapter.stack_started_ms == 9000u);
	aius_network_teensy_poll(
		&adapter, 9000u + AIUS_NETWORK_TEENSY_LINK_DEADLINE_MS, &event);
	expect_event(event, AIUS_NET_EVENT_RESOURCE_FAULT, 8u,
	             AIUS_NET_FAULT_LINK_LOST);

	/* 5. The deadline is wrap-safe: a stack start just before the millis()
	 * wrap still expires exactly one deadline later.
	 */
	reset_adapter(&adapter);
	g_fake.link = AIUS_NETWORK_TEENSY_LINK_DOWN;
	aius_network_teensy_apply(&adapter, UINT32_MAX - 10u,
	                         action(AIUS_NET_ACTION_START_STACK), &event);
	expect_event(event, AIUS_NET_EVENT_NONE, 0u, AIUS_NET_FAULT_NONE);
	aius_network_teensy_poll(
		&adapter,
		(uint32_t)(UINT32_MAX - 10u +
		           AIUS_NETWORK_TEENSY_LINK_DEADLINE_MS - 1u),
		&event);
	expect_event(event, AIUS_NET_EVENT_NONE, 0u, AIUS_NET_FAULT_NONE);
	aius_network_teensy_poll(
		&adapter,
		(uint32_t)(UINT32_MAX - 10u +
		           AIUS_NETWORK_TEENSY_LINK_DEADLINE_MS),
		&event);
	expect_event(event, AIUS_NET_EVENT_RESOURCE_FAULT, 7u,
	             AIUS_NET_FAULT_LINK_LOST);

	g_fake.link = AIUS_NETWORK_TEENSY_LINK_UP;
}

void expect_io(AiusIoEvent event, AiusIoResult result, size_t bytes,
	           uint32_t fault)
{
	assert(event.result == result);
	assert(event.bytes == bytes);
	assert(event.fault == fault);
}

AiusIoEvent call_io(Operation operation, AiusNetworkTeensy *adapter,
	                uint8_t *buffer, size_t length)
{
	switch (operation) {
	case OP_ACCEPT:
		return aius_network_teensy_accept(adapter);
	case OP_READ:
		return aius_network_teensy_read_once(adapter, buffer, length);
	case OP_WRITE:
		return aius_network_teensy_write_once(adapter, buffer, length);
	case OP_CLOSE:
		return aius_network_teensy_close_once(adapter);
	case OP_UDP_RECEIVE:
		return aius_network_teensy_udp_receive_once(adapter, buffer, length);
	case OP_UDP_SEND:
		return aius_network_teensy_udp_send_once(adapter, buffer, length);
	default:
		assert(false);
		return {AIUS_IO_FAULT, 0u, AIUS_NET_FAULT_DRIVER_PROTOCOL};
	}
}

size_t io_slot(Operation operation)
{
	switch (operation) {
	case OP_ACCEPT:
		return 0u;
	case OP_READ:
		return 1u;
	case OP_WRITE:
		return 2u;
	case OP_CLOSE:
		return 3u;
	case OP_UDP_RECEIVE:
		return 4u;
	case OP_UDP_SEND:
		return 5u;
	default:
		assert(false);
		return 0u;
	}
}

bool data_operation(Operation operation)
{
	return operation == OP_READ || operation == OP_WRITE ||
	       operation == OP_UDP_RECEIVE || operation == OP_UDP_SEND;
}

void prepare_online(AiusNetworkTeensy *adapter)
{
	reset_adapter(adapter);
	make_online(adapter);
	g_fake.log_count = 0u;
}

void test_one_shot_io_matrix()
{
	const Operation operations[] = {
		OP_ACCEPT,
		OP_READ,
		OP_WRITE,
		OP_CLOSE,
		OP_UDP_RECEIVE,
		OP_UDP_SEND,
	};
	const AiusIoEvent control_results[] = {
		{AIUS_IO_PROGRESS, 0u, 0u},
		{AIUS_IO_WOULD_BLOCK, 0u, 0u},
		{AIUS_IO_CLOSED, 0u, 0u},
		{AIUS_IO_FAULT, 0u, AIUS_NET_FAULT_SOCKET},
	};
	const AiusIoEvent data_results[] = {
		{AIUS_IO_PROGRESS, 1u, 0u},
		{AIUS_IO_PROGRESS, 8u, 0u},
		{AIUS_IO_WOULD_BLOCK, 0u, 0u},
		{AIUS_IO_CLOSED, 0u, 0u},
		{AIUS_IO_FAULT, 0u, AIUS_NET_FAULT_SOCKET},
	};
	const AiusIoEvent malformed_control[] = {
		{AIUS_IO_PROGRESS, 1u, 0u},
		{AIUS_IO_WOULD_BLOCK, 1u, 0u},
		{AIUS_IO_CLOSED, 0u, AIUS_NET_FAULT_SOCKET},
		{AIUS_IO_FAULT, 1u, AIUS_NET_FAULT_SOCKET},
		{AIUS_IO_FAULT, 0u, 0u},
		{static_cast<AiusIoResult>(-1), 0u, 0u},
		{static_cast<AiusIoResult>(99), 0u, 0u},
	};
	const AiusIoEvent malformed_data[] = {
		{AIUS_IO_PROGRESS, 0u, 0u},
		{AIUS_IO_PROGRESS, 9u, 0u},
		{AIUS_IO_WOULD_BLOCK, 1u, 0u},
		{AIUS_IO_CLOSED, 0u, AIUS_NET_FAULT_SOCKET},
		{AIUS_IO_FAULT, 1u, AIUS_NET_FAULT_SOCKET},
		{AIUS_IO_FAULT, 0u, 0u},
		{static_cast<AiusIoResult>(-1), 0u, 0u},
		{static_cast<AiusIoResult>(99), 0u, 0u},
	};
	uint8_t buffer[8] = {};

	for (Operation operation : operations) {
		const bool is_data = data_operation(operation);
		const AiusIoEvent *valid =
			is_data ? data_results : control_results;
		const size_t valid_count =
			is_data ? sizeof(data_results) / sizeof(data_results[0])
			        : sizeof(control_results) / sizeof(control_results[0]);
		for (size_t index = 0u; index < valid_count; index++) {
			AiusNetworkTeensy adapter;
			prepare_online(&adapter);
			g_fake.io[io_slot(operation)] = valid[index];
			const AiusIoEvent actual =
				call_io(operation, &adapter, buffer, sizeof(buffer));
			expect_io(actual, valid[index].result, valid[index].bytes,
			          valid[index].fault);
			assert(g_fake.log_count == 1u);
			assert(g_fake.log[0] == operation);
		}

		const AiusIoEvent *malformed =
			is_data ? malformed_data : malformed_control;
		const size_t malformed_count =
			is_data
				? sizeof(malformed_data) / sizeof(malformed_data[0])
				: sizeof(malformed_control) /
					  sizeof(malformed_control[0]);
		for (size_t index = 0u; index < malformed_count; index++) {
			AiusNetworkTeensy adapter;
			prepare_online(&adapter);
			g_fake.io[io_slot(operation)] = malformed[index];
			expect_io(call_io(operation, &adapter, buffer, sizeof(buffer)),
			          AIUS_IO_FAULT, 0u,
			          AIUS_NET_FAULT_DRIVER_PROTOCOL);
			assert(g_fake.log_count == 1u);
			assert(g_fake.log[0] == operation);
		}

		AiusNetworkTeensy offline;
		prepare_online(&offline);
		offline.online = false;
		expect_io(call_io(operation, &offline, buffer, sizeof(buffer)),
		          AIUS_IO_CLOSED, 0u, 0u);
		assert(g_fake.log_count == 0u);

		if (is_data) {
			AiusNetworkTeensy zero_capacity;
			prepare_online(&zero_capacity);
			expect_io(call_io(operation, &zero_capacity, buffer, 0u),
			          AIUS_IO_WOULD_BLOCK, 0u, 0u);
			assert(g_fake.log_count == 0u);

			AiusNetworkTeensy invalid_buffer;
			prepare_online(&invalid_buffer);
			expect_io(call_io(operation, &invalid_buffer, nullptr, 1u),
			          AIUS_IO_FAULT, 0u,
			          AIUS_NET_FAULT_DRIVER_PROTOCOL);
			assert(g_fake.log_count == 0u);
		}
	}

	AiusNetworkTeensy no_listener;
	prepare_online(&no_listener);
	no_listener.listener_open = false;
	expect_io(aius_network_teensy_accept(&no_listener), AIUS_IO_CLOSED, 0u,
	          0u);
	assert(g_fake.log_count == 0u);

	AiusNetworkTeensy no_udp;
	prepare_online(&no_udp);
	no_udp.udp_open = false;
	expect_io(aius_network_teensy_udp_receive_once(
		          &no_udp, buffer, sizeof(buffer)),
	          AIUS_IO_CLOSED, 0u, 0u);
	expect_io(aius_network_teensy_udp_send_once(
		          &no_udp, buffer, sizeof(buffer)),
	          AIUS_IO_CLOSED, 0u, 0u);
	assert(g_fake.log_count == 0u);

	AiusNetworkTeensy idempotent_close;
	prepare_online(&idempotent_close);
	g_fake.io[3] = {AIUS_IO_PROGRESS, 0u, 0u};
	expect_io(aius_network_teensy_close_once(&idempotent_close),
	          AIUS_IO_PROGRESS, 0u, 0u);
	g_fake.io[3] = {AIUS_IO_CLOSED, 0u, 0u};
	expect_io(aius_network_teensy_close_once(&idempotent_close),
	          AIUS_IO_CLOSED, 0u, 0u);
	assert(g_fake.log_count == 2u);
	assert(g_fake.log[0] == OP_CLOSE && g_fake.log[1] == OP_CLOSE);
}

/* The status accessors must be callable in any supervisor state, must touch the
 * stack at most once per call, and must do nothing else -- so every check below
 * also asserts the adapter is unchanged and that no lifecycle or socket
 * primitive ran. A call that returns at all is the "never blocks" evidence the
 * fakes can give; the accessors contain no loop and no retry.
 */
void expect_link(const AiusNetworkTeensy *adapter,
	             AiusNetworkTeensyLink expected)
{
	AiusNetworkTeensy before = {};
	if (adapter != nullptr) {
		memcpy(&before, adapter, sizeof(before));
	}
	const size_t log_before = g_fake.log_count;
	const uint32_t address_reads_before =
		aius_network_teensy_test_address_reads();
	const size_t expected_reads =
		(adapter != nullptr && adapter->stack_started) ? 1u : 0u;

	assert(aius_network_teensy_link(adapter) == expected);

	assert(g_fake.log_count == log_before + expected_reads);
	if (expected_reads == 1u) {
		assert(g_fake.log[g_fake.log_count - 1u] == OP_LINK_STATUS);
	}
	assert(aius_network_teensy_test_address_reads() ==
	       address_reads_before);
	if (adapter != nullptr) {
		assert(memcmp(&before, adapter, sizeof(before)) == 0);
	}
}

void expect_address(const AiusNetworkTeensy *adapter, bool usable,
	                uint8_t first, uint8_t second, uint8_t third,
	                uint8_t fourth)
{
	AiusNetworkTeensy before = {};
	if (adapter != nullptr) {
		memcpy(&before, adapter, sizeof(before));
	}
	uint8_t address[4] = {0xffu, 0xffu, 0xffu, 0xffu};
	const size_t log_before = g_fake.log_count;
	const uint32_t address_reads_before =
		aius_network_teensy_test_address_reads();
	const uint32_t expected_reads =
		(adapter != nullptr && adapter->stack_started) ? 1u : 0u;

	assert(aius_network_teensy_address(adapter, address) == usable);

	assert(address[0] == first);
	assert(address[1] == second);
	assert(address[2] == third);
	assert(address[3] == fourth);
	assert(aius_network_teensy_test_address_reads() ==
	       address_reads_before + expected_reads);
	assert(g_fake.log_count == log_before);
	if (adapter != nullptr) {
		assert(memcmp(&before, adapter, sizeof(before)) == 0);
	}
}

void drive_teardown(AiusNetworkTeensy *adapter, uint32_t generation)
{
	AiusNetEvent event = {};
	aius_network_teensy_apply(adapter, 20u,
	                         action(AIUS_NET_ACTION_TEARDOWN, generation),
	                         &event);
	for (size_t step = 0u; step < 4u &&
	                       event.kind != AIUS_NET_EVENT_TEARDOWN_DONE;
	     step++) {
		aius_network_teensy_apply(adapter, 21u,
		                         action(AIUS_NET_ACTION_NONE, 0u), &event);
	}
	expect_event(event, AIUS_NET_EVENT_TEARDOWN_DONE, generation,
	             AIUS_NET_FAULT_NONE);
	assert(!adapter->stack_started);
	assert(!adapter->dhcp_started);
	assert(!adapter->lease_acquired);
}

void test_status_accessors_across_lifecycle()
{
	const uint8_t first_lease[4] = {192u, 168u, 4u, 21u};
	const uint8_t second_lease[4] = {10u, 0u, 0u, 7u};
	const uint8_t no_lease[4] = {0u, 0u, 0u, 0u};
	uint8_t scratch[4] = {0xffu, 0xffu, 0xffu, 0xffu};
	AiusNetworkTeensy adapter;
	reset_adapter(&adapter);

	/* Before the stack starts there is nothing to ask, and the accessors ask
	 * nothing.
	 */
	expect_link(&adapter, AIUS_NETWORK_TEENSY_LINK_UNKNOWN);
	expect_address(&adapter, false, 0u, 0u, 0u, 0u);
	assert(!aius_network_teensy_dhcp_pending(&adapter));
	assert(g_fake.log_count == 0u);
	assert(aius_network_teensy_test_address_reads() == 0u);

	expect_link(nullptr, AIUS_NETWORK_TEENSY_LINK_UNKNOWN);
	expect_address(nullptr, false, 0u, 0u, 0u, 0u);
	assert(!aius_network_teensy_address(&adapter, nullptr));
	assert(!aius_network_teensy_dhcp_pending(nullptr));
	assert(g_fake.log_count == 0u);
	assert(aius_network_teensy_test_address_reads() == 0u);

	/* Stack up, lease outstanding: the read happens and reports the address
	 * as unusable rather than serializing a half-configured stack.
	 */
	make_stack_ready(&adapter);
	g_fake.log_count = 0u;
	assert(aius_network_teensy_dhcp_pending(&adapter));
	expect_address(&adapter, false, 0u, 0u, 0u, 0u);
	assert(aius_network_teensy_test_address_reads() == 1u);

	/* Every link state maps one for one, and an out-of-contract driver value
	 * degrades to UNKNOWN instead of leaking through.
	 */
	expect_link(&adapter, AIUS_NETWORK_TEENSY_LINK_UP);
	g_fake.link = AIUS_NETWORK_TEENSY_LINK_DOWN;
	expect_link(&adapter, AIUS_NETWORK_TEENSY_LINK_DOWN);
	g_fake.link = AIUS_NETWORK_TEENSY_LINK_UNKNOWN;
	expect_link(&adapter, AIUS_NETWORK_TEENSY_LINK_UNKNOWN);
	g_fake.link = static_cast<AiusNetworkTeensyLink>(77);
	expect_link(&adapter, AIUS_NETWORK_TEENSY_LINK_UNKNOWN);
	g_fake.link = AIUS_NETWORK_TEENSY_LINK_UP;

	/* Lease acquired: pending clears, the update counter advances, and the
	 * four octets reach the caller.
	 */
	aius_network_teensy_test_set_address(first_lease);
	aius_network_teensy_test_callback(&adapter, 7u,
	                                 AIUS_NET_EVENT_LEASE_ACQUIRED,
	                                 AIUS_NET_FAULT_NONE);
	assert(adapter.lease_acquired);
	assert(adapter.dhcp_updates == 1u);
	assert(!aius_network_teensy_dhcp_pending(&adapter));
	expect_address(&adapter, true, 192u, 168u, 4u, 21u);

	/* A lease callback captured under a dead generation neither clears
	 * pending nor counts.
	 */
	aius_network_teensy_test_callback(&adapter, 6u,
	                                  AIUS_NET_EVENT_LEASE_ACQUIRED,
	                                  AIUS_NET_FAULT_NONE);
	assert(adapter.dhcp_updates == 1u);

	/* Online changes neither answer. */
	AiusNetEvent event = {};
	aius_network_teensy_apply(&adapter, 5u,
	                         action(AIUS_NET_ACTION_PUBLISH_ONLINE), &event);
	aius_network_teensy_apply(&adapter, 6u,
	                         action(AIUS_NET_ACTION_NONE, 0u), &event);
	assert(adapter.online);
	expect_address(&adapter, true, 192u, 168u, 4u, 21u);
	expect_link(&adapter, AIUS_NETWORK_TEENSY_LINK_UP);

	/* After teardown the stack is gone: defined answers, no reads. */
	drive_teardown(&adapter, 7u);
	aius_network_teensy_test_set_address(no_lease);
	g_fake.log_count = 0u;
	const uint32_t reads_after_teardown =
		aius_network_teensy_test_address_reads();
	expect_link(&adapter, AIUS_NETWORK_TEENSY_LINK_UNKNOWN);
	expect_address(&adapter, false, 0u, 0u, 0u, 0u);
	assert(!aius_network_teensy_dhcp_pending(&adapter));
	assert(g_fake.log_count == 0u);
	assert(aius_network_teensy_test_address_reads() ==
	       reads_after_teardown);

	/* A restarted generation reports the new lease, and the update counter
	 * stays monotonic across the restart.
	 */
	aius_network_teensy_apply(&adapter, 30u,
	                         action(AIUS_NET_ACTION_START_STACK, 8u),
	                         &event);
	expect_event(event, AIUS_NET_EVENT_STACK_STARTED, 8u,
	             AIUS_NET_FAULT_NONE);
	assert(!aius_network_teensy_dhcp_pending(&adapter));
	expect_address(&adapter, false, 0u, 0u, 0u, 0u);
	aius_network_teensy_apply(&adapter, 31u,
	                         action(AIUS_NET_ACTION_START_DHCP, 8u), &event);
	assert(aius_network_teensy_dhcp_pending(&adapter));

	aius_network_teensy_test_callback(&adapter, 7u,
	                                  AIUS_NET_EVENT_LEASE_ACQUIRED,
	                                  AIUS_NET_FAULT_NONE);
	assert(aius_network_teensy_dhcp_pending(&adapter));
	assert(adapter.dhcp_updates == 1u);

	aius_network_teensy_test_set_address(second_lease);
	aius_network_teensy_test_callback(&adapter, 8u,
	                                  AIUS_NET_EVENT_LEASE_ACQUIRED,
	                                  AIUS_NET_FAULT_NONE);
	assert(!aius_network_teensy_dhcp_pending(&adapter));
	assert(adapter.dhcp_updates == 2u);
	expect_address(&adapter, true, 10u, 0u, 0u, 7u);
	expect_link(&adapter, AIUS_NETWORK_TEENSY_LINK_UP);

	/* An unwired adapter answers without reaching for a table. */
	aius_network_teensy_test_set_primitives(nullptr);
	assert(aius_network_teensy_link(&adapter) ==
	       AIUS_NETWORK_TEENSY_LINK_UNKNOWN);
	assert(!aius_network_teensy_address(&adapter, scratch));
	assert(scratch[0] == 0u && scratch[1] == 0u && scratch[2] == 0u &&
	       scratch[3] == 0u);
	assert(aius_network_teensy_test_address_reads() == 0u);
}

void drive_teardown_at(AiusNetworkTeensy *adapter, uint32_t generation,
	                   uint32_t now_ms)
{
	AiusNetEvent event = {};
	aius_network_teensy_apply(adapter, now_ms,
	                         action(AIUS_NET_ACTION_TEARDOWN, generation),
	                         &event);
	for (size_t step = 0u; step < 4u &&
	                       event.kind != AIUS_NET_EVENT_TEARDOWN_DONE;
	     step++) {
		aius_network_teensy_apply(adapter, now_ms + 1u,
		                         action(AIUS_NET_ACTION_NONE, 0u), &event);
	}
	expect_event(event, AIUS_NET_EVENT_TEARDOWN_DONE, generation,
	             AIUS_NET_FAULT_NONE);
}

/* The measurement surface, driven through the shape it was built to settle: one
 * boot in which generation 1 initialises a cold interface and its DHCP window
 * expires, and generation 2 reuses the warm one and binds.
 *
 * These are assertions about what is *reported*, not about what the adapter
 * does. Every action, event, deadline and call order asserted elsewhere in this
 * file is driven through a table with no diagnostic readers at all, which is
 * what makes "diagnostic only" checkable rather than claimed.
 */
void test_generation_diagnostics()
{
	AiusNetworkTeensy adapter;
	AiusNetEvent event = {};
	uint32_t tx = 9u;
	uint32_t rx = 9u;

	reset_adapter(&adapter);
	aius_network_teensy_test_set_diag_primitives(&kDiagPrimitives);

	/* Nothing measured, and nothing read, before a stack exists. */
	assert(aius_network_teensy_diag_first(&adapter) == nullptr);
	assert(aius_network_teensy_diag(&adapter)->generation == 0u);
	assert(aius_network_teensy_link_live(&adapter) ==
	       AIUS_NETWORK_TEENSY_LINK_UNKNOWN);
	assert(aius_network_teensy_dhcp_state(&adapter) ==
	       AIUS_NETWORK_TEENSY_DHCP_STATE_NONE);
	assert(!aius_network_teensy_frames(&adapter, &tx, &rx));
	assert(tx == 0u && rx == 0u);
	assert(g_diag.link_live_calls == 0u);
	assert(g_diag.dhcp_state_calls == 0u);
	assert(g_diag.frames_calls == 0u);

	/* Generation 1, cold interface: beginNoWait() performs the real netif
	 * init and blocks two seconds inside it, and the cached link reads up the
	 * instant it returns -- so the gate is released by an init-time snapshot,
	 * which is exactly what link_up_source has to be able to say.
	 */
	g_diag.netif_initialized = false;
	g_diag.stack_start_cost_ms = 2013u;
	g_diag.uptime_ms = 1800u;
	g_diag.link_live = AIUS_NETWORK_TEENSY_LINK_DOWN;
	g_diag.dhcp_state = 1u;
	g_diag.frames_ok = true;
	g_diag.tx = 100u;
	g_diag.rx = 200u;
	g_fake.link = AIUS_NETWORK_TEENSY_LINK_UP;

	aius_network_teensy_apply(&adapter, 3820u,
	                         action(AIUS_NET_ACTION_START_STACK, 1u), &event);
	/* Link-up is observed here and STACK_STARTED is still withheld: this is
	 * the generation that ran the real netif init, and on the board that one
	 * reports link up for seconds before the FEC carries a frame. */
	expect_event(event, AIUS_NET_EVENT_NONE, 0u, AIUS_NET_FAULT_NONE);
	const AiusNetworkTeensyDiag *diag = aius_network_teensy_diag(&adapter);
	assert(diag->generation == 1u);
	assert(diag->netif_init == AIUS_NETWORK_TEENSY_NETIF_INIT_PERFORMED);
	assert(diag->stack_start_blocking_ms == 2013u);
	assert(diag->link_at_stack_start == AIUS_NETWORK_TEENSY_LINK_UP);
	assert(diag->link_up_source == AIUS_NETWORK_TEENSY_LINK_UP_STACK_START);
	assert(diag->link_up_ms == 0u);
	assert(!diag->link_callback_up_seen);
	assert(g_diag.netif_initialized_calls == 1u);
	assert(g_diag.uptime_calls == 2u);
	assert(adapter.link_up_reported);
	assert(adapter.rx_ready_required && adapter.rx_ready_pending);
	assert(diag->rx_ready_source == AIUS_NETWORK_TEENSY_RX_READY_NONE);

	/* The link service's own fresh observation arrives afterwards. The latch
	 * drops the duplicate STACK_STARTED; the measurement keeps it, because
	 * "the snapshot released the gate and a fresh read agreed 30 ms later" is
	 * precisely the distinction being drawn.
	 */
	aius_network_teensy_test_callback(&adapter, 1u,
	                                  AIUS_NET_EVENT_STACK_STARTED,
	                                  AIUS_NET_FAULT_NONE);
	aius_network_teensy_poll(&adapter, 3850u, &event);
	expect_event(event, AIUS_NET_EVENT_NONE, 0u, AIUS_NET_FAULT_NONE);
	assert(diag->link_callback_up_seen);
	assert(diag->link_callback_up_ms == 30u);
	assert(diag->link_callbacks == 1u);
	assert(diag->link_up_source == AIUS_NETWORK_TEENSY_LINK_UP_STACK_START);

	/* While the gate is open the epoch's single read is the counter one, and
	 * it is a replacement rather than an addition: thirty-two epochs spend
	 * thirty-two statistics reads and not one link_live, dhcp_state or netif
	 * probe. Nothing has arrived, so nothing is released.
	 */
	const unsigned waiting_link_live = g_diag.link_live_calls;
	const unsigned waiting_dhcp_state = g_diag.dhcp_state_calls;
	const unsigned waiting_frames = g_diag.frames_calls;
	const unsigned waiting_netif = g_diag.netif_initialized_calls;
	for (uint32_t tick = 0u; tick < 32u; tick++) {
		aius_network_teensy_poll(&adapter, 3900u + tick, &event);
		expect_event(event, AIUS_NET_EVENT_NONE, 0u,
		             AIUS_NET_FAULT_NONE);
	}
	assert(g_diag.frames_calls == waiting_frames + 32u);
	assert(g_diag.link_live_calls == waiting_link_live);
	assert(g_diag.dhcp_state_calls == waiting_dhcp_state);
	assert(g_diag.netif_initialized_calls == waiting_netif);
	assert(adapter.rx_ready_pending);

	/* One ambient frame reaches the MAC. That is the evidence, and the gate
	 * releases on it -- 170 ms after stack start, not at the fallback. */
	g_diag.rx = 201u;
	aius_network_teensy_poll(&adapter, 3990u, &event);
	expect_event(event, AIUS_NET_EVENT_STACK_STARTED, 1u,
	             AIUS_NET_FAULT_NONE);
	assert(!adapter.rx_ready_pending);
	assert(diag->rx_ready_source == AIUS_NETWORK_TEENSY_RX_READY_COUNTER);
	assert(diag->rx_ready_ms == 170u);

	/* The epoch pays nothing. poll() is the one call the supervisor makes
	 * every epoch, and once the gate has released it makes no diagnostic
	 * call at all.
	 */
	const unsigned link_live_calls = g_diag.link_live_calls;
	const unsigned dhcp_state_calls = g_diag.dhcp_state_calls;
	const unsigned frames_calls = g_diag.frames_calls;
	const unsigned uptime_calls = g_diag.uptime_calls;
	const unsigned netif_calls = g_diag.netif_initialized_calls;
	for (uint32_t tick = 0u; tick < 32u; tick++) {
		aius_network_teensy_poll(&adapter, 4000u + tick, &event);
	}
	assert(g_diag.link_live_calls == link_live_calls);
	assert(g_diag.dhcp_state_calls == dhcp_state_calls);
	assert(g_diag.frames_calls == frames_calls);
	assert(g_diag.uptime_calls == uptime_calls);
	assert(g_diag.netif_initialized_calls == netif_calls);

	aius_network_teensy_apply(&adapter, 4100u,
	                         action(AIUS_NET_ACTION_START_DHCP, 1u), &event);
	assert(diag->dhcp_started_seen);
	assert(diag->dhcp_start_ms == 280u);
	assert(diag->dhcp_state_at_start == 1u);
	assert(diag->frames_valid);
	assert(g_diag.dhcp_state_calls == dhcp_state_calls + 1u);
	assert(g_diag.frames_calls == frames_calls + 1u);

	/* The DHCP deadline expires. FNET is still in its own SELECTING state --
	 * it believes it sent a DISCOVER -- while the MAC's transmit counter has
	 * not moved, and the cached link still reads up beside a fresh read that
	 * does not. That triple is the whole measurement.
	 */
	g_diag.dhcp_state = 2u;
	g_diag.link_live = AIUS_NETWORK_TEENSY_LINK_DOWN;
	drive_teardown_at(&adapter, 1u, 8834u);
	assert(diag->end_reason == AIUS_NETWORK_TEENSY_END_TEARDOWN);
	assert(diag->end_ms == 5014u);
	assert(diag->dhcp_state_at_end == 2u);
	assert(diag->link_at_end == AIUS_NETWORK_TEENSY_LINK_UP);
	assert(diag->link_live_at_end == AIUS_NETWORK_TEENSY_LINK_DOWN);
	assert(diag->frames_valid);
	assert(diag->tx_frames == 0u);
	assert(diag->rx_frames == 0u);

	/* Generation 2, warm interface: the netif init is skipped entirely,
	 * beginNoWait() returns at once, and nothing is waited for. This is the
	 * generation that binds in 19 ms on the board, so STACK_STARTED lands in
	 * the apply call and not one counter read is spent getting there. */
	g_diag.netif_initialized = true;
	g_diag.stack_start_cost_ms = 0u;
	g_diag.dhcp_state = 1u;
	g_diag.link_live = AIUS_NETWORK_TEENSY_LINK_UP;
	const unsigned frames_before_warm = g_diag.frames_calls;
	aius_network_teensy_apply(&adapter, 9800u,
	                         action(AIUS_NET_ACTION_START_STACK, 2u), &event);
	expect_event(event, AIUS_NET_EVENT_STACK_STARTED, 2u,
	             AIUS_NET_FAULT_NONE);
	assert(!adapter.rx_ready_required && !adapter.rx_ready_pending);
	assert(g_diag.frames_calls == frames_before_warm);
	const AiusNetworkTeensyDiag *first =
		aius_network_teensy_diag_first(&adapter);
	assert(first != nullptr);
	assert(first->generation == 1u);
	assert(first->netif_init == AIUS_NETWORK_TEENSY_NETIF_INIT_PERFORMED);
	assert(first->stack_start_blocking_ms == 2013u);
	assert(first->tx_frames == 0u);
	assert(first->rx_ready_source == AIUS_NETWORK_TEENSY_RX_READY_COUNTER);
	assert(first->rx_ready_ms == 170u);
	assert(diag->generation == 2u);
	assert(diag->netif_init == AIUS_NETWORK_TEENSY_NETIF_INIT_SKIPPED);
	assert(diag->stack_start_blocking_ms == 0u);
	assert(diag->end_reason == AIUS_NETWORK_TEENSY_END_NONE);
	assert(diag->rx_ready_source ==
	       AIUS_NETWORK_TEENSY_RX_READY_NOT_REQUIRED);

	aius_network_teensy_apply(&adapter, 9805u,
	                         action(AIUS_NET_ACTION_START_DHCP, 2u), &event);
	g_diag.tx = 104u;
	g_diag.rx = 207u;
	g_diag.dhcp_state = 5u;
	aius_network_teensy_apply(&adapter, 9810u,
	                         action(AIUS_NET_ACTION_PUBLISH_ONLINE, 2u),
	                         &event);
	aius_network_teensy_apply(&adapter, 9811u,
	                         action(AIUS_NET_ACTION_NONE, 0u), &event);
	assert(adapter.online);
	assert(diag->end_reason == AIUS_NETWORK_TEENSY_END_ONLINE);
	assert(diag->end_ms == 11u);
	assert(diag->dhcp_state_at_end == 5u);
	assert(diag->tx_frames == 4u);
	assert(diag->rx_frames == 6u);

	/* The retained record is the first generation, not the last: by the time
	 * this surface is readable the board is already on a later one. */
	assert(aius_network_teensy_diag_first(&adapter)->generation == 1u);

	/* Live readers while a stack is up. The cached member and a fresh read
	 * disagreeing is reportable rather than reconciled. */
	drive_teardown_at(&adapter, 2u, 20000u);
	aius_network_teensy_apply(&adapter, 20100u,
	                         action(AIUS_NET_ACTION_START_STACK, 3u), &event);
	assert(aius_network_teensy_diag_first(&adapter)->generation == 1u);
	g_diag.link_live = AIUS_NETWORK_TEENSY_LINK_DOWN;
	assert(aius_network_teensy_link(&adapter) == AIUS_NETWORK_TEENSY_LINK_UP);
	assert(aius_network_teensy_link_live(&adapter) ==
	       AIUS_NETWORK_TEENSY_LINK_DOWN);
	assert(aius_network_teensy_dhcp_state(&adapter) == 5u);
	assert(aius_network_teensy_frames(&adapter, &tx, &rx));
	assert(tx == 104u && rx == 207u);

	/* A link-lost callback counts as a link callback of the other polarity
	 * and leaves the first connected timestamp alone. */
	aius_network_teensy_test_callback(&adapter, 3u,
	                                  AIUS_NET_EVENT_RESOURCE_FAULT,
	                                  AIUS_NET_FAULT_LINK_LOST);
	aius_network_teensy_poll(&adapter, 20150u, &event);
	expect_event(event, AIUS_NET_EVENT_RESOURCE_FAULT, 3u,
	             AIUS_NET_FAULT_LINK_LOST);
	assert(diag->link_callbacks == 1u);
	assert(!diag->link_callback_up_seen);

	/* After teardown the readers answer without touching a dead stack. */
	drive_teardown_at(&adapter, 3u, 20200u);
	const unsigned live_after = g_diag.link_live_calls;
	const unsigned state_after = g_diag.dhcp_state_calls;
	const unsigned frames_after = g_diag.frames_calls;
	assert(aius_network_teensy_link_live(&adapter) ==
	       AIUS_NETWORK_TEENSY_LINK_UNKNOWN);
	assert(aius_network_teensy_dhcp_state(&adapter) ==
	       AIUS_NETWORK_TEENSY_DHCP_STATE_NONE);
	assert(!aius_network_teensy_frames(&adapter, &tx, &rx));
	assert(tx == 0u && rx == 0u);
	assert(g_diag.link_live_calls == live_after);
	assert(g_diag.dhcp_state_calls == state_after);
	assert(g_diag.frames_calls == frames_after);
}

size_t count_operations(Operation wanted)
{
	size_t seen = 0u;
	for (size_t index = 0u; index < g_fake.log_count; index++) {
		if (g_fake.log[index] == wanted) {
			seen++;
		}
	}
	return seen;
}

/* Opens generation 7 on an interface this generation initialises, with the link
 * already up: START_STACK observes link-up and withholds STACK_STARTED, because
 * link-up is the signal that was already saying "up" while the FEC carried
 * nothing.
 */
void begin_cold_generation(AiusNetworkTeensy *adapter, uint32_t now_ms,
	                       uint32_t rx, bool frames_ok)
{
	AiusNetEvent event = {};

	reset_adapter(adapter);
	aius_network_teensy_test_set_diag_primitives(&kDiagPrimitives);
	g_diag.netif_initialized = false;
	g_diag.frames_ok = frames_ok;
	g_diag.tx = 3u;
	g_diag.rx = rx;
	g_fake.link = AIUS_NETWORK_TEENSY_LINK_UP;

	aius_network_teensy_apply(adapter, now_ms,
	                         action(AIUS_NET_ACTION_START_STACK), &event);
	expect_event(event, AIUS_NET_EVENT_NONE, 0u, AIUS_NET_FAULT_NONE);
	assert(adapter->stack_started);
	assert(adapter->link_up_reported);
	assert(adapter->rx_ready_required && adapter->rx_ready_pending);
	assert(aius_network_teensy_diag(adapter)->rx_ready_source ==
	       AIUS_NETWORK_TEENSY_RX_READY_NONE);
}

/* The receive-readiness gate: the fix for the first-attempt DHCP failure, whose
 * measured cause is an interface that reports link up and passes no traffic.
 *
 * Every case here asserts one of the four properties the gate has to have --
 * released early on evidence, released anyway on a bound, never armed for the
 * generation that already works, and wrap-safe against free-running hardware
 * counters that can and do wrap.
 */
void test_receive_readiness_gate()
{
	AiusNetworkTeensy adapter;
	AiusNetEvent event = {};

	/* Bound: at most one DHCP deadline. Waiting for evidence that a DISCOVER can
	 * leave the board never costs more than sending one and finding out.
	 *
	 * `<=`, not `==`, matching the adapter's assert -- the two constants answer to
	 * different measurements and were only coincidentally equal. See
	 * aius_network_teensy.cpp for why raising this one to follow the DHCP deadline
	 * would have lengthened bring-up for nothing. */
	static_assert(AIUS_NETWORK_TEENSY_RX_READY_DEADLINE_MS <=
		              AIUS_NET_DHCP_DEADLINE_MS,
		          "readiness fallback must not exceed the DHCP deadline");

	/* Evidence arrives, so DHCP starts on it rather than on the bound. The
	 * counter read replaces the link read for the wait: no OP_LINK_STATUS is
	 * logged across it, so the epoch's single underlying read is unchanged. */
	begin_cold_generation(&adapter, 1000u, 500u, true);
	const size_t link_reads = count_operations(OP_LINK_STATUS);
	aius_network_teensy_poll(&adapter, 1001u, &event);
	expect_event(event, AIUS_NET_EVENT_NONE, 0u, AIUS_NET_FAULT_NONE);
	assert(g_diag.frames_calls == 1u);
	aius_network_teensy_poll(&adapter, 1002u, &event);
	expect_event(event, AIUS_NET_EVENT_NONE, 0u, AIUS_NET_FAULT_NONE);
	g_diag.rx = 501u;
	aius_network_teensy_poll(&adapter, 1003u, &event);
	expect_event(event, AIUS_NET_EVENT_STACK_STARTED, 7u,
	             AIUS_NET_FAULT_NONE);
	assert(!adapter.rx_ready_pending);
	assert(count_operations(OP_LINK_STATUS) == link_reads);
	assert(g_diag.frames_calls == 3u);
	const AiusNetworkTeensyDiag *diag = aius_network_teensy_diag(&adapter);
	assert(diag->rx_ready_source == AIUS_NETWORK_TEENSY_RX_READY_COUNTER);
	assert(diag->rx_ready_ms == 3u);

	/* And it is released exactly once: the next epoch is an ordinary link
	 * health read again, not a second STACK_STARTED. */
	aius_network_teensy_poll(&adapter, 1004u, &event);
	expect_event(event, AIUS_NET_EVENT_NONE, 0u, AIUS_NET_FAULT_NONE);
	assert(count_operations(OP_LINK_STATUS) == link_reads + 1u);

	/* DHCP then gets its whole deadline from where it actually starts. The
	 * gate delays the attempt; it never shortens it. */
	aius_network_teensy_apply(&adapter, 1005u,
	                         action(AIUS_NET_ACTION_START_DHCP), &event);
	expect_event(event, AIUS_NET_EVENT_NONE, 0u, AIUS_NET_FAULT_NONE);
	assert(adapter.dhcp_started);
	assert(diag->dhcp_start_ms == 5u);

	/* Nothing ever arrives. A silent segment must not deadlock the board, so
	 * the bound releases STACK_STARTED anyway -- at the deadline, not before
	 * it, and not never. */
	begin_cold_generation(&adapter, 1000u, 77u, true);
	aius_network_teensy_poll(&adapter, 1001u, &event);
	expect_event(event, AIUS_NET_EVENT_NONE, 0u, AIUS_NET_FAULT_NONE);
	aius_network_teensy_poll(
		&adapter, 1000u + AIUS_NETWORK_TEENSY_RX_READY_DEADLINE_MS - 1u,
		&event);
	expect_event(event, AIUS_NET_EVENT_NONE, 0u, AIUS_NET_FAULT_NONE);
	assert(adapter.rx_ready_pending);
	aius_network_teensy_poll(
		&adapter, 1000u + AIUS_NETWORK_TEENSY_RX_READY_DEADLINE_MS,
		&event);
	expect_event(event, AIUS_NET_EVENT_STACK_STARTED, 7u,
	             AIUS_NET_FAULT_NONE);
	assert(!adapter.rx_ready_pending);
	diag = aius_network_teensy_diag(&adapter);
	assert(diag->rx_ready_source == AIUS_NETWORK_TEENSY_RX_READY_DEADLINE);
	assert(diag->rx_ready_ms == AIUS_NETWORK_TEENSY_RX_READY_DEADLINE_MS);

	/* A driver that refuses the statistics call leaves no evidence to wait
	 * for. That is the same bounded fallback, not a stall. */
	begin_cold_generation(&adapter, 2000u, 5u, false);
	aius_network_teensy_poll(&adapter, 2001u, &event);
	expect_event(event, AIUS_NET_EVENT_NONE, 0u, AIUS_NET_FAULT_NONE);
	assert(!adapter.rx_ready_baseline_valid);
	aius_network_teensy_poll(
		&adapter, 2000u + AIUS_NETWORK_TEENSY_RX_READY_DEADLINE_MS,
		&event);
	expect_event(event, AIUS_NET_EVENT_STACK_STARTED, 7u,
	             AIUS_NET_FAULT_NONE);
	assert(aius_network_teensy_diag(&adapter)->rx_ready_source ==
	       AIUS_NETWORK_TEENSY_RX_READY_DEADLINE);

	/* Wrap safety, both clocks that matter. RMON_R_PACKETS is a free-running
	 * 32-bit hardware counter: a frame received across its wrap is still a
	 * frame, and a comparison that read it as "went backwards, so nothing
	 * arrived" would hold DHCP to the fallback for no reason. */
	begin_cold_generation(&adapter, 1000u, 0xFFFFFFFFu, true);
	aius_network_teensy_poll(&adapter, 1001u, &event);
	expect_event(event, AIUS_NET_EVENT_NONE, 0u, AIUS_NET_FAULT_NONE);
	assert(adapter.rx_ready_baseline == 0xFFFFFFFFu);
	g_diag.rx = 1u;
	aius_network_teensy_poll(&adapter, 1002u, &event);
	expect_event(event, AIUS_NET_EVENT_STACK_STARTED, 7u,
	             AIUS_NET_FAULT_NONE);
	assert(aius_network_teensy_diag(&adapter)->rx_ready_source ==
	       AIUS_NETWORK_TEENSY_RX_READY_COUNTER);

	/* One step short of the wrap is still movement. */
	begin_cold_generation(&adapter, 1000u, 0xFFFFFFFEu, true);
	aius_network_teensy_poll(&adapter, 1001u, &event);
	g_diag.rx = 0xFFFFFFFFu;
	aius_network_teensy_poll(&adapter, 1002u, &event);
	expect_event(event, AIUS_NET_EVENT_STACK_STARTED, 7u,
	             AIUS_NET_FAULT_NONE);

	/* And the millis() wrap the fallback deadline is computed across cannot
	 * turn a five-second wait into a forty-nine-day one. */
	begin_cold_generation(&adapter, 0xFFFFF000u, 12u, true);
	aius_network_teensy_poll(&adapter, 0xFFFFF001u, &event);
	expect_event(event, AIUS_NET_EVENT_NONE, 0u, AIUS_NET_FAULT_NONE);
	aius_network_teensy_poll(&adapter,
		                     0xFFFFF000u +
		                         AIUS_NETWORK_TEENSY_RX_READY_DEADLINE_MS,
		                     &event);
	expect_event(event, AIUS_NET_EVENT_STACK_STARTED, 7u,
	             AIUS_NET_FAULT_NONE);
	assert(aius_network_teensy_diag(&adapter)->rx_ready_source ==
	       AIUS_NETWORK_TEENSY_RX_READY_DEADLINE);

	/* The generation that inherits a settled interface waits for nothing and
	 * spends no counter read. It is the one that binds in 19 ms today, and
	 * nothing here may slow it down. */
	reset_adapter(&adapter);
	aius_network_teensy_test_set_diag_primitives(&kDiagPrimitives);
	g_diag.netif_initialized = true;
	g_diag.frames_ok = true;
	g_fake.link = AIUS_NETWORK_TEENSY_LINK_UP;
	aius_network_teensy_apply(&adapter, 1000u,
	                         action(AIUS_NET_ACTION_START_STACK), &event);
	expect_event(event, AIUS_NET_EVENT_STACK_STARTED, 7u,
	             AIUS_NET_FAULT_NONE);
	assert(!adapter.rx_ready_required && !adapter.rx_ready_pending);
	assert(g_diag.frames_calls == 0u);
	assert(aius_network_teensy_diag(&adapter)->rx_ready_source ==
	       AIUS_NETWORK_TEENSY_RX_READY_NOT_REQUIRED);
	aius_network_teensy_poll(&adapter, 1001u, &event);
	expect_event(event, AIUS_NET_EVENT_NONE, 0u, AIUS_NET_FAULT_NONE);
	assert(g_diag.frames_calls == 0u);

	/* A link that comes up late arms the gate from the callback, not only
	 * from the stack-start read, and losing the link inside the wait still
	 * reaches the supervisor: the callback is an independent observer and
	 * its event is drained ahead of the counter read. */
	reset_adapter(&adapter);
	aius_network_teensy_test_set_diag_primitives(&kDiagPrimitives);
	g_diag.netif_initialized = false;
	g_diag.frames_ok = true;
	g_diag.rx = 40u;
	g_fake.link = AIUS_NETWORK_TEENSY_LINK_DOWN;
	aius_network_teensy_apply(&adapter, 1000u,
	                         action(AIUS_NET_ACTION_START_STACK), &event);
	expect_event(event, AIUS_NET_EVENT_NONE, 0u, AIUS_NET_FAULT_NONE);
	assert(adapter.rx_ready_required && !adapter.link_up_reported);
	aius_network_teensy_test_callback(&adapter, 7u,
	                                  AIUS_NET_EVENT_STACK_STARTED,
	                                  AIUS_NET_FAULT_NONE);
	aius_network_teensy_poll(&adapter, 1010u, &event);
	expect_event(event, AIUS_NET_EVENT_NONE, 0u, AIUS_NET_FAULT_NONE);
	assert(adapter.link_up_reported && adapter.rx_ready_pending);
	assert(aius_network_teensy_diag(&adapter)->link_up_source ==
	       AIUS_NETWORK_TEENSY_LINK_UP_CALLBACK);
	aius_network_teensy_test_callback(&adapter, 7u,
	                                  AIUS_NET_EVENT_RESOURCE_FAULT,
	                                  AIUS_NET_FAULT_LINK_LOST);
	aius_network_teensy_poll(&adapter, 1020u, &event);
	expect_event(event, AIUS_NET_EVENT_RESOURCE_FAULT, 7u,
	             AIUS_NET_FAULT_LINK_LOST);

	/* Teardown closes the gate with the generation that owned it, and the
	 * next generation decides for itself whether it needs one. */
	drive_teardown_at(&adapter, 7u, 1030u);
	assert(!adapter.rx_ready_required && !adapter.rx_ready_pending);
	g_diag.netif_initialized = true;
	g_fake.link = AIUS_NETWORK_TEENSY_LINK_UP;
	aius_network_teensy_apply(&adapter, 1040u,
	                         action(AIUS_NET_ACTION_START_STACK, 8u), &event);
	expect_event(event, AIUS_NET_EVENT_STACK_STARTED, 8u,
	             AIUS_NET_FAULT_NONE);
	assert(!adapter.rx_ready_required && !adapter.rx_ready_pending);
}

/* The diagnostic readers are optional, and the adapter drives identically
 * without them: this is the property that keeps a measurement from being able
 * to fault the transport, and it is what lets the runtime-target suite keep its
 * own sixteen-entry table unchanged.
 */
void test_diagnostics_are_optional()
{
	AiusNetworkTeensy adapter;
	uint32_t tx = 1u;
	uint32_t rx = 1u;

	reset_adapter(&adapter);
	make_online(&adapter);

	const AiusNetworkTeensyDiag *diag = aius_network_teensy_diag(&adapter);
	assert(diag->generation == 7u);
	assert(diag->end_reason == AIUS_NETWORK_TEENSY_END_ONLINE);
	assert(diag->netif_init == AIUS_NETWORK_TEENSY_NETIF_INIT_UNKNOWN);
	assert(diag->stack_start_blocking_ms == 0u);
	assert(diag->dhcp_state_at_start == AIUS_NETWORK_TEENSY_DHCP_STATE_NONE);
	assert(diag->dhcp_state_at_end == AIUS_NETWORK_TEENSY_DHCP_STATE_NONE);
	assert(diag->link_live_at_end == AIUS_NETWORK_TEENSY_LINK_UNKNOWN);
	assert(!diag->frames_valid);
	/* What the table does supply is still measured. */
	assert(diag->link_at_stack_start == AIUS_NETWORK_TEENSY_LINK_UP);
	assert(diag->link_up_source == AIUS_NETWORK_TEENSY_LINK_UP_STACK_START);
	/* And with no netif probe there is no readiness requirement, so the gate
	 * is transparent: this whole file's lifecycle, callback, deadline and I/O
	 * coverage drives an adapter the gate never arms on. */
	assert(diag->rx_ready_source ==
	       AIUS_NETWORK_TEENSY_RX_READY_NOT_REQUIRED);

	assert(aius_network_teensy_link_live(&adapter) ==
	       AIUS_NETWORK_TEENSY_LINK_UNKNOWN);
	assert(aius_network_teensy_dhcp_state(&adapter) ==
	       AIUS_NETWORK_TEENSY_DHCP_STATE_NONE);
	assert(!aius_network_teensy_frames(&adapter, &tx, &rx));
	assert(tx == 0u && rx == 0u);

	/* And every reader answers for a null adapter or a null out-parameter. */
	assert(aius_network_teensy_diag(nullptr) == nullptr);
	assert(aius_network_teensy_diag_first(nullptr) == nullptr);
	assert(aius_network_teensy_link_live(nullptr) ==
	       AIUS_NETWORK_TEENSY_LINK_UNKNOWN);
	assert(aius_network_teensy_dhcp_state(nullptr) ==
	       AIUS_NETWORK_TEENSY_DHCP_STATE_NONE);
	assert(!aius_network_teensy_frames(nullptr, &tx, &rx));
	assert(!aius_network_teensy_frames(&adapter, nullptr, &rx));
	assert(!aius_network_teensy_frames(&adapter, &tx, nullptr));
}

} // namespace

int main()
{
	test_init_and_action_matrix();
	test_action_resource_fault_matrix();
	test_action_protocol_and_preemption();
	test_teardown_order_and_fault_matrix();
	test_identical_teardown_reapplication_preserves_fault();
	test_callback_fifo_stale_duplicate_overflow_and_health();
	test_link_gate_before_dhcp();
	test_one_shot_io_matrix();
	test_status_accessors_across_lifecycle();
	test_generation_diagnostics();
	test_receive_readiness_gate();
	test_diagnostics_are_optional();
	return 0;
}
