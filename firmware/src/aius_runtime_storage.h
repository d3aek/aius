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
 *
 * A job may also be started by the composition root itself, on behalf of boot,
 * with no HTTP client involved (aius_runtime_storage_start_root_job). One job
 * runs at a time and neither owner preempts the other: see that function and
 * aius_runtime_storage_service for the two directions of the contention rule.
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

/* driver_fault reported when a registered publication consumer refuses the
 * payload, mapped by the portable engine to AIUS_SD_FAULT_PUBLISH. Distinct
 * from AIUS_HTTP_SD_OUTPUT_CANCEL_FAULT so the two are told apart in a log. */
#define AIUS_RUNTIME_STORAGE_PUBLISH_REJECT_FAULT UINT32_C(0x50554243)

extern "C" {

/* Lifecycle of the most recent root-initiated job. HTTP-owned jobs never move
 * it; their result travels through the correlated AiusHttpEvent instead. */
typedef enum {
	AIUS_RUNTIME_STORAGE_ROOT_IDLE,
	AIUS_RUNTIME_STORAGE_ROOT_RUNNING,
	AIUS_RUNTIME_STORAGE_ROOT_COMPLETE,
	AIUS_RUNTIME_STORAGE_ROOT_FAILED
} AiusRuntimeStorageRootState;

/* The publication consumer: where a job's AIUS_SD_ACTION_PUBLISH_CONFIG bytes
 * go. `data` is the job's own transfer buffer and is valid only for the call.
 * Returning false faults the job with AIUS_SD_FAULT_PUBLISH rather than
 * completing it, so a consumer that cannot accept the content says so. */
typedef struct {
	void *context;
	bool (*deliver)(void *context, const uint8_t *data, size_t length);
} AiusRuntimeStoragePublish;

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
	/* An HTTP action has claimed the job and no terminal correlated result
	 * has been published yet. Unlike http_authority, which is per-action and
	 * cleared by every published result, this spans the whole job, so a
	 * root-initiated start cannot land on top of a live HTTP job. */
	bool http_owned;
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
	 * and discarded because no publication consumer is registered. A
	 * registered consumer that refuses the payload faults the job instead,
	 * so this counts only the genuinely-unconsumed case. */
	uint32_t publish_dropped;
	/* Where a published payload goes. Null `deliver` keeps the delivered
	 * drop-and-count behaviour. */
	AiusRuntimeStoragePublish publish;
	/* The root-initiated job's lifecycle; RUNNING is the only state in which
	 * the service advances a job with no HTTP action driving it. */
	AiusRuntimeStorageRootState root_state;
} AiusRuntimeStorage;

/* Memory only: initializes the job against the embedded transfer buffer and the
 * SD adapter, with no publication consumer. No SD primitive is called. */
void aius_runtime_storage_construct(AiusRuntimeStorage *service);

/* Memory only: registers (or, with a null `ops` or a null `ops->deliver`,
 * clears) the publication consumer. Construction-time wiring; the consumer runs
 * in SD-checkpoint context, never per audio block. */
void aius_runtime_storage_set_publish(
	AiusRuntimeStorage *service,
	const AiusRuntimeStoragePublish *ops);

/* Memory only: starts a job nobody asked for over HTTP -- the boot-time
 * CONFIG_LOAD, PROBE and USAGE_REFRESH loads. No SD primitive is called; the
 * job is advanced one primitive per epoch by aius_runtime_storage_service()
 * with no HTTP action, exactly as a drain is.
 *
 * The contention rule is reject, not queue or defer: the service owns one job
 * and one 512-byte transfer buffer, so a queue would have to retain a second
 * request including its three 128-byte paths, and a deferred start would owe an
 * unbounded-latency promise while an HTTP job that may itself restart runs. A
 * refusal is retried by the caller's own next epoch at zero cost.
 *
 * Returns true when the job was accepted and is now RUNNING. Returns false and
 * leaves the service untouched when it is busy: a live HTTP job (http_owned),
 * an outstanding HTTP action or undelivered HTTP result, a drain in progress,
 * or a root job already RUNNING. Returns false with root_state FAILED when the
 * portable engine rejected the request itself; the typed fault is then readable
 * through aius_runtime_storage_root_result(). `request` need only outlive the
 * call -- the engine copies every path. */
bool aius_runtime_storage_start_root_job(
	AiusRuntimeStorage *service,
	const AiusSdJobRequest *request,
	uint32_t now_ms);

/* The terminal result of the most recent root-initiated job, or null while none
 * has reached a terminal state. Valid until the next job of either owner
 * starts; PROBE reports media_present and USAGE_REFRESH the byte counts. */
const AiusSdResult *aius_runtime_storage_root_result(
	const AiusRuntimeStorage *service);

/* One SD checkpoint. `http_action` is the SD-backed action the HTTP service
 * produced this epoch (or NULL). The correlated result, when one is produced,
 * is retained in `pending_http_event` with `http_event_present` set and copied
 * to `http_event` for the caller.
 *
 * With no action and a root-initiated job RUNNING, the job is advanced one
 * primitive per epoch with no HTTP correlation. An HTTP action arriving while a
 * root job is RUNNING is refused with a correlated FAILED/AIUS_SD_FAULT_PROTOCOL
 * result rather than restarting the job underneath it -- the other direction of
 * the contention rule. Refusing in-band keeps the epoch green; the dispatcher's
 * sd_busy callback is the intended first line of defence.
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
 * teardown). A nonterminal HTTP-owned job is cancelled once and then drained
 * autonomously, one primitive per epoch, so a teardown can never leave an open
 * SD handle behind an owner that no longer exists. A root-initiated job has no
 * HTTP owner to lose and keeps running. */
void aius_runtime_storage_release_http(AiusRuntimeStorage *service);

} // extern "C"

#endif
