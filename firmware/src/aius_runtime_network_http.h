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
