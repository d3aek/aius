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

bool stack_start()
{
	return operation(OP_STACK_START);
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
};

AiusNetAction action(AiusNetActionKind kind, uint32_t generation = 7u)
{
	return {kind, generation};
}

void reset_fake()
{
	memset(&g_fake, 0, sizeof(g_fake));
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

	aius_network_teensy_apply(
		&adapter, 1u, action(AIUS_NET_ACTION_START_STACK), &event);
	expect_event(event, AIUS_NET_EVENT_STACK_STARTED, 7u,
	             AIUS_NET_FAULT_NONE);
	assert(adapter.stack_started);
	assert(g_fake.log_count == 2u);
	assert(g_fake.log[0] == OP_CALLBACKS_INSTALL);
	assert(g_fake.log[1] == OP_STACK_START);

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

	aius_network_teensy_test_callback(
		&adapter, 6u, AIUS_NET_EVENT_LEASE_ACQUIRED, AIUS_NET_FAULT_NONE);
	aius_network_teensy_test_callback(
		&adapter, 7u, AIUS_NET_EVENT_STACK_STARTED, AIUS_NET_FAULT_NONE);
	aius_network_teensy_test_callback(
		&adapter, 7u, AIUS_NET_EVENT_STACK_STARTED, AIUS_NET_FAULT_NONE);
	aius_network_teensy_poll(&adapter, 1u, &event);
	expect_event(event, AIUS_NET_EVENT_STACK_STARTED, 7u,
	             AIUS_NET_FAULT_NONE);
	assert(aius_network_teensy_test_stale_callbacks() == 1u);
	assert(g_fake.log_count == 0u);
	aius_network_teensy_poll(&adapter, 2u, &event);
	expect_event(event, AIUS_NET_EVENT_STACK_STARTED, 7u,
	             AIUS_NET_FAULT_NONE);
	assert(g_fake.log_count == 0u);

	aius_network_teensy_poll(&adapter, 3u, &event);
	expect_event(event, AIUS_NET_EVENT_NONE, 0u, AIUS_NET_FAULT_NONE);
	assert(g_fake.log_count == 1u);
	assert(g_fake.log[0] == OP_LINK_STATUS);

	g_fake.link = AIUS_NETWORK_TEENSY_LINK_DOWN;
	aius_network_teensy_poll(&adapter, 4u, &event);
	expect_event(event, AIUS_NET_EVENT_RESOURCE_FAULT, 7u,
	             AIUS_NET_FAULT_LINK_LOST);
	assert(g_fake.log_count == 2u);

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

} // namespace

int main()
{
	test_init_and_action_matrix();
	test_action_resource_fault_matrix();
	test_action_protocol_and_preemption();
	test_teardown_order_and_fault_matrix();
	test_identical_teardown_reapplication_preserves_fault();
	test_callback_fifo_stale_duplicate_overflow_and_health();
	test_one_shot_io_matrix();
	test_status_accessors_across_lifecycle();
	return 0;
}
