#ifndef AIUS_NETWORK_TEENSY_H
#define AIUS_NETWORK_TEENSY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "aius_io.h"
#include "aius_net.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Link state without a vendor type. The values match the NativeEthernet
 * EthernetLinkStatus ordering (Unknown, LinkON, LinkOFF) so a status surface
 * that used to publish the raw vendor value keeps its numbering.
 */
typedef enum {
	AIUS_NETWORK_TEENSY_LINK_UNKNOWN = 0,
	AIUS_NETWORK_TEENSY_LINK_UP = 1,
	AIUS_NETWORK_TEENSY_LINK_DOWN = 2
} AiusNetworkTeensyLink;

/* How long a generation may wait for its link to come up before the adapter
 * calls it a resource fault. Starting the stack is not evidence that the link
 * is up -- Ethernet.beginNoWait() deliberately returns before the PHY has
 * negotiated -- so AIUS_NET_EVENT_STACK_STARTED is emitted only once link-up
 * has actually been observed. That observation may never arrive (no cable, dead
 * PHY, an unresponsive MDIO read that degrades to UNKNOWN), and the portable
 * supervisor has no deadline of its own in STACK_START, so this deadline is
 * what keeps the wait bounded: on expiry the adapter emits a current-generation
 * RESOURCE_FAULT and the supervisor tears down, backs off and retries with the
 * next generation. It sits above a realistic autonegotiation (1-2 s) so a link
 * that is merely slow is not torn down, and below the 5 s DHCP deadline so a
 * dead link is never the more expensive failure of the two.
 */
#define AIUS_NETWORK_TEENSY_LINK_DEADLINE_MS 3000u

/* How long a generation that initialised the interface may spend, measured from
 * the same stack start the link deadline is measured from, waiting for evidence
 * that the interface is actually carrying traffic before DHCP starts anyway.
 *
 * Link-up is not that evidence. Measured on the board: generation 1 reported
 * link up throughout, spent 1,635 ms inside beginNoWait() resetting the PHY and
 * completing autonegotiation, started DHCP 22 ms later, and then received four
 * frames in five seconds on a segment whose ambient broadcast traffic delivers
 * about twenty-four -- so FNET sent a DISCOVER the FEC never carried and sat in
 * SELECTING until the deadline. Generation 2 skips fnet_netif_init() entirely,
 * inherits the settled interface and binds in 19 ms.
 *
 * The receive frame counter is the evidence, because ambient traffic arrives
 * continuously: a counter that has moved since the wait began is a frame the MAC
 * actually clocked in. This deadline is what keeps the wait bounded when it has
 * not moved -- a silent segment must not deadlock the board, so DHCP starts
 * anyway and the existing 5 s DHCP deadline, teardown and backoff apply exactly
 * as before.
 *
 * The value is one DHCP deadline (AIUS_NET_DHCP_DEADLINE_MS, asserted equal in
 * the implementation): waiting for evidence that a DISCOVER can leave the board
 * is never allowed to cost more than discovering the same thing the hard way.
 * Measuring it from stack start rather than from link-up is what keeps a whole
 * generation's pre-DHCP time bounded by that one figure -- the 1,635 ms
 * beginNoWait() already spends is counted against the budget rather than added
 * to it, which on the measured board leaves about 3.4 s of settle time.
 *
 * It costs the fast path nothing. Only a generation whose beginNoWait() actually
 * ran fnet_netif_init() arms the wait at all; the generation that skips
 * initialisation -- the one that binds in 19 ms today -- releases STACK_STARTED
 * in the same call it always did, with no extra underlying call.
 */
#define AIUS_NETWORK_TEENSY_RX_READY_DEADLINE_MS 5000u

/* "No DHCP client is registered on the interface", reported where FNET's own
 * DHCP client state would otherwise go. FNET's state enum is private to
 * service/dhcp/fnet_dhcp_cln.c and its public getter, fnet_dhcp_cln_get_state(),
 * dereferences its descriptor without a null check -- so the adapter looks the
 * descriptor up first and reports this sentinel rather than calling the getter
 * with nothing. 255 cannot collide: the private enum runs 0..9.
 */
#define AIUS_NETWORK_TEENSY_DHCP_STATE_NONE 0xFFu

/* Which observer of link-up released the generation's STACK_STARTED. The three
 * are not equivalent evidence, and telling them apart is the whole point of
 * this field: NativeEthernet's linkStatus() returns a cached member whose
 * initial value is sampled inside beginNoWait() (NativeEthernet.cpp:166), so a
 * STACK_START release is an init-time snapshot, while a callback release is the
 * FNET link service's own fresh PHY read.
 */
typedef enum {
	AIUS_NETWORK_TEENSY_LINK_UP_NONE = 0,
	AIUS_NETWORK_TEENSY_LINK_UP_STACK_START = 1,
	AIUS_NETWORK_TEENSY_LINK_UP_CALLBACK = 2,
	AIUS_NETWORK_TEENSY_LINK_UP_POLL = 3
} AiusNetworkTeensyLinkUpSource;

/* Whether the generation's beginNoWait() ran fnet_netif_init() or skipped it. */
typedef enum {
	AIUS_NETWORK_TEENSY_NETIF_INIT_UNKNOWN = 0,
	AIUS_NETWORK_TEENSY_NETIF_INIT_PERFORMED = 1,
	AIUS_NETWORK_TEENSY_NETIF_INIT_SKIPPED = 2
} AiusNetworkTeensyNetifInit;

/* What released the generation's receive-readiness gate, and therefore its
 * STACK_STARTED. NOT_REQUIRED is the generation that skipped fnet_netif_init()
 * and never waited; COUNTER is the frame the MAC clocked in; DEADLINE is the
 * bounded fallback that starts DHCP on a segment that delivered nothing. NONE
 * means the gate is still closed, or the generation never reached link-up.
 */
typedef enum {
	AIUS_NETWORK_TEENSY_RX_READY_NONE = 0,
	AIUS_NETWORK_TEENSY_RX_READY_NOT_REQUIRED = 1,
	AIUS_NETWORK_TEENSY_RX_READY_COUNTER = 2,
	AIUS_NETWORK_TEENSY_RX_READY_DEADLINE = 3
} AiusNetworkTeensyRxReady;

/* Why a generation's diagnostic record stopped accumulating. */
typedef enum {
	AIUS_NETWORK_TEENSY_END_NONE = 0,
	AIUS_NETWORK_TEENSY_END_TEARDOWN = 1,
	AIUS_NETWORK_TEENSY_END_ONLINE = 2
} AiusNetworkTeensyEnd;

/* Per-generation measurement record. The record itself is diagnostic: no field
 * below is read by an adapter decision, and deleting the struct would not change
 * one action, event or deadline.
 *
 * The readers behind it are no longer purely diagnostic, and saying so is the
 * point of this paragraph. Two entries of AiusNetworkTeensyDiagPrimitives feed
 * the receive-readiness gate as well as this record -- netif_initialized() says
 * whether the generation must wait at all, and frames() supplies the evidence it
 * waits for. An adapter wired without that table still behaves identically: no
 * probe means no readiness requirement, so the gate is transparent and every
 * measurement reads unavailable, which is exactly how the whole lifecycle suite
 * drives it.
 *
 * It exists to settle one question with data rather than inference -- why the
 * first DHCP attempt of every boot receives nothing for its full deadline while
 * the second acquires a lease in under a second. Every millisecond field is an
 * offset from stack start, because the absolute value carries no information
 * the reader has and the offset is what the question is about.
 *
 * What each field costs, so a later reader does not have to re-derive it: the
 * whole record is filled from at most ten underlying calls per generation in the
 * epoch's action path (START_STACK, START_DHCP, teardown, publish-online), plus
 * one frame-counter read per epoch while a readiness gate is open. That read is
 * not additional: it replaces the link-status read the same epoch would
 * otherwise have made, so an epoch still performs exactly one underlying read,
 * and only the generation that initialised the interface pays it at all. None is
 * per audio block, and the only one that can reach the PHY is the fresh link
 * read, which FNET itself throttles to one MDIO transaction per 200 ms across
 * all callers (fnet_netif.c:1021) and which its own link service already
 * performs at exactly that rate.
 */
typedef struct {
	uint32_t generation;

	/* millis() at stack start, the base every *_ms offset below is measured
	 * from. The only absolute value in the record.
	 */
	uint32_t stack_started_ms_absolute;

	/* Whether beginNoWait() performed the real fnet_netif_init() for this
	 * generation or found the interface already initialised and skipped it
	 * (NativeEthernet.cpp:151). Sampled before the call, so it describes what
	 * the call was about to do. AiusNetworkTeensyNetifInit -- tri-state,
	 * because "the probe was not available" is not the same answer as
	 * "the interface was already up" and must not be readable as one.
	 */
	uint8_t netif_init;

	/* Wall time beginNoWait() itself took. It is not the "no wait" its name
	 * promises on the generation that initialises the netif: fnet_netif_init()
	 * resets the PHY and blocks in _fnet_eth_phy_init() until autonegotiation
	 * completes or FNET_CFG_CPU_ETH_ATONEGOTIATION_TIMEOUT (2000 ms) expires.
	 * Saturates rather than wrapping.
	 */
	uint16_t stack_start_blocking_ms;

	/* The cached link value the START_STACK gate read, immediately after
	 * beginNoWait() returned. AiusNetworkTeensyLink, or UNKNOWN when the
	 * stack failed to start and the read was never reached.
	 */
	uint8_t link_at_stack_start;

	/* The most recent cached link value the adapter has read, refreshed by
	 * every read it already performs and by nothing else. It is what
	 * link_at_end reports, so naming the cached value at the end of a
	 * generation costs no additional read.
	 */
	uint8_t link_cached_last;

	/* Which observer released STACK_STARTED, and when.
	 * AiusNetworkTeensyLinkUpSource; link_up_ms is meaningful only when it is
	 * not NONE.
	 */
	uint8_t link_up_source;
	uint32_t link_up_ms;

	/* The FNET link service's own fresh observation, independent of the latch
	 * above: when its callback first delivered connected, and how many link
	 * callbacks of either polarity the generation saw. The service starts each
	 * generation with its connection flag clear, so an already-up link still
	 * produces one callback -- which makes this a complete answer to "when did
	 * a fresh read first report up", at no extra call.
	 */
	bool link_callback_up_seen;
	uint32_t link_callback_up_ms;
	uint8_t link_callbacks;

	/* What released the receive-readiness gate and when, as an offset from
	 * stack start. AiusNetworkTeensyRxReady; rx_ready_ms is meaningful only
	 * when the source is COUNTER or DEADLINE. This is the field that says
	 * whether the gate did its job: COUNTER means a frame arrived and DHCP
	 * started on evidence, DEADLINE means nothing arrived within the bound
	 * and DHCP started anyway. dhcp_start_ms shows the same story in
	 * milliseconds; this shows the reason.
	 */
	uint8_t rx_ready_source;
	uint32_t rx_ready_ms;

	/* When START_DHCP ran, and FNET's own DHCP client state immediately after
	 * dhcpStart() returned.
	 */
	bool dhcp_started_seen;
	uint32_t dhcp_start_ms;
	uint8_t dhcp_state_at_start;

	/* The end of the generation: teardown or online, whichever came first.
	 * AiusNetworkTeensyEnd. dhcp_state_at_end is FNET's client state at that
	 * moment -- the discriminating value when a DHCP deadline expires.
	 * link_at_end is the last cached value the adapter read and
	 * link_live_at_end a fresh read taken at the end itself, so the two can be
	 * compared at the decisive moment; they disagreeing is the observation
	 * that would convict the cached member.
	 */
	uint8_t end_reason;
	uint32_t end_ms;
	uint8_t dhcp_state_at_end;
	uint8_t link_at_end;
	uint8_t link_live_at_end;

	/* Ethernet frames the MAC counted between START_DHCP and the end of the
	 * generation, from FNET's netif statistics (the FEC's RMON_T_PACKETS /
	 * RMON_R_PACKETS hardware counters). This is the frame-level discriminator:
	 * a DHCP window that ends with tx_frames == 0 never handed a DISCOVER to
	 * the MAC at all, while a non-zero count means the MAC clocked one out and
	 * it was lost past the FEC. False frames_valid means the driver refused the
	 * statistics call and neither count means anything.
	 */
	bool frames_valid;
	uint32_t tx_frames;
	uint32_t rx_frames;
	uint32_t tx_at_dhcp_start;
	uint32_t rx_at_dhcp_start;
} AiusNetworkTeensyDiag;

/* lease_acquired and dhcp_updates are DHCP state the adapter already sees on
 * its own lease callback: lease_acquired is generation-scoped and cleared at
 * every stack start and teardown, dhcp_updates counts leases since init and is
 * never cleared. Neither costs an underlying read.
 *
 * link_up_reported is the generation-scoped latch that makes STACK_STARTED
 * exactly-once: the stack-start link read, the NativeEthernet link callback and
 * the poll link read are three observers of one event, and whichever sees it
 * first sets the latch. stack_started_ms stamps both bounded waits below.
 *
 * The rx_ready_* fields are the receive-readiness gate, and they are decision
 * state rather than measurement. rx_ready_required is set once per generation
 * from the netif-init probe: only the generation that actually initialised the
 * interface waits, so the generation that inherits a settled one is never
 * delayed. rx_ready_pending is open between link-up and the release, and it is
 * what makes poll() spend its one underlying read on the frame counters instead
 * of the link status for that window. rx_ready_baseline is the first counter
 * value sampled once the gate opens -- lazily, on the first poll of the wait, so
 * arming costs no call of its own -- and rx_ready_baseline_valid says whether it
 * has been taken. Movement is an unsigned difference against that baseline, so
 * these free-running MAC counters may wrap without the gate concluding that
 * nothing arrived.
 *
 * diag records the live generation; diag_first keeps the first generation that
 * ever ended, because the generation under investigation is always generation 1
 * and nothing can read this surface until a later generation has brought HTTP
 * up. diag_first_valid says whether that copy has been taken.
 */
typedef struct {
	uint32_t generation;
	bool stack_started;
	bool dhcp_started;
	bool lease_acquired;
	bool online;
	bool listener_open;
	bool udp_open;
	bool link_up_reported;
	bool rx_ready_required;
	bool rx_ready_pending;
	bool rx_ready_baseline_valid;
	uint32_t rx_ready_baseline;
	uint32_t stack_started_ms;
	AiusNetEvent pending[4];
	uint8_t pending_read;
	uint8_t pending_count;
	bool callback_overflow;
	AiusNetAction active_action;
	uint8_t action_step;
	bool action_fault;
	uint32_t dhcp_updates;
	AiusNetworkTeensyDiag diag;
	AiusNetworkTeensyDiag diag_first;
	bool diag_first_valid;
} AiusNetworkTeensy;

void aius_network_teensy_init(AiusNetworkTeensy *adapter);

/* START_STACK starts the stack and then takes the one permitted link read. It
 * emits STACK_STARTED only when that read says the link is already up *and*
 * this generation needs no receive-readiness wait -- which is the generation
 * that skipped fnet_netif_init(), the one that binds in 19 ms today. Otherwise
 * it emits nothing and the wait continues in poll(). now_ms stamps both the
 * link-wait and readiness deadlines.
 */
void aius_network_teensy_apply(AiusNetworkTeensy *adapter, uint32_t now_ms,
	                           AiusNetAction action, AiusNetEvent *event);

/* Drains at most one queued callback event, then takes at most one underlying
 * read -- of the link status, or, while a receive-readiness gate is open, of the
 * MAC's frame counters instead. Never both, so the per-epoch cost is unchanged.
 *
 * While a started generation has not yet reported link-up, the link read emits
 * STACK_STARTED on the first UP, or one RESOURCE_FAULT once
 * AIUS_NETWORK_TEENSY_LINK_DEADLINE_MS has passed since the stack started.
 * A generation that initialised the interface does not emit STACK_STARTED on
 * link-up; it opens the readiness gate instead, and the counter read emits
 * STACK_STARTED on the first frame received since the gate opened, or at
 * AIUS_NETWORK_TEENSY_RX_READY_DEADLINE_MS after the stack started, whichever
 * comes first.
 *
 * After link-up has been reported and the gate has released, the link read is
 * the health check it always was: a DOWN read is AIUS_NET_FAULT_LINK_LOST.
 * Link loss during the readiness window is still seen -- the NativeEthernet link
 * callback is an independent observer and its event is drained ahead of the
 * read. A queued STACK_STARTED for a generation that already reported link-up is
 * dropped rather than delivered twice.
 */
void aius_network_teensy_poll(AiusNetworkTeensy *adapter, uint32_t now_ms,
	                          AiusNetEvent *event);

AiusIoEvent aius_network_teensy_accept(AiusNetworkTeensy *adapter);

AiusIoEvent aius_network_teensy_read_once(AiusNetworkTeensy *adapter,
	                                      uint8_t *buffer, size_t capacity);

AiusIoEvent aius_network_teensy_write_once(AiusNetworkTeensy *adapter,
	                                       const uint8_t *buffer,
	                                       size_t length);

AiusIoEvent aius_network_teensy_close_once(AiusNetworkTeensy *adapter);

AiusIoEvent aius_network_teensy_udp_receive_once(AiusNetworkTeensy *adapter,
	                                             uint8_t *buffer,
	                                             size_t capacity);

AiusIoEvent aius_network_teensy_udp_send_once(AiusNetworkTeensy *adapter,
	                                          const uint8_t *buffer,
	                                          size_t length);

/* Status accessors for the discovery surfaces (status JSON, UDP status payload,
 * USB ID). Each performs at most one underlying read, never waits, and is safe
 * in any supervisor state: before the stack starts, while an action is in
 * flight, and after teardown they return the not-available answer without
 * touching a dead stack.
 */

/* UNKNOWN whenever the stack is not started, so a caller can distinguish
 * "no link" from "nothing to ask".
 */
AiusNetworkTeensyLink aius_network_teensy_link(
	const AiusNetworkTeensy *adapter);

/* Writes the current IPv4 address as four octets, first octet first, and
 * returns whether that address is usable. A false return always leaves
 * 0.0.0.0 behind, so an unchecked caller still serializes a defined value.
 */
bool aius_network_teensy_address(const AiusNetworkTeensy *adapter,
	                             uint8_t address[4]);

/* DHCP has been asked for a lease on the current generation and no lease has
 * arrived yet. Reads adapter state only.
 */
bool aius_network_teensy_dhcp_pending(const AiusNetworkTeensy *adapter);

/* The measurement surface. Diagnostic only, and every entry below is safe in
 * any supervisor state.
 *
 * The two records read adapter memory and cost nothing. The three live readers
 * each perform at most one underlying call, never wait, and answer for a
 * stack that is not started without touching it -- they are meant for a status
 * serializer, not for the epoch.
 */
const AiusNetworkTeensyDiag *aius_network_teensy_diag(
	const AiusNetworkTeensy *adapter);

/* The first generation that ever ended, or NULL while none has. */
const AiusNetworkTeensyDiag *aius_network_teensy_diag_first(
	const AiusNetworkTeensy *adapter);

/* A fresh link observation, deliberately distinct from
 * aius_network_teensy_link(): that one returns NativeEthernet's cached member,
 * this one asks the interface. Costs at most one MDIO transaction per 200 ms --
 * FNET throttles the PHY read itself and its link service already polls at that
 * rate, so in practice this reads back the value that poll left behind.
 */
AiusNetworkTeensyLink aius_network_teensy_link_live(
	const AiusNetworkTeensy *adapter);

/* FNET's own DHCP client state, or AIUS_NETWORK_TEENSY_DHCP_STATE_NONE when no
 * client is registered on the interface. A descriptor lookup and a field read;
 * no PHY, no socket, no wait.
 */
uint8_t aius_network_teensy_dhcp_state(const AiusNetworkTeensy *adapter);

/* The MAC's transmitted and received frame counters. Returns false and leaves
 * both zero when the driver has no statistics to give.
 */
bool aius_network_teensy_frames(const AiusNetworkTeensy *adapter, uint32_t *tx,
	                            uint32_t *rx);

#ifdef AIUS_NETWORK_TEENSY_TESTING

typedef struct {
	void (*callbacks_install)(AiusNetworkTeensy *adapter,
	                          uint32_t generation);
	void (*callbacks_clear)(void);
	bool (*stack_start)(void);
	bool (*dhcp_start)(void);
	bool (*listener_open)(void);
	bool (*udp_open)(void);
	bool (*listener_close)(void);
	bool (*udp_close)(void);
	bool (*stack_close)(void);
	AiusNetworkTeensyLink (*link_status)(void);
	AiusIoEvent (*accept_once)(void);
	AiusIoEvent (*read_once)(uint8_t *buffer, size_t capacity);
	AiusIoEvent (*write_once)(const uint8_t *buffer, size_t length);
	AiusIoEvent (*close_once)(void);
	AiusIoEvent (*udp_receive_once)(uint8_t *buffer, size_t capacity);
	AiusIoEvent (*udp_send_once)(const uint8_t *buffer, size_t length);
} AiusNetworkTeensyPrimitives;

/* The measurement readers, deliberately a second table rather than five more
 * entries in the one above. That table is the lifecycle and socket-I/O
 * failure-injection seam and every entry in it is required; these are optional
 * as a unit, and an adapter wired without them behaves as it did before the
 * receive-readiness gate existed and reports every measurement as unavailable.
 *
 * Two of them are no longer purely diagnostic: netif_initialized() decides
 * whether a generation arms the readiness gate, and frames() supplies the
 * evidence that releases it. That is deliberate -- the counters were already
 * read for the record, and the alternative was a second mechanism observing the
 * same thing. Keeping the table separate and optional is still what makes the
 * claim checkable: with no table installed the gate never arms, which is how the
 * whole lifecycle, callback, deadline and I/O suite drives the adapter.
 */
typedef struct {
	bool (*netif_initialized)(void);
	AiusNetworkTeensyLink (*link_live)(void);
	uint8_t (*dhcp_state)(void);
	bool (*frames)(uint32_t *tx, uint32_t *rx);
	uint32_t (*uptime_ms)(void);
} AiusNetworkTeensyDiagPrimitives;

/* Clears the diagnostic table as well, so a case that wants one installs it. */
void aius_network_teensy_test_set_primitives(
	const AiusNetworkTeensyPrimitives *primitives);

void aius_network_teensy_test_set_diag_primitives(
	const AiusNetworkTeensyDiagPrimitives *primitives);

void aius_network_teensy_test_callback(AiusNetworkTeensy *adapter,
	                                   uint32_t generation,
	                                   AiusNetEventKind kind,
	                                   AiusNetFault fault);

uint32_t aius_network_teensy_test_stale_callbacks(void);

/* The address read is a compile-time seam rather than an entry in the
 * primitives table above: that table is the lifecycle and socket-I/O
 * failure-injection seam, and a status read needs no failure injection, only a
 * value and a read count.
 */
void aius_network_teensy_test_set_address(const uint8_t address[4]);

uint32_t aius_network_teensy_test_address_reads(void);

#endif

#ifdef __cplusplus
}
#endif

#endif
