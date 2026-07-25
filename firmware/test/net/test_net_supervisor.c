#include "aius_io.h"
#include "aius_net.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void saturating_increment(uint32_t *value)
{
	if (*value != UINT32_MAX)
		(*value)++;
}

static int deadline_reached(uint32_t now_ms, uint32_t deadline_ms)
{
	return (int32_t)(now_ms - deadline_ms) >= 0;
}

static void action_none(AiusNetAction *action)
{
	action->kind = AIUS_NET_ACTION_NONE;
	action->generation = 0;
}

static void action_set(AiusNetAction *action, AiusNetActionKind kind,
	                   uint32_t generation)
{
	action->kind = kind;
	action->generation = generation;
}

typedef enum {
	EXPECT_NO_EVENT,
	EXPECT_IGNORE,
	EXPECT_DHCP_WAIT,
	EXPECT_ONLINE,
	EXPECT_TEARDOWN,
	EXPECT_BACKOFF,
	EXPECT_BACKOFF_CLEANUP_FAULT
} ExpectedTransition;

/* Rows are AiusNetState; columns are AiusNetEventKind. This is the normative
 * public transition oracle, deliberately represented as data rather than as a
 * second implementation of the production switch. */
static const ExpectedTransition transition_table[6][6] = {
	[AIUS_NET_STATE_STACK_START] = {
		EXPECT_NO_EVENT,
		EXPECT_DHCP_WAIT,
		EXPECT_IGNORE,
		EXPECT_TEARDOWN,
		EXPECT_IGNORE,
		EXPECT_IGNORE,
	},
	[AIUS_NET_STATE_DHCP_WAIT] = {
		EXPECT_NO_EVENT,
		EXPECT_IGNORE,
		EXPECT_ONLINE,
		EXPECT_TEARDOWN,
		EXPECT_IGNORE,
		EXPECT_IGNORE,
	},
	[AIUS_NET_STATE_ONLINE] = {
		EXPECT_NO_EVENT,
		EXPECT_IGNORE,
		EXPECT_IGNORE,
		EXPECT_TEARDOWN,
		EXPECT_IGNORE,
		EXPECT_IGNORE,
	},
	[AIUS_NET_STATE_TEARDOWN] = {
		EXPECT_NO_EVENT,
		EXPECT_IGNORE,
		EXPECT_IGNORE,
		EXPECT_IGNORE,
		EXPECT_BACKOFF,
		EXPECT_BACKOFF_CLEANUP_FAULT,
	},
	[AIUS_NET_STATE_BACKOFF] = {
		EXPECT_NO_EVENT,
		EXPECT_IGNORE,
		EXPECT_IGNORE,
		EXPECT_IGNORE,
		EXPECT_IGNORE,
		EXPECT_IGNORE,
	},
	[AIUS_NET_STATE_RECOVERY_SAFE] = {
		EXPECT_NO_EVENT,
		EXPECT_IGNORE,
		EXPECT_IGNORE,
		EXPECT_IGNORE,
		EXPECT_IGNORE,
		EXPECT_IGNORE,
	},
};

static void model_begin_teardown(AiusNetSupervisor *supervisor,
	                             AiusNetFault fault,
	                             AiusNetAction *action)
{
	supervisor->state = AIUS_NET_STATE_TEARDOWN;
	supervisor->deadline_ms = 0;
	supervisor->last_fault = fault;
	saturating_increment(&supervisor->restarts);
	action_set(action, AIUS_NET_ACTION_TEARDOWN, supervisor->generation);
}

static void table_model_step(AiusNetSupervisor *supervisor, uint32_t now_ms,
	                         AiusNetEvent event, AiusNetAction *action)
{
	action_none(action);
	if (event.kind != AIUS_NET_EVENT_NONE) {
		if (event.generation != supervisor->generation) {
			saturating_increment(&supervisor->ignored_events);
		} else {
			ExpectedTransition expected =
				transition_table[supervisor->state][event.kind];
			switch (expected) {
			case EXPECT_IGNORE:
				saturating_increment(
					&supervisor->ignored_events);
				break;
			case EXPECT_DHCP_WAIT:
				supervisor->state = AIUS_NET_STATE_DHCP_WAIT;
				supervisor->deadline_ms =
					now_ms + AIUS_NET_DHCP_DEADLINE_MS;
				action_set(action, AIUS_NET_ACTION_START_DHCP,
				           supervisor->generation);
				return;
			case EXPECT_ONLINE:
				supervisor->state = AIUS_NET_STATE_ONLINE;
				supervisor->deadline_ms = 0;
				action_set(action,
				           AIUS_NET_ACTION_PUBLISH_ONLINE,
				           supervisor->generation);
				return;
			case EXPECT_TEARDOWN:
				model_begin_teardown(supervisor, event.fault,
				                     action);
				return;
			case EXPECT_BACKOFF:
			case EXPECT_BACKOFF_CLEANUP_FAULT:
				if (expected ==
				    EXPECT_BACKOFF_CLEANUP_FAULT)
					saturating_increment(
						&supervisor->cleanup_faults);
				supervisor->state = AIUS_NET_STATE_BACKOFF;
				supervisor->deadline_ms =
					now_ms + AIUS_NET_BACKOFF_MS;
				return;
			case EXPECT_NO_EVENT:
				assert(0);
			}
		}
	}

	if (supervisor->state == AIUS_NET_STATE_DHCP_WAIT &&
	    deadline_reached(now_ms, supervisor->deadline_ms)) {
		model_begin_teardown(supervisor,
		                     AIUS_NET_FAULT_DHCP_TIMEOUT, action);
		return;
	}
	if (supervisor->state == AIUS_NET_STATE_BACKOFF &&
	    deadline_reached(now_ms, supervisor->deadline_ms)) {
		supervisor->generation++;
		if (supervisor->generation == 0)
			supervisor->generation = 1;
		supervisor->state = AIUS_NET_STATE_STACK_START;
		supervisor->deadline_ms = 0;
		action_set(action, AIUS_NET_ACTION_START_STACK,
		           supervisor->generation);
	}
}

static void assert_supervisor_equal(const AiusNetSupervisor *actual,
	                                const AiusNetSupervisor *expected)
{
	assert(actual->state == expected->state);
	assert(actual->generation == expected->generation);
	assert(actual->deadline_ms == expected->deadline_ms);
	assert(actual->restarts == expected->restarts);
	assert(actual->ignored_events == expected->ignored_events);
	assert(actual->cleanup_faults == expected->cleanup_faults);
	assert(actual->last_fault == expected->last_fault);
}

static void assert_action_equal(const AiusNetAction *actual,
	                            const AiusNetAction *expected)
{
	assert(actual->kind == expected->kind);
	assert(actual->generation == expected->generation);
}

static int state_has_live_generation(AiusNetState state)
{
	return state == AIUS_NET_STATE_STACK_START ||
	       state == AIUS_NET_STATE_DHCP_WAIT ||
	       state == AIUS_NET_STATE_ONLINE;
}

static int is_single_transition(AiusNetState before, AiusNetState after)
{
	if (before == after)
		return 1;
	switch (before) {
	case AIUS_NET_STATE_STACK_START:
		return after == AIUS_NET_STATE_DHCP_WAIT ||
		       after == AIUS_NET_STATE_TEARDOWN;
	case AIUS_NET_STATE_DHCP_WAIT:
		return after == AIUS_NET_STATE_ONLINE ||
		       after == AIUS_NET_STATE_TEARDOWN;
	case AIUS_NET_STATE_ONLINE:
		return after == AIUS_NET_STATE_TEARDOWN;
	case AIUS_NET_STATE_TEARDOWN:
		return after == AIUS_NET_STATE_BACKOFF;
	case AIUS_NET_STATE_BACKOFF:
		return after == AIUS_NET_STATE_STACK_START;
	case AIUS_NET_STATE_RECOVERY_SAFE:
		return 0;
	}
	return 0;
}

static void assert_step_invariants(const AiusNetSupervisor *before,
	                               const AiusNetSupervisor *after,
	                               const AiusNetAction *action)
{
	assert(after->state >= AIUS_NET_STATE_STACK_START);
	assert(after->state <= AIUS_NET_STATE_RECOVERY_SAFE);
	assert(is_single_transition(before->state, after->state));
	if (after->state == AIUS_NET_STATE_RECOVERY_SAFE)
		assert(after->generation == 0);
	else
		assert(after->generation != 0);
	if (after->state == AIUS_NET_STATE_TEARDOWN ||
	    after->state == AIUS_NET_STATE_BACKOFF) {
		assert(!state_has_live_generation(after->state));
		assert(after->generation == before->generation);
		assert(action->kind == AIUS_NET_ACTION_NONE ||
		       action->kind == AIUS_NET_ACTION_TEARDOWN);
	}
	assert(action->kind >= AIUS_NET_ACTION_NONE);
	assert(action->kind <= AIUS_NET_ACTION_PUBLISH_ONLINE);
	if (action->kind == AIUS_NET_ACTION_NONE) {
		assert(action->generation == 0);
		return;
	}

	assert(action->generation != 0);
	switch (action->kind) {
	case AIUS_NET_ACTION_START_STACK:
		assert(before->state == AIUS_NET_STATE_BACKOFF);
		assert(after->state == AIUS_NET_STATE_STACK_START);
		assert(state_has_live_generation(after->state));
		assert(action->generation == after->generation);
		break;
	case AIUS_NET_ACTION_START_DHCP:
		assert(before->state == AIUS_NET_STATE_STACK_START);
		assert(after->state == AIUS_NET_STATE_DHCP_WAIT);
		assert(action->generation == before->generation);
		assert(action->generation == after->generation);
		break;
	case AIUS_NET_ACTION_TEARDOWN:
		assert(state_has_live_generation(before->state));
		assert(after->state == AIUS_NET_STATE_TEARDOWN);
		assert(!state_has_live_generation(after->state));
		assert(action->generation == before->generation);
		assert(action->generation == after->generation);
		break;
	case AIUS_NET_ACTION_PUBLISH_ONLINE:
		assert(before->state == AIUS_NET_STATE_DHCP_WAIT);
		assert(after->state == AIUS_NET_STATE_ONLINE);
		assert(state_has_live_generation(after->state));
		assert(action->generation == after->generation);
		break;
	case AIUS_NET_ACTION_NONE:
		assert(0);
	}
}

static void assert_stale_only_ignored(const AiusNetSupervisor *before,
	                                  const AiusNetSupervisor *after,
	                                  const AiusNetAction *action)
{
	AiusNetSupervisor expected = *before;
	saturating_increment(&expected.ignored_events);
	assert_supervisor_equal(after, &expected);
	assert(action->kind == AIUS_NET_ACTION_NONE);
	assert(action->generation == 0);
}

static AiusNetSupervisor supervisor_for_state(AiusNetState state)
{
	AiusNetSupervisor supervisor = {
		.state = state,
		.generation = state == AIUS_NET_STATE_RECOVERY_SAFE ? 0u : 17u,
		.deadline_ms = 1000u,
		.restarts = 3u,
		.ignored_events = 4u,
		.cleanup_faults = 5u,
		.last_fault = AIUS_NET_FAULT_UDP,
	};
	return supervisor;
}

static void test_io_abi(void)
{
	AiusIoEvent event = {
		.result = AIUS_IO_PROGRESS,
		.bytes = 7,
		.fault = 0,
	};
	assert(event.result == AIUS_IO_PROGRESS);
	assert(event.bytes == 7);
	assert(event.fault == 0);
}

static void test_initialization_and_null_rejection(void)
{
	AiusNetSupervisor supervisor;
	AiusNetAction action = {
		.kind = AIUS_NET_ACTION_TEARDOWN,
		.generation = 99,
	};
	memset(&supervisor, 0xa5, sizeof(supervisor));
	aius_net_supervisor_init(&supervisor, UINT32_MAX, false, &action);
	assert(supervisor.state == AIUS_NET_STATE_STACK_START);
	assert(supervisor.generation == 1);
	assert(supervisor.deadline_ms == 0);
	assert(supervisor.restarts == 0);
	assert(supervisor.ignored_events == 0);
	assert(supervisor.cleanup_faults == 0);
	assert(supervisor.last_fault == AIUS_NET_FAULT_NONE);
	assert(action.kind == AIUS_NET_ACTION_START_STACK);
	assert(action.generation == 1);

	aius_net_supervisor_init(&supervisor, 123, true, &action);
	assert(supervisor.state == AIUS_NET_STATE_RECOVERY_SAFE);
	assert(supervisor.generation == 0);
	assert(action.kind == AIUS_NET_ACTION_NONE);
	assert(action.generation == 0);

	AiusNetSupervisor untouched = supervisor_for_state(AIUS_NET_STATE_ONLINE);
	AiusNetSupervisor before = untouched;
	aius_net_supervisor_init(&untouched, 0, false, NULL);
	assert_supervisor_equal(&untouched, &before);

	AiusNetAction untouched_action = {
		.kind = AIUS_NET_ACTION_TEARDOWN,
		.generation = 42,
	};
	aius_net_supervisor_init(NULL, 0, false, &untouched_action);
	assert(untouched_action.kind == AIUS_NET_ACTION_TEARDOWN);
	assert(untouched_action.generation == 42);

	AiusNetEvent event = {
		.kind = AIUS_NET_EVENT_RESOURCE_FAULT,
		.generation = before.generation,
		.fault = AIUS_NET_FAULT_LINK_LOST,
	};
	aius_net_supervisor_step(&untouched, 0, event, NULL);
	assert_supervisor_equal(&untouched, &before);
	aius_net_supervisor_step(NULL, 0, event, &untouched_action);
	assert(untouched_action.kind == AIUS_NET_ACTION_TEARDOWN);
	assert(untouched_action.generation == 42);
}

static void test_exhaustive_state_event_matrix(void)
{
	for (int state = AIUS_NET_STATE_STACK_START;
	     state <= AIUS_NET_STATE_RECOVERY_SAFE; state++) {
		for (int kind = AIUS_NET_EVENT_NONE;
		     kind <= AIUS_NET_EVENT_TEARDOWN_FAULT; kind++) {
			AiusNetSupervisor actual =
				supervisor_for_state((AiusNetState)state);
			AiusNetSupervisor expected = actual;
			AiusNetSupervisor before = actual;
			AiusNetEvent event = {
				.kind = (AiusNetEventKind)kind,
				.generation = actual.generation,
				.fault = AIUS_NET_FAULT_SOCKET,
			};
			AiusNetAction action = {
				.kind = AIUS_NET_ACTION_TEARDOWN,
				.generation = UINT32_MAX,
			};
			AiusNetAction expected_action;
			table_model_step(&expected, 100, event,
			                 &expected_action);
			aius_net_supervisor_step(&actual, 100, event, &action);
			assert_supervisor_equal(&actual, &expected);
			assert_action_equal(&action, &expected_action);
			assert_step_invariants(&before, &actual, &action);
		}
	}
}

static void test_deadline_precedence_and_wrap(void)
{
	AiusNetSupervisor supervisor =
		supervisor_for_state(AIUS_NET_STATE_DHCP_WAIT);
	supervisor.deadline_ms = 500;
	AiusNetEvent lease = {
		.kind = AIUS_NET_EVENT_LEASE_ACQUIRED,
		.generation = supervisor.generation,
		.fault = AIUS_NET_FAULT_NONE,
	};
	AiusNetAction action;
	aius_net_supervisor_step(&supervisor, 500, lease, &action);
	assert(supervisor.state == AIUS_NET_STATE_ONLINE);
	assert(action.kind == AIUS_NET_ACTION_PUBLISH_ONLINE);

	supervisor = supervisor_for_state(AIUS_NET_STATE_DHCP_WAIT);
	supervisor.deadline_ms = 500;
	AiusNetEvent stale_lease = lease;
	stale_lease.generation = supervisor.generation - 1;
	aius_net_supervisor_step(&supervisor, 500, stale_lease, &action);
	assert(supervisor.state == AIUS_NET_STATE_TEARDOWN);
	assert(supervisor.ignored_events == 5);
	assert(supervisor.restarts == 4);
	assert(supervisor.last_fault == AIUS_NET_FAULT_DHCP_TIMEOUT);
	assert(action.kind == AIUS_NET_ACTION_TEARDOWN);
	assert(action.generation == 17);

	supervisor = supervisor_for_state(AIUS_NET_STATE_DHCP_WAIT);
	supervisor.deadline_ms = 10;
	AiusNetEvent none = {
		.kind = AIUS_NET_EVENT_NONE,
		.generation = 0,
		.fault = AIUS_NET_FAULT_NONE,
	};
	aius_net_supervisor_step(&supervisor, UINT32_MAX - 2u, none,
	                         &action);
	assert(supervisor.state == AIUS_NET_STATE_DHCP_WAIT);
	assert(action.kind == AIUS_NET_ACTION_NONE);
	aius_net_supervisor_step(&supervisor, 10, none, &action);
	assert(supervisor.state == AIUS_NET_STATE_TEARDOWN);

	supervisor = supervisor_for_state(AIUS_NET_STATE_STACK_START);
	AiusNetEvent started = {
		.kind = AIUS_NET_EVENT_STACK_STARTED,
		.generation = supervisor.generation,
		.fault = AIUS_NET_FAULT_NONE,
	};
	aius_net_supervisor_step(&supervisor, UINT32_MAX - 1000u, started,
	                         &action);
	assert(supervisor.deadline_ms ==
	       UINT32_MAX - 1000u + AIUS_NET_DHCP_DEADLINE_MS);
	assert(action.kind == AIUS_NET_ACTION_START_DHCP);

	supervisor = supervisor_for_state(AIUS_NET_STATE_BACKOFF);
	supervisor.deadline_ms = 25;
	aius_net_supervisor_step(&supervisor, UINT32_MAX - 4u, none,
	                         &action);
	assert(supervisor.state == AIUS_NET_STATE_BACKOFF);
	aius_net_supervisor_step(&supervisor, 25, none, &action);
	assert(supervisor.state == AIUS_NET_STATE_STACK_START);
	assert(supervisor.generation == 18);
	assert(action.kind == AIUS_NET_ACTION_START_STACK);
}

static void test_generation_wrap_and_counter_saturation(void)
{
	AiusNetSupervisor supervisor =
		supervisor_for_state(AIUS_NET_STATE_BACKOFF);
	supervisor.generation = UINT32_MAX;
	supervisor.deadline_ms = 77;
	AiusNetEvent none = {
		.kind = AIUS_NET_EVENT_NONE,
		.generation = 0,
		.fault = AIUS_NET_FAULT_NONE,
	};
	AiusNetAction action;
	aius_net_supervisor_step(&supervisor, 77, none, &action);
	assert(supervisor.state == AIUS_NET_STATE_STACK_START);
	assert(supervisor.generation == 1);
	assert(action.kind == AIUS_NET_ACTION_START_STACK);
	assert(action.generation == 1);

	supervisor = supervisor_for_state(AIUS_NET_STATE_ONLINE);
	supervisor.restarts = UINT32_MAX;
	AiusNetEvent fault = {
		.kind = AIUS_NET_EVENT_RESOURCE_FAULT,
		.generation = supervisor.generation,
		.fault = AIUS_NET_FAULT_LINK_LOST,
	};
	aius_net_supervisor_step(&supervisor, 0, fault, &action);
	assert(supervisor.restarts == UINT32_MAX);

	supervisor = supervisor_for_state(AIUS_NET_STATE_ONLINE);
	supervisor.ignored_events = UINT32_MAX;
	AiusNetEvent stale = fault;
	stale.generation--;
	aius_net_supervisor_step(&supervisor, 0, stale, &action);
	assert(supervisor.ignored_events == UINT32_MAX);

	supervisor = supervisor_for_state(AIUS_NET_STATE_TEARDOWN);
	supervisor.cleanup_faults = UINT32_MAX;
	AiusNetEvent teardown_fault = {
		.kind = AIUS_NET_EVENT_TEARDOWN_FAULT,
		.generation = supervisor.generation,
		.fault = AIUS_NET_FAULT_TEARDOWN,
	};
	aius_net_supervisor_step(&supervisor, 100, teardown_fault, &action);
	assert(supervisor.state == AIUS_NET_STATE_BACKOFF);
	assert(supervisor.cleanup_faults == UINT32_MAX);
	assert(supervisor.last_fault == AIUS_NET_FAULT_UDP);
	assert(action.kind == AIUS_NET_ACTION_NONE);
}

static void test_duplicates_and_reachability(void)
{
	AiusNetSupervisor supervisor;
	AiusNetAction action;
	aius_net_supervisor_init(&supervisor, 0, false, &action);
	AiusNetEvent started = {
		.kind = AIUS_NET_EVENT_STACK_STARTED,
		.generation = supervisor.generation,
		.fault = AIUS_NET_FAULT_NONE,
	};
	aius_net_supervisor_step(&supervisor, 1, started, &action);
	assert(supervisor.state == AIUS_NET_STATE_DHCP_WAIT);
	assert(action.kind == AIUS_NET_ACTION_START_DHCP);
	aius_net_supervisor_step(&supervisor, 2, started, &action);
	assert(supervisor.state == AIUS_NET_STATE_DHCP_WAIT);
	assert(supervisor.ignored_events == 1);
	assert(action.kind == AIUS_NET_ACTION_NONE);

	AiusNetEvent lease = {
		.kind = AIUS_NET_EVENT_LEASE_ACQUIRED,
		.generation = supervisor.generation,
		.fault = AIUS_NET_FAULT_NONE,
	};
	aius_net_supervisor_step(&supervisor, 3, lease, &action);
	assert(supervisor.state == AIUS_NET_STATE_ONLINE);
	assert(action.kind == AIUS_NET_ACTION_PUBLISH_ONLINE);
	aius_net_supervisor_step(&supervisor, 4, lease, &action);
	assert(supervisor.state == AIUS_NET_STATE_ONLINE);
	assert(supervisor.ignored_events == 2);
	assert(action.kind == AIUS_NET_ACTION_NONE);

	AiusNetEvent fault = {
		.kind = AIUS_NET_EVENT_RESOURCE_FAULT,
		.generation = supervisor.generation,
		.fault = AIUS_NET_FAULT_LINK_LOST,
	};
	aius_net_supervisor_step(&supervisor, 5, fault, &action);
	assert(supervisor.state == AIUS_NET_STATE_TEARDOWN);
	AiusNetEvent done = {
		.kind = AIUS_NET_EVENT_TEARDOWN_DONE,
		.generation = supervisor.generation,
		.fault = AIUS_NET_FAULT_NONE,
	};
	aius_net_supervisor_step(&supervisor, 6, done, &action);
	assert(supervisor.state == AIUS_NET_STATE_BACKOFF);

	aius_net_supervisor_init(&supervisor, 0, true, &action);
	assert(supervisor.state == AIUS_NET_STATE_RECOVERY_SAFE);
}

/* STACK_START is the state the target adapter's link gate sits in, so pin the
 * two properties the gate depends on.
 *
 * First, STACK_STARTED is the only way to START_DHCP: the supervisor asks for
 * DHCP because it was told the stack is ready, never because time passed. That
 * is what lets the adapter withhold STACK_STARTED until it has actually
 * observed link-up, and it is why a DISCOVER can no longer be sent onto a link
 * that has not negotiated.
 *
 * Second, STACK_START has no deadline of its own -- unlike DHCP_WAIT and
 * BACKOFF, no amount of elapsed time moves it. Withholding STACK_STARTED
 * forever would therefore be a permanent stall, so the boundedness of the wait
 * has to come from the adapter (AIUS_NETWORK_TEENSY_LINK_DEADLINE_MS), which
 * reaches the supervisor as a current-generation RESOURCE_FAULT. This asserts
 * the stall so the guarantee cannot be silently moved here later.
 */
static void test_stack_start_waits_for_its_event(void)
{
	AiusNetSupervisor supervisor;
	AiusNetAction action;
	AiusNetEvent none = {
		.kind = AIUS_NET_EVENT_NONE,
		.generation = 0,
		.fault = AIUS_NET_FAULT_NONE,
	};

	aius_net_supervisor_init(&supervisor, 0, false, &action);
	assert(supervisor.state == AIUS_NET_STATE_STACK_START);

	/* No event, arbitrarily far into the future, still STACK_START. */
	const uint32_t clocks[] = {1u, 1000u, AIUS_NET_DHCP_DEADLINE_MS,
	                           AIUS_NET_DHCP_DEADLINE_MS * 10u,
	                           UINT32_MAX / 2u, UINT32_MAX};
	for (size_t index = 0; index < sizeof(clocks) / sizeof(clocks[0]);
	     index++) {
		aius_net_supervisor_step(&supervisor, clocks[index], none,
		                         &action);
		assert(supervisor.state == AIUS_NET_STATE_STACK_START);
		assert(supervisor.generation == 1);
		assert(action.kind == AIUS_NET_ACTION_NONE);
	}

	/* Every other current-generation event either does nothing or tears
	 * down; none of them starts DHCP. */
	for (int kind = AIUS_NET_EVENT_LEASE_ACQUIRED;
	     kind <= AIUS_NET_EVENT_TEARDOWN_FAULT; kind++) {
		AiusNetSupervisor probe;
		AiusNetEvent event = {
			.kind = (AiusNetEventKind)kind,
			.generation = 1,
			.fault = AIUS_NET_FAULT_LINK_LOST,
		};
		aius_net_supervisor_init(&probe, 0, false, &action);
		aius_net_supervisor_step(&probe, 10, event, &action);
		assert(action.kind != AIUS_NET_ACTION_START_DHCP);
		assert(probe.state != AIUS_NET_STATE_DHCP_WAIT);
	}

	/* The adapter's link deadline arrives as a resource fault, and the
	 * supervisor turns it into bounded recovery: teardown, backoff, next
	 * generation, which starts its own bounded wait. */
	AiusNetEvent link_timeout = {
		.kind = AIUS_NET_EVENT_RESOURCE_FAULT,
		.generation = 1,
		.fault = AIUS_NET_FAULT_LINK_LOST,
	};
	aius_net_supervisor_init(&supervisor, 0, false, &action);
	aius_net_supervisor_step(&supervisor, 3000, link_timeout, &action);
	assert(supervisor.state == AIUS_NET_STATE_TEARDOWN);
	assert(supervisor.last_fault == AIUS_NET_FAULT_LINK_LOST);
	assert(supervisor.restarts == 1);
	assert(action.kind == AIUS_NET_ACTION_TEARDOWN);
	assert(action.generation == 1);

	AiusNetEvent done = {
		.kind = AIUS_NET_EVENT_TEARDOWN_DONE,
		.generation = 1,
		.fault = AIUS_NET_FAULT_NONE,
	};
	aius_net_supervisor_step(&supervisor, 3001, done, &action);
	assert(supervisor.state == AIUS_NET_STATE_BACKOFF);
	aius_net_supervisor_step(&supervisor, 3001 + AIUS_NET_BACKOFF_MS, none,
	                         &action);
	assert(supervisor.state == AIUS_NET_STATE_STACK_START);
	assert(supervisor.generation == 2);
	assert(action.kind == AIUS_NET_ACTION_START_STACK);
	assert(action.generation == 2);

	/* And the new generation waits for its own STACK_STARTED. */
	aius_net_supervisor_step(&supervisor, 3001 + AIUS_NET_BACKOFF_MS * 100u,
	                         none, &action);
	assert(supervisor.state == AIUS_NET_STATE_STACK_START);
	assert(action.kind == AIUS_NET_ACTION_NONE);

	AiusNetEvent started = {
		.kind = AIUS_NET_EVENT_STACK_STARTED,
		.generation = 2,
		.fault = AIUS_NET_FAULT_NONE,
	};
	aius_net_supervisor_step(&supervisor, 9000, started, &action);
	assert(supervisor.state == AIUS_NET_STATE_DHCP_WAIT);
	assert(action.kind == AIUS_NET_ACTION_START_DHCP);
	assert(supervisor.deadline_ms == 9000 + AIUS_NET_DHCP_DEADLINE_MS);
}

/* The Teensy adapter delays STACK_STARTED twice over before DHCP may start: once
 * while it waits for link-up, and again while it waits for evidence that the
 * interface is actually carrying traffic, because the board reports link up for
 * seconds before the FEC passes a frame. Both waits are bounded there. What has
 * to be true here is that the supervisor charges neither of them to DHCP: the
 * deadline is armed when STACK_STARTED arrives, however late that is, so a
 * delayed attempt is a whole attempt rather than a truncated one.
 */
static void test_delayed_stack_started_keeps_full_dhcp_budget(void)
{
	/* Nothing in the adapter can make a delay longer than its two bounds,
	 * but the supervisor's answer must not depend on the figure at all --
	 * including across a millis() wrap, where a naive comparison would place
	 * the deadline in the past. */
	const uint32_t starts[] = {0u,
	                           1u,
	                           1635u,
	                           3000u + AIUS_NET_DHCP_DEADLINE_MS,
	                           AIUS_NET_DHCP_DEADLINE_MS * 100u,
	                           UINT32_MAX - 1u,
	                           UINT32_MAX};

	for (size_t index = 0; index < sizeof(starts) / sizeof(starts[0]);
	     index++) {
		AiusNetSupervisor supervisor;
		AiusNetAction action;
		AiusNetEvent none = {
			.kind = AIUS_NET_EVENT_NONE,
			.generation = 0,
			.fault = AIUS_NET_FAULT_NONE,
		};
		AiusNetEvent started = {
			.kind = AIUS_NET_EVENT_STACK_STARTED,
			.generation = 1,
			.fault = AIUS_NET_FAULT_NONE,
		};

		aius_net_supervisor_init(&supervisor, 0, false, &action);
		assert(action.kind == AIUS_NET_ACTION_START_STACK);

		aius_net_supervisor_step(&supervisor, starts[index], started,
		                         &action);
		assert(supervisor.state == AIUS_NET_STATE_DHCP_WAIT);
		assert(action.kind == AIUS_NET_ACTION_START_DHCP);
		assert(supervisor.deadline_ms ==
		       (uint32_t)(starts[index] + AIUS_NET_DHCP_DEADLINE_MS));

		/* One millisecond short of the deadline is still DHCP_WAIT, so
		 * the delayed attempt genuinely gets its full budget. */
		aius_net_supervisor_step(
			&supervisor,
			(uint32_t)(starts[index] + AIUS_NET_DHCP_DEADLINE_MS - 1u),
			none, &action);
		assert(supervisor.state == AIUS_NET_STATE_DHCP_WAIT);
		assert(action.kind == AIUS_NET_ACTION_NONE);

		/* A lease at the last moment is still a lease. */
		AiusNetEvent lease = {
			.kind = AIUS_NET_EVENT_LEASE_ACQUIRED,
			.generation = 1,
			.fault = AIUS_NET_FAULT_NONE,
		};
		aius_net_supervisor_step(
			&supervisor,
			(uint32_t)(starts[index] + AIUS_NET_DHCP_DEADLINE_MS),
			lease, &action);
		assert(supervisor.state == AIUS_NET_STATE_ONLINE);
		assert(action.kind == AIUS_NET_ACTION_PUBLISH_ONLINE);
	}

	/* And a delayed attempt that does expire is still bounded recovery, not
	 * a stall: the composition of the adapter's waits with this deadline
	 * reaches BACKOFF and the next generation exactly as a prompt one does.
	 */
	AiusNetSupervisor supervisor;
	AiusNetAction action;
	AiusNetEvent none = {
		.kind = AIUS_NET_EVENT_NONE,
		.generation = 0,
		.fault = AIUS_NET_FAULT_NONE,
	};
	AiusNetEvent started = {
		.kind = AIUS_NET_EVENT_STACK_STARTED,
		.generation = 1,
		.fault = AIUS_NET_FAULT_NONE,
	};
	AiusNetEvent done = {
		.kind = AIUS_NET_EVENT_TEARDOWN_DONE,
		.generation = 1,
		.fault = AIUS_NET_FAULT_NONE,
	};
	const uint32_t late = 3000u + AIUS_NET_DHCP_DEADLINE_MS;

	aius_net_supervisor_init(&supervisor, 0, false, &action);
	aius_net_supervisor_step(&supervisor, late, started, &action);
	assert(supervisor.state == AIUS_NET_STATE_DHCP_WAIT);

	aius_net_supervisor_step(&supervisor, late + AIUS_NET_DHCP_DEADLINE_MS,
	                         none, &action);
	assert(supervisor.state == AIUS_NET_STATE_TEARDOWN);
	assert(supervisor.last_fault == AIUS_NET_FAULT_DHCP_TIMEOUT);
	assert(action.kind == AIUS_NET_ACTION_TEARDOWN);
	assert(action.generation == 1);

	aius_net_supervisor_step(&supervisor, late + AIUS_NET_DHCP_DEADLINE_MS,
	                         done, &action);
	assert(supervisor.state == AIUS_NET_STATE_BACKOFF);
	aius_net_supervisor_step(&supervisor,
	                         late + AIUS_NET_DHCP_DEADLINE_MS +
	                             AIUS_NET_BACKOFF_MS,
	                         none, &action);
	assert(supervisor.state == AIUS_NET_STATE_STACK_START);
	assert(supervisor.generation == 2);
	assert(action.kind == AIUS_NET_ACTION_START_STACK);
}

static uint32_t random_next(uint32_t *state)
{
	*state = *state * 1664525u + 1013904223u;
	return *state;
}

static int model_deadline_due(const AiusNetSupervisor *supervisor,
	                          uint32_t now_ms)
{
	return (supervisor->state == AIUS_NET_STATE_DHCP_WAIT ||
	        supervisor->state == AIUS_NET_STATE_BACKOFF) &&
	       deadline_reached(now_ms, supervisor->deadline_ms);
}

static void test_generated_sequences(void)
{
	int saw_online = 0;
	int saw_backoff = 0;
	int saw_recovery_safe = 0;
	uint32_t stale_events = 0;

	for (uint32_t sequence = 0; sequence < 10000u; sequence++) {
		uint32_t random = sequence + 1u;
		uint32_t now_ms = random_next(&random);
		bool recovery_safe = sequence % 17u == 0;
		AiusNetSupervisor actual;
		AiusNetSupervisor expected = {
			.state = recovery_safe
				         ? AIUS_NET_STATE_RECOVERY_SAFE
				         : AIUS_NET_STATE_STACK_START,
			.generation = recovery_safe ? 0u : 1u,
			.deadline_ms = 0,
			.restarts = 0,
			.ignored_events = 0,
			.cleanup_faults = 0,
			.last_fault = AIUS_NET_FAULT_NONE,
		};
		AiusNetAction action;
		AiusNetAction expected_action = {
			.kind = recovery_safe ? AIUS_NET_ACTION_NONE
			                      : AIUS_NET_ACTION_START_STACK,
			.generation = recovery_safe ? 0u : 1u,
		};
		aius_net_supervisor_init(&actual, now_ms, recovery_safe,
		                         &action);
		assert_supervisor_equal(&actual, &expected);
		assert_action_equal(&action, &expected_action);

		for (unsigned step = 0; step < 64u; step++) {
			now_ms += random_next(&random) % 2001u;
			AiusNetEvent event = {
				.kind = (AiusNetEventKind)(
					random_next(&random) % 6u),
				.generation = expected.generation,
				.fault = (AiusNetFault)(
					1u + random_next(&random) % 10u),
			};
			int make_stale = (random_next(&random) & 3u) == 0;
			if (make_stale && model_deadline_due(&expected, now_ms))
				make_stale = 0;
			if (make_stale)
				event.generation =
					expected.generation == UINT32_MAX
						? 1u
						: expected.generation + 1u;
			if (step % 8u == 0 &&
			    expected.state == AIUS_NET_STATE_STACK_START) {
				event.kind = AIUS_NET_EVENT_STACK_STARTED;
				event.generation = expected.generation;
				make_stale = 0;
			} else if (step % 8u == 1 &&
			           expected.state == AIUS_NET_STATE_DHCP_WAIT) {
				event.kind = AIUS_NET_EVENT_LEASE_ACQUIRED;
				event.generation = expected.generation;
				make_stale = 0;
			} else if (step % 8u == 2 &&
			           expected.state == AIUS_NET_STATE_TEARDOWN) {
				event.kind = AIUS_NET_EVENT_TEARDOWN_DONE;
				event.generation = expected.generation;
				make_stale = 0;
			}

			AiusNetSupervisor before = expected;
			table_model_step(&expected, now_ms, event,
			                 &expected_action);
			action.kind = AIUS_NET_ACTION_TEARDOWN;
			action.generation = UINT32_MAX;
			aius_net_supervisor_step(&actual, now_ms, event,
			                         &action);
			assert_supervisor_equal(&actual, &expected);
			assert_action_equal(&action, &expected_action);
			assert_step_invariants(&before, &actual, &action);
			if (make_stale &&
			    event.kind != AIUS_NET_EVENT_NONE) {
				assert_stale_only_ignored(&before, &actual,
				                          &action);
				stale_events++;
			}
			saw_online |= actual.state == AIUS_NET_STATE_ONLINE;
			saw_backoff |= actual.state == AIUS_NET_STATE_BACKOFF;
			saw_recovery_safe |=
				actual.state == AIUS_NET_STATE_RECOVERY_SAFE;
		}
	}
	assert(saw_online);
	assert(saw_backoff);
	assert(saw_recovery_safe);
	assert(stale_events > 10000u);
}

int main(void)
{
	test_io_abi();
	test_initialization_and_null_rejection();
	test_exhaustive_state_event_matrix();
	test_deadline_precedence_and_wrap();
	test_generation_wrap_and_counter_saturation();
	test_duplicates_and_reachability();
	test_stack_start_waits_for_its_event();
	test_delayed_stack_started_keeps_full_dhcp_budget();
	test_generated_sequences();
	puts("ok network supervisor");
	return 0;
}
