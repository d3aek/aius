/* Bounded SD storage runtime service.
 *
 * One concern: owning the portable SD job, its 512-byte transfer buffer and the
 * one-shot Teensy SD adapter, and bridging them to the HTTP transaction's
 * correlated SD action/event protocol. Construction touches memory only.
 *
 * The normative rule this service exists to keep is one SD primitive per epoch.
 * The Teensy SD library is synchronous and offers no cancellable call, so a
 * checkpoint that performs two primitives doubles the worst-case wedge the 8 s
 * watchdog has to absorb. Every path below therefore performs at most one
 * aius_sd_teensy_execute() call and retains its event for the next epoch.
 *
 * HTTP authority is correlated: one AiusHttpAction claims it, and it is held
 * until this service returns the matching AiusHttpEvent. The composition root
 * brokers both directions; this service never calls the network/HTTP service.
 */

#ifndef AIUS_RUNTIME_STORAGE_H
#define AIUS_RUNTIME_STORAGE_H

#ifndef __cplusplus
#error "aius_runtime_storage.h requires C++ (AiusSdTeensy embeds an SD File)"
#endif

#include "aius_http_transaction.h"
#include "aius_runtime.h"
#include "aius_sd.h"
#include "aius_sd_teensy.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The job transfer slice, and therefore the sd_capacity reported to HTTP on
 * SD_STARTED. Matches AIUS_HTTP_SD_CAPACITY_MAX. */
#define AIUS_RUNTIME_STORAGE_TRANSFER 512u

/* SD_STARTED reports this as sd_capacity, and the transaction rejects anything
 * above AIUS_HTTP_SD_CAPACITY_MAX. Keep the two in lockstep at compile time
 * rather than in a comment. */
static_assert(AIUS_RUNTIME_STORAGE_TRANSFER == AIUS_HTTP_SD_CAPACITY_MAX,
              "SD transfer slice must equal the HTTP SD capacity bound");

extern "C" {

typedef struct {
	AiusSdJob job;
	AiusSdTeensy target;
	uint8_t sd_transfer[AIUS_RUNTIME_STORAGE_TRANSFER];
	/* The adapter result awaiting delivery to the job. */
	AiusSdEvent pending_event;
	/* The correlated HTTP event awaiting delivery to the transaction. */
	AiusHttpEvent pending_http_event;
	uint64_t http_action_id;
	AiusHttpActionKind http_action;
	bool event_present;
	bool http_event_present;
	bool http_authority;
	/* One outstanding correlated AIUS_SD_ACTION_OUTPUT chunk, resolved by
	 * the transaction's ACK_DOWNLOAD or FAIL_DOWNLOAD. */
	uint64_t output_action_id;
	bool output_outstanding;
	/* Cancellation has been requested (SD_CANCEL, FAIL_DOWNLOAD or an HTTP
	 * release), so a terminal job result reports CANCELLED, not FAILED. */
	bool cancelling;
	/* The job emitted its one COMPLETE/FAIL notification, so it is
	 * restartable and a further cancel resolves immediately. */
	bool terminal_seen;
	/* Draining a job whose HTTP owner was discarded: advanced one primitive
	 * per epoch with no HTTP correlation until the job is terminal. */
	bool draining;
	/* Saturating count of AIUS_SD_ACTION_PUBLISH_CONFIG actions acknowledged
	 * and discarded because no publication consumer exists yet. Phase 14
	 * routes CONFIG_LOAD publication to the application. */
	uint32_t publish_dropped;
} AiusRuntimeStorage;

/* Memory only: initializes the job against the embedded transfer buffer and the
 * SD adapter. No SD primitive is called. */
void aius_runtime_storage_construct(AiusRuntimeStorage *service);

/* One SD checkpoint. `http_action` is the SD-backed action the HTTP service
 * produced this epoch (or NULL). The correlated result, when one is produced,
 * is retained in `pending_http_event` with `http_event_present` set and copied
 * to `http_event` for the caller.
 *
 * Returns false only for an invalid call: a null service, budget or http_event,
 * a budget that is not the SD descriptor, a non-SD action kind, or a new action
 * arriving while HTTP authority is still outstanding. */
bool aius_runtime_storage_service(
	AiusRuntimeStorage *service,
	const AiusRuntimeBudget *budget,
	uint32_t now_ms,
	const AiusHttpAction *http_action,
	AiusHttpEvent *http_event);

/* Releases HTTP authority after the transaction was discarded (network
 * teardown). A nonterminal job is cancelled once and then drained autonomously,
 * one primitive per epoch, so a teardown can never leave an open SD handle
 * behind an owner that no longer exists. */
void aius_runtime_storage_release_http(AiusRuntimeStorage *service);

} // extern "C"

#endif
