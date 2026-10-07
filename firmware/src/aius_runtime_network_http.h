/* Bounded network + HTTP runtime service.
 *
 * One concern: owning the network generation (portable supervisor + Teensy
 * adapter) and the single HTTP transaction that lives inside it. Construction
 * touches memory only. Each service call consumes at most one retained input,
 * executes the exact supplied checkpoint budget, and publishes at most one
 * retained output; it never calls another service, the scheduler, or a
 * watchdog.
 *
 * The NETWORK checkpoint applies strict one-operation precedence:
 *
 *   1. a pending supervisor action, or adapter-retained multi-call work, calls
 *      aius_network_teensy_apply() once (NONE advances retained work);
 *   2. otherwise a retained adapter event calls aius_net_supervisor_step() once;
 *   3. otherwise aius_network_teensy_poll() runs once; an idle poll is followed
 *      by one aius_net_supervisor_step(NONE) so the supervisor's only
 *      time-driven transitions -- the DHCP deadline and the backoff expiry --
 *      are still evaluated. Nothing else ever produces an event for them, so
 *      without that step DHCP_WAIT and BACKOFF would both be terminal.
 *
 * It never applies and polls in the same epoch, and performs at most one adapter
 * operation per epoch. Applying a newly issued
 * AIUS_NET_ACTION_TEARDOWN discards the HTTP transaction *before* the adapter
 * releases listener, UDP or stack resources, so a late transaction can never
 * touch a resource the teardown already freed.
 *
 * The HTTP/storage bridge is explicit and root-brokered: the composition root
 * passes one retained storage event in and one SD-backed HTTP action out. This
 * module performs no SD primitive and owns no SD job.
 */

#ifndef AIUS_RUNTIME_NETWORK_HTTP_H
#define AIUS_RUNTIME_NETWORK_HTTP_H

#include "aius_http.h"
#include "aius_http_transaction.h"
#include "aius_net.h"
#include "aius_network_teensy.h"
#include "aius_optical_app.h"
#include "aius_runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Admission hook (Phase 4f F1/F2). Consulted at the DISPATCH arm before
 * aius_optical_app_http_dispatch (AIUS_HTTP_ADMIT_ROUTE) and at the SD_START arm
 * before a job starts (AIUS_HTTP_ADMIT_SD_START). The default admits everything.
 *
 *   ADMIT  proceed.
 *   SHED   answer 503 now: a ROUTE query gets {"error":"audio_busy"} without
 *          dispatching; an SD_START query fails the start the same way. Both
 *          carry "Retry-After: 1" and call aius_runtime_http_note_shed().
 *   DEFER  SD_START only (a ROUTE DEFER is treated as ADMIT): hold the start
 *          and ask again next HTTP epoch, with no transaction step. The
 *          callback owns the deadline (it returns SHED when its 500 ms millis()
 *          timer from the first deferral expires) and any sd_fifo_defer()
 *          bookkeeping; this module only obeys.
 *
 * sd_job is the transaction's job request (kind, incl. OTA staging); NULL for
 * ROUTE. Mode lives in the callback (it reads the SD mode itself). */
typedef enum {
	AIUS_HTTP_ADMIT_ROUTE = 0,
	AIUS_HTTP_ADMIT_SD_START
} AiusHttpAdmitKind;

typedef enum {
	AIUS_HTTP_VERDICT_ADMIT = 0,
	AIUS_HTTP_VERDICT_SHED,
	AIUS_HTTP_VERDICT_DEFER
} AiusHttpAdmitVerdict;

typedef struct {
	AiusHttpAdmitKind kind;
	AiusHttpRoute route;
	AiusHttpMethod method;
	const AiusSdJobRequest *sd_job;
} AiusHttpAdmitQuery;

/* Link-time hooks. The platform (optical_main.cpp) defines both; this module
 * defines weak defaults (admit everything, ignore sheds) so host builds work
 * unchanged. aius_runtime_http_note_shed() is called once per SHED verdict. */
AiusHttpAdmitVerdict aius_runtime_http_admit(const AiusHttpAdmitQuery *query);
void aius_runtime_http_note_shed(void);

typedef struct {
	AiusNetSupervisor supervisor;
	AiusNetworkTeensy target;
	AiusHttpTransaction http;
	uint8_t http_request[AIUS_HTTP_MAX_REQUEST];
	uint8_t http_response[AIUS_HTTP_MAX_RESPONSE];
	AiusNetAction pending_network_action;
	AiusNetEvent pending_network_event;
	AiusHttpEvent pending_http_event;
	AiusHttpApplicationResponse pending_application_response;
	bool network_action_present;
	bool network_event_present;
	bool http_event_present;
	bool application_response_present;
	/* Set by the NETWORK checkpoint when a teardown discarded the HTTP
	 * transaction; the root check-and-clears it to release the storage
	 * service's stranded HTTP authority. Not a service-to-service call. */
	bool http_discarded;
	/* True when the last aius_runtime_http_service() call actually stepped
	 * the transaction, and therefore consumed the storage event it was
	 * given. The root clears the storage event only then, so an offline
	 * epoch cannot silently drop a correlated SD result. */
	bool http_stepped;
	/* A DEFERred SD_START is held: the transaction's SD_START action is still
	 * outstanding and is re-issued from its state when admitted. Sits in the
	 * padding before the counters (no struct growth); cleared by teardown. */
	bool deferred_start_present;
	/* A DEFERred SD_START is held: the transaction's SD_START action is still
	 * outstanding and is re-issued from its state when admitted. Sits in the
	 * padding before the counters (no struct growth); cleared by teardown. */
	/* Status counters, observed at this seam because the legacy producers
	 * died with service_http() at the cutover. They survive a network
	 * teardown (http_init() re-inits only the transaction) and reset at
	 * construct. Semantics:
	 *
	 *   http_requests      one per DISPATCH action -- a parsed request
	 *                      handed to the application, whether it answers
	 *                      immediately, starts an SD job, or returns null.
	 *   http_rejections    one per connection first-faulting with
	 *                      PROTOCOL or REQUEST_CAPACITY -- the fixed 4xx
	 *                      refusals (bad request line, unknown route, bad
	 *                      method, oversize, and the null-dispatch 400).
	 *   http_client_drops  transport losses, counted per occurrence at the
	 *                      seam's own IO observation (FAULT on any
	 *                      transport action, CLOSED mid-read/mid-write)
	 *                      plus one per connection first-faulting with a
	 *                      request/response/upload/download timeout -- the
	 *                      modern spelling of the legacy idle/stall drop.
	 *
	 * The fourth legacy counter, http_service_overruns, is a wall-clock
	 * measurement and belongs to the composition root's checkpoint hook,
	 * not to this portable seam. */
	uint32_t http_requests;
	uint32_t http_rejections;
	uint32_t http_client_drops;
} AiusRuntimeNetworkHttp;

/* Memory only. Initializes the transaction against the embedded buffers,
 * initializes the supervisor with `recovery_safe`, and retains the supervisor's
 * initial action. No target primitive is called. */
void aius_runtime_network_http_construct(
	AiusRuntimeNetworkHttp *service,
	uint32_t now_ms,
	bool recovery_safe);

/* One NETWORK checkpoint. Returns false only for an invalid call (null service
 * or budget, or a budget that is not the NETWORK descriptor). */
bool aius_runtime_network_service(
	AiusRuntimeNetworkHttp *service,
	const AiusRuntimeBudget *budget,
	uint32_t now_ms);

/* One HTTP checkpoint: at most one transaction step, and at most one resulting
 * transport primitive or application dispatch. An SD-backed action is returned
 * through `storage_action` instead of being executed here. `storage_event` is
 * the storage service's retained correlated result, or NULL.
 *
 * Returns false only for an invalid call (null service, budget, or
 * storage_action, or a budget that is not the HTTP descriptor). */
bool aius_runtime_http_service(
	AiusRuntimeNetworkHttp *service,
	const AiusRuntimeBudget *budget,
	uint32_t now_ms,
	AiusOpticalAppDispatcher *app,
	const AiusHttpEvent *storage_event,
	AiusHttpAction *storage_action);

#ifdef __cplusplus
}
#endif

#endif
