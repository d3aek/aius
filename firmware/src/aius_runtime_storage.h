/* Bounded SD storage runtime service.
 *
 * One concern: owning the portable SD job, its 512-byte transfer buffer, its
 * model-sized publication buffer and the one-shot Teensy SD adapter, and
 * bridging them to the HTTP transaction's correlated SD action/event protocol.
 * Construction touches memory only.
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

#include "aius_dsp_config.h"
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

/* The transfer buffer is DMA- and cache-maintenance-touched (see its alignas
 * in AiusRuntimeStorage): its length must span whole 32-byte cache lines, or
 * its tail shares a line with the field below and every straddled
 * invalidation discards that neighbour's cached writes. */
static_assert(AIUS_RUNTIME_STORAGE_TRANSFER % 32u == 0u,
              "the SD transfer buffer must span whole cache lines");

/* SD_STARTED reports this as sd_capacity, and the transaction rejects anything
 * above AIUS_HTTP_SD_CAPACITY_MAX. Keep the two in lockstep at compile time
 * rather than in a comment. */
static_assert(AIUS_RUNTIME_STORAGE_TRANSFER == AIUS_HTTP_SD_CAPACITY_MAX,
              "SD transfer slice must equal the HTTP SD capacity bound");

/* The publication accumulation buffer, sized for the largest file the
 * application publishes: the DSP model JSON. It is deliberately NOT the transfer
 * slice. Growing the slice to match would size every transfer -- download reads
 * and upload input included -- making each SD primitive a 12 KB blocking call,
 * against the one-primitive-per-epoch rule this service exists to keep. Instead
 * the engine reads into this buffer AIUS_RUNTIME_STORAGE_TRANSFER bytes at a
 * time and publishes it once. It lives inside the DMAMEM-placed runtime object,
 * so it costs OCRAM and leaves DTCM untouched. */
#define AIUS_RUNTIME_STORAGE_PUBLISH_CAPACITY AIUS_DSP_MODEL_MAX_JSON_BYTES

static_assert(AIUS_RUNTIME_STORAGE_PUBLISH_CAPACITY >=
                      AIUS_RUNTIME_STORAGE_TRANSFER,
              "publication buffer must hold at least one transfer slice");

/* driver_fault reported when a registered publication consumer refuses the
 * payload, mapped by the portable engine to AIUS_SD_FAULT_PUBLISH. Distinct
 * from AIUS_HTTP_SD_OUTPUT_CANCEL_FAULT so the two are told apart in a log. */
#define AIUS_RUNTIME_STORAGE_PUBLISH_REJECT_FAULT UINT32_C(0x50554243)

/* Phase 4g: driver_fault of the FAULT event that answers a CACHE_FILL chunk
 * the sink refused (aius_runtime_storage_root_output false: cache capacity
 * full, read-back mismatch). The job closes its file and fails with
 * AIUS_SD_FAULT_IO and this value; that terminal is a cache event the sink
 * counted, so it is neither a root failure nor latched in last_failure. A
 * CLOSE that then faults replaces it (CLEANUP) and counts as usual. */
#define AIUS_RUNTIME_STORAGE_FILL_SINK_FAULT UINT32_C(0x46494C4C)

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
 * go. `data` is the job's publication buffer and is valid only for the call.
 * Returning false faults the job with AIUS_SD_FAULT_PUBLISH rather than
 * completing it, so a consumer that cannot accept the content says so. */
typedef struct {
	void *context;
	bool (*deliver)(void *context, const uint8_t *data, size_t length);
} AiusRuntimeStoragePublish;

/* Who owned the job a diagnostic record describes. DRAIN is an HTTP-owned job
 * whose owner was discarded and which finished on its own. */
typedef enum {
	AIUS_RUNTIME_STORAGE_OWNER_NONE,
	AIUS_RUNTIME_STORAGE_OWNER_HTTP,
	AIUS_RUNTIME_STORAGE_OWNER_ROOT,
	AIUS_RUNTIME_STORAGE_OWNER_DRAIN
} AiusRuntimeStorageOwner;

/* One failed job, latched so a 500 on the wire can be attributed after the
 * fact. Everything here is already known at the moment of failure and costs a
 * handful of stores on a path that has already failed; nothing is sampled per
 * epoch and nothing is read from the card.
 *
 * The fields answer the questions a failure cannot otherwise be told apart by:
 *
 *   refused      the job never ran -- an HTTP action arrived while a root job
 *                owned the volume and was refused in-band. True here means the
 *                contention rule fired, not that the card misbehaved.
 *   job_kind     which job failed (or, when refused, which job was running).
 *   fault_phase  where in the job the first fault landed. BEGIN separates a
 *                mount failure from a write failure from an install rename.
 *   driver_fault the adapter's own word: DRIVER means the card returned false,
 *                PROTOCOL means this adapter refused to ask it.
 *   root_state /
 *   draining     what else owned the volume when it happened.
 *   mounts_in_job  SD.begin() calls between this job's start and its failure.
 *                A healthy job mounts exactly once, its own; two or more means
 *                something remounted the volume underneath it.
 */
typedef struct {
	/* Nth failure latched; zero means none has been, so the record is
	 * absent rather than zeroed. */
	uint32_t sequence;
	uint32_t at_ms;
	uint8_t owner;       /* AiusRuntimeStorageOwner */
	uint8_t job_kind;    /* AiusSdJobKind */
	uint8_t fault;       /* AiusSdFault */
	uint8_t fault_phase; /* AiusSdPhase */
	uint8_t job_state;   /* AiusSdState */
	uint8_t root_state;  /* AiusRuntimeStorageRootState */
	bool draining;
	bool refused;
	uint32_t driver_fault;
	uint32_t adapter_fault;
	uint32_t mounts_in_job;
	uint32_t longest_primitive_us;
	uint64_t transferred;
} AiusRuntimeStorageFailure;

/* Saturating lifetime counters plus the last failure. Read by the status
 * surface; never by the audio path. */
typedef struct {
	uint32_t http_jobs;
	uint32_t root_jobs;
	uint32_t http_failures;
	uint32_t root_failures;
	/* HTTP actions refused because a root job owned the volume. This is the
	 * decisive counter for the probe-versus-card question: a nonzero value
	 * says an upload was refused by the contention rule rather than by the
	 * card. */
	uint32_t busy_refusals;
	uint32_t cancellations;
	uint32_t drains;
	AiusRuntimeStorageFailure last_failure;
} AiusRuntimeStorageDiag;

typedef struct {
	AiusSdJob job;
	AiusSdTeensy target;
	/* CACHE-LINE ISOLATED, NOT MERELY PLACED. This is the buffer every SD
	 * action reads into and writes from, and on the target the driver's DMA
	 * cache maintenance operates on WHOLE 32-byte lines: invalidating a
	 * buffer whose head is not line-aligned rounds DOWN into the adapter
	 * struct directly above -- discarding its cached (newer) field values,
	 * which is precisely the 2026-08-01 regression: adding one pointer to
	 * the embedded OTA seam shifted this buffer's interior offset by 8, the
	 * straddled invalidation began eating `target`'s bookkeeping, and every
	 * SD write turned into corrupted state, protocol faults, card retries
	 * measured in seconds, and watchdog resets. alignas(32) pins the head
	 * to a line boundary and 512 is a whole number of lines, so neither
	 * neighbour ever shares a line with DMA'd bytes -- robust against any
	 * future upstream struct growth instead of lucky about it. */
	alignas(32) uint8_t sd_transfer[AIUS_RUNTIME_STORAGE_TRANSFER];
	/* Accumulates a publishing job's bytes across its many bounded reads, so
	 * one AIUS_SD_ACTION_PUBLISH_CONFIG can carry a whole model-sized file. */
	uint8_t sd_publish[AIUS_RUNTIME_STORAGE_PUBLISH_CAPACITY];
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
	/* Failure attribution -- see AiusRuntimeStorageDiag. */
	AiusRuntimeStorageDiag diag;
	/* The adapter's mount count when the running job started, so a failure
	 * can report mounts during the job rather than since boot. */
	uint32_t job_mounts_at_start;
	/* Who started the running job, carried so a terminal failure names its
	 * owner without re-deriving it from flags that the terminal itself
	 * clears. */
	uint8_t job_owner;
	/* Phase 4g: the running root job was cancelled by
	 * aius_runtime_storage_cancel_root(), so its CANCELLED terminal counts
	 * a cancellation, not a root failure. Sits in the tail padding. */
	bool root_cancelling;
} AiusRuntimeStorage;

/* Phase 4g (aius-sample-rate-following.md G6, G8, G9): link-time hooks for the
 * one root job kind that streams, AIUS_SD_JOB_CACHE_FILL. Weak defaults keep
 * every other build unchanged; optical_main.cpp defines both.
 *
 * aius_runtime_storage_root_output: the sink for a CACHE_FILL root job's
 * AIUS_SD_ACTION_OUTPUT chunk (`offset` into a file of `size` bytes, both
 * absolute, also for a fill resumed at a nonzero initial_offset). True
 * answers OK and the job's next READ runs in the same epoch (the OUTPUT is
 * memory only, so the epoch still spends one card primitive); false faults the
 * job. The default refuses, which is the pre-4g answer to a root OUTPUT. Any
 * other root job's OUTPUT keeps that refusal and never reaches the hook.
 *
 * aius_runtime_storage_fill_step_defer: consulted once per epoch before a
 * RUNNING, uncancelled CACHE_FILL root job is advanced; true skips this
 * epoch's step (no primitive, no state change). Never consulted for any other
 * root job, a drain or an HTTP job, which keep 4f's every-epoch rule, nor for
 * a fill being cancelled, so a cancel always closes its file promptly. The
 * default never defers. */
bool aius_runtime_storage_root_output(const AiusSdJob *job,
                                      const uint8_t *data, size_t length,
                                      uint64_t offset, uint64_t size);
bool aius_runtime_storage_fill_step_defer(const AiusSdJob *job);

/* Phase 4g G9: cancels the running root job at its next primitive boundary
 * (aius_sd_job_cancel: CLOSE, then FAIL with AIUS_SD_FAULT_CANCELLED, one
 * primitive per epoch). Memory only. Its terminal is root_state FAILED with
 * that fault, counted in diag.cancellations rather than root_failures and
 * never latched as a failure. Returns false when no root job is RUNNING or it
 * already reached its terminal. */
bool aius_runtime_storage_cancel_root(AiusRuntimeStorage *service);

/* Memory only: initializes the job against the embedded transfer buffer, the
 * embedded publication buffer and the SD adapter, with no publication consumer.
 * No SD primitive is called. */
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

/* Appends the `sd_diag` object to a JSON body being assembled at `*length`,
 * advancing it. Memory only: every value is already latched, so nothing is
 * sampled and no SD primitive is spent. Returns false on capacity exhaustion,
 * leaving `*length` unchanged from the last complete append.
 *
 * It lives here rather than in the status serializer because this service owns
 * every field: the counters, the adapter's own totals, and the shape a reader
 * has to interpret. The serializer's job is one call.
 *
 * `last_failure` is served as null when none has been latched, so "no failure
 * yet" cannot be misread as a failure with zeroed fields.
 *
 * `http` is the live transaction, and it is the ONE field this service does not
 * own: `upload_supplied` is the transaction's running count of body bytes
 * accepted by the job, and it exists nowhere else. Passing the transaction in
 * rather than mirroring the count here is deliberate -- a mirror maintained
 * beside the authority is precisely the drift this object exists to remove, and
 * the composition root already holds both. Null is admissible and serializes
 * `upload_supplied` as 0: a caller with no transaction wired (a unit test, or a
 * teardown that discarded it) gets a truthful "nothing supplied" rather than a
 * refusal, because a diagnostic must not fail the status body it rides in. */
bool aius_runtime_storage_diag_json(
	const AiusRuntimeStorage *service,
	const AiusHttpTransaction *http,
	char *output,
	size_t capacity,
	size_t *length);

/* Releases HTTP authority after the transaction was discarded (network
 * teardown). A nonterminal HTTP-owned job is cancelled once and then drained
 * autonomously, one primitive per epoch, so a teardown can never leave an open
 * SD handle behind an owner that no longer exists. A root-initiated job has no
 * HTTP owner to lose and keeps running. */
void aius_runtime_storage_release_http(AiusRuntimeStorage *service);

} // extern "C"

#endif
