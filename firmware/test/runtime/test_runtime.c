#include "aius_runtime.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARRAY_COUNT(values) (sizeof(values) / sizeof((values)[0]))
#define MAX_TRACE_EVENTS 22u

static const char *current_key = "startup";

#define CHECK(condition) check((condition), #condition, __LINE__)

static void check(int condition, const char *expression, int line)
{
	if (condition)
		return;
	fprintf(stderr, "runtime oracle mismatch [%s] line %d: %s\n",
	        current_key, line, expression);
	exit(1);
}

typedef struct {
	const char *key;
	uint16_t checkpoint;
	uint16_t max_calls;
	uint16_t max_rx_bytes;
	uint16_t max_tx_bytes;
	uint32_t service_budget_ms;
} BudgetCase;

static const BudgetCase budget_cases[] = {
	{"budget.reset-capture", AIUS_BOOT_CHECKPOINT_RESET_CAPTURE,
	 2u, 0u, 0u, 250u},
	{"budget.usb-rx", AIUS_BOOT_CHECKPOINT_USB_RX,
	 64u, 64u, 0u, 0u},
	{"budget.usb-tx", AIUS_BOOT_CHECKPOINT_USB_TX,
	 1u, 0u, 64u, 500u},
	/* 1 ms: the measurement loopback's sliced analysis is hosted here and
	 * "no epoch computes more than its budget" must bind to a declared
	 * number (aius_runtime.c's row comment). */
	{"budget.metrics", AIUS_BOOT_CHECKPOINT_METRICS,
	 1u, 0u, 0u, 1u},
	{"budget.network", AIUS_BOOT_CHECKPOINT_NETWORK,
	 1u, 0u, 0u, 0u},
	{"budget.http", AIUS_BOOT_CHECKPOINT_HTTP,
	 1u, 256u, 512u, 2u},
	{"budget.udp", AIUS_BOOT_CHECKPOINT_UDP,
	 1u, 532u, 532u, 2u},
	{"budget.replay", AIUS_BOOT_CHECKPOINT_REPLAY,
	 1u, 2048u, 0u, 0u},
	{"budget.sd", AIUS_BOOT_CHECKPOINT_SD,
	 1u, 512u, 512u, 0u},
	{"budget.audio", AIUS_BOOT_CHECKPOINT_AUDIO,
	 1u, 0u, 0u, 8u},
	{"budget.epoch-complete", AIUS_BOOT_CHECKPOINT_EPOCH_COMPLETE,
	 0u, 0u, 0u, 0u},
};

static const AiusRuntimeBudget *stable_budget_pointers[11];

static void validate_unique_keys(
	const char *registry_key,
	const char *const *keys,
	size_t count,
	size_t expected_count)
{
	current_key = registry_key;
	CHECK(count == expected_count);
	for (size_t i = 0; i < count; ++i) {
		CHECK(keys[i] != NULL);
		CHECK(keys[i][0] != '\0');
		for (size_t j = i + 1u; j < count; ++j)
			CHECK(strcmp(keys[i], keys[j]) != 0);
	}
}

static const BudgetCase *budget_case(uint16_t checkpoint)
{
	for (size_t i = 0; i < ARRAY_COUNT(budget_cases); ++i)
		if (budget_cases[i].checkpoint == checkpoint)
			return &budget_cases[i];
	return NULL;
}

static void check_budget_value(
	const AiusRuntimeBudget *budget,
	const BudgetCase *expected)
{
	CHECK(budget != NULL);
	CHECK(expected != NULL);
	CHECK(budget->checkpoint == expected->checkpoint);
	CHECK(budget->max_calls == expected->max_calls);
	CHECK(budget->max_rx_bytes == expected->max_rx_bytes);
	CHECK(budget->max_tx_bytes == expected->max_tx_bytes);
	CHECK(budget->service_budget_ms == expected->service_budget_ms);
}

static void test_budget_registry(void)
{
	const char *keys[ARRAY_COUNT(budget_cases)];

	for (size_t i = 0; i < ARRAY_COUNT(budget_cases); ++i)
		keys[i] = budget_cases[i].key;
	validate_unique_keys(
		"registry.budgets", keys, ARRAY_COUNT(budget_cases), 11u);

	for (size_t i = 0; i < ARRAY_COUNT(budget_cases); ++i) {
		const BudgetCase *expected = &budget_cases[i];
		current_key = expected->key;
		for (size_t j = i + 1u; j < ARRAY_COUNT(budget_cases); ++j)
			CHECK(expected->checkpoint != budget_cases[j].checkpoint);
		const AiusRuntimeBudget *first =
			aius_runtime_budget(expected->checkpoint);
		const AiusRuntimeBudget *second =
			aius_runtime_budget(expected->checkpoint);
		CHECK(first == second);
		check_budget_value(first, expected);
		stable_budget_pointers[i] = first;
	}
}

static void test_invalid_selector_registry(void)
{
	static bool seen[UINT16_MAX + 1u];
	size_t count = 0u;

	current_key = "registry.invalid-selectors";
	for (uint32_t selector = 11u; selector <= UINT16_MAX; ++selector) {
		uint16_t key = (uint16_t)selector;
		CHECK(!seen[key]);
		seen[key] = true;
		CHECK(aius_runtime_budget(key) == NULL);
		count++;
	}
	CHECK(count == 65525u);
}

typedef enum {
	EVENT_CHECKPOINT,
	EVENT_SERVICE,
	EVENT_FEED
} EventKind;

typedef struct {
	EventKind kind;
	uint16_t checkpoint;
	const AiusRuntimeBudget *budget;
	void *context;
} TraceEvent;

typedef struct {
	TraceEvent events[MAX_TRACE_EVENTS];
	size_t event_count;
	const uint16_t *service_order;
	size_t service_order_count;
	size_t service_count;
	size_t fail_on_service;
	size_t feed_count;
	void *expected_context;
	bool epoch_complete_seen;
	bool service_failed;
} TraceState;

static const uint16_t normal_order[] = {
	AIUS_BOOT_CHECKPOINT_RESET_CAPTURE,
	AIUS_BOOT_CHECKPOINT_USB_RX,
	AIUS_BOOT_CHECKPOINT_USB_TX,
	AIUS_BOOT_CHECKPOINT_METRICS,
	AIUS_BOOT_CHECKPOINT_NETWORK,
	AIUS_BOOT_CHECKPOINT_HTTP,
	AIUS_BOOT_CHECKPOINT_UDP,
	AIUS_BOOT_CHECKPOINT_REPLAY,
	AIUS_BOOT_CHECKPOINT_AUDIO,
	AIUS_BOOT_CHECKPOINT_SD,
};

static const uint16_t recovery_safe_order[] = {
	AIUS_BOOT_CHECKPOINT_USB_RX,
	AIUS_BOOT_CHECKPOINT_USB_TX,
};

static TraceState *active_trace;

static void trace_init(
	TraceState *trace,
	const uint16_t *service_order,
	size_t service_order_count,
	size_t fail_on_service,
	void *expected_context)
{
	memset(trace, 0, sizeof(*trace));
	trace->service_order = service_order;
	trace->service_order_count = service_order_count;
	trace->fail_on_service = fail_on_service;
	trace->expected_context = expected_context;
}

static void record_event(
	TraceState *trace,
	EventKind kind,
	uint16_t checkpoint,
	const AiusRuntimeBudget *budget,
	void *context)
{
	CHECK(trace->event_count < ARRAY_COUNT(trace->events));
	TraceEvent *event = &trace->events[trace->event_count++];
	event->kind = kind;
	event->checkpoint = checkpoint;
	event->budget = budget;
	event->context = context;
}

static void trace_checkpoint(void *context, uint16_t checkpoint)
{
	TraceState *trace = active_trace;
	CHECK(trace != NULL);
	CHECK(context == trace->expected_context);
	CHECK(!trace->service_failed);
	CHECK(!trace->epoch_complete_seen);
	if (trace->service_count < trace->service_order_count) {
		CHECK(checkpoint ==
		      trace->service_order[trace->service_count]);
	} else {
		CHECK(trace->service_count == trace->service_order_count);
		CHECK(checkpoint ==
		      AIUS_BOOT_CHECKPOINT_EPOCH_COMPLETE);
		trace->epoch_complete_seen = true;
	}
	record_event(
		trace, EVENT_CHECKPOINT, checkpoint, NULL, context);
}

static bool trace_service(void *context, const AiusRuntimeBudget *budget)
{
	TraceState *trace = active_trace;
	CHECK(trace != NULL);
	CHECK(context == trace->expected_context);
	CHECK(!trace->service_failed);
	CHECK(!trace->epoch_complete_seen);
	CHECK(trace->service_count < trace->service_order_count);

	uint16_t checkpoint = trace->service_order[trace->service_count];
	CHECK(trace->event_count != 0u);
	const TraceEvent *checkpoint_event =
		&trace->events[trace->event_count - 1u];
	CHECK(checkpoint_event->kind == EVENT_CHECKPOINT);
	CHECK(checkpoint_event->checkpoint == checkpoint);

	const BudgetCase *expected = budget_case(checkpoint);
	const AiusRuntimeBudget *canonical =
		aius_runtime_budget(checkpoint);
	CHECK(budget == canonical);
	check_budget_value(budget, expected);
	record_event(trace, EVENT_SERVICE, checkpoint, budget, context);

	trace->service_count++;
	if (trace->service_count == trace->fail_on_service) {
		trace->service_failed = true;
		return false;
	}
	return true;
}

static void trace_feed(void *context)
{
	TraceState *trace = active_trace;
	CHECK(trace != NULL);
	CHECK(context == trace->expected_context);
	CHECK(!trace->service_failed);
	CHECK(trace->epoch_complete_seen);
	CHECK(trace->service_count == trace->service_order_count);
	CHECK(trace->feed_count == 0u);
	record_event(
		trace, EVENT_FEED,
		AIUS_BOOT_CHECKPOINT_EPOCH_COMPLETE, NULL, context);
	trace->feed_count++;
}

static AiusRuntimeOps trace_ops(void *context)
{
	AiusRuntimeOps ops = {
		.context = context,
		.service = trace_service,
		.checkpoint = trace_checkpoint,
		.feed_watchdog = trace_feed,
	};
	return ops;
}

static void verify_service_prefix(
	const TraceState *trace,
	size_t service_count,
	size_t expected_event_count)
{
	CHECK(trace->event_count == expected_event_count);
	CHECK(trace->service_count == service_count);
	CHECK(trace->feed_count == 0u);
	CHECK(!trace->epoch_complete_seen);
	for (size_t i = 0; i < service_count; ++i) {
		const TraceEvent *checkpoint = &trace->events[i * 2u];
		const TraceEvent *service = &trace->events[i * 2u + 1u];
		uint16_t expected_checkpoint = trace->service_order[i];
		CHECK(checkpoint->kind == EVENT_CHECKPOINT);
		CHECK(checkpoint->checkpoint == expected_checkpoint);
		CHECK(checkpoint->budget == NULL);
		CHECK(checkpoint->context == trace->expected_context);
		CHECK(service->kind == EVENT_SERVICE);
		CHECK(service->checkpoint == expected_checkpoint);
		CHECK(service->budget ==
		      aius_runtime_budget(expected_checkpoint));
		CHECK(service->context == trace->expected_context);
	}
}

static void verify_complete_trace(
	const TraceState *trace,
	size_t expected_event_count)
{
	CHECK(expected_event_count == trace->service_order_count * 2u + 2u);
	CHECK(trace->event_count == expected_event_count);
	CHECK(trace->service_count == trace->service_order_count);
	CHECK(trace->feed_count == 1u);
	CHECK(trace->epoch_complete_seen);
	CHECK(!trace->service_failed);

	for (size_t i = 0; i < trace->service_order_count; ++i) {
		const TraceEvent *checkpoint = &trace->events[i * 2u];
		const TraceEvent *service = &trace->events[i * 2u + 1u];
		uint16_t expected_checkpoint = trace->service_order[i];
		CHECK(checkpoint->kind == EVENT_CHECKPOINT);
		CHECK(checkpoint->checkpoint == expected_checkpoint);
		CHECK(checkpoint->budget == NULL);
		CHECK(checkpoint->context == trace->expected_context);
		CHECK(service->kind == EVENT_SERVICE);
		CHECK(service->checkpoint == expected_checkpoint);
		CHECK(service->budget ==
		      aius_runtime_budget(expected_checkpoint));
		CHECK(service->context == trace->expected_context);
	}

	const TraceEvent *complete =
		&trace->events[expected_event_count - 2u];
	const TraceEvent *feed =
		&trace->events[expected_event_count - 1u];
	CHECK(complete->kind == EVENT_CHECKPOINT);
	CHECK(complete->checkpoint ==
	      AIUS_BOOT_CHECKPOINT_EPOCH_COMPLETE);
	CHECK(complete->budget == NULL);
	CHECK(complete->context == trace->expected_context);
	CHECK(feed->kind == EVENT_FEED);
	CHECK(feed->checkpoint ==
	      AIUS_BOOT_CHECKPOINT_EPOCH_COMPLETE);
	CHECK(feed->budget == NULL);
	CHECK(feed->context == trace->expected_context);
}

typedef struct {
	const char *key;
	bool recovery_safe;
	const uint16_t *order;
	size_t order_count;
	size_t event_count;
} CompleteTraceCase;

static const CompleteTraceCase complete_trace_cases[] = {
	{"trace.complete.normal", false, normal_order,
	 ARRAY_COUNT(normal_order), 22u},
	{"trace.complete.recovery-safe", true, recovery_safe_order,
	 ARRAY_COUNT(recovery_safe_order), 6u},
};

static void test_complete_trace_registry(void)
{
	const char *keys[ARRAY_COUNT(complete_trace_cases)];
	for (size_t i = 0; i < ARRAY_COUNT(complete_trace_cases); ++i)
		keys[i] = complete_trace_cases[i].key;
	validate_unique_keys(
		"registry.complete-traces", keys,
		ARRAY_COUNT(complete_trace_cases), 2u);

	for (size_t i = 0; i < ARRAY_COUNT(complete_trace_cases); ++i) {
		const CompleteTraceCase *test = &complete_trace_cases[i];
		TraceState trace;
		int context_marker = 0;
		trace_init(
			&trace, test->order, test->order_count, 0u,
			&context_marker);
		AiusRuntimeOps ops = trace_ops(&context_marker);
		active_trace = &trace;
		current_key = test->key;
		bool result =
			aius_runtime_epoch(&ops, test->recovery_safe);
		CHECK(result);
		CHECK(result == (trace.feed_count == 1u));
		verify_complete_trace(&trace, test->event_count);
	}
	active_trace = NULL;
}

typedef struct {
	const char *key;
	bool recovery_safe;
	const uint16_t *order;
	size_t order_count;
	size_t fail_on_service;
	size_t event_count;
} FailureTraceCase;

static const FailureTraceCase failure_trace_cases[] = {
	{"trace.fail.normal.0", false, normal_order,
	 ARRAY_COUNT(normal_order), 1u, 2u},
	{"trace.fail.normal.1", false, normal_order,
	 ARRAY_COUNT(normal_order), 2u, 4u},
	{"trace.fail.normal.2", false, normal_order,
	 ARRAY_COUNT(normal_order), 3u, 6u},
	{"trace.fail.normal.3", false, normal_order,
	 ARRAY_COUNT(normal_order), 4u, 8u},
	{"trace.fail.normal.4", false, normal_order,
	 ARRAY_COUNT(normal_order), 5u, 10u},
	{"trace.fail.normal.5", false, normal_order,
	 ARRAY_COUNT(normal_order), 6u, 12u},
	{"trace.fail.normal.6", false, normal_order,
	 ARRAY_COUNT(normal_order), 7u, 14u},
	{"trace.fail.normal.7", false, normal_order,
	 ARRAY_COUNT(normal_order), 8u, 16u},
	{"trace.fail.normal.8", false, normal_order,
	 ARRAY_COUNT(normal_order), 9u, 18u},
	{"trace.fail.normal.9", false, normal_order,
	 ARRAY_COUNT(normal_order), 10u, 20u},
	{"trace.fail.recovery-safe.0", true, recovery_safe_order,
	 ARRAY_COUNT(recovery_safe_order), 1u, 2u},
	{"trace.fail.recovery-safe.1", true, recovery_safe_order,
	 ARRAY_COUNT(recovery_safe_order), 2u, 4u},
};

static void test_failure_trace_registry(void)
{
	const char *keys[ARRAY_COUNT(failure_trace_cases)];
	for (size_t i = 0; i < ARRAY_COUNT(failure_trace_cases); ++i)
		keys[i] = failure_trace_cases[i].key;
	validate_unique_keys(
		"registry.failure-traces", keys,
		ARRAY_COUNT(failure_trace_cases), 12u);

	for (size_t i = 0; i < ARRAY_COUNT(failure_trace_cases); ++i) {
		const FailureTraceCase *test = &failure_trace_cases[i];
		TraceState trace;
		int context_marker = 0;
		trace_init(
			&trace, test->order, test->order_count,
			test->fail_on_service, &context_marker);
		AiusRuntimeOps ops = trace_ops(&context_marker);
		active_trace = &trace;
		current_key = test->key;
		bool result =
			aius_runtime_epoch(&ops, test->recovery_safe);
		CHECK(!result);
		CHECK(result == (trace.feed_count == 1u));
		CHECK(trace.service_failed);
		verify_service_prefix(
			&trace, test->fail_on_service, test->event_count);
	}
	active_trace = NULL;
}

typedef struct {
	const char *key;
	bool null_ops;
	bool service_present;
	bool checkpoint_present;
	bool feed_present;
	bool null_context;
	bool expected_result;
	size_t expected_event_count;
} OpsValidityCase;

static const OpsValidityCase ops_validity_cases[] = {
	{"ops.null", true, false, false, false, false, false, 0u},
	{"ops.callbacks.000", false, false, false, false, false, false, 0u},
	{"ops.callbacks.001", false, false, false, true, false, false, 0u},
	{"ops.callbacks.010", false, false, true, false, false, false, 0u},
	{"ops.callbacks.011", false, false, true, true, false, false, 0u},
	{"ops.callbacks.100", false, true, false, false, false, false, 0u},
	{"ops.callbacks.101", false, true, false, true, false, false, 0u},
	{"ops.callbacks.110", false, true, true, false, false, false, 0u},
	{"ops.callbacks.111", false, true, true, true, false, true, 22u},
	{"ops.null-context", false, true, true, true, true, true, 22u},
};

static void test_ops_validity_registry(void)
{
	const char *keys[ARRAY_COUNT(ops_validity_cases)];
	for (size_t i = 0; i < ARRAY_COUNT(ops_validity_cases); ++i)
		keys[i] = ops_validity_cases[i].key;
	validate_unique_keys(
		"registry.ops-validity", keys,
		ARRAY_COUNT(ops_validity_cases), 10u);

	for (size_t i = 0; i < ARRAY_COUNT(ops_validity_cases); ++i) {
		const OpsValidityCase *test = &ops_validity_cases[i];
		TraceState trace;
		int context_marker = 0;
		void *context =
			test->null_context ? NULL : &context_marker;
		trace_init(
			&trace, normal_order, ARRAY_COUNT(normal_order),
			0u, context);
		AiusRuntimeOps ops = {
			.context = context,
			.service =
				test->service_present ? trace_service : NULL,
			.checkpoint = test->checkpoint_present
				? trace_checkpoint
				: NULL,
			.feed_watchdog =
				test->feed_present ? trace_feed : NULL,
		};
		const AiusRuntimeOps *argument =
			test->null_ops ? NULL : &ops;
		active_trace = &trace;
		current_key = test->key;
		bool result = aius_runtime_epoch(argument, false);
		CHECK(result == test->expected_result);
		CHECK(trace.event_count == test->expected_event_count);
		if (test->expected_result) {
			CHECK(result == (trace.feed_count == 1u));
			verify_complete_trace(
				&trace, test->expected_event_count);
		} else {
			CHECK(trace.service_count == 0u);
			CHECK(trace.feed_count == 0u);
			CHECK(!trace.epoch_complete_seen);
		}
	}
	active_trace = NULL;
}

typedef struct {
	AiusRuntimeOps *target;
	void *replacement_context;
	bool context_alias;
	bool mutated;
} MutationState;

static MutationState *active_mutation;

static bool poison_service(
	void *context,
	const AiusRuntimeBudget *budget)
{
	(void)context;
	(void)budget;
	CHECK(0);
	return false;
}

static void poison_checkpoint(void *context, uint16_t checkpoint)
{
	(void)context;
	(void)checkpoint;
	CHECK(0);
}

static void poison_feed(void *context)
{
	(void)context;
	CHECK(0);
}

static void mutating_checkpoint(void *context, uint16_t checkpoint)
{
	trace_checkpoint(context, checkpoint);
	MutationState *mutation = active_mutation;
	CHECK(mutation != NULL);
	if (mutation->mutated)
		return;
	if (mutation->context_alias)
		CHECK(context == mutation->target);
	mutation->target->context = mutation->replacement_context;
	mutation->target->service = poison_service;
	mutation->target->checkpoint = poison_checkpoint;
	mutation->target->feed_watchdog = poison_feed;
	mutation->mutated = true;
}

typedef struct {
	const char *key;
	bool context_alias;
} AliasMutationCase;

static const AliasMutationCase alias_mutation_cases[] = {
	{"snapshot.original-storage", false},
	{"snapshot.context-alias", true},
};

static void test_alias_mutation_registry(void)
{
	const char *keys[ARRAY_COUNT(alias_mutation_cases)];
	for (size_t i = 0; i < ARRAY_COUNT(alias_mutation_cases); ++i)
		keys[i] = alias_mutation_cases[i].key;
	validate_unique_keys(
		"registry.alias-mutation", keys,
		ARRAY_COUNT(alias_mutation_cases), 2u);

	for (size_t i = 0; i < ARRAY_COUNT(alias_mutation_cases); ++i) {
		const AliasMutationCase *test = &alias_mutation_cases[i];
		TraceState trace;
		int context_marker = 0;
		int replacement_marker = 0;
		AiusRuntimeOps ops = trace_ops(NULL);
		void *entry_context = test->context_alias
			? (void *)&ops
			: (void *)&context_marker;
		ops.context = entry_context;
		ops.checkpoint = mutating_checkpoint;
		trace_init(
			&trace, normal_order, ARRAY_COUNT(normal_order),
			0u, entry_context);
		MutationState mutation = {
			.target = &ops,
			.replacement_context = &replacement_marker,
			.context_alias = test->context_alias,
			.mutated = false,
		};
		active_trace = &trace;
		active_mutation = &mutation;
		current_key = test->key;
		bool result = aius_runtime_epoch(&ops, false);
		CHECK(result);
		CHECK(mutation.mutated);
		CHECK(ops.context == &replacement_marker);
		CHECK(ops.service == poison_service);
		CHECK(ops.checkpoint == poison_checkpoint);
		CHECK(ops.feed_watchdog == poison_feed);
		verify_complete_trace(&trace, 22u);
	}
	active_mutation = NULL;
	active_trace = NULL;
}

static void test_sequential_calls(void)
{
	TraceState trace;
	int context_marker = 0;
	AiusRuntimeOps ops = trace_ops(&context_marker);

	current_key = "calls.sequential.normal";
	trace_init(
		&trace, normal_order, ARRAY_COUNT(normal_order),
		0u, &context_marker);
	active_trace = &trace;
	CHECK(aius_runtime_epoch(&ops, false));
	verify_complete_trace(&trace, 22u);

	current_key = "calls.sequential.recovery-safe";
	trace_init(
		&trace, recovery_safe_order,
		ARRAY_COUNT(recovery_safe_order), 0u, &context_marker);
	active_trace = &trace;
	CHECK(aius_runtime_epoch(&ops, true));
	verify_complete_trace(&trace, 6u);
	active_trace = NULL;
}

typedef struct {
	TraceState outer;
	TraceState inner;
	AiusRuntimeOps inner_ops;
	bool inner_called;
	bool inner_result;
} ReentryState;

static ReentryState *active_reentry;

static bool reentrant_service(
	void *context,
	const AiusRuntimeBudget *budget)
{
	ReentryState *reentry = active_reentry;
	CHECK(reentry != NULL);
	CHECK(active_trace == &reentry->outer);
	if (!reentry->inner_called) {
		reentry->inner_called = true;
		active_trace = &reentry->inner;
		reentry->inner_result =
			aius_runtime_epoch(&reentry->inner_ops, true);
		active_trace = &reentry->outer;
		CHECK(reentry->inner_result);
	}
	return trace_service(context, budget);
}

static void test_distinct_context_reentry(void)
{
	ReentryState reentry;
	int outer_context = 0;
	int inner_context = 0;
	memset(&reentry, 0, sizeof(reentry));
	trace_init(
		&reentry.outer, normal_order, ARRAY_COUNT(normal_order),
		0u, &outer_context);
	trace_init(
		&reentry.inner, recovery_safe_order,
		ARRAY_COUNT(recovery_safe_order), 0u, &inner_context);
	reentry.inner_ops = trace_ops(&inner_context);
	AiusRuntimeOps outer_ops = trace_ops(&outer_context);
	outer_ops.service = reentrant_service;

	current_key = "calls.distinct-context-reentry";
	active_reentry = &reentry;
	active_trace = &reentry.outer;
	bool result = aius_runtime_epoch(&outer_ops, false);
	CHECK(result);
	CHECK(reentry.inner_called);
	CHECK(reentry.inner_result);
	verify_complete_trace(&reentry.inner, 6u);
	verify_complete_trace(&reentry.outer, 22u);
	active_trace = NULL;
	active_reentry = NULL;
}

typedef struct {
	const char *key;
	void (*run)(void);
} CallBehaviorCase;

static const CallBehaviorCase call_behavior_cases[] = {
	{"calls.sequential", test_sequential_calls},
	{"calls.distinct-context-reentry", test_distinct_context_reentry},
};

static void test_call_behavior_registry(void)
{
	const char *keys[ARRAY_COUNT(call_behavior_cases)];
	for (size_t i = 0; i < ARRAY_COUNT(call_behavior_cases); ++i)
		keys[i] = call_behavior_cases[i].key;
	validate_unique_keys(
		"registry.call-behaviors", keys,
		ARRAY_COUNT(call_behavior_cases), 2u);
	for (size_t i = 0; i < ARRAY_COUNT(call_behavior_cases); ++i) {
		current_key = call_behavior_cases[i].key;
		call_behavior_cases[i].run();
	}
}

static void test_budget_pointer_stability_after_epochs(void)
{
	current_key = "budget.stability-after-epochs";
	CHECK(ARRAY_COUNT(budget_cases) ==
	      ARRAY_COUNT(stable_budget_pointers));
	for (size_t i = 0; i < ARRAY_COUNT(budget_cases); ++i) {
		const AiusRuntimeBudget *current =
			aius_runtime_budget(budget_cases[i].checkpoint);
		CHECK(current == stable_budget_pointers[i]);
		check_budget_value(current, &budget_cases[i]);
	}
}

int main(void)
{
	test_budget_registry();
	test_invalid_selector_registry();
	test_complete_trace_registry();
	test_failure_trace_registry();
	test_ops_validity_registry();
	test_alias_mutation_registry();
	test_call_behavior_registry();
	test_budget_pointer_stability_after_epochs();
	puts("runtime scheduler oracle passed");
	return 0;
}
