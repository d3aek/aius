/* Independent oracle for the composed Teensy runtime (Phase 12).
 *
 * Drives the real composition root, the real three services, the real portable
 * scheduler, the real HTTP/SD/USB engines and the real application dispatcher
 * against fake network, SD, CDC, clock, checkpoint and watchdog primitives. The
 * fakes sit at the lowest seam each adapter exposes, so every layer under test
 * is production code.
 *
 * Registries assert exact counts and reject duplicate keys, matching the style
 * of test/runtime/test_runtime.c and test/optical-app/test_optical_app.c.
 */

#include "aius_cdc_teensy.h"
#include "aius_network_teensy.h"
#include "aius_runtime_network_http.h"
#include "aius_runtime_storage.h"
#include "aius_runtime_teensy.h"
#include "aius_runtime_usb.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aius_producer_yield.h"

#define ARRAY_COUNT(values) (sizeof(values) / sizeof((values)[0]))
#define CHECK(condition) check((condition), #condition, __LINE__)

static const char *current_key = "startup";

static void check(bool condition, const char *expression, int line)
{
	if (condition)
		return;
	fprintf(stderr, "runtime-target oracle mismatch [%s] line %d: %s\n",
	        current_key, line, expression);
	exit(1);
}

static void validate_unique_keys(const char *registry_key,
                                 const char *const *keys, size_t count,
                                 size_t expected_count)
{
	current_key = registry_key;
	CHECK(count == expected_count);
	for (size_t i = 0; i < count; ++i) {
		CHECK(keys[i] != nullptr);
		CHECK(keys[i][0] != '\0');
		for (size_t j = i + 1u; j < count; ++j)
			CHECK(strcmp(keys[i], keys[j]) != 0);
	}
}

/* ------------------------------------------------------------------ */
/* fake clock                                                          */
/* ------------------------------------------------------------------ */

static uint32_t g_micros_now;

uint32_t micros(void)
{
	return ++g_micros_now;
}

static uint32_t g_millis_now;

/* Always advances: a stalled clock would hang the very drain loop the
 * bootloader bound exists to cap. */
uint32_t millis(void)
{
	return ++g_millis_now;
}

/* ------------------------------------------------------------------ */
/* fake CDC / Serial                                                   */
/* ------------------------------------------------------------------ */

struct SerialFake {
	bool connected;
	int available_for_write;
	/* Transmit-ring drain script: after `drain_after_polls` further
	 * availableForWrite() calls the ring reports `drained_available`, which
	 * models the host retiring the queued transfer. Zero disables it. */
	unsigned drain_after_polls;
	int drained_available;
	const uint8_t *rx;
	size_t rx_length;
	size_t rx_offset;
	uint8_t tx[512];
	size_t tx_length;
	size_t write_accept;
	unsigned reboots;
	unsigned read_calls;
	unsigned write_calls;
	unsigned capacity_calls;
	unsigned flush_calls;
	/* flush_calls sampled when _reboot_Teensyduino_() ran, so "flushed
	 * before handing over" is provable rather than inferred. */
	unsigned flush_calls_at_reboot;
};

static SerialFake g_serial;

FakeUsbSerial Serial;

FakeUsbSerial::operator bool()
{
	return g_serial.connected;
}

int FakeUsbSerial::read()
{
	g_serial.read_calls++;
	if (g_serial.rx_offset >= g_serial.rx_length)
		return -1;
	return (int)g_serial.rx[g_serial.rx_offset++];
}

int FakeUsbSerial::availableForWrite()
{
	g_serial.capacity_calls++;
	if (g_serial.drain_after_polls != 0u &&
	    --g_serial.drain_after_polls == 0u)
		g_serial.available_for_write = g_serial.drained_available;
	return g_serial.available_for_write;
}

size_t FakeUsbSerial::write(const uint8_t *data, size_t length)
{
	size_t accepted = g_serial.write_accept != 0u &&
			          g_serial.write_accept < length ?
			          g_serial.write_accept :
			          length;
	g_serial.write_calls++;
	for (size_t i = 0; i < accepted; ++i)
		if (g_serial.tx_length < sizeof(g_serial.tx))
			g_serial.tx[g_serial.tx_length++] = data[i];
	return accepted;
}

void FakeUsbSerial::flush()
{
	g_serial.flush_calls++;
}

void _reboot_Teensyduino_(void)
{
	g_serial.reboots++;
	g_serial.flush_calls_at_reboot = g_serial.flush_calls;
}

/* A scriptable CDC primitive table independent of the Serial fake, so the
 * adapter's normalization can be driven past what a real driver can return. */
struct CdcFake {
	void *expected_context;
	bool connected;
	int read_value;
	int capacity_value;
	/* Transmit-ring drain script, as SerialFake's: after this many further
	 * available_for_write() calls the ring reports drained_capacity_value.
	 * Zero leaves capacity_value alone, so a ring that never returns to its
	 * high-water mark models a host that never takes the response. */
	unsigned drain_after_polls;
	int drained_capacity_value;
	size_t write_result;
	uint32_t millis_value;
	uint32_t millis_step;
	unsigned connected_calls;
	unsigned read_calls;
	unsigned capacity_calls;
	unsigned write_calls;
	unsigned flush_calls;
	unsigned millis_calls;
	unsigned bootloader_calls;
	/* flush_calls sampled when the entry primitive ran. */
	unsigned flush_calls_at_entry;
	size_t last_write_length;
};

static CdcFake g_cdc;

static bool cdc_connected(void *context)
{
	CHECK(context == g_cdc.expected_context);
	g_cdc.connected_calls++;
	return g_cdc.connected;
}

static int cdc_read(void *context)
{
	CHECK(context == g_cdc.expected_context);
	g_cdc.read_calls++;
	return g_cdc.read_value;
}

static int cdc_available_for_write(void *context)
{
	CHECK(context == g_cdc.expected_context);
	g_cdc.capacity_calls++;
	if (g_cdc.drain_after_polls != 0u &&
	    --g_cdc.drain_after_polls == 0u)
		g_cdc.capacity_value = g_cdc.drained_capacity_value;
	return g_cdc.capacity_value;
}

static size_t cdc_write(void *context, const uint8_t *data, size_t length)
{
	CHECK(context == g_cdc.expected_context);
	CHECK(data != nullptr);
	g_cdc.write_calls++;
	g_cdc.last_write_length = length;
	return g_cdc.write_result != 0u ? g_cdc.write_result : length;
}

static void cdc_flush(void *context)
{
	CHECK(context == g_cdc.expected_context);
	g_cdc.flush_calls++;
}

static uint32_t cdc_millis(void *context)
{
	CHECK(context == g_cdc.expected_context);
	g_cdc.millis_calls++;
	/* Always advances, whatever the test scripted: a stalled clock would
	 * hang the drain loop the bootloader bound exists to cap. */
	g_cdc.millis_value +=
		g_cdc.millis_step != 0u ? g_cdc.millis_step : 1u;
	return g_cdc.millis_value;
}

static void cdc_enter_bootloader(void *context)
{
	CHECK(context == g_cdc.expected_context);
	g_cdc.bootloader_calls++;
	g_cdc.flush_calls_at_entry = g_cdc.flush_calls;
}

static AiusCdcTeensyPrimitives cdc_primitives(void)
{
	AiusCdcTeensyPrimitives primitives;

	memset(&primitives, 0, sizeof(primitives));
	primitives.context = &g_cdc;
	primitives.connected = cdc_connected;
	primitives.read = cdc_read;
	primitives.available_for_write = cdc_available_for_write;
	primitives.write = cdc_write;
	primitives.flush = cdc_flush;
	primitives.millis = cdc_millis;
	primitives.enter_bootloader = cdc_enter_bootloader;
	g_cdc.expected_context = &g_cdc;
	return primitives;
}

/* ------------------------------------------------------------------ */
/* fake SD                                                             */
/* ------------------------------------------------------------------ */

struct SdFake {
	bool begin_ok;
	bool media_present;
	/* SdFat's own FAT-type numbering for the mounted volume, and how many
	 * times the adapter read it. The read is not a card primitive, so it
	 * must never move primitive_calls. */
	uint8_t fat_type;
	unsigned fat_type_reads;
	bool exists_result;
	bool remove_ok;
	bool open_ok;
	/* One byte past the service's publication capacity, so a file the job
	 * must refuse can be staged as well as the largest it must accept. */
	uint8_t data[AIUS_RUNTIME_STORAGE_PUBLISH_CAPACITY + 1u];
	size_t data_length;
	size_t read_offset;
	unsigned primitive_calls;
	unsigned primitive_calls_epoch;
	unsigned open_calls;
	unsigned close_calls;
	unsigned read_calls;
	/* Every read primitive's requested length, so a bounded-slice claim is
	 * checked per call rather than inferred from the call count. */
	size_t read_lengths[64];
	unsigned remove_calls;
	unsigned exists_calls;
	bool file_open;
	/* A card that accepts the open and then refuses the write, which is what
	 * SdFat reports as a zero-length write. The alternative hypothesis for
	 * the intermittent upload 500, staged so the diagnostic record can be
	 * shown to tell it apart from the contention one. */
	bool write_fails;
	/* The vendored driver's error state: the code a failed primitive leaves
	 * (clearError() zeroes it), DAT3 card detection, the 1 s busy-timeout
	 * count, and the mode the last begin() asked for. */
	uint8_t error_code;
	bool card_detected;
	uint32_t busy_timeouts;
	unsigned clear_error_calls;
	bool begin_dma;
	/* Phase 4e: card primitives seen inside and outside the producer-yield
	 * SD window (aius_runtime_storage.cpp's window around the execute). */
	unsigned windowed_primitives;
	unsigned unwindowed_primitives;
};

static SdFake g_sd;

SDClass SD;

static void sd_primitive(void)
{
	g_sd.primitive_calls++;
	g_sd.primitive_calls_epoch++;
	if (g_aius_producer_yield.window_depth != 0u)
		g_sd.windowed_primitives++;
	else
		g_sd.unwindowed_primitives++;
}

/* The producer-yield platform half, faked: no pass ever runs here (no audio,
 * no work pending), so the library only counts and keeps its window. */
extern "C" {
uint32_t aius_producer_yield_plat_cycles(void) { return 0u; }
uint32_t aius_producer_yield_plat_ipsr(void) { return 0u; }
uint32_t aius_producer_yield_plat_primask(void) { return 0u; }
bool aius_producer_yield_plat_irq_software_on(void) { return true; }
bool aius_producer_yield_plat_audio_ready(void) { return false; }
bool aius_producer_yield_plat_recovery_safe(void) { return false; }
bool aius_producer_yield_plat_work_pending(void) { return false; }
void aius_producer_yield_plat_run_pass(void) {}
uint16_t aius_producer_yield_plat_ledger_checkpoint(void) { return 0u; }
void aius_producer_yield_plat_ledger_record(uint16_t checkpoint)
{
	(void)checkpoint;
}
}

File::File() : valid_(false) {}
File::File(bool valid) : valid_(valid) {}

File::operator bool() const
{
	return valid_;
}

bool File::truncate(uint64_t size)
{
	(void)size;
	sd_primitive();
	g_sd.data_length = 0u;
	return true;
}

bool File::seek(uint64_t position)
{
	sd_primitive();
	if (position > g_sd.data_length)
		return false;
	g_sd.read_offset = (size_t)position;
	return true;
}

size_t File::read(uint8_t *buffer, size_t length)
{
	size_t remaining;

	sd_primitive();
	if (g_sd.read_calls < ARRAY_COUNT(g_sd.read_lengths))
		g_sd.read_lengths[g_sd.read_calls] = length;
	g_sd.read_calls++;
	remaining = g_sd.data_length - g_sd.read_offset;
	if (length > remaining)
		length = remaining;
	memcpy(buffer, g_sd.data + g_sd.read_offset, length);
	g_sd.read_offset += length;
	return length;
}

size_t File::write(const uint8_t *buffer, size_t length)
{
	sd_primitive();
	if (g_sd.write_fails)
		return 0u;
	for (size_t i = 0; i < length; ++i)
		if (g_sd.data_length < sizeof(g_sd.data))
			g_sd.data[g_sd.data_length++] = buffer[i];
	return length;
}

uint64_t File::size()
{
	sd_primitive();
	return g_sd.data_length;
}

void File::close()
{
	sd_primitive();
	g_sd.close_calls++;
	g_sd.file_open = false;
}

bool SDClass::begin(uint8_t chip_select, bool dma)
{
	(void)chip_select;
	sd_primitive();
	g_sd.begin_dma = dma;
	return g_sd.begin_ok;
}

/* Not card primitives: they read or clear the driver's error state. */
void SDClass::clearError()
{
	g_sd.clear_error_calls++;
	g_sd.error_code = 0u;
}

uint8_t SDClass::sdErrorCode()
{
	return g_sd.error_code;
}

bool SDClass::cardDetected()
{
	return g_sd.card_detected;
}

uint32_t SDClass::busyTimeouts()
{
	return g_sd.busy_timeouts;
}

bool SDClass::mediaPresent()
{
	sd_primitive();
	return g_sd.media_present;
}

bool SDClass::mkdir(const char *path)
{
	(void)path;
	sd_primitive();
	return true;
}

File SDClass::open(const char *path, uint8_t mode)
{
	(void)path;
	(void)mode;
	sd_primitive();
	g_sd.open_calls++;
	if (!g_sd.open_ok)
		return File(false);
	g_sd.file_open = true;
	g_sd.read_offset = 0u;
	return File(true);
}

bool SDClass::exists(const char *path)
{
	(void)path;
	sd_primitive();
	g_sd.exists_calls++;
	return g_sd.exists_result;
}

bool SDClass::rename(const char *path, const char *path_to)
{
	(void)path;
	(void)path_to;
	sd_primitive();
	return true;
}

bool SDClass::remove(const char *path)
{
	(void)path;
	sd_primitive();
	g_sd.remove_calls++;
	return g_sd.remove_ok;
}

uint64_t SDClass::totalSize()
{
	sd_primitive();
	return 1024u;
}

uint64_t SDClass::usedSize()
{
	sd_primitive();
	return 512u;
}

/* Deliberately not sd_primitive(): the real fatType() reads the type the mount
 * already cached on the volume, so it starts no card transaction. */
uint8_t FsVolume::fatType() const
{
	g_sd.fat_type_reads++;
	return g_sd.fat_type;
}

FsVolume *SdFs::vol()
{
	return &volume_;
}

/* ------------------------------------------------------------------ */
/* fake network primitives                                             */
/* ------------------------------------------------------------------ */

struct NetFake {
	bool stack_start_ok;
	bool dhcp_start_ok;
	bool listener_open_ok;
	bool udp_open_ok;
	bool listener_close_ok;
	bool udp_close_ok;
	bool stack_close_ok;
	AiusNetworkTeensyLink link;
	bool client_pending;
	const uint8_t *rx;
	size_t rx_length;
	size_t rx_offset;
	uint8_t tx[4096];
	size_t tx_length;
	size_t write_accept;
	/* one-shot read injections, consumed before the rx buffer */
	unsigned read_fault_once;
	unsigned read_closed_once;
	/* per-primitive counters */
	unsigned install_calls;
	unsigned clear_calls;
	unsigned stack_start_calls;
	unsigned dhcp_start_calls;
	unsigned listener_open_calls;
	unsigned udp_open_calls;
	unsigned listener_close_calls;
	unsigned udp_close_calls;
	unsigned stack_close_calls;
	unsigned link_calls;
	unsigned accept_calls;
	unsigned read_calls;
	unsigned write_calls;
	unsigned close_calls;
	unsigned io_calls_epoch;
	unsigned lifecycle_calls_epoch;
	/* teardown ordering witness */
	uint64_t http_action_id_at_cleanup;
	bool cleanup_witnessed;
};

static NetFake g_net;
static AiusRuntimeNetworkHttp *g_witness_service;

static void net_record_cleanup(void)
{
	if (g_witness_service == nullptr || g_net.cleanup_witnessed)
		return;
	g_net.http_action_id_at_cleanup =
		g_witness_service->http.next_action_id;
	g_net.cleanup_witnessed = true;
}

static AiusIoEvent io_event(AiusIoResult result, size_t bytes, uint32_t fault)
{
	AiusIoEvent event;

	event.result = result;
	event.bytes = bytes;
	event.fault = fault;
	return event;
}

static void net_callbacks_install(AiusNetworkTeensy *adapter, uint32_t generation)
{
	(void)adapter;
	(void)generation;
	g_net.install_calls++;
	g_net.lifecycle_calls_epoch++;
}

static void net_callbacks_clear(void)
{
	g_net.clear_calls++;
	g_net.lifecycle_calls_epoch++;
	net_record_cleanup();
}

static bool net_stack_start(void)
{
	g_net.stack_start_calls++;
	g_net.lifecycle_calls_epoch++;
	return g_net.stack_start_ok;
}

static bool net_dhcp_start(void)
{
	g_net.dhcp_start_calls++;
	g_net.lifecycle_calls_epoch++;
	return g_net.dhcp_start_ok;
}

static bool net_listener_open(void)
{
	g_net.listener_open_calls++;
	g_net.lifecycle_calls_epoch++;
	return g_net.listener_open_ok;
}

static bool net_udp_open(void)
{
	g_net.udp_open_calls++;
	g_net.lifecycle_calls_epoch++;
	return g_net.udp_open_ok;
}

static bool net_listener_close(void)
{
	g_net.listener_close_calls++;
	g_net.lifecycle_calls_epoch++;
	net_record_cleanup();
	return g_net.listener_close_ok;
}

static bool net_udp_close(void)
{
	g_net.udp_close_calls++;
	g_net.lifecycle_calls_epoch++;
	net_record_cleanup();
	return g_net.udp_close_ok;
}

static bool net_stack_close(void)
{
	g_net.stack_close_calls++;
	g_net.lifecycle_calls_epoch++;
	net_record_cleanup();
	return g_net.stack_close_ok;
}

static AiusNetworkTeensyLink net_link_status(void)
{
	g_net.link_calls++;
	return g_net.link;
}

static AiusIoEvent net_accept_once(void)
{
	g_net.accept_calls++;
	g_net.io_calls_epoch++;
	if (!g_net.client_pending)
		return io_event(AIUS_IO_WOULD_BLOCK, 0u, 0u);
	g_net.client_pending = false;
	return io_event(AIUS_IO_PROGRESS, 0u, 0u);
}

static AiusIoEvent net_read_once(uint8_t *buffer, size_t capacity)
{
	size_t remaining;

	g_net.read_calls++;
	g_net.io_calls_epoch++;
	if (g_net.read_fault_once != 0u) {
		g_net.read_fault_once--;
		return io_event(AIUS_IO_FAULT, 0u, 77u);
	}
	if (g_net.read_closed_once != 0u) {
		g_net.read_closed_once--;
		return io_event(AIUS_IO_CLOSED, 0u, 0u);
	}
	remaining = g_net.rx_length - g_net.rx_offset;
	if (remaining == 0u)
		return io_event(AIUS_IO_WOULD_BLOCK, 0u, 0u);
	if (capacity < remaining)
		remaining = capacity;
	memcpy(buffer, g_net.rx + g_net.rx_offset, remaining);
	g_net.rx_offset += remaining;
	return io_event(AIUS_IO_PROGRESS, remaining, 0u);
}

static AiusIoEvent net_write_once(const uint8_t *buffer, size_t length)
{
	size_t accepted = g_net.write_accept != 0u &&
			          g_net.write_accept < length ?
			          g_net.write_accept :
			          length;

	g_net.write_calls++;
	g_net.io_calls_epoch++;
	for (size_t i = 0; i < accepted; ++i)
		if (g_net.tx_length < sizeof(g_net.tx))
			g_net.tx[g_net.tx_length++] = buffer[i];
	return io_event(AIUS_IO_PROGRESS, accepted, 0u);
}

static AiusIoEvent net_close_once(void)
{
	g_net.close_calls++;
	g_net.io_calls_epoch++;
	return io_event(AIUS_IO_PROGRESS, 0u, 0u);
}

static AiusIoEvent net_udp_receive_once(uint8_t *buffer, size_t capacity)
{
	(void)buffer;
	(void)capacity;
	return io_event(AIUS_IO_WOULD_BLOCK, 0u, 0u);
}

static AiusIoEvent net_udp_send_once(const uint8_t *buffer, size_t length)
{
	(void)buffer;
	(void)length;
	return io_event(AIUS_IO_WOULD_BLOCK, 0u, 0u);
}

static const AiusNetworkTeensyPrimitives kNetPrimitives = {
	net_callbacks_install, net_callbacks_clear,   net_stack_start,
	net_dhcp_start,        net_listener_open,     net_udp_open,
	net_listener_close,    net_udp_close,         net_stack_close,
	net_link_status,       net_accept_once,       net_read_once,
	net_write_once,        net_close_once,        net_udp_receive_once,
	net_udp_send_once,
	/* stack_poll: null exercises the socket surface exactly as before
	 * (the struct's own contract for fakes). */
	nullptr
};

/* ------------------------------------------------------------------ */
/* fake platform ops                                                   */
/* ------------------------------------------------------------------ */

#define MAX_TRACE 32u

struct PlatformFake {
	void *expected_context;
	bool reset_capture_ok;
	bool metrics_ok;
	bool udp_ok;
	bool replay_ok;
	bool audio_ok;
	AiusRuntimeTeensyBootStatus status;
	uint16_t checkpoints[MAX_TRACE];
	size_t checkpoint_count;
	const AiusRuntimeBudget *budgets[MAX_TRACE];
	uint16_t budget_checkpoints[MAX_TRACE];
	size_t budget_count;
	unsigned feeds;
	unsigned reset_capture_calls;
	unsigned metrics_calls;
	unsigned udp_calls;
	unsigned replay_calls;
	unsigned audio_calls;
};

static PlatformFake g_platform;

static void record_budget(const AiusRuntimeBudget *budget, uint16_t checkpoint)
{
	CHECK(budget != nullptr);
	CHECK(budget == aius_runtime_budget(checkpoint));
	if (g_platform.budget_count < MAX_TRACE) {
		g_platform.budgets[g_platform.budget_count] = budget;
		g_platform.budget_checkpoints[g_platform.budget_count] =
			checkpoint;
		g_platform.budget_count++;
	}
}

static bool platform_reset_capture(void *context,
                                  const AiusRuntimeBudget *budget,
                                  uint32_t now_ms,
                                  AiusRuntimeTeensyBootStatus *status)
{
	(void)now_ms;
	CHECK(context == g_platform.expected_context);
	CHECK(status != nullptr);
	record_budget(budget, AIUS_BOOT_CHECKPOINT_RESET_CAPTURE);
	g_platform.reset_capture_calls++;
	if (!g_platform.reset_capture_ok) {
		/* A failing capture writes a partial status the composition must
		 * refuse to publish. */
		status->boot_generation = 0xdeadbeefu;
		return false;
	}
	*status = g_platform.status;
	return true;
}

static bool platform_metrics(void *context, const AiusRuntimeBudget *budget,
                             uint32_t now_ms)
{
	(void)now_ms;
	CHECK(context == g_platform.expected_context);
	record_budget(budget, AIUS_BOOT_CHECKPOINT_METRICS);
	g_platform.metrics_calls++;
	return g_platform.metrics_ok;
}

static bool platform_udp(void *context, const AiusRuntimeBudget *budget,
                         uint32_t now_ms)
{
	(void)now_ms;
	CHECK(context == g_platform.expected_context);
	record_budget(budget, AIUS_BOOT_CHECKPOINT_UDP);
	g_platform.udp_calls++;
	return g_platform.udp_ok;
}

static bool platform_replay(void *context, const AiusRuntimeBudget *budget,
                            uint32_t now_ms)
{
	(void)now_ms;
	CHECK(context == g_platform.expected_context);
	record_budget(budget, AIUS_BOOT_CHECKPOINT_REPLAY);
	g_platform.replay_calls++;
	return g_platform.replay_ok;
}

static bool platform_audio(void *context, const AiusRuntimeBudget *budget,
                           uint32_t now_ms)
{
	(void)now_ms;
	CHECK(context == g_platform.expected_context);
	record_budget(budget, AIUS_BOOT_CHECKPOINT_AUDIO);
	g_platform.audio_calls++;
	return g_platform.audio_ok;
}

static void platform_checkpoint(void *context, uint16_t checkpoint)
{
	CHECK(context == g_platform.expected_context);
	if (g_platform.checkpoint_count < MAX_TRACE)
		g_platform.checkpoints[g_platform.checkpoint_count++] =
			checkpoint;
}

static void platform_feed(void *context)
{
	CHECK(context == g_platform.expected_context);
	g_platform.feeds++;
}

/* The publication consumer, shared by the composed platform table and the
 * standalone storage-service rows. `expected_context` pins whichever context
 * the registration under test promised to hand back. */
struct PublishFake {
	void *expected_context;
	bool accept;
	unsigned calls;
	size_t last_length;
	/* The consumer's own bound, per row: the service may now publish up to a
	 * model-sized payload, so a row that expects no more than one transfer
	 * slice says so instead of relying on `last` being that small. */
	size_t max_length;
	uint8_t last[AIUS_RUNTIME_STORAGE_PUBLISH_CAPACITY];
};

static PublishFake g_publish;

static bool publish_config(void *context, const uint8_t *data, size_t length)
{
	CHECK(context == g_publish.expected_context);
	CHECK(data != nullptr);
	CHECK(length <= sizeof(g_publish.last));
	CHECK(g_publish.max_length != 0u);
	CHECK(length <= g_publish.max_length);
	g_publish.calls++;
	g_publish.last_length = length;
	memcpy(g_publish.last, data, length);
	return g_publish.accept;
}

static AiusRuntimeTeensyPlatformOps platform_ops(void)
{
	AiusRuntimeTeensyPlatformOps ops;

	memset(&ops, 0, sizeof(ops));
	ops.context = &g_platform;
	ops.reset_capture = platform_reset_capture;
	ops.metrics = platform_metrics;
	ops.udp = platform_udp;
	ops.replay = platform_replay;
	ops.audio = platform_audio;
	ops.checkpoint = platform_checkpoint;
	ops.feed_watchdog = platform_feed;
	ops.publish_config = publish_config;
	g_platform.expected_context = &g_platform;
	g_publish.expected_context = &g_platform;
	return ops;
}

/* ------------------------------------------------------------------ */
/* fake application                                                    */
/* ------------------------------------------------------------------ */

struct AppFake {
	bool sd_ready;
	bool sd_busy;
	bool auth_ok;
	const char *status_body;
	const char *usb_payload;
	size_t usb_payload_length;
	unsigned status_calls;
	unsigned reboot_calls;
	unsigned usb_payload_calls;
	/* Recorder read lease. `recorder_read_ok` flips the read from its
	 * default refusal to a 200 that borrows recorder_body, which is what
	 * arms the response's completion hook; the counters then prove the
	 * borrow is given back. */
	bool recorder_read_ok;
	unsigned recorder_acquires;
	unsigned recorder_releases;
	unsigned recorder_release_slot;
};

static AppFake g_app;

static bool app_sd_ready(void *context)
{
	(void)context;
	return g_app.sd_ready;
}

static bool app_sd_busy(void *context)
{
	(void)context;
	return g_app.sd_busy;
}

static bool app_auth_matches(void *context, const uint8_t *token, size_t length)
{
	(void)context;
	(void)token;
	(void)length;
	return g_app.auth_ok;
}

static size_t write_text(const char *text, uint8_t *body, size_t capacity)
{
	const size_t length = strlen(text);

	if (length > capacity)
		return 0u;
	memcpy(body, text, length);
	return length;
}

static size_t app_write_home(void *context, uint8_t *body, size_t capacity)
{
	(void)context;
	return write_text("<html></html>", body, capacity);
}

static size_t app_write_status(void *context, uint8_t *body, size_t capacity)
{
	(void)context;
	g_app.status_calls++;
	return write_text(g_app.status_body, body, capacity);
}

static size_t app_write_events(void *context, uint32_t cursor, uint8_t *body,
                               size_t capacity)
{
	(void)context;
	(void)cursor;
	return write_text("{\"events\":[]}", body, capacity);
}

static size_t app_write_dsp_config(void *context, uint8_t *body, size_t capacity)
{
	(void)context;
	return write_text("{\"dsp\":true}", body, capacity);
}

static size_t app_write_meters(void *context, uint8_t *body, size_t capacity)
{
	(void)context;
	return write_text("{\"meters\":true}", body, capacity);
}

static size_t app_write_replay_status(void *context, uint8_t *body,
                                      size_t capacity)
{
	(void)context;
	return write_text("{\"replay\":false}", body, capacity);
}

static size_t app_write_replay_list(void *context, uint8_t *body, size_t capacity)
{
	(void)context;
	return write_text("{\"replays\":[]}", body, capacity);
}

static AiusOpticalAppStoreResult app_store_public_config(void *context,
                                                        const uint8_t *body,
                                                        size_t length)
{
	(void)context;
	(void)body;
	(void)length;
	return AIUS_OPTICAL_APP_STORE_OK;
}

static AiusOpticalAppStoreResult app_store_dsp_config(void *context,
                                                     const uint8_t *body,
                                                     size_t length)
{
	(void)context;
	(void)body;
	(void)length;
	return AIUS_OPTICAL_APP_STORE_OK;
}

static AiusOpticalAppReplayResult app_replay_start(void *context, const char *id,
                                                  bool loop)
{
	(void)context;
	(void)id;
	(void)loop;
	return AIUS_OPTICAL_APP_REPLAY_OK;
}

static AiusOpticalAppReplayResult app_replay_delete(void *context,
                                                   const char *id)
{
	(void)context;
	(void)id;
	return AIUS_OPTICAL_APP_REPLAY_OK;
}

static void app_replay_stop(void *context)
{
	(void)context;
}

static void app_set_test_running(void *context, bool running)
{
	(void)context;
	(void)running;
}

static void app_clear_counters(void *context)
{
	(void)context;
}

static void app_request_reboot(void *context)
{
	(void)context;
	g_app.reboot_calls++;
}

static size_t app_write_usb_payload(void *context, AiusUsbCommandKind kind,
                                    char *payload, size_t capacity)
{
	(void)context;
	(void)kind;
	g_app.usb_payload_calls++;
	if (g_app.usb_payload_length == 0u ||
	    g_app.usb_payload_length >= capacity)
		return 0u;
	memcpy(payload, g_app.usb_payload, g_app.usb_payload_length);
	payload[g_app.usb_payload_length] = '\0';
	return g_app.usb_payload_length;
}

static uint32_t app_upload_id(void *context)
{
	(void)context;
	return 7u;
}

static size_t app_write_measure(void *context, uint8_t *body, size_t capacity)
{
	(void)context;
	int written = snprintf((char *)body, capacity, "{\"body\":\"measure\"}");
	return written > 0 && (size_t)written < capacity ? (size_t)written : 0u;
}

static bool app_measure_start(void *context, const char *vector_id,
                              uint32_t f0_l_hz, uint32_t f0_r_hz,
                              const uint32_t *probes_hz, size_t probe_count)
{
	(void)context;
	(void)vector_id;
	(void)f0_l_hz;
	(void)f0_r_hz;
	(void)probes_hz;
	(void)probe_count;
	return true;
}

static size_t app_write_recorder(void *context, uint8_t *body,
                                 size_t capacity)
{
	(void)context;
	int written = snprintf((char *)body, capacity, "{\"state\":0}");
	return written > 0 && (size_t)written < capacity ? (size_t)written : 0u;
}

static bool app_recorder_freeze(void *context)
{
	(void)context;
	return true;
}

static bool app_recorder_thaw(void *context)
{
	(void)context;
	return true;
}

static bool app_recorder_config(void *context, uint32_t mask,
                                float gr_threshold_db,
                                uint32_t gr_hold_blocks,
                                uint32_t postroll_blocks)
{
	(void)context;
	(void)mask;
	(void)gr_threshold_db;
	(void)gr_hold_blocks;
	(void)postroll_blocks;
	return true;
}

/* The bytes a 200 borrows. Static so the pointer the response hands the
 * transport outlives every step of the send, exactly as the PSRAM ring does on
 * target, and AIUS_HTTP_MAX_BODY-sized so it takes the STREAM path
 * (> AIUS_HTTP_MAX_RESPONSE, so no copy) and needs many AIUS_HTTP_TX_QUOTA
 * writes to drain -- both true of a real recorder chunk, which runs to
 * AIUS_RECORDER_READ_MAX_BYTES. The many-writes part is what lets a test drop
 * the link with the borrow still genuinely outstanding. */
static uint8_t g_recorder_chunk[AIUS_HTTP_MAX_BODY];

static AiusOpticalAppRecorderReadResult app_recorder_read(
	void *context, bool has_slot, uint32_t slot, uint32_t offset_bytes,
	uint32_t length_bytes, uint32_t generation, const uint8_t **data,
	uint32_t *length, uint32_t *resolved_slot)
{
	AppFake *fake = (AppFake *)context;
	(void)has_slot;
	(void)offset_bytes;
	(void)length_bytes;
	(void)generation;
	*resolved_slot = slot;
	if (!fake->recorder_read_ok)
		return AIUS_OPTICAL_APP_RECORDER_READ_REFUSED;
	*data = g_recorder_chunk;
	*length = (uint32_t)sizeof(g_recorder_chunk);
	fake->recorder_acquires++;
	return AIUS_OPTICAL_APP_RECORDER_READ_OK;
}

static void app_recorder_read_release(void *context, uint32_t slot)
{
	AppFake *fake = (AppFake *)context;
	fake->recorder_releases++;
	fake->recorder_release_slot = slot;
}

static AiusOpticalApp app_table(void)
{
	AiusOpticalApp app;

	memset(&app, 0, sizeof(app));
	app.context = &g_app;
	app.sd_ready = app_sd_ready;
	app.sd_busy = app_sd_busy;
	app.auth_matches = app_auth_matches;
	app.write_home = app_write_home;
	app.write_status = app_write_status;
	app.write_events = app_write_events;
	app.write_dsp_config = app_write_dsp_config;
	app.write_meters = app_write_meters;
	app.write_replay_status = app_write_replay_status;
	app.write_replay_list = app_write_replay_list;
	app.store_public_config = app_store_public_config;
	app.store_dsp_config = app_store_dsp_config;
	app.replay_start = app_replay_start;
	app.replay_delete = app_replay_delete;
	app.replay_stop = app_replay_stop;
	app.set_test_running = app_set_test_running;
	app.clear_counters = app_clear_counters;
	app.request_reboot = app_request_reboot;
	app.write_usb_payload = app_write_usb_payload;
	app.upload_id = app_upload_id;
	app.write_measure = app_write_measure;
	app.measure_start = app_measure_start;
	app.write_recorder = app_write_recorder;
	app.recorder_freeze = app_recorder_freeze;
	app.recorder_thaw = app_recorder_thaw;
	app.recorder_config = app_recorder_config;
	app.recorder_read = app_recorder_read;
	app.recorder_read_release = app_recorder_read_release;
	return app;
}

/* ------------------------------------------------------------------ */
/* harness                                                             */
/* ------------------------------------------------------------------ */

static AiusRuntimeTeensy g_runtime;

static void reset_world(bool recovery_safe)
{
	AiusOpticalApp app = app_table();
	AiusCdcTeensyPrimitives cdc = cdc_primitives();
	AiusRuntimeTeensyPlatformOps platform = platform_ops();

	memset(&g_serial, 0, sizeof(g_serial));
	memset(&g_cdc, 0, sizeof(g_cdc));
	memset(&g_sd, 0, sizeof(g_sd));
	memset(&g_net, 0, sizeof(g_net));
	memset(&g_app, 0, sizeof(g_app));
	memset(&g_publish, 0, sizeof(g_publish));

	g_cdc.expected_context = &g_cdc;
	/* platform_ops() ran before the memsets above cleared the fake. */
	g_publish.expected_context = &g_platform;
	g_publish.accept = true;
	g_publish.max_length = AIUS_RUNTIME_STORAGE_TRANSFER;
	g_platform.expected_context = &g_platform;
	g_platform.reset_capture_ok = true;
	g_platform.metrics_ok = true;
	g_platform.udp_ok = true;
	g_platform.replay_ok = true;
	g_platform.audio_ok = true;
	g_platform.checkpoint_count = 0u;
	g_platform.budget_count = 0u;
	g_platform.feeds = 0u;
	g_platform.reset_capture_calls = 0u;
	g_platform.metrics_calls = 0u;
	g_platform.udp_calls = 0u;
	g_platform.replay_calls = 0u;
	g_platform.audio_calls = 0u;
	g_platform.status.boot_generation = 11u;
	g_platform.status.reset_source = 0x20u;
	g_platform.status.watchdog_early_resets = 1u;

	g_sd.begin_ok = true;
	g_sd.media_present = true;
	g_sd.card_detected = true;
	g_sd.open_ok = true;
	g_sd.remove_ok = true;

	g_net.stack_start_ok = true;
	g_net.dhcp_start_ok = true;
	g_net.listener_open_ok = true;
	g_net.udp_open_ok = true;
	g_net.listener_close_ok = true;
	g_net.udp_close_ok = true;
	g_net.stack_close_ok = true;
	g_net.link = AIUS_NETWORK_TEENSY_LINK_UP;

	g_app.sd_ready = true;
	g_app.auth_ok = true;
	g_app.status_body = "{\"ok\":true}";

	aius_network_teensy_test_set_primitives(&kNetPrimitives);
	aius_runtime_teensy_construct(&g_runtime, &app, &cdc, &platform, 0u,
	                             recovery_safe);
	g_witness_service = &g_runtime.network_http;
}

static bool run_epoch(uint32_t now_ms)
{
	bool ok;

	g_sd.primitive_calls_epoch = 0u;
	g_net.io_calls_epoch = 0u;
	g_net.lifecycle_calls_epoch = 0u;
	g_platform.checkpoint_count = 0u;
	ok = aius_runtime_teensy_epoch(&g_runtime, now_ms);
	/* The normative one-primitive-per-epoch rules. */
	CHECK(g_sd.primitive_calls_epoch <= 1u);
	CHECK(g_net.io_calls_epoch <= 1u);
	return ok;
}

/* Walks the published-generation handshake. Each step is one NETWORK operation:
 * apply, step, apply, poll, step, apply, apply. */
static void bring_online(uint32_t *now_ms)
{
	CHECK(run_epoch((*now_ms)++));
	CHECK(g_net.stack_start_calls == 1u);
	CHECK(g_net.install_calls == 1u);
	CHECK(g_net.dhcp_start_calls == 0u);

	CHECK(run_epoch((*now_ms)++)); /* supervisor step: START_DHCP */
	CHECK(g_net.lifecycle_calls_epoch == 0u);

	CHECK(run_epoch((*now_ms)++));
	CHECK(g_net.dhcp_start_calls == 1u);

	aius_network_teensy_test_callback(&g_runtime.network_http.target, 1u,
	                                  AIUS_NET_EVENT_LEASE_ACQUIRED,
	                                  AIUS_NET_FAULT_NONE);
	CHECK(run_epoch((*now_ms)++)); /* poll drains the callback */
	CHECK(g_net.lifecycle_calls_epoch == 0u);

	CHECK(run_epoch((*now_ms)++)); /* supervisor step: PUBLISH_ONLINE */
	CHECK(g_net.lifecycle_calls_epoch == 0u);

	CHECK(run_epoch((*now_ms)++));
	CHECK(g_net.listener_open_calls == 1u);
	CHECK(g_net.udp_open_calls == 0u);

	CHECK(run_epoch((*now_ms)++));
	CHECK(g_net.udp_open_calls == 1u);
	CHECK(g_runtime.network_http.target.online);
	CHECK(g_runtime.network_http.supervisor.state ==
	      AIUS_NET_STATE_ONLINE);
}

/* ------------------------------------------------------------------ */
/* 1. CDC adapter normalization                                        */
/* ------------------------------------------------------------------ */

static void test_cdc_adapter(void)
{
	static const char *const keys[] = {
		"cdc.primitives.complete",  "cdc.primitives.incomplete",
		"cdc.construct.null",       "cdc.ops.null",
		"cdc.connected",            "cdc.capacity.clamp",
		"cdc.read.progress",        "cdc.read.would-block",
		"cdc.read.closed",          "cdc.read.sentinel",
		"cdc.write.progress",       "cdc.write.partial",
		"cdc.write.would-block",    "cdc.write.closed",
		"cdc.write.oversize",       "cdc.write.bad-length",
		"cdc.fault.saturates",      "cdc.bootloader",
		"cdc.bootloader.drain",     "cdc.bootloader.drain-bounded",
		"cdc.bootloader.no-clock",  "cdc.serial.table"
	};
	AiusCdcTeensyPrimitives primitives = cdc_primitives();
	AiusCdcTeensy adapter;
	AiusCdcTeensyOps ops;
	AiusIoEvent event;
	uint8_t byte = 0u;
	uint8_t payload[8] = {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u};

	validate_unique_keys("registry.cdc", keys, ARRAY_COUNT(keys), 22u);

	current_key = "cdc.primitives.complete";
	CHECK(aius_cdc_teensy_primitives_complete(&primitives));

	current_key = "cdc.primitives.incomplete";
	CHECK(!aius_cdc_teensy_primitives_complete(nullptr));
	for (size_t i = 0; i < 7u; ++i) {
		AiusCdcTeensyPrimitives broken = primitives;

		switch (i) {
		case 0: broken.connected = nullptr; break;
		case 1: broken.read = nullptr; break;
		case 2: broken.available_for_write = nullptr; break;
		case 3: broken.write = nullptr; break;
		case 4: broken.flush = nullptr; break;
		case 5: broken.millis = nullptr; break;
		default: broken.enter_bootloader = nullptr; break;
		}
		CHECK(!aius_cdc_teensy_primitives_complete(&broken));
	}

	current_key = "cdc.construct.null";
	aius_cdc_teensy_construct(nullptr, &primitives);
	memset(&adapter, 0xa5, sizeof(adapter));
	aius_cdc_teensy_construct(&adapter, nullptr);
	CHECK(adapter.protocol_faults == 0u);
	CHECK(adapter.last_fault == 0u);
	CHECK(adapter.primitives.read == nullptr);

	current_key = "cdc.ops.null";
	ops = aius_cdc_teensy_ops(nullptr);
	CHECK(ops.context == nullptr && ops.connected == nullptr);

	aius_cdc_teensy_construct(&adapter, &primitives);
	ops = aius_cdc_teensy_ops(&adapter);
	CHECK(ops.context == &adapter);

	current_key = "cdc.connected";
	g_cdc.connected = false;
	CHECK(!ops.connected(ops.context));
	g_cdc.connected = true;
	CHECK(ops.connected(ops.context));

	current_key = "cdc.capacity.clamp";
	g_cdc.capacity_value = -3;
	CHECK(ops.write_capacity(ops.context) == 0u);
	g_cdc.capacity_value = 0;
	CHECK(ops.write_capacity(ops.context) == 0u);
	g_cdc.capacity_value = 17;
	CHECK(ops.write_capacity(ops.context) == 17u);
	g_cdc.capacity_value = 64;
	CHECK(ops.write_capacity(ops.context) == 64u);
	g_cdc.capacity_value = 4096;
	CHECK(ops.write_capacity(ops.context) == 64u);

	current_key = "cdc.read.progress";
	g_cdc.read_value = 0x41;
	event = ops.read_byte(ops.context, &byte);
	CHECK(event.result == AIUS_IO_PROGRESS && event.bytes == 1u &&
	      event.fault == 0u && byte == 0x41u);

	current_key = "cdc.read.would-block";
	g_cdc.read_value = -1;
	event = ops.read_byte(ops.context, &byte);
	CHECK(event.result == AIUS_IO_WOULD_BLOCK && event.bytes == 0u &&
	      event.fault == 0u);

	current_key = "cdc.read.closed";
	g_cdc.connected = false;
	event = ops.read_byte(ops.context, &byte);
	CHECK(event.result == AIUS_IO_CLOSED && event.bytes == 0u &&
	      event.fault == 0u);
	CHECK(adapter.protocol_faults == 0u);
	g_cdc.connected = true;

	current_key = "cdc.read.sentinel";
	g_cdc.read_value = -2;
	event = ops.read_byte(ops.context, &byte);
	CHECK(event.result == AIUS_IO_FAULT && event.bytes == 0u &&
	      event.fault == AIUS_CDC_ADAPTER_PROTOCOL_FAULT);
	CHECK(adapter.protocol_faults == 1u);
	CHECK(adapter.last_fault == AIUS_CDC_ADAPTER_PROTOCOL_FAULT);
	g_cdc.read_value = 256;
	event = ops.read_byte(ops.context, &byte);
	CHECK(event.result == AIUS_IO_FAULT && adapter.protocol_faults == 2u);
	event = ops.read_byte(ops.context, nullptr);
	CHECK(event.result == AIUS_IO_FAULT && adapter.protocol_faults == 3u);

	current_key = "cdc.write.progress";
	g_cdc.write_result = 0u;
	event = ops.write(ops.context, payload, 8u);
	CHECK(event.result == AIUS_IO_PROGRESS && event.bytes == 8u &&
	      event.fault == 0u);
	CHECK(g_cdc.last_write_length == 8u);

	current_key = "cdc.write.partial";
	g_cdc.write_result = 3u;
	event = ops.write(ops.context, payload, 8u);
	CHECK(event.result == AIUS_IO_PROGRESS && event.bytes == 3u);

	current_key = "cdc.write.would-block";
	g_cdc.write_result = SIZE_MAX; /* sentinel for "return zero" below */
	g_cdc.write_result = 0u;
	g_cdc.capacity_value = 0;
	{
		/* A zero primitive result is WOULD_BLOCK; drive it with an
		 * explicit override rather than the default pass-through. */
		struct Override {
			static size_t write(void *context, const uint8_t *data,
			                    size_t length)
			{
				(void)context;
				(void)data;
				(void)length;
				return 0u;
			}
		};
		AiusCdcTeensyPrimitives zero = primitives;
		AiusCdcTeensy zero_adapter;
		AiusCdcTeensyOps zero_ops;

		zero.write = Override::write;
		aius_cdc_teensy_construct(&zero_adapter, &zero);
		zero_ops = aius_cdc_teensy_ops(&zero_adapter);
		event = zero_ops.write(zero_ops.context, payload, 4u);
		CHECK(event.result == AIUS_IO_WOULD_BLOCK &&
		      event.bytes == 0u && event.fault == 0u);
		CHECK(zero_adapter.protocol_faults == 0u);
	}

	current_key = "cdc.write.closed";
	g_cdc.connected = false;
	event = ops.write(ops.context, payload, 4u);
	CHECK(event.result == AIUS_IO_CLOSED && event.bytes == 0u);
	g_cdc.connected = true;

	current_key = "cdc.write.oversize";
	{
		struct Override {
			static size_t write(void *context, const uint8_t *data,
			                    size_t length)
			{
				(void)context;
				(void)data;
				return length + 1u;
			}
		};
		AiusCdcTeensyPrimitives big = primitives;
		AiusCdcTeensy big_adapter;
		AiusCdcTeensyOps big_ops;

		big.write = Override::write;
		aius_cdc_teensy_construct(&big_adapter, &big);
		big_ops = aius_cdc_teensy_ops(&big_adapter);
		event = big_ops.write(big_ops.context, payload, 4u);
		CHECK(event.result == AIUS_IO_FAULT &&
		      event.fault == AIUS_CDC_ADAPTER_PROTOCOL_FAULT);
		CHECK(big_adapter.protocol_faults == 1u);
	}

	current_key = "cdc.write.bad-length";
	{
		const unsigned before = adapter.protocol_faults;

		event = ops.write(ops.context, payload, 0u);
		CHECK(event.result == AIUS_IO_FAULT);
		event = ops.write(ops.context, payload, 65u);
		CHECK(event.result == AIUS_IO_FAULT);
		event = ops.write(ops.context, nullptr, 4u);
		CHECK(event.result == AIUS_IO_FAULT);
		CHECK(adapter.protocol_faults == before + 3u);
	}

	current_key = "cdc.fault.saturates";
	adapter.protocol_faults = UINT32_MAX;
	g_cdc.read_value = -9;
	(void)ops.read_byte(ops.context, &byte);
	CHECK(adapter.protocol_faults == UINT32_MAX);

	current_key = "cdc.bootloader";
	{
		const unsigned before = g_cdc.bootloader_calls;

		/* The ring is already at the high-water mark this adapter has
		 * seen, so the drain observes it on its first poll and the entry
		 * is immediate -- but the flush still precedes it. */
		g_cdc.capacity_value = 4096;
		g_cdc.drain_after_polls = 0u;
		g_cdc.flush_calls = 0u;
		ops.enter_bootloader(ops.context);
		CHECK(g_cdc.bootloader_calls == before + 1u);
		CHECK(g_cdc.flush_calls == 1u);
		CHECK(g_cdc.flush_calls_at_entry == 1u);
		CHECK(adapter.tx_free_high_water == 4096u);
		CHECK(adapter.bootloader_drained);
	}

	/* The defect this drain exists for: Serial.write() reports bytes
	 * buffered, not transmitted, so a full-length write event is not
	 * evidence the host has the response. The adapter must poll the ring
	 * back to its idle free space before handing the board to HalfKay. */
	current_key = "cdc.bootloader.drain";
	{
		AiusCdcTeensy drain_adapter;
		AiusCdcTeensyOps drain_ops;
		unsigned polls_before;

		g_cdc.connected = true;
		g_cdc.capacity_value = 6144;
		g_cdc.drain_after_polls = 0u;
		g_cdc.flush_calls = 0u;
		g_cdc.millis_calls = 0u;
		g_cdc.millis_value = 0u;
		g_cdc.millis_step = 1u;
		g_cdc.bootloader_calls = 0u;
		aius_cdc_teensy_construct(&drain_adapter, &primitives);
		drain_ops = aius_cdc_teensy_ops(&drain_adapter);

		/* One ordinary idle epoch fixes the ring's idle capacity. */
		CHECK(drain_ops.write_capacity(drain_ops.context) == 64u);
		CHECK(drain_adapter.tx_free_high_water == 6144u);

		/* The response is queued: the ring stays short of idle until the
		 * host retires the transfer four polls later. */
		g_cdc.capacity_value = 4096;
		g_cdc.drained_capacity_value = 6144;
		g_cdc.drain_after_polls = 4u;
		polls_before = g_cdc.capacity_calls;
		drain_ops.enter_bootloader(drain_ops.context);
		CHECK(g_cdc.capacity_calls == polls_before + 4u);
		CHECK(g_cdc.flush_calls == 1u);
		CHECK(g_cdc.flush_calls_at_entry == 1u);
		CHECK(g_cdc.bootloader_calls == 1u);
		CHECK(drain_adapter.bootloader_drained);
		CHECK(drain_adapter.bootloader_drain_ms <
		      AIUS_CDC_TEENSY_DRAIN_BOUND_MS);
	}

	current_key = "cdc.bootloader.drain-bounded";
	{
		AiusCdcTeensy stuck_adapter;
		AiusCdcTeensyOps stuck_ops;

		g_cdc.capacity_value = 6144;
		g_cdc.drain_after_polls = 0u;
		g_cdc.flush_calls = 0u;
		g_cdc.millis_calls = 0u;
		g_cdc.millis_value = 0u;
		g_cdc.millis_step = 1u;
		g_cdc.bootloader_calls = 0u;
		aius_cdc_teensy_construct(&stuck_adapter, &primitives);
		stuck_ops = aius_cdc_teensy_ops(&stuck_adapter);
		CHECK(stuck_ops.write_capacity(stuck_ops.context) == 64u);

		/* A host that never takes the transfer must cost a bounded stall,
		 * not the board: entry still happens. */
		g_cdc.capacity_value = 4096;
		stuck_ops.enter_bootloader(stuck_ops.context);
		CHECK(!stuck_adapter.bootloader_drained);
		CHECK(stuck_adapter.bootloader_drain_ms ==
		      AIUS_CDC_TEENSY_DRAIN_BOUND_MS);
		CHECK(g_cdc.flush_calls == 1u);
		CHECK(g_cdc.bootloader_calls == 1u);
		CHECK(g_cdc.flush_calls_at_entry == 1u);
	}

	current_key = "cdc.bootloader.no-clock";
	{
		AiusCdcTeensyPrimitives clockless = primitives;
		AiusCdcTeensy clockless_adapter;
		AiusCdcTeensyOps clockless_ops;

		clockless.millis = nullptr;
		g_cdc.capacity_value = 4096;
		g_cdc.drain_after_polls = 0u;
		g_cdc.flush_calls = 0u;
		g_cdc.millis_calls = 0u;
		g_cdc.bootloader_calls = 0u;
		aius_cdc_teensy_construct(&clockless_adapter, &clockless);
		clockless_ops = aius_cdc_teensy_ops(&clockless_adapter);

		/* No clock means no bound, so there is no wait at all rather than
		 * an unbounded one. */
		clockless_ops.enter_bootloader(clockless_ops.context);
		CHECK(g_cdc.millis_calls == 0u);
		CHECK(g_cdc.flush_calls == 1u);
		CHECK(g_cdc.bootloader_calls == 1u);
		CHECK(clockless_adapter.bootloader_drain_ms == 0u);
	}

	current_key = "cdc.serial.table";
	{
		AiusCdcTeensyPrimitives serial =
			aius_cdc_teensy_serial_primitives();
		AiusCdcTeensy serial_adapter;
		AiusCdcTeensyOps serial_ops;
		static const uint8_t line[] = {'A', 'B'};

		CHECK(aius_cdc_teensy_primitives_complete(&serial));
		aius_cdc_teensy_construct(&serial_adapter, &serial);
		serial_ops = aius_cdc_teensy_ops(&serial_adapter);

		memset(&g_serial, 0, sizeof(g_serial));
		g_serial.connected = true;
		g_serial.available_for_write = 128;
		g_serial.rx = line;
		g_serial.rx_length = sizeof(line);

		CHECK(serial_ops.connected(serial_ops.context));
		CHECK(serial_ops.write_capacity(serial_ops.context) == 64u);
		event = serial_ops.read_byte(serial_ops.context, &byte);
		CHECK(event.result == AIUS_IO_PROGRESS && byte == 'A');
		event = serial_ops.read_byte(serial_ops.context, &byte);
		CHECK(event.result == AIUS_IO_PROGRESS && byte == 'B');
		event = serial_ops.read_byte(serial_ops.context, &byte);
		CHECK(event.result == AIUS_IO_WOULD_BLOCK);
		event = serial_ops.write(serial_ops.context, line,
		                        sizeof(line));
		CHECK(event.result == AIUS_IO_PROGRESS && event.bytes == 2u);
		CHECK(g_serial.tx_length == 2u && g_serial.tx[0] == 'A');
		/* The production table drains through the same path: the ring
		 * returns to its idle free space three polls after the flush, and
		 * only then is _reboot_Teensyduino_() reached. */
		g_serial.available_for_write = 64;
		g_serial.drained_available = 128;
		g_serial.drain_after_polls = 3u;
		serial_ops.enter_bootloader(serial_ops.context);
		CHECK(g_serial.reboots == 1u);
		CHECK(g_serial.flush_calls == 1u);
		CHECK(g_serial.flush_calls_at_reboot == 1u);
		CHECK(g_serial.drain_after_polls == 0u);
		CHECK(serial_adapter.bootloader_drained);
		g_serial.connected = false;
		CHECK(!serial_ops.connected(serial_ops.context));
		event = serial_ops.read_byte(serial_ops.context, &byte);
		CHECK(event.result == AIUS_IO_CLOSED);
		CHECK(serial_adapter.protocol_faults == 0u);
	}
}

/* ------------------------------------------------------------------ */
/* 2. exact budgets reach every service                                */
/* ------------------------------------------------------------------ */

static void test_service_budget_selectivity(void)
{
	static const char *const keys[] = {
		"budget.usb-rx.selective", "budget.usb-tx.selective",
		"budget.network.selective", "budget.http.selective",
		"budget.sd.selective",     "budget.null",
		"budget.platform.exact"
	};
	uint32_t now_ms = 1u;
	AiusHttpAction storage_action;
	AiusHttpEvent storage_event;

	validate_unique_keys("registry.budget", keys, ARRAY_COUNT(keys), 7u);
	reset_world(false);

	for (uint16_t checkpoint = 0u;
	     checkpoint <= AIUS_BOOT_CHECKPOINT_EPOCH_COMPLETE; ++checkpoint) {
		const AiusRuntimeBudget *budget =
			aius_runtime_budget(checkpoint);

		current_key = "budget.usb-rx.selective";
		CHECK(aius_runtime_usb_rx_service(&g_runtime.usb, budget) ==
		      (checkpoint == AIUS_BOOT_CHECKPOINT_USB_RX));

		current_key = "budget.usb-tx.selective";
		CHECK(aius_runtime_usb_tx_service(&g_runtime.usb, budget, 0u,
		                                  &g_runtime.app) ==
		      (checkpoint == AIUS_BOOT_CHECKPOINT_USB_TX));

		current_key = "budget.network.selective";
		CHECK(aius_runtime_network_service(&g_runtime.network_http,
		                                   budget, now_ms) ==
		      (checkpoint == AIUS_BOOT_CHECKPOINT_NETWORK));

		current_key = "budget.http.selective";
		CHECK(aius_runtime_http_service(&g_runtime.network_http, budget,
		                                now_ms, &g_runtime.app, nullptr,
		                                &storage_action) ==
		      (checkpoint == AIUS_BOOT_CHECKPOINT_HTTP));

		current_key = "budget.sd.selective";
		CHECK(aius_runtime_storage_service(&g_runtime.storage, budget,
		                                   now_ms, nullptr,
		                                   &storage_event) ==
		      (checkpoint == AIUS_BOOT_CHECKPOINT_SD));
	}

	current_key = "budget.null";
	CHECK(!aius_runtime_usb_rx_service(&g_runtime.usb, nullptr));
	CHECK(!aius_runtime_usb_rx_service(nullptr,
	                                   aius_runtime_budget(
		                                   AIUS_BOOT_CHECKPOINT_USB_RX)));
	CHECK(!aius_runtime_usb_tx_service(nullptr, nullptr, 0u, nullptr));
	CHECK(!aius_runtime_network_service(nullptr, nullptr, 0u));
	CHECK(!aius_runtime_http_service(&g_runtime.network_http,
	                                 aius_runtime_budget(
		                                 AIUS_BOOT_CHECKPOINT_HTTP),
	                                 0u, &g_runtime.app, nullptr, nullptr));
	CHECK(!aius_runtime_storage_service(&g_runtime.storage,
	                                    aius_runtime_budget(
		                                    AIUS_BOOT_CHECKPOINT_SD),
	                                    0u, nullptr, nullptr));

	current_key = "budget.platform.exact";
	reset_world(false);
	g_platform.budget_count = 0u;
	const bool completed = run_epoch(now_ms);
	CHECK(g_runtime.valid);
	CHECK(completed);
	CHECK(g_platform.reset_capture_calls == 1u);
	CHECK(g_platform.metrics_calls == 1u);
	CHECK(g_platform.udp_calls == 1u);
	CHECK(g_platform.replay_calls == 1u);
	/* Two AUDIO dispatches per epoch since output-jitter Phase B
	 * (f7e770cf0): the ring is topped up on BOTH sides of SD. */
	CHECK(g_platform.audio_calls == 2u);
	/* record_budget() asserts pointer identity with aius_runtime_budget()
	 * for each platform checkpoint; here we pin the exact set and order. */
	CHECK(g_platform.budget_count == 6u);
	CHECK(g_platform.budget_checkpoints[0] ==
	      AIUS_BOOT_CHECKPOINT_RESET_CAPTURE);
	CHECK(g_platform.budget_checkpoints[1] ==
	      AIUS_BOOT_CHECKPOINT_METRICS);
	CHECK(g_platform.budget_checkpoints[2] == AIUS_BOOT_CHECKPOINT_UDP);
	CHECK(g_platform.budget_checkpoints[3] == AIUS_BOOT_CHECKPOINT_REPLAY);
	CHECK(g_platform.budget_checkpoints[4] == AIUS_BOOT_CHECKPOINT_AUDIO);
	CHECK(g_platform.budget_checkpoints[5] == AIUS_BOOT_CHECKPOINT_AUDIO);
	CHECK(g_platform.budgets[0]->service_budget_ms == 250u);
	CHECK(g_platform.budgets[2]->max_rx_bytes == 532u);
	CHECK(g_platform.budgets[3]->max_rx_bytes == 2048u);
	CHECK(g_platform.budgets[4]->max_calls == 1u);
	CHECK(g_platform.budgets[4]->max_rx_bytes == 0u);
	CHECK(g_platform.budgets[4]->max_tx_bytes == 0u);
	CHECK(g_platform.budgets[4]->service_budget_ms == 8u);
}

/* ------------------------------------------------------------------ */
/* 3. USB_RX bounded drain                                             */
/* ------------------------------------------------------------------ */

static void test_usb_rx_quota(void)
{
	static const char *const keys[] = {
		"rx.disconnected",  "rx.full-quota",     "rx.buffer-full",
		"rx.would-block",   "rx.closed",         "rx.fault",
		"rx.malformed",     "rx.no-portable-step"
	};
	const AiusRuntimeBudget *budget =
		aius_runtime_budget(AIUS_BOOT_CHECKPOINT_USB_RX);

	validate_unique_keys("registry.rx", keys, ARRAY_COUNT(keys), 8u);

	current_key = "rx.disconnected";
	reset_world(false);
	g_cdc.connected = false;
	g_runtime.usb.usb_rx_length = 5u;
	CHECK(aius_runtime_usb_rx_service(&g_runtime.usb, budget));
	CHECK(g_runtime.usb.usb_rx_length == 0u);
	CHECK(g_cdc.read_calls == 0u);
	CHECK(!g_runtime.usb.rx_terminal);

	current_key = "rx.full-quota";
	reset_world(false);
	g_cdc.connected = true;
	g_cdc.read_value = 'x';
	CHECK(aius_runtime_usb_rx_service(&g_runtime.usb, budget));
	CHECK(g_cdc.read_calls == 64u);
	CHECK(g_runtime.usb.usb_rx_length == 64u);
	CHECK(g_runtime.usb.runtime.state == AIUS_USB_RUNTIME_IDLE);

	current_key = "rx.buffer-full";
	g_cdc.read_calls = 0u;
	CHECK(aius_runtime_usb_rx_service(&g_runtime.usb, budget));
	CHECK(g_cdc.read_calls == 0u);
	CHECK(g_runtime.usb.usb_rx_length == 64u);

	current_key = "rx.would-block";
	reset_world(false);
	g_cdc.connected = true;
	g_cdc.read_value = -1;
	CHECK(aius_runtime_usb_rx_service(&g_runtime.usb, budget));
	CHECK(g_cdc.read_calls == 1u);
	CHECK(g_runtime.usb.usb_rx_length == 0u);
	CHECK(!g_runtime.usb.rx_terminal);

	current_key = "rx.closed";
	reset_world(false);
	g_runtime.usb.usb_rx_length = 3u;
	g_cdc.connected = true;
	g_cdc.read_value = 'a';
	CHECK(aius_runtime_usb_rx_service(&g_runtime.usb, budget));
	CHECK(g_runtime.usb.usb_rx_length == 64u);
	g_cdc.connected = false;
	/* Connection loss between reads: connected() is sampled once up front,
	 * so force the terminal result through the adapter's own CLOSED path. */
	g_runtime.usb.usb_rx_length = 0u;
	g_cdc.connected = true;
	{
		AiusRuntimeUsb service;
		AiusCdcTeensyPrimitives primitives = cdc_primitives();
		struct Override {
			static int read(void *context)
			{
				(void)context;
				g_cdc.read_calls++;
				g_cdc.connected = false;
				return -1;
			}
		};

		primitives.read = Override::read;
		aius_runtime_usb_construct(&service, &primitives);
		g_cdc.read_calls = 0u;
		CHECK(aius_runtime_usb_rx_service(&service, budget));
		CHECK(g_cdc.read_calls == 1u);
		CHECK(service.usb_rx_length == 0u);
	}

	current_key = "rx.fault";
	reset_world(false);
	g_cdc.connected = true;
	{
		AiusRuntimeUsb service;
		AiusCdcTeensyPrimitives primitives = cdc_primitives();
		struct Override {
			static int read(void *context)
			{
				(void)context;
				g_cdc.read_calls++;
				return g_cdc.read_calls <= 2u ? 'q' : -5;
			}
		};

		primitives.read = Override::read;
		aius_runtime_usb_construct(&service, &primitives);
		g_cdc.read_calls = 0u;
		CHECK(aius_runtime_usb_rx_service(&service, budget));
		CHECK(g_cdc.read_calls == 3u);
		CHECK(service.usb_rx_length == 0u);
		CHECK(service.rx_terminal);
		CHECK(service.adapter.last_fault ==
		      AIUS_CDC_ADAPTER_PROTOCOL_FAULT);
		/* Counted once, inside the adapter -- not again by the service. */
		CHECK(service.adapter.protocol_faults == 1u);
	}

	current_key = "rx.malformed";
	{
		AiusRuntimeUsb service;
		AiusCdcTeensyPrimitives primitives = cdc_primitives();
		AiusCdcTeensyOps ops;
		struct Override {
			static AiusIoEvent read_byte(void *context, uint8_t *byte)
			{
				(void)context;
				(void)byte;
				/* PROGRESS with two bytes: impossible for a
				 * one-byte read, so an adapter protocol fault. */
				return io_event(AIUS_IO_PROGRESS, 2u, 0u);
			}
		};

		aius_runtime_usb_construct(&service, &primitives);
		ops = service.ops;
		ops.read_byte = Override::read_byte;
		service.ops = ops;
		g_cdc.connected = true;
		CHECK(aius_runtime_usb_rx_service(&service, budget));
		CHECK(service.rx_terminal);
		CHECK(service.usb_rx_length == 0u);
		CHECK(service.adapter.protocol_faults == 1u);
		CHECK(service.adapter.last_fault ==
		      AIUS_CDC_ADAPTER_PROTOCOL_FAULT);
	}

	current_key = "rx.no-portable-step";
	reset_world(false);
	g_cdc.connected = true;
	g_cdc.read_value = 'z';
	CHECK(aius_runtime_usb_rx_service(&g_runtime.usb, budget));
	/* No portable step means no write can have been offered. */
	CHECK(g_cdc.write_calls == 0u);
	CHECK(g_cdc.capacity_calls == 0u);
}

/* ------------------------------------------------------------------ */
/* 4. USB_TX transaction                                               */
/* ------------------------------------------------------------------ */

static size_t serial_command(AiusRuntimeUsb *service, const char *line)
{
	const size_t length = strlen(line);

	CHECK(length <= sizeof(service->usb_rx));
	memcpy(service->usb_rx, line, length);
	service->usb_rx_length = length;
	return length;
}

static void test_usb_tx(void)
{
	static const char *const keys[] = {
		"tx.protocol-round-trip", "tx.id-payload",
		"tx.zero-capacity",       "tx.partial-write",
		"tx.bootloader",          "tx.bootloader-retry",
		"tx.disconnect-clears",   "tx.rx-terminal"
	};
	const AiusRuntimeBudget *rx_budget =
		aius_runtime_budget(AIUS_BOOT_CHECKPOINT_USB_RX);
	const AiusRuntimeBudget *tx_budget =
		aius_runtime_budget(AIUS_BOOT_CHECKPOINT_USB_TX);

	validate_unique_keys("registry.tx", keys, ARRAY_COUNT(keys), 8u);

	current_key = "tx.protocol-round-trip";
	reset_world(false);
	g_cdc.connected = true;
	g_cdc.capacity_value = 64;
	(void)serial_command(&g_runtime.usb, "AIUS PROTOCOL 1\n");
	/* epoch 1: parse + dispatch */
	CHECK(aius_runtime_usb_tx_service(&g_runtime.usb, tx_budget, 0u,
	                                  &g_runtime.app));
	CHECK(g_runtime.usb.usb_rx_length == 0u);
	CHECK(g_runtime.usb.application_response_present);
	CHECK(g_runtime.usb.usb_verb[0] == 'P');
	CHECK(strcmp(g_runtime.usb.usb_verb, "PROTOCOL") == 0);
	CHECK(g_cdc.write_calls == 0u);
	/* epoch 2: format + one write */
	CHECK(aius_runtime_usb_tx_service(&g_runtime.usb, tx_budget, 1u,
	                                  &g_runtime.app));
	CHECK(g_cdc.write_calls == 1u);
	CHECK(g_runtime.usb.write_event_present);
	/* epoch 3: consume the write result, back to IDLE */
	CHECK(aius_runtime_usb_tx_service(&g_runtime.usb, tx_budget, 2u,
	                                  &g_runtime.app));
	CHECK(g_runtime.usb.runtime.state == AIUS_USB_RUNTIME_IDLE);
	CHECK(g_cdc.write_calls == 1u);

	current_key = "tx.id-payload";
	reset_world(false);
	g_cdc.connected = true;
	g_cdc.capacity_value = 64;
	g_app.usb_payload = "protocol=1 mode=optical";
	g_app.usb_payload_length = strlen("protocol=1 mode=optical");
	(void)serial_command(&g_runtime.usb, "AIUS ID 9\n");
	CHECK(aius_runtime_usb_tx_service(&g_runtime.usb, tx_budget, 0u,
	                                  &g_runtime.app));
	CHECK(g_app.usb_payload_calls == 1u);
	CHECK(strcmp(g_runtime.usb.usb_verb, "ID") == 0);
	CHECK(strcmp(g_runtime.usb.usb_payload,
	             "protocol=1 mode=optical") == 0);
	CHECK(g_runtime.usb.application_response.payload ==
	      g_runtime.usb.usb_payload);
	CHECK(g_runtime.usb.application_response.verb ==
	      g_runtime.usb.usb_verb);

	current_key = "tx.zero-capacity";
	reset_world(false);
	g_cdc.connected = true;
	g_cdc.capacity_value = 0;
	(void)serial_command(&g_runtime.usb, "AIUS PROTOCOL 1\n");
	CHECK(aius_runtime_usb_tx_service(&g_runtime.usb, tx_budget, 0u,
	                                  &g_runtime.app));
	CHECK(aius_runtime_usb_tx_service(&g_runtime.usb, tx_budget, 1u,
	                                  &g_runtime.app));
	CHECK(g_cdc.write_calls == 0u);
	CHECK(g_runtime.usb.runtime.response_offset == 0u);
	g_cdc.capacity_value = 64;
	CHECK(aius_runtime_usb_tx_service(&g_runtime.usb, tx_budget, 2u,
	                                  &g_runtime.app));
	CHECK(g_cdc.write_calls == 1u);

	current_key = "tx.partial-write";
	reset_world(false);
	g_cdc.connected = true;
	g_cdc.capacity_value = 64;
	g_cdc.write_result = 4u;
	(void)serial_command(&g_runtime.usb, "AIUS PROTOCOL 1\n");
	CHECK(aius_runtime_usb_tx_service(&g_runtime.usb, tx_budget, 0u,
	                                  &g_runtime.app));
	CHECK(aius_runtime_usb_tx_service(&g_runtime.usb, tx_budget, 1u,
	                                  &g_runtime.app));
	CHECK(g_cdc.write_calls == 1u);
	CHECK(aius_runtime_usb_tx_service(&g_runtime.usb, tx_budget, 2u,
	                                  &g_runtime.app));
	CHECK(g_cdc.write_calls == 2u);
	CHECK(g_runtime.usb.runtime.response_offset == 4u);

	current_key = "tx.bootloader";
	reset_world(false);
	g_cdc.connected = true;
	g_cdc.capacity_value = 64;
	(void)serial_command(&g_runtime.usb, "AIUS BOOTLOADER 3\n");
	CHECK(aius_runtime_usb_tx_service(&g_runtime.usb, tx_budget, 0u,
	                                  &g_runtime.app));
	CHECK(strcmp(g_runtime.usb.usb_verb, "BOOTLOADER") == 0);
	CHECK(aius_runtime_usb_tx_service(&g_runtime.usb, tx_budget, 1u,
	                                  &g_runtime.app));
	CHECK(g_cdc.write_calls == 1u);
	CHECK(g_runtime.usb.runtime.state == AIUS_USB_RUNTIME_ACK_PENDING);
	CHECK(g_cdc.bootloader_calls == 0u);
	CHECK(aius_runtime_usb_tx_service(&g_runtime.usb, tx_budget, 2u,
	                                  &g_runtime.app));
	CHECK(g_cdc.bootloader_calls == 1u);
	CHECK(g_runtime.usb.bootloader_entered);
	CHECK(g_cdc.flush_calls == 1u);
	CHECK(g_cdc.flush_calls_at_entry == 1u);
	/* The entry primitive returned, so HalfKay was not reached and the board
	 * is still running. The next epoch releases the terminal state instead
	 * of latching it, and emits nothing on its own. */
	CHECK(aius_runtime_usb_tx_service(&g_runtime.usb, tx_budget, 3u,
	                                  &g_runtime.app));
	CHECK(g_cdc.bootloader_calls == 1u);
	CHECK(g_runtime.usb.runtime.state == AIUS_USB_RUNTIME_IDLE);
	CHECK(g_runtime.usb.runtime.bootloader_entry_failures == 1u);

	current_key = "tx.bootloader-retry";
	/* A repeat request over the same runtime is answered and entered again.
	 * A one-shot latch here would swallow it silently and leave the Program
	 * button as the only way back into the loader. */
	(void)serial_command(&g_runtime.usb, "AIUS BOOTLOADER 4\n");
	CHECK(aius_runtime_usb_tx_service(&g_runtime.usb, tx_budget, 4u,
	                                  &g_runtime.app));
	CHECK(strcmp(g_runtime.usb.usb_verb, "BOOTLOADER") == 0);
	CHECK(aius_runtime_usb_tx_service(&g_runtime.usb, tx_budget, 5u,
	                                  &g_runtime.app));
	CHECK(g_cdc.write_calls == 2u);
	CHECK(g_runtime.usb.runtime.state == AIUS_USB_RUNTIME_ACK_PENDING);
	CHECK(aius_runtime_usb_tx_service(&g_runtime.usb, tx_budget, 6u,
	                                  &g_runtime.app));
	CHECK(g_cdc.bootloader_calls == 2u);
	CHECK(g_cdc.flush_calls == 2u);
	CHECK(g_cdc.flush_calls_at_entry == 2u);
	CHECK(g_runtime.usb.runtime.bootloader_entry_failures == 1u);

	current_key = "tx.disconnect-clears";
	reset_world(false);
	g_cdc.connected = true;
	g_cdc.capacity_value = 64;
	(void)serial_command(&g_runtime.usb, "AIUS PRO");
	g_cdc.connected = false;
	CHECK(aius_runtime_usb_tx_service(&g_runtime.usb, tx_budget, 0u,
	                                  &g_runtime.app));
	CHECK(g_runtime.usb.usb_rx_length == 0u);
	CHECK(g_cdc.capacity_calls == 0u);

	current_key = "tx.rx-terminal";
	reset_world(false);
	g_cdc.connected = true;
	g_runtime.usb.rx_terminal = true;
	(void)serial_command(&g_runtime.usb, "AIUS PROTOCOL 1\n");
	CHECK(aius_runtime_usb_tx_service(&g_runtime.usb, tx_budget, 0u,
	                                  &g_runtime.app));
	/* rx_terminal suppresses the connection sample for the epoch, so no
	 * capacity is sampled and staged input is discarded. */
	CHECK(g_cdc.capacity_calls == 0u);
	CHECK(g_runtime.usb.usb_rx_length == 0u);
	CHECK(aius_runtime_usb_rx_service(&g_runtime.usb, rx_budget));
	CHECK(!g_runtime.usb.rx_terminal);
}

/* ------------------------------------------------------------------ */
/* 5. NETWORK precedence                                              */
/* ------------------------------------------------------------------ */

static void test_network_precedence(void)
{
	static const char *const keys[] = {
		"net.construct-retains-action", "net.online-handshake",
		"net.idle-poll",                "net.link-loss",
		"net.dhcp-deadline",            "net.backoff-restart"
	};
	uint32_t now_ms = 1u;

	validate_unique_keys("registry.net", keys, ARRAY_COUNT(keys), 6u);

	current_key = "net.construct-retains-action";
	reset_world(false);
	CHECK(g_runtime.network_http.network_action_present);
	CHECK(g_runtime.network_http.pending_network_action.kind ==
	      AIUS_NET_ACTION_START_STACK);
	CHECK(g_net.install_calls == 0u);
	CHECK(g_net.stack_start_calls == 0u);

	current_key = "net.online-handshake";
	bring_online(&now_ms);
	CHECK(g_net.install_calls == 1u);
	CHECK(g_net.stack_start_calls == 1u);
	CHECK(g_net.dhcp_start_calls == 1u);
	CHECK(g_net.listener_open_calls == 1u);
	CHECK(g_net.udp_open_calls == 1u);
	CHECK(g_net.clear_calls == 0u);

	current_key = "net.idle-poll";
	{
		const unsigned before = g_net.link_calls;

		CHECK(run_epoch(now_ms++));
		CHECK(g_net.link_calls == before + 1u);
		CHECK(g_net.lifecycle_calls_epoch == 0u);
		CHECK(!g_runtime.network_http.network_event_present);
		CHECK(!g_runtime.network_http.network_action_present);
	}

	current_key = "net.link-loss";
	g_net.link = AIUS_NETWORK_TEENSY_LINK_DOWN;
	CHECK(run_epoch(now_ms++)); /* poll -> RESOURCE_FAULT retained */
	CHECK(g_runtime.network_http.network_event_present);
	CHECK(g_runtime.network_http.pending_network_event.fault ==
	      AIUS_NET_FAULT_LINK_LOST);
	CHECK(run_epoch(now_ms++)); /* supervisor step -> TEARDOWN */
	CHECK(g_runtime.network_http.network_action_present);
	CHECK(g_runtime.network_http.pending_network_action.kind ==
	      AIUS_NET_ACTION_TEARDOWN);
	CHECK(g_runtime.network_http.supervisor.state ==
	      AIUS_NET_STATE_TEARDOWN);

	current_key = "net.dhcp-deadline";
	reset_world(false);
	now_ms = 1u;
	CHECK(run_epoch(now_ms++)); /* START_STACK */
	CHECK(run_epoch(now_ms++)); /* step -> START_DHCP, deadline armed */
	CHECK(run_epoch(now_ms++)); /* apply START_DHCP */
	CHECK(g_runtime.network_http.supervisor.state ==
	      AIUS_NET_STATE_DHCP_WAIT);
	/* No lease, link up: only the idle-poll supervisor step can expire the
	 * 5 s deadline. */
	CHECK(run_epoch(AIUS_NET_DHCP_DEADLINE_MS + 3u));
	CHECK(g_runtime.network_http.supervisor.state ==
	      AIUS_NET_STATE_TEARDOWN);
	CHECK(g_runtime.network_http.supervisor.last_fault ==
	      AIUS_NET_FAULT_DHCP_TIMEOUT);
	CHECK(g_runtime.network_http.network_action_present);

	current_key = "net.backoff-restart";
	{
		uint32_t clock = AIUS_NET_DHCP_DEADLINE_MS + 4u;
		unsigned guard = 0u;

		/* Drain teardown, then let backoff expire and restart. */
		while (g_runtime.network_http.supervisor.state !=
			       AIUS_NET_STATE_BACKOFF &&
		       guard < 12u) {
			CHECK(run_epoch(clock++));
			guard++;
		}
		CHECK(g_runtime.network_http.supervisor.state ==
		      AIUS_NET_STATE_BACKOFF);
		CHECK(g_net.clear_calls == 1u);
		CHECK(g_net.stack_close_calls == 1u);
		CHECK(run_epoch(clock + AIUS_NET_BACKOFF_MS + 1u));
		CHECK(g_runtime.network_http.supervisor.state ==
		      AIUS_NET_STATE_STACK_START);
		CHECK(g_runtime.network_http.supervisor.generation == 2u);
	}
}

/* ------------------------------------------------------------------ */
/* 6. HTTP through the composed epoch                                 */
/* ------------------------------------------------------------------ */

static const char *const kStatusRequest =
	"GET /api/status HTTP/1.1\r\nHost: aius\r\n\r\n";

static void drive_epochs(uint32_t *now_ms, unsigned count)
{
	for (unsigned i = 0; i < count; ++i)
		CHECK(run_epoch((*now_ms)++));
}

static bool sink_contains(const char *needle)
{
	const size_t length = strlen(needle);

	if (g_net.tx_length < length)
		return false;
	for (size_t offset = 0; offset + length <= g_net.tx_length; ++offset)
		if (memcmp(g_net.tx + offset, needle, length) == 0)
			return true;
	return false;
}

static void test_http_epoch(void)
{
	static const char *const keys[] = {
		"http.offline-untouched", "http.status-round-trip",
		"http.one-io-per-epoch",  "http.close-once"
	};
	uint32_t now_ms = 1u;

	validate_unique_keys("registry.http", keys, ARRAY_COUNT(keys), 4u);

	current_key = "http.offline-untouched";
	reset_world(false);
	CHECK(run_epoch(now_ms++));
	CHECK(g_net.accept_calls == 0u);
	CHECK(!g_runtime.network_http.http_stepped);

	current_key = "http.status-round-trip";
	reset_world(false);
	now_ms = 1u;
	bring_online(&now_ms);
	g_net.client_pending = true;
	g_net.rx = (const uint8_t *)kStatusRequest;
	g_net.rx_length = strlen(kStatusRequest);
	g_net.rx_offset = 0u;
	drive_epochs(&now_ms, 12u);
	CHECK(g_net.accept_calls >= 1u);
	CHECK(g_net.read_calls >= 1u);
	CHECK(g_net.write_calls >= 1u);
	CHECK(g_app.status_calls == 1u);
	CHECK(sink_contains("HTTP/1.0 200 OK"));
	CHECK(sink_contains("{\"ok\":true}"));
	CHECK(sink_contains("Content-Type: application/json"));

	current_key = "http.one-io-per-epoch";
	/* run_epoch() already asserts <= 1 transport primitive per epoch; pin
	 * the aggregate so a future change cannot batch them silently. */
	CHECK(g_net.accept_calls + g_net.read_calls + g_net.write_calls +
	          g_net.close_calls <=
	      12u);

	current_key = "http.close-once";
	drive_epochs(&now_ms, 6u);
	CHECK(g_net.close_calls == 1u);
}

/* ------------------------------------------------------------------ */
/* 6b. HTTP status counters at the runtime seam                        */
/*                                                                     */
/* The legacy producers died with service_http() at the cutover; the   */
/* reinstated ones live where the transaction is observable. Each      */
/* scenario pins one counting mechanism: DISPATCH actions (requests),  */
/* first-fault transitions (rejections, timeout drops), and the seam's */
/* own IO observation (transport drops -- FAULT anywhere, CLOSED       */
/* mid-read). The last scenario pins that a network teardown, which    */
/* re-inits the transaction, does not zero the counters.               */
/* ------------------------------------------------------------------ */

static void test_http_counters(void)
{
	static const char *const keys[] = {
		"counters.dispatch",        "counters.rejection",
		"counters.drop-timeout",    "counters.drop-transport",
		"counters.drop-peer-close", "counters.survive-restart"
	};
	static const char *const kBadMethodRequest =
		"BOGUS /api/status HTTP/1.1\r\nHost: aius\r\n\r\n";
	uint32_t now_ms;

	validate_unique_keys("registry.counters", keys, ARRAY_COUNT(keys), 6u);

	current_key = "counters.dispatch";
	reset_world(false);
	now_ms = 1u;
	bring_online(&now_ms);
	CHECK(g_runtime.network_http.http_requests == 0u);
	CHECK(g_runtime.network_http.http_rejections == 0u);
	CHECK(g_runtime.network_http.http_client_drops == 0u);
	g_net.client_pending = true;
	g_net.rx = (const uint8_t *)kStatusRequest;
	g_net.rx_length = strlen(kStatusRequest);
	g_net.rx_offset = 0u;
	drive_epochs(&now_ms, 12u);
	CHECK(g_app.status_calls == 1u);
	CHECK(g_runtime.network_http.http_requests == 1u);
	CHECK(g_runtime.network_http.http_rejections == 0u);
	CHECK(g_runtime.network_http.http_client_drops == 0u);

	current_key = "counters.rejection";
	reset_world(false);
	now_ms = 1u;
	bring_online(&now_ms);
	g_net.client_pending = true;
	g_net.rx = (const uint8_t *)kBadMethodRequest;
	g_net.rx_length = strlen(kBadMethodRequest);
	g_net.rx_offset = 0u;
	drive_epochs(&now_ms, 12u);
	CHECK(sink_contains("method_not_allowed"));
	CHECK(g_runtime.network_http.http_requests == 0u);
	CHECK(g_runtime.network_http.http_rejections == 1u);
	CHECK(g_runtime.network_http.http_client_drops == 0u);

	current_key = "counters.drop-timeout";
	reset_world(false);
	now_ms = 1u;
	bring_online(&now_ms);
	/* A client that connects and never sends: the request deadline is the
	 * modern spelling of the legacy 150 ms idle drop. */
	g_net.client_pending = true;
	for (unsigned i = 0; i < 10u; ++i) {
		CHECK(run_epoch(now_ms));
		now_ms += 40u;
	}
	CHECK(sink_contains("request_timeout"));
	CHECK(g_runtime.network_http.http_requests == 0u);
	CHECK(g_runtime.network_http.http_rejections == 0u);
	CHECK(g_runtime.network_http.http_client_drops == 1u);

	current_key = "counters.drop-transport";
	reset_world(false);
	now_ms = 1u;
	bring_online(&now_ms);
	g_net.client_pending = true;
	g_net.read_fault_once = 1u;
	drive_epochs(&now_ms, 10u);
	/* Counted once at the seam's IO observation; the TRANSPORT fault the
	 * transaction records when it consumes the same result must not count
	 * again. */
	CHECK(g_runtime.network_http.http_client_drops == 1u);
	CHECK(g_runtime.network_http.http_requests == 0u);
	CHECK(g_runtime.network_http.http_rejections == 0u);

	current_key = "counters.drop-peer-close";
	reset_world(false);
	now_ms = 1u;
	bring_online(&now_ms);
	g_net.client_pending = true;
	g_net.read_closed_once = 1u;
	drive_epochs(&now_ms, 10u);
	CHECK(g_runtime.network_http.http_client_drops == 1u);
	CHECK(g_runtime.network_http.http_requests == 0u);
	CHECK(g_runtime.network_http.http_rejections == 0u);

	current_key = "counters.survive-restart";
	reset_world(false);
	now_ms = 1u;
	bring_online(&now_ms);
	g_net.client_pending = true;
	g_net.rx = (const uint8_t *)kStatusRequest;
	g_net.rx_length = strlen(kStatusRequest);
	g_net.rx_offset = 0u;
	drive_epochs(&now_ms, 12u);
	CHECK(g_runtime.network_http.http_requests == 1u);
	/* Link loss tears the generation down and http_init() re-inits the
	 * transaction; the counters answer "since boot", not "since the last
	 * generation", so they must survive it. */
	g_net.link = AIUS_NETWORK_TEENSY_LINK_DOWN;
	drive_epochs(&now_ms, 8u);
	CHECK(!g_runtime.network_http.target.online);
	CHECK(g_runtime.network_http.http_requests == 1u);
	CHECK(g_runtime.network_http.http_rejections == 0u);
	CHECK(g_runtime.network_http.http_client_drops == 0u);
}

/* ------------------------------------------------------------------ */
/* 7. HTTP -> storage bridge                                          */
/* ------------------------------------------------------------------ */

static void test_storage_bridge(void)
{
	static const char *const keys[] = {
		"sd.download",        "sd.one-primitive-per-epoch",
		"sd.delete-missing",  "sd.invalid-start",
		"sd.publish-dropped", "sd.release-drains",
		"sd.upload-supply"
	};
	uint32_t now_ms = 1u;
	static const char *const download_request =
		"GET /sd/www/data.bin HTTP/1.1\r\nHost: aius\r\n\r\n";
	static const char *const delete_request =
		"DELETE /sd/www/gone.bin HTTP/1.1\r\nHost: aius\r\n"
		"X-Aius-SD-Auth: 0123456789abcdef\r\n\r\n";

	validate_unique_keys("registry.sd", keys, ARRAY_COUNT(keys), 7u);

	current_key = "sd.download";
	reset_world(false);
	bring_online(&now_ms);
	memcpy(g_sd.data, "hello-sd", 8u);
	g_sd.data_length = 8u;
	g_net.client_pending = true;
	g_net.rx = (const uint8_t *)download_request;
	g_net.rx_length = strlen(download_request);
	g_net.rx_offset = 0u;
	drive_epochs(&now_ms, 40u);
	CHECK(sink_contains("HTTP/1.0 200 OK"));
	CHECK(sink_contains("hello-sd"));
	CHECK(g_sd.open_calls == 1u);
	CHECK(g_sd.close_calls == 1u);
	CHECK(g_net.close_calls == 1u);

	current_key = "sd.one-primitive-per-epoch";
	/* Every SD primitive above went through run_epoch()'s per-epoch bound. */
	CHECK(g_sd.primitive_calls >= 5u);

	current_key = "sd.delete-missing";
	reset_world(false);
	now_ms = 1u;
	bring_online(&now_ms);
	g_sd.exists_result = false;
	g_net.client_pending = true;
	g_net.rx = (const uint8_t *)delete_request;
	g_net.rx_length = strlen(delete_request);
	g_net.rx_offset = 0u;
	drive_epochs(&now_ms, 30u);
	CHECK(g_sd.exists_calls == 1u);
	CHECK(g_sd.remove_calls == 0u);
	CHECK(sink_contains("HTTP/1.0 404 Not Found"));
	CHECK(sink_contains("sd_not_found"));

	current_key = "sd.invalid-start";
	{
		AiusRuntimeStorage storage;
		AiusHttpAction action;
		AiusSdJobRequest request;
		AiusHttpEvent event;
		const AiusRuntimeBudget *budget =
			aius_runtime_budget(AIUS_BOOT_CHECKPOINT_SD);

		aius_runtime_storage_construct(&storage);
		memset(&request, 0, sizeof(request));
		/* PROBE forbids every path; a target path makes it invalid. */
		request.kind = AIUS_SD_JOB_PROBE;
		request.target_path = "/aius/x";
		memset(&action, 0, sizeof(action));
		action.kind = AIUS_HTTP_ACTION_SD_START;
		action.action_id = 41u;
		action.sd_request = &request;
		g_sd.primitive_calls = 0u;
		CHECK(aius_runtime_storage_service(&storage, budget, 1u, &action,
		                                   &event));
		CHECK(event.kind == AIUS_HTTP_EVENT_SD_RESULT);
		CHECK(event.action_id == 41u);
		CHECK(event.sd_kind == AIUS_HTTP_SD_FAILED);
		CHECK(event.sd_fault == AIUS_SD_FAULT_INVALID_REQUEST);
		CHECK(g_sd.primitive_calls == 0u);
		CHECK(!storage.http_authority);
		/* Rejected start still leaves the job restartable. */
		memset(&request, 0, sizeof(request));
		request.kind = AIUS_SD_JOB_PROBE;
		action.action_id = 42u;
		storage.http_event_present = false;
		CHECK(aius_runtime_storage_service(&storage, budget, 2u, &action,
		                                   &event));
		CHECK(event.sd_kind == AIUS_HTTP_SD_STARTED);
		CHECK(event.sd_capacity == AIUS_RUNTIME_STORAGE_TRANSFER);

		/* Invalid-call rows: a non-SD action kind, a zero action ID, a
		 * step with no operation, and a download acknowledgement with no
		 * borrowed chunk are all root protocol violations. */
		memset(&action, 0, sizeof(action));
		action.kind = AIUS_HTTP_ACTION_READ;
		action.action_id = 44u;
		CHECK(!aius_runtime_storage_service(&storage, budget, 3u,
		                                    &action, &event));
		action.kind = AIUS_HTTP_ACTION_SD_STEP;
		action.action_id = 0u;
		action.sd_operation = AIUS_HTTP_SD_OP_ADVANCE;
		CHECK(!aius_runtime_storage_service(&storage, budget, 3u,
		                                    &action, &event));
		action.action_id = 45u;
		action.sd_operation = AIUS_HTTP_SD_OP_NONE;
		CHECK(!aius_runtime_storage_service(&storage, budget, 3u,
		                                    &action, &event));
		action.sd_operation = AIUS_HTTP_SD_OP_ACK_DOWNLOAD;
		CHECK(!storage.output_outstanding);
		CHECK(!aius_runtime_storage_service(&storage, budget, 3u,
		                                    &action, &event));
		action.kind = AIUS_HTTP_ACTION_SD_START;
		action.sd_request = nullptr;
		CHECK(!aius_runtime_storage_service(&storage, budget, 3u,
		                                    &action, &event));
		/* None of the rejections claimed authority or spent an epoch. */
		CHECK(!storage.http_authority);
		CHECK(g_sd.primitive_calls == 0u);
	}

	current_key = "sd.publish-dropped";
	{
		AiusRuntimeStorage storage;
		AiusHttpAction action;
		AiusSdJobRequest request;
		AiusHttpEvent event;
		const AiusRuntimeBudget *budget =
			aius_runtime_budget(AIUS_BOOT_CHECKPOINT_SD);
		unsigned guard = 0u;

		aius_runtime_storage_construct(&storage);
		memset(&g_sd, 0, sizeof(g_sd));
		g_sd.begin_ok = true;
		g_sd.open_ok = true;
		memcpy(g_sd.data, "cfg", 3u);
		g_sd.data_length = 3u;
		memset(&request, 0, sizeof(request));
		request.kind = AIUS_SD_JOB_CONFIG_LOAD;
		request.target_path = "/aius/dsp.json";
		request.content_type = "application/json";
		request.publish = true;
		request.publish_capacity = 64u;
		memset(&action, 0, sizeof(action));
		action.kind = AIUS_HTTP_ACTION_SD_START;
		action.action_id = 51u;
		action.sd_request = &request;
		CHECK(aius_runtime_storage_service(&storage, budget, 1u, &action,
		                                   &event));
		CHECK(event.sd_kind == AIUS_HTTP_SD_STARTED);

		action.kind = AIUS_HTTP_ACTION_SD_STEP;
		action.sd_operation = AIUS_HTTP_SD_OP_ADVANCE;
		action.sd_request = nullptr;
		while (guard < 24u &&
		       event.sd_kind != AIUS_HTTP_SD_COMPLETE &&
		       event.sd_kind != AIUS_HTTP_SD_FAILED) {
			action.action_id = 60u + guard;
			storage.http_event_present = false;
			CHECK(aius_runtime_storage_service(&storage, budget,
			                                   2u + guard, &action,
			                                   &event));
			guard++;
		}
		CHECK(event.sd_kind == AIUS_HTTP_SD_COMPLETE);
		CHECK(storage.publish_dropped == 1u);
	}

	current_key = "sd.release-drains";
	{
		AiusRuntimeStorage storage;
		AiusHttpAction action;
		AiusSdJobRequest request;
		AiusHttpEvent event;
		const AiusRuntimeBudget *budget =
			aius_runtime_budget(AIUS_BOOT_CHECKPOINT_SD);
		unsigned guard = 0u;

		aius_runtime_storage_construct(&storage);
		memset(&g_sd, 0, sizeof(g_sd));
		g_sd.begin_ok = true;
		g_sd.open_ok = true;
		memcpy(g_sd.data, "0123456789", 10u);
		g_sd.data_length = 10u;
		memset(&request, 0, sizeof(request));
		request.kind = AIUS_SD_JOB_DOWNLOAD;
		request.target_path = "/aius/data.bin";
		request.content_type = "application/octet-stream";
		memset(&action, 0, sizeof(action));
		action.kind = AIUS_HTTP_ACTION_SD_START;
		action.action_id = 71u;
		action.sd_request = &request;
		CHECK(aius_runtime_storage_service(&storage, budget, 1u, &action,
		                                   &event));
		CHECK(event.sd_kind == AIUS_HTTP_SD_STARTED);

		action.kind = AIUS_HTTP_ACTION_SD_STEP;
		action.sd_operation = AIUS_HTTP_SD_OP_ADVANCE;
		action.sd_request = nullptr;
		while (guard < 12u && event.sd_kind !=
			                      AIUS_HTTP_SD_DOWNLOAD_DATA) {
			action.action_id = 80u + guard;
			storage.http_event_present = false;
			CHECK(aius_runtime_storage_service(&storage, budget,
			                                   2u + guard, &action,
			                                   &event));
			guard++;
		}
		CHECK(event.sd_kind == AIUS_HTTP_SD_DOWNLOAD_DATA);
		CHECK(event.sd_length == 10u);
		CHECK(event.sd_size == 10u);
		CHECK(storage.output_outstanding);
		CHECK(g_sd.file_open);

		/* The owner disappears mid-chunk. */
		aius_runtime_storage_release_http(&storage);
		CHECK(!storage.http_authority);
		CHECK(storage.draining);
		guard = 0u;
		while (guard < 24u && !storage.terminal_seen) {
			CHECK(aius_runtime_storage_service(&storage, budget,
			                                   40u + guard, nullptr,
			                                   &event));
			CHECK(event.kind == AIUS_HTTP_EVENT_NONE);
			guard++;
		}
		CHECK(storage.terminal_seen);
		CHECK(!storage.draining);
		CHECK(!g_sd.file_open);
		CHECK(g_sd.close_calls == 1u);
	}

	current_key = "sd.upload-supply";
	{
		/* The HTTP upload leg cannot be reached end-to-end today:
		 * accept_application() in aius_http_transaction.c admits
		 * AIUS_SD_JOB_UPLOAD only for a streamed request, so the buffered
		 * PUTs cannot express one (Phase 14's open decision). The
		 * storage service's NEED_UPLOAD/SUPPLY_UPLOAD mapping is driven
		 * directly instead. */
		AiusRuntimeStorage storage;
		AiusHttpAction action;
		AiusSdJobRequest request;
		AiusHttpEvent event;
		const AiusRuntimeBudget *budget =
			aius_runtime_budget(AIUS_BOOT_CHECKPOINT_SD);
		static const uint8_t payload[4] = {'a', 'b', 'c', 'd'};
		unsigned guard = 0u;

		aius_runtime_storage_construct(&storage);
		memset(&g_sd, 0, sizeof(g_sd));
		g_sd.begin_ok = true;
		g_sd.open_ok = true;
		g_sd.exists_result = true;
		g_sd.remove_ok = true;
		memset(&request, 0, sizeof(request));
		request.kind = AIUS_SD_JOB_UPLOAD;
		request.install_mode = AIUS_SD_INSTALL_REPLACE;
		request.target_path = "/aius/a.bin";
		request.temp_path = "/aius/b.tmp";
		request.backup_path = "/aius/c.bak";
		request.total = sizeof(payload);
		memset(&action, 0, sizeof(action));
		action.kind = AIUS_HTTP_ACTION_SD_START;
		action.action_id = 91u;
		action.sd_request = &request;
		CHECK(aius_runtime_storage_service(&storage, budget, 1u, &action,
		                                   &event));
		CHECK(event.sd_kind == AIUS_HTTP_SD_STARTED);

		action.kind = AIUS_HTTP_ACTION_SD_STEP;
		action.sd_operation = AIUS_HTTP_SD_OP_ADVANCE;
		action.sd_request = nullptr;
		while (guard < 20u && event.sd_kind != AIUS_HTTP_SD_NEED_UPLOAD) {
			action.action_id = 100u + guard;
			storage.http_event_present = false;
			g_sd.primitive_calls_epoch = 0u;
			CHECK(aius_runtime_storage_service(&storage, budget,
			                                   2u + guard, &action,
			                                   &event));
			CHECK(g_sd.primitive_calls_epoch <= 1u);
			guard++;
		}
		CHECK(event.sd_kind == AIUS_HTTP_SD_NEED_UPLOAD);
		CHECK(event.sd_length == sizeof(payload));
		CHECK(g_sd.file_open);

		action.sd_operation = AIUS_HTTP_SD_OP_SUPPLY_UPLOAD;
		action.action_id = 130u;
		action.data = payload;
		action.length = sizeof(payload);
		storage.http_event_present = false;
		g_sd.primitive_calls_epoch = 0u;
		CHECK(aius_runtime_storage_service(&storage, budget, 40u, &action,
		                                   &event));
		CHECK(event.sd_kind == AIUS_HTTP_SD_PROGRESS);
		/* supply() copies into the job buffer and selects WRITE without a
		 * driver action, so the supply epoch spends no primitive. */
		CHECK(g_sd.primitive_calls_epoch == 0u);

		action.sd_operation = AIUS_HTTP_SD_OP_ADVANCE;
		action.data = nullptr;
		action.length = 0u;
		guard = 0u;
		while (guard < 30u && event.sd_kind != AIUS_HTTP_SD_COMPLETE &&
		       event.sd_kind != AIUS_HTTP_SD_FAILED) {
			action.action_id = 140u + guard;
			storage.http_event_present = false;
			g_sd.primitive_calls_epoch = 0u;
			CHECK(aius_runtime_storage_service(&storage, budget,
			                                   41u + guard, &action,
			                                   &event));
			CHECK(g_sd.primitive_calls_epoch <= 1u);
			guard++;
		}
		CHECK(event.sd_kind == AIUS_HTTP_SD_COMPLETE);
		CHECK(g_sd.data_length == sizeof(payload));
		CHECK(!g_sd.file_open);
		CHECK(storage.publish_dropped == 0u);
	}
}

/* ------------------------------------------------------------------ */
/* 7b. root-initiated jobs and the publication consumer                */
/* ------------------------------------------------------------------ */

/* Prepares a standalone storage service over a fake card holding `length`
 * bytes of a deterministic pattern, with the publication consumer registered
 * only when `consume` is set. */
static void arm_root_storage(AiusRuntimeStorage *storage, size_t length,
                             bool consume)
{
	AiusRuntimeStoragePublish publish;

	aius_runtime_storage_construct(storage);
	memset(&g_sd, 0, sizeof(g_sd));
	memset(&g_publish, 0, sizeof(g_publish));
	g_publish.expected_context = &g_publish;
	g_publish.accept = true;
	g_publish.max_length = AIUS_RUNTIME_STORAGE_TRANSFER;
	g_sd.begin_ok = true;
	g_sd.open_ok = true;
	g_sd.media_present = true;
	g_sd.card_detected = true;
	CHECK(length <= sizeof(g_sd.data));
	for (size_t i = 0; i < length; ++i)
		g_sd.data[i] = (uint8_t)((i * 7u + 3u) & 0xffu);
	g_sd.data_length = length;
	if (!consume)
		return;
	memset(&publish, 0, sizeof(publish));
	publish.context = &g_publish;
	publish.deliver = publish_config;
	aius_runtime_storage_set_publish(storage, &publish);
}

static AiusSdJobRequest config_load_request(size_t publish_capacity)
{
	AiusSdJobRequest request;

	memset(&request, 0, sizeof(request));
	request.kind = AIUS_SD_JOB_CONFIG_LOAD;
	request.target_path = "/aius/dsp.json";
	request.content_type = "application/json";
	request.publish = true;
	request.publish_capacity = publish_capacity;
	return request;
}

/* Advances an ownerless job with no HTTP action, one epoch at a time, until it
 * leaves RUNNING. Each pass asserts the epoch's one-primitive budget. */
static void drive_root_job(AiusRuntimeStorage *storage, uint32_t *now_ms,
                           unsigned limit)
{
	const AiusRuntimeBudget *budget =
		aius_runtime_budget(AIUS_BOOT_CHECKPOINT_SD);
	AiusHttpEvent event;
	unsigned guard = 0u;

	while (guard < limit &&
	       storage->root_state == AIUS_RUNTIME_STORAGE_ROOT_RUNNING) {
		g_sd.primitive_calls_epoch = 0u;
		CHECK(aius_runtime_storage_service(storage, budget,
		                                   (*now_ms)++, nullptr,
		                                   &event));
		CHECK(event.kind == AIUS_HTTP_EVENT_NONE);
		CHECK(g_sd.primitive_calls_epoch <= 1u);
		guard++;
	}
}

static void test_root_storage_job(void)
{
	static const char *const keys[] = {
		"root.publish-intact",       "root.publish-wiring",
		"root.publish-model-sized",  "root.publish-oversize",
		"root.publish-dropped",
		"root.publish-refused",      "root.probe",
		"root.invalid-request",      "root.rejects-under-http",
		"root.refuses-http",         "root.survives-release",
		"root.composed-epoch",       "root.composed-invalid"
	};
	const AiusRuntimeBudget *budget =
		aius_runtime_budget(AIUS_BOOT_CHECKPOINT_SD);
	AiusRuntimeStorage storage;
	AiusSdJobRequest request;
	AiusHttpEvent event;
	uint32_t now_ms = 1u;

	validate_unique_keys("registry.root", keys, ARRAY_COUNT(keys), 13u);

	current_key = "root.publish-intact";
	{
		/* A payload filling one whole transfer slice, delivered by a
		 * single read. The publication buffer is larger, so this is the
		 * boundary at which the read stops being one primitive rather
		 * than the boundary at which the job stops being startable. */
		arm_root_storage(&storage, AIUS_RUNTIME_STORAGE_TRANSFER, true);
		request = config_load_request(AIUS_RUNTIME_STORAGE_TRANSFER);
		now_ms = 1u;
		CHECK(aius_runtime_storage_start_root_job(&storage, &request,
		                                          now_ms++));
		CHECK(storage.root_state ==
		      AIUS_RUNTIME_STORAGE_ROOT_RUNNING);
		/* Starting is memory only. */
		CHECK(g_sd.primitive_calls == 0u);
		drive_root_job(&storage, &now_ms, 32u);
		CHECK(storage.root_state ==
		      AIUS_RUNTIME_STORAGE_ROOT_COMPLETE);
		CHECK(g_publish.calls == 1u);
		CHECK(g_publish.last_length == AIUS_RUNTIME_STORAGE_TRANSFER);
		CHECK(memcmp(g_publish.last, g_sd.data,
		             AIUS_RUNTIME_STORAGE_TRANSFER) == 0);
		CHECK(storage.publish_dropped == 0u);
		CHECK(!g_sd.file_open);
		CHECK(g_sd.close_calls == 1u);
		CHECK(aius_runtime_storage_root_result(&storage) != nullptr);
		CHECK(aius_runtime_storage_root_result(&storage)->fault ==
		      AIUS_SD_FAULT_NONE);
		CHECK(g_sd.read_calls == 1u);
		CHECK(g_sd.read_lengths[0] == AIUS_RUNTIME_STORAGE_TRANSFER);
	}

	current_key = "root.publish-wiring";
	{
		/* The service owns both buffers and keeps them distinct: the
		 * transfer slice sizes every primitive, the publication buffer
		 * only the accumulated payload. */
		arm_root_storage(&storage, 0u, true);
		CHECK(storage.job.capacity == AIUS_RUNTIME_STORAGE_TRANSFER);
		CHECK(storage.job.buffer == storage.sd_transfer);
		CHECK(storage.job.publish_buffer == storage.sd_publish);
		CHECK(storage.job.publish_buffer_capacity ==
		      AIUS_RUNTIME_STORAGE_PUBLISH_CAPACITY);
		/* Above the publication buffer the engine still refuses. */
		request = config_load_request(
			AIUS_RUNTIME_STORAGE_PUBLISH_CAPACITY + 1u);
		CHECK(!aius_runtime_storage_start_root_job(&storage, &request,
		                                           1u));
		CHECK(storage.root_state == AIUS_RUNTIME_STORAGE_ROOT_FAILED);
		CHECK(aius_runtime_storage_root_result(&storage)->fault ==
		      AIUS_SD_FAULT_CAPACITY);
		CHECK(g_sd.primitive_calls == 0u);
	}

	current_key = "root.publish-model-sized";
	{
		/* The S3 case: a model-sized config published intact, read in
		 * transfer-slice primitives, one per epoch. */
		arm_root_storage(&storage,
		                 AIUS_RUNTIME_STORAGE_PUBLISH_CAPACITY, true);
		g_publish.max_length = AIUS_RUNTIME_STORAGE_PUBLISH_CAPACITY;
		request = config_load_request(
			AIUS_RUNTIME_STORAGE_PUBLISH_CAPACITY);
		now_ms = 1u;
		CHECK(aius_runtime_storage_start_root_job(&storage, &request,
		                                          now_ms++));
		CHECK(g_sd.primitive_calls == 0u);
		drive_root_job(&storage, &now_ms, 64u);
		CHECK(storage.root_state ==
		      AIUS_RUNTIME_STORAGE_ROOT_COMPLETE);
		/* One publish, carrying every byte, unchanged. */
		CHECK(g_publish.calls == 1u);
		CHECK(g_publish.last_length ==
		      AIUS_RUNTIME_STORAGE_PUBLISH_CAPACITY);
		CHECK(memcmp(g_publish.last, g_sd.data,
		             AIUS_RUNTIME_STORAGE_PUBLISH_CAPACITY) == 0);
		CHECK(storage.publish_dropped == 0u);
		/* The buffer grew; the primitive did not. */
		CHECK(g_sd.read_calls ==
		      AIUS_RUNTIME_STORAGE_PUBLISH_CAPACITY /
			      AIUS_RUNTIME_STORAGE_TRANSFER);
		CHECK(g_sd.read_calls == 24u);
		for (unsigned index = 0u; index < g_sd.read_calls; ++index)
			CHECK(g_sd.read_lengths[index] ==
			      AIUS_RUNTIME_STORAGE_TRANSFER);
		CHECK(g_sd.close_calls == 1u);
		CHECK(!g_sd.file_open);
		CHECK(aius_runtime_storage_root_result(&storage)->fault ==
		      AIUS_SD_FAULT_NONE);
		CHECK(aius_runtime_storage_root_result(&storage)
		              ->transferred_bytes ==
		      AIUS_RUNTIME_STORAGE_PUBLISH_CAPACITY);
	}

	current_key = "root.publish-oversize";
	{
		/* One byte past the publication buffer is refused at the size
		 * event, before any of it is read. */
		arm_root_storage(&storage,
		                 AIUS_RUNTIME_STORAGE_PUBLISH_CAPACITY + 1u,
		                 true);
		g_publish.max_length = AIUS_RUNTIME_STORAGE_PUBLISH_CAPACITY;
		request = config_load_request(
			AIUS_RUNTIME_STORAGE_PUBLISH_CAPACITY);
		now_ms = 1u;
		CHECK(aius_runtime_storage_start_root_job(&storage, &request,
		                                          now_ms++));
		drive_root_job(&storage, &now_ms, 64u);
		CHECK(storage.root_state == AIUS_RUNTIME_STORAGE_ROOT_FAILED);
		CHECK(aius_runtime_storage_root_result(&storage)->fault ==
		      AIUS_SD_FAULT_CAPACITY);
		CHECK(g_publish.calls == 0u);
		CHECK(g_sd.read_calls == 0u);
		CHECK(!g_sd.file_open);
	}

	current_key = "root.publish-dropped";
	{
		arm_root_storage(&storage, 3u, false);
		request = config_load_request(64u);
		now_ms = 1u;
		CHECK(aius_runtime_storage_start_root_job(&storage, &request,
		                                          now_ms++));
		drive_root_job(&storage, &now_ms, 32u);
		CHECK(storage.root_state ==
		      AIUS_RUNTIME_STORAGE_ROOT_COMPLETE);
		CHECK(storage.publish_dropped == 1u);
		CHECK(g_publish.calls == 0u);
	}

	current_key = "root.publish-refused";
	{
		arm_root_storage(&storage, 8u, true);
		g_publish.accept = false;
		request = config_load_request(64u);
		now_ms = 1u;
		CHECK(aius_runtime_storage_start_root_job(&storage, &request,
		                                          now_ms++));
		drive_root_job(&storage, &now_ms, 32u);
		CHECK(storage.root_state == AIUS_RUNTIME_STORAGE_ROOT_FAILED);
		CHECK(g_publish.calls == 1u);
		/* A refusal is a job fault, not a silent drop. */
		CHECK(storage.publish_dropped == 0u);
		CHECK(aius_runtime_storage_root_result(&storage)->fault ==
		      AIUS_SD_FAULT_PUBLISH);
		CHECK(aius_runtime_storage_root_result(&storage)
		              ->driver_fault ==
		      AIUS_RUNTIME_STORAGE_PUBLISH_REJECT_FAULT);
	}

	current_key = "root.probe";
	{
		arm_root_storage(&storage, 0u, true);
		/* The composed probe is the only producer of the status
		 * surface's filesystem type, so it is asserted end to end:
		 * driver numbering in, portable enum out, and -- through
		 * drive_root_job()'s per-epoch check -- no extra primitive. */
		g_sd.fat_type = 32u;
		memset(&request, 0, sizeof(request));
		request.kind = AIUS_SD_JOB_PROBE;
		now_ms = 1u;
		CHECK(aius_runtime_storage_start_root_job(&storage, &request,
		                                          now_ms++));
		drive_root_job(&storage, &now_ms, 32u);
		CHECK(storage.root_state ==
		      AIUS_RUNTIME_STORAGE_ROOT_COMPLETE);
		CHECK(aius_runtime_storage_root_result(&storage)
		              ->media_present);
		CHECK(aius_runtime_storage_root_result(&storage)->filesystem ==
		      AIUS_SD_FILESYSTEM_FAT32);
		/* BEGIN then MEDIA_PRESENT, and nothing else. */
		CHECK(g_sd.primitive_calls == 2u);
		CHECK(g_sd.fat_type_reads == 1u);
		CHECK(g_publish.calls == 0u);

		/* No card: the type the driver still holds is not reported. */
		arm_root_storage(&storage, 0u, true);
		g_sd.media_present = false;
		g_sd.fat_type = 16u;
		memset(&request, 0, sizeof(request));
		request.kind = AIUS_SD_JOB_PROBE;
		now_ms = 1u;
		CHECK(aius_runtime_storage_start_root_job(&storage, &request,
		                                          now_ms++));
		drive_root_job(&storage, &now_ms, 32u);
		CHECK(storage.root_state ==
		      AIUS_RUNTIME_STORAGE_ROOT_COMPLETE);
		CHECK(!aius_runtime_storage_root_result(&storage)
		               ->media_present);
		CHECK(aius_runtime_storage_root_result(&storage)->filesystem ==
		      AIUS_SD_FILESYSTEM_UNKNOWN);
		CHECK(g_sd.fat_type_reads == 0u);
	}

	current_key = "root.invalid-request";
	{
		arm_root_storage(&storage, 0u, true);
		memset(&request, 0, sizeof(request));
		/* PROBE forbids every path. */
		request.kind = AIUS_SD_JOB_PROBE;
		request.target_path = "/aius/x";
		CHECK(!aius_runtime_storage_start_root_job(&storage, &request,
		                                           1u));
		CHECK(storage.root_state == AIUS_RUNTIME_STORAGE_ROOT_FAILED);
		CHECK(aius_runtime_storage_root_result(&storage)->fault ==
		      AIUS_SD_FAULT_INVALID_REQUEST);
		CHECK(g_sd.primitive_calls == 0u);
		/* Null rows, and a rejected start leaves the service usable. */
		CHECK(!aius_runtime_storage_start_root_job(nullptr, &request,
		                                           1u));
		CHECK(!aius_runtime_storage_start_root_job(&storage, nullptr,
		                                           1u));
		memset(&request, 0, sizeof(request));
		request.kind = AIUS_SD_JOB_PROBE;
		CHECK(aius_runtime_storage_start_root_job(&storage, &request,
		                                          2u));
		CHECK(storage.root_state ==
		      AIUS_RUNTIME_STORAGE_ROOT_RUNNING);
		CHECK(aius_runtime_storage_root_result(&storage) == nullptr);
	}

	current_key = "root.rejects-under-http";
	{
		AiusHttpAction action;
		unsigned guard = 0u;

		arm_root_storage(&storage, 10u, true);
		memset(&request, 0, sizeof(request));
		request.kind = AIUS_SD_JOB_DOWNLOAD;
		request.target_path = "/aius/data.bin";
		request.content_type = "application/octet-stream";
		memset(&action, 0, sizeof(action));
		action.kind = AIUS_HTTP_ACTION_SD_START;
		action.action_id = 201u;
		action.sd_request = &request;
		CHECK(aius_runtime_storage_service(&storage, budget, 1u,
		                                   &action, &event));
		CHECK(event.sd_kind == AIUS_HTTP_SD_STARTED);
		CHECK(storage.http_owned);

		/* Direction one: the root defers to a live HTTP job, with no
		 * side effect on the service. */
		{
			AiusSdJobRequest probe;

			memset(&probe, 0, sizeof(probe));
			probe.kind = AIUS_SD_JOB_PROBE;
			CHECK(!aius_runtime_storage_start_root_job(
				&storage, &probe, 2u));
			CHECK(storage.root_state ==
			      AIUS_RUNTIME_STORAGE_ROOT_IDLE);
			CHECK(storage.job.kind == AIUS_SD_JOB_DOWNLOAD);
			/* The undelivered STARTED result alone blocks it. */
			CHECK(storage.http_event_present);

			action.kind = AIUS_HTTP_ACTION_SD_STEP;
			action.sd_operation = AIUS_HTTP_SD_OP_ADVANCE;
			action.sd_request = nullptr;
			while (guard < 24u &&
			       event.sd_kind != AIUS_HTTP_SD_COMPLETE &&
			       event.sd_kind != AIUS_HTTP_SD_FAILED) {
				action.action_id = 210u + guard;
				storage.http_event_present = false;
				CHECK(aius_runtime_storage_service(
					&storage, budget, 3u + guard, &action,
					&event));
				if (event.sd_kind ==
				    AIUS_HTTP_SD_DOWNLOAD_DATA) {
					action.sd_operation =
						AIUS_HTTP_SD_OP_ACK_DOWNLOAD;
				} else {
					action.sd_operation =
						AIUS_HTTP_SD_OP_ADVANCE;
				}
				CHECK(!aius_runtime_storage_start_root_job(
					&storage, &probe, 3u + guard));
				guard++;
			}
			CHECK(event.sd_kind == AIUS_HTTP_SD_COMPLETE);
			/* The terminal result ends HTTP's ownership, so the
			 * root may start once that result is delivered. */
			CHECK(!storage.http_owned);
			storage.http_event_present = false;
			CHECK(aius_runtime_storage_start_root_job(&storage,
			                                          &probe, 60u));
			CHECK(storage.root_state ==
			      AIUS_RUNTIME_STORAGE_ROOT_RUNNING);
		}
	}

	current_key = "root.refuses-http";
	{
		AiusHttpAction action;
		AiusSdJobRequest download;
		unsigned open_calls;

		arm_root_storage(&storage, 8u, true);
		request = config_load_request(64u);
		now_ms = 1u;
		CHECK(aius_runtime_storage_start_root_job(&storage, &request,
		                                          now_ms++));
		/* Get the root job past its first primitive. */
		g_sd.primitive_calls_epoch = 0u;
		CHECK(aius_runtime_storage_service(&storage, budget, now_ms++,
		                                   nullptr, &event));
		CHECK(g_sd.primitive_calls_epoch == 1u);
		open_calls = g_sd.open_calls;

		/* Direction two: HTTP is refused in-band rather than restarting
		 * the root job underneath it. */
		memset(&download, 0, sizeof(download));
		download.kind = AIUS_SD_JOB_DOWNLOAD;
		download.target_path = "/aius/data.bin";
		download.content_type = "application/octet-stream";
		memset(&action, 0, sizeof(action));
		action.kind = AIUS_HTTP_ACTION_SD_START;
		action.action_id = 301u;
		action.sd_request = &download;
		g_sd.primitive_calls_epoch = 0u;
		CHECK(aius_runtime_storage_service(&storage, budget, now_ms++,
		                                   &action, &event));
		CHECK(event.kind == AIUS_HTTP_EVENT_SD_RESULT);
		CHECK(event.action_id == 301u);
		CHECK(event.sd_kind == AIUS_HTTP_SD_FAILED);
		CHECK(event.sd_fault == AIUS_SD_FAULT_PROTOCOL);
		CHECK(event.sd_driver_fault == 0u);
		CHECK(g_sd.primitive_calls_epoch == 0u);
		/* The root job kept both its kind and its open handle. */
		CHECK(storage.root_state ==
		      AIUS_RUNTIME_STORAGE_ROOT_RUNNING);
		CHECK(storage.job.kind == AIUS_SD_JOB_CONFIG_LOAD);
		CHECK(g_sd.open_calls == open_calls);
		CHECK(!storage.http_owned);
		CHECK(!storage.http_authority);

		storage.http_event_present = false;
		drive_root_job(&storage, &now_ms, 32u);
		CHECK(storage.root_state ==
		      AIUS_RUNTIME_STORAGE_ROOT_COMPLETE);
		CHECK(g_publish.calls == 1u);
		CHECK(g_publish.last_length == 8u);
	}

	current_key = "root.survives-release";
	{
		arm_root_storage(&storage, 8u, true);
		request = config_load_request(64u);
		now_ms = 1u;
		CHECK(aius_runtime_storage_start_root_job(&storage, &request,
		                                          now_ms++));
		CHECK(aius_runtime_storage_service(&storage, budget, now_ms++,
		                                   nullptr, &event));
		/* A network teardown has no authority over a root job. */
		aius_runtime_storage_release_http(&storage);
		CHECK(storage.root_state ==
		      AIUS_RUNTIME_STORAGE_ROOT_RUNNING);
		CHECK(!storage.draining);
		CHECK(!storage.cancelling);
		drive_root_job(&storage, &now_ms, 32u);
		CHECK(storage.root_state ==
		      AIUS_RUNTIME_STORAGE_ROOT_COMPLETE);
		CHECK(g_publish.calls == 1u);
	}

	current_key = "root.composed-epoch";
	{
		unsigned guard = 0u;

		reset_world(false);
		now_ms = 1u;
		memcpy(g_sd.data, "{\"dsp\":1}", 9u);
		g_sd.data_length = 9u;
		request = config_load_request(64u);
		CHECK(aius_runtime_teensy_start_storage_job(&g_runtime,
		                                            &request));
		CHECK(g_runtime.storage.root_state ==
		      AIUS_RUNTIME_STORAGE_ROOT_RUNNING);
		CHECK(g_sd.primitive_calls == 0u);
		while (guard < 24u &&
		       g_runtime.storage.root_state ==
			       AIUS_RUNTIME_STORAGE_ROOT_RUNNING) {
			CHECK(run_epoch(now_ms++));
			guard++;
		}
		CHECK(g_runtime.storage.root_state ==
		      AIUS_RUNTIME_STORAGE_ROOT_COMPLETE);
		/* The platform table's optional entry carried the payload. */
		CHECK(g_publish.calls == 1u);
		CHECK(g_publish.last_length == 9u);
		CHECK(memcmp(g_publish.last, "{\"dsp\":1}", 9u) == 0);
		CHECK(g_runtime.storage.publish_dropped == 0u);
	}

	current_key = "root.composed-invalid";
	{
		AiusOpticalApp app = app_table();
		AiusCdcTeensyPrimitives cdc = cdc_primitives();
		AiusRuntimeTeensyPlatformOps platform = platform_ops();

		request = config_load_request(64u);
		CHECK(!aius_runtime_teensy_start_storage_job(nullptr,
		                                             &request));
		platform.feed_watchdog = nullptr;
		aius_runtime_teensy_construct(&g_runtime, &app, &cdc, &platform,
		                             0u, false);
		CHECK(!g_runtime.valid);
		CHECK(!aius_runtime_teensy_start_storage_job(&g_runtime,
		                                             &request));
		/* A composition with no consumer keeps the counting path. */
		platform = platform_ops();
		platform.publish_config = nullptr;
		aius_runtime_teensy_construct(&g_runtime, &app, &cdc, &platform,
		                             0u, false);
		CHECK(g_runtime.valid);
		CHECK(g_runtime.storage.publish.deliver == nullptr);
	}
}

/* ------------------------------------------------------------------ */
/* 7c. failure attribution                                             */
/* ------------------------------------------------------------------ */

/* Runs one HTTP-owned upload to its terminal result, returning the last event.
 * The transaction leg is driven directly for the reason sd.upload-supply gives:
 * the buffered PUT routes cannot express an upload job. */
static AiusHttpSdEventKind run_http_upload(AiusRuntimeStorage *storage,
                                           uint32_t *now_ms)
{
	const AiusRuntimeBudget *budget =
		aius_runtime_budget(AIUS_BOOT_CHECKPOINT_SD);
	static const uint8_t payload[4] = {'a', 'b', 'c', 'd'};
	AiusSdJobRequest request;
	AiusHttpAction action;
	AiusHttpEvent event;
	unsigned guard = 0u;
	uint64_t action_id = 1000u;

	memset(&request, 0, sizeof(request));
	request.kind = AIUS_SD_JOB_UPLOAD;
	request.install_mode = AIUS_SD_INSTALL_REPLACE;
	request.target_path = "/aius/a.bin";
	request.temp_path = "/aius/b.tmp";
	request.backup_path = "/aius/c.bak";
	request.total = sizeof(payload);
	memset(&action, 0, sizeof(action));
	memset(&event, 0, sizeof(event));
	action.kind = AIUS_HTTP_ACTION_SD_START;
	action.action_id = action_id++;
	action.sd_request = &request;
	CHECK(aius_runtime_storage_service(storage, budget, (*now_ms)++, &action,
	                                   &event));
	if (event.sd_kind == AIUS_HTTP_SD_FAILED)
		return event.sd_kind;
	CHECK(event.sd_kind == AIUS_HTTP_SD_STARTED);

	action.kind = AIUS_HTTP_ACTION_SD_STEP;
	action.sd_request = nullptr;
	action.sd_operation = AIUS_HTTP_SD_OP_ADVANCE;
	while (guard < 60u && event.sd_kind != AIUS_HTTP_SD_COMPLETE &&
	       event.sd_kind != AIUS_HTTP_SD_FAILED) {
		if (event.sd_kind == AIUS_HTTP_SD_NEED_UPLOAD) {
			action.sd_operation = AIUS_HTTP_SD_OP_SUPPLY_UPLOAD;
			action.data = payload;
			action.length = sizeof(payload);
		} else {
			action.sd_operation = AIUS_HTTP_SD_OP_ADVANCE;
			action.data = nullptr;
			action.length = 0u;
		}
		action.action_id = action_id++;
		storage->http_event_present = false;
		g_sd.primitive_calls_epoch = 0u;
		CHECK(aius_runtime_storage_service(storage, budget, (*now_ms)++,
		                                   &action, &event));
		CHECK(g_sd.primitive_calls_epoch <= 1u);
		guard++;
	}
	return event.sd_kind;
}

/* Two different causes reach the client as one 500 sd_failure: a job the card
 * failed, and an action the contention rule refused because a root job owned
 * the volume. Nothing on the wire tells them apart, and a 1-in-10 intermittent
 * cannot be diagnosed by re-running it. These cases assert that the latched
 * record does tell them apart, and name the reading that convicts each. */
static void test_storage_diagnostics(void)
{
	static const char *const keys[] = {
		"diag.clean",         "diag.busy-refusal",
		"diag.card-refuses-write", "diag.mount-failure",
		"diag.json-shape",    "diag.upload-progress",
		"diag.json-capacity",  "diag.null-guards"
	};
	const AiusRuntimeBudget *budget =
		aius_runtime_budget(AIUS_BOOT_CHECKPOINT_SD);
	AiusRuntimeStorage storage;
	AiusSdJobRequest request;
	AiusHttpEvent event;
	char json[2048];
	size_t length;
	uint32_t now_ms = 1u;

	validate_unique_keys("registry.diag", keys, ARRAY_COUNT(keys), 8u);

	current_key = "diag.clean";
	{
		/* A healthy run leaves no failure record at all, so "absent"
		 * is distinguishable from "zeroed". */
		arm_root_storage(&storage, 0u, true);
		CHECK(storage.diag.last_failure.sequence == 0u);
		CHECK(storage.diag.root_jobs == 0u);
		memset(&request, 0, sizeof(request));
		request.kind = AIUS_SD_JOB_PROBE;
		now_ms = 1u;
		CHECK(aius_runtime_storage_start_root_job(&storage, &request,
		                                          now_ms++));
		CHECK(storage.diag.root_jobs == 1u);
		drive_root_job(&storage, &now_ms, 32u);
		CHECK(storage.root_state ==
		      AIUS_RUNTIME_STORAGE_ROOT_COMPLETE);
		CHECK(storage.diag.root_failures == 0u);
		CHECK(storage.diag.busy_refusals == 0u);
		CHECK(storage.diag.last_failure.sequence == 0u);
		/* The probe mounted exactly once, its own. */
		CHECK(storage.target.mounts == 1u);
		CHECK(storage.target.driver_faults == 0u);
		CHECK(storage.target.protocol_faults == 0u);

		/* And a healthy upload after it, which REUSES the probe's mount
		 * rather than re-running SD.begin(). This assertion previously
		 * required two mounts, which recorded the defect: SD.begin() has
		 * no idempotence of its own and re-runs the whole card
		 * identification and volume mount, and roughly one in eleven of
		 * those blocked for SdFat's full one-second busy timeout and then
		 * faulted. */
		CHECK(run_http_upload(&storage, &now_ms) ==
		      AIUS_HTTP_SD_COMPLETE);
		CHECK(storage.diag.http_jobs == 1u);
		CHECK(storage.diag.http_failures == 0u);
		CHECK(storage.diag.last_failure.sequence == 0u);
		CHECK(storage.target.mounts == 1u);
		CHECK(storage.target.mount_reuses == 1u);
	}

	current_key = "diag.busy-refusal";
	{
		/* The contention hypothesis. A root PROBE owns the volume when
		 * an upload's SD_START arrives; the action is refused in-band
		 * and the client sees 500 sd_failure with no card involved.
		 *
		 * Convicting reading: busy_refusals nonzero, last_failure
		 * refused true, and its job_kind naming the ROOT job that was
		 * running -- PROBE (0) for the one-second lifecycle tick. */
		arm_root_storage(&storage, 0u, true);
		memset(&request, 0, sizeof(request));
		request.kind = AIUS_SD_JOB_PROBE;
		now_ms = 1u;
		CHECK(aius_runtime_storage_start_root_job(&storage, &request,
		                                          now_ms++));
		/* One primitive in, so the probe is genuinely mid-job. */
		CHECK(aius_runtime_storage_service(&storage, budget, now_ms++,
		                                   nullptr, &event));
		CHECK(storage.target.mounts == 1u);

		CHECK(run_http_upload(&storage, &now_ms) ==
		      AIUS_HTTP_SD_FAILED);
		CHECK(storage.diag.busy_refusals == 1u);
		CHECK(storage.diag.http_failures == 0u);
		CHECK(storage.diag.last_failure.sequence == 1u);
		CHECK(storage.diag.last_failure.refused);
		CHECK(storage.diag.last_failure.owner ==
		      AIUS_RUNTIME_STORAGE_OWNER_HTTP);
		CHECK(storage.diag.last_failure.job_kind ==
		      AIUS_SD_JOB_PROBE);
		CHECK(storage.diag.last_failure.root_state ==
		      AIUS_RUNTIME_STORAGE_ROOT_RUNNING);
		CHECK(storage.diag.last_failure.fault ==
		      AIUS_SD_FAULT_PROTOCOL);
		/* Nothing was asked of the card, so no card-side evidence
		 * exists -- which is exactly how this differs from a genuine
		 * write failure. */
		CHECK(storage.diag.last_failure.driver_fault == 0u);
		CHECK(storage.diag.last_failure.fault_phase ==
		      AIUS_SD_PHASE_NONE);
		CHECK(storage.target.driver_faults == 0u);
		CHECK(storage.target.protocol_faults == 0u);
		/* The refused action never started a job, so the HTTP job
		 * counter did not move and the probe kept the volume. */
		CHECK(storage.diag.http_jobs == 0u);
		CHECK(storage.root_state ==
		      AIUS_RUNTIME_STORAGE_ROOT_RUNNING);
	}

	current_key = "diag.card-refuses-write";
	{
		/* The card hypothesis, same 500 on the wire. Convicting
		 * reading: refused false, fault_phase UPLOAD_WRITE, the root
		 * job idle, and mounts_in_job exactly one -- the job's own, so
		 * nothing remounted the volume underneath it. */
		arm_root_storage(&storage, 0u, true);
		g_sd.exists_result = true;
		g_sd.remove_ok = true;
		g_sd.write_fails = true;
		now_ms = 1u;
		CHECK(run_http_upload(&storage, &now_ms) ==
		      AIUS_HTTP_SD_FAILED);
		CHECK(storage.diag.busy_refusals == 0u);
		CHECK(storage.diag.http_failures == 1u);
		CHECK(storage.diag.http_jobs == 1u);
		CHECK(storage.diag.last_failure.sequence == 1u);
		CHECK(!storage.diag.last_failure.refused);
		CHECK(storage.diag.last_failure.owner ==
		      AIUS_RUNTIME_STORAGE_OWNER_HTTP);
		CHECK(storage.diag.last_failure.job_kind ==
		      AIUS_SD_JOB_UPLOAD);
		CHECK(storage.diag.last_failure.fault ==
		      AIUS_SD_FAULT_SHORT_IO);
		CHECK(storage.diag.last_failure.fault_phase ==
		      AIUS_SD_PHASE_UPLOAD_WRITE);
		CHECK(storage.diag.last_failure.root_state ==
		      AIUS_RUNTIME_STORAGE_ROOT_IDLE);
		CHECK(!storage.diag.last_failure.draining);
		CHECK(storage.diag.last_failure.mounts_in_job == 1u);
		CHECK(storage.diag.last_failure.transferred == 0u);
		/* The handle was released despite the failure. */
		CHECK(!g_sd.file_open);
	}

	current_key = "diag.mount-failure";
	{
		/* A card that will not mount at all: same terminal shape, but
		 * the phase is BEGIN and the adapter recorded the driver
		 * saying no. */
		arm_root_storage(&storage, 0u, true);
		g_sd.begin_ok = false;
		now_ms = 1u;
		CHECK(run_http_upload(&storage, &now_ms) ==
		      AIUS_HTTP_SD_FAILED);
		CHECK(storage.diag.last_failure.fault_phase ==
		      AIUS_SD_PHASE_BEGIN);
		CHECK(storage.diag.last_failure.driver_fault ==
		      AIUS_SD_TEENSY_DRIVER_FAULT);
		CHECK(storage.diag.last_failure.adapter_fault ==
		      AIUS_SD_TEENSY_DRIVER_FAULT);
		CHECK(storage.diag.last_failure.mounts_in_job == 1u);
		CHECK(storage.target.driver_faults == 1u);
		CHECK(storage.target.protocol_faults == 0u);
	}

	current_key = "diag.json-shape";
	{
		arm_root_storage(&storage, 0u, true);
		length = 0u;
		json[0] = '\0';
		CHECK(aius_runtime_storage_diag_json(&storage, nullptr, json,
		                                     sizeof(json), &length));
		CHECK(length == strlen(json));
		CHECK(strstr(json, ",\"sd_diag\":{") == json);
		/* No failure yet is served as null, never as a zeroed record. */
		CHECK(strstr(json, "\"last_failure\":null}") != nullptr);
		CHECK(strstr(json, "\"busy_refusals\":0") != nullptr);
		CHECK(strstr(json, "\"mounts\":0") != nullptr);
		/* Phase 4e keys: mode, the two non-fault classes, the driver's
		 * busy timeouts and the wait hook's own record. */
		CHECK(strstr(json, "\"sd_mode\":\"none\","
		                   "\"sd_mode_requested\":\"fifo\",\"open_misses\":0,"
		                   "\"absent_mounts\":0,\"busy_timeouts\":0,"
		                   "\"hook_calls\":") != nullptr);
		CHECK(strstr(json, "\"sd_window_nonyield_max_us\":") != nullptr);

		g_sd.windowed_primitives = 0u;
		g_sd.unwindowed_primitives = 0u;
		g_sd.write_fails = true;
		g_sd.exists_result = true;
		g_sd.remove_ok = true;
		now_ms = 1u;
		CHECK(run_http_upload(&storage, &now_ms) ==
		      AIUS_HTTP_SD_FAILED);
		length = 0u;
		json[0] = '\0';
		CHECK(aius_runtime_storage_diag_json(&storage, nullptr, json,
		                                     sizeof(json), &length));
		CHECK(length == strlen(json));
		CHECK(strstr(json, "\"last_failure\":{\"seq\":1,") != nullptr);
		/* Every card primitive of the job (mount included) ran inside the
		 * SD window, and the window closed after each. */
		CHECK(g_sd.windowed_primitives > 0u);
		CHECK(g_sd.unwindowed_primitives == 0u);
		CHECK(g_aius_producer_yield.window_depth == 0u);
		CHECK(strstr(json, "\"refused\":false") != nullptr);
		{
			/* The mode route's effect: the adapter records DMA and
			 * sd_diag serves it; the next mount passes it to begin. */
			char mode_json[2048];
			size_t mode_length = 0u;
			aius_sd_teensy_set_mode(&storage.target, true);
			mode_json[0] = '\0';
			CHECK(aius_runtime_storage_diag_json(
				&storage, nullptr, mode_json, sizeof(mode_json),
				&mode_length));
			/* No file open, so the mount drops: requested dma,
			 * nothing mounted until the next BEGIN. */
			CHECK(!storage.target.mounted);
			CHECK(strstr(mode_json, "\"sd_mode\":\"none\","
			                        "\"sd_mode_requested\":\"dma\"") !=
			      nullptr);
			/* A mounted volume reports its mode. */
			storage.target.mounted = true;
			mode_length = 0u;
			CHECK(aius_runtime_storage_diag_json(
				&storage, nullptr, mode_json, sizeof(mode_json),
				&mode_length));
			CHECK(strstr(mode_json, "\"sd_mode\":\"dma\","
			                        "\"sd_mode_requested\":\"dma\"") !=
			      nullptr);
			storage.target.mounted = false;
			aius_sd_teensy_set_mode(&storage.target, false);
		}
		CHECK(strstr(json, "\"mounts_in_job\":1") != nullptr);
		CHECK(strstr(json, "\"http_faults\":1") != nullptr);
		/* AIUS_SD_PHASE_UPLOAD_WRITE is the 18th phase; the wire
		 * carries the number, and this pins it so a reader's decode
		 * table cannot silently drift. */
		CHECK(AIUS_SD_PHASE_UPLOAD_WRITE == 18);
		CHECK(strstr(json, "\"fault_phase\":18") != nullptr);
		CHECK(json[length - 1u] == '}');
	}

	current_key = "diag.upload-progress";
	{
		/* Prerequisite 2 of docs/plans/aius-ota-upgrade.md: an upload is
		 * auditable WHILE it runs, not only after it fails. Both numbers
		 * were live and neither was on the wire.
		 *
		 * The transaction is faked rather than driven, because the point
		 * under test is that the serializer reads `upload_supplied` from
		 * the transaction the caller hands it -- the transaction's own
		 * accounting of that field is the http suite's job
		 * (test_upload_supplied_tracks_the_supplied_body). A distinctive
		 * value proves the field is read rather than defaulted. */
		AiusHttpTransaction http;

		arm_root_storage(&storage, 0u, true);
		g_sd.exists_result = true;
		g_sd.remove_ok = true;
		now_ms = 1u;
		memset(&http, 0, sizeof(http));

		/* Before the job: nothing supplied, nothing committed, and no
		 * total, so a reader cannot mistake an idle service for one
		 * mid-transfer. */
		length = 0u;
		json[0] = '\0';
		CHECK(aius_runtime_storage_diag_json(&storage, &http, json,
		                                     sizeof(json), &length));
		CHECK(strstr(json, "\"upload_supplied\":0,") != nullptr);
		CHECK(strstr(json, "\"job_transferred\":0,") != nullptr);
		CHECK(strstr(json, "\"job_total\":0,") != nullptr);

		CHECK(run_http_upload(&storage, &now_ms) ==
		      AIUS_HTTP_SD_COMPLETE);
		/* run_http_upload() pushes a 4-byte payload as one chunk, so a
		 * completed job has committed its whole requested total. */
		CHECK(storage.job.transferred == 4u);
		CHECK(storage.job.total == 4u);
		http.upload_supplied = 4u;
		length = 0u;
		json[0] = '\0';
		CHECK(aius_runtime_storage_diag_json(&storage, &http, json,
		                                     sizeof(json), &length));
		CHECK(strstr(json, "\"upload_supplied\":4,") != nullptr);
		CHECK(strstr(json, "\"job_transferred\":4,") != nullptr);
		CHECK(strstr(json, "\"job_total\":4,") != nullptr);

		/* The in-flight reading the pair exists to expose: bytes off the
		 * socket that the card has not taken yet. A mirror kept inside
		 * this service could not show it, because the service only sees
		 * the slice it was handed. */
		http.upload_supplied = 512u;
		length = 0u;
		json[0] = '\0';
		CHECK(aius_runtime_storage_diag_json(&storage, &http, json,
		                                     sizeof(json), &length));
		CHECK(strstr(json, "\"upload_supplied\":512,") != nullptr);
		CHECK(strstr(json, "\"job_transferred\":4,") != nullptr);

		/* uint64 all the way to the wire: a multi-megabyte image is the
		 * whole point, and a 32-bit truncation would wrap silently. */
		http.upload_supplied = UINT64_C(4294967296);
		length = 0u;
		json[0] = '\0';
		CHECK(aius_runtime_storage_diag_json(&storage, &http, json,
		                                     sizeof(json), &length));
		CHECK(strstr(json, "\"upload_supplied\":4294967296,") !=
		      nullptr);

		/* A null transaction is a truthful zero, not a refusal: the
		 * diagnostic must never fail the status body it rides in. */
		length = 0u;
		json[0] = '\0';
		CHECK(aius_runtime_storage_diag_json(&storage, nullptr, json,
		                                     sizeof(json), &length));
		CHECK(strstr(json, "\"upload_supplied\":0,") != nullptr);
		CHECK(strstr(json, "\"job_transferred\":4,") != nullptr);
	}

	current_key = "diag.json-capacity";
	{
		/* A body that cannot hold the object is refused, not truncated
		 * into invalid JSON. Every prefix length is exercised. */
		size_t full;

		length = 0u;
		CHECK(aius_runtime_storage_diag_json(&storage, nullptr, json,
		                                     sizeof(json), &length));
		full = length;
		/* The object joins a status body that already carries net_diag,
		 * inside an 8 KiB response. Bounding it here keeps a later
		 * field addition from discovering the ceiling on the board. */
		CHECK(full > 32u && full < 640u);
		for (size_t cap = 1u; cap <= full; ++cap) {
			length = 0u;
			CHECK(!aius_runtime_storage_diag_json(&storage, nullptr,
			                                      json, cap,
			                                      &length));
			CHECK(length < cap);
		}
		/* An already-full body refuses without writing. */
		length = sizeof(json);
		CHECK(!aius_runtime_storage_diag_json(&storage, nullptr, json,
		                                      sizeof(json), &length));
	}

	current_key = "diag.null-guards";
	{
		length = 0u;
		CHECK(!aius_runtime_storage_diag_json(nullptr, nullptr, json,
		                                      sizeof(json), &length));
		CHECK(!aius_runtime_storage_diag_json(&storage, nullptr, nullptr,
		                                      sizeof(json), &length));
		CHECK(!aius_runtime_storage_diag_json(&storage, nullptr, json,
		                                      sizeof(json), nullptr));
	}
}

/* ------------------------------------------------------------------ */
/* 8. teardown discards HTTP before resource cleanup                   */
/* ------------------------------------------------------------------ */

static void test_teardown_ordering(void)
{
	static const char *const keys[] = {
		"teardown.http-discarded-first", "teardown.storage-released",
		"teardown.recorder-lease-released"
	};
	uint32_t now_ms = 1u;
	static const char *const download_request =
		"GET /sd/www/data.bin HTTP/1.1\r\nHost: aius\r\n\r\n";
	static const char *const recorder_request =
		"GET /api/recorder/data?offset=0&length=8&generation=0 HTTP/1.1\r\n"
		"Host: aius\r\nX-Aius-SD-Auth: token\r\n\r\n";

	validate_unique_keys("registry.teardown", keys, ARRAY_COUNT(keys), 3u);

	current_key = "teardown.http-discarded-first";
	reset_world(false);
	bring_online(&now_ms);
	memcpy(g_sd.data, "0123456789", 10u);
	g_sd.data_length = 10u;
	g_net.client_pending = true;
	g_net.rx = (const uint8_t *)download_request;
	g_net.rx_length = strlen(download_request);
	g_net.rx_offset = 0u;
	/* Get a transaction in flight: an SD job started and action IDs issued. */
	drive_epochs(&now_ms, 8u);
	CHECK(g_runtime.network_http.http.next_action_id > 1u);

	g_net.link = AIUS_NETWORK_TEENSY_LINK_DOWN;
	CHECK(run_epoch(now_ms++)); /* poll -> RESOURCE_FAULT */
	CHECK(run_epoch(now_ms++)); /* step -> TEARDOWN action retained */
	CHECK(g_runtime.network_http.pending_network_action.kind ==
	      AIUS_NET_ACTION_TEARDOWN);
	CHECK(!g_net.cleanup_witnessed);
	CHECK(run_epoch(now_ms++)); /* apply TEARDOWN: discard, then cleanup */
	CHECK(g_net.cleanup_witnessed);
	/* The witness sampled the transaction at the first cleanup primitive.
	 * A freshly initialized transaction has issued no action IDs. */
	CHECK(g_net.http_action_id_at_cleanup == 1u);
	CHECK(g_runtime.network_http.http.next_action_id == 1u);
	CHECK(!g_runtime.network_http.http_event_present);

	current_key = "teardown.storage-released";
	CHECK(!g_runtime.storage.http_authority);
	CHECK(g_runtime.storage.cancelling);
	{
		unsigned guard = 0u;

		while (guard < 30u && !g_runtime.storage.terminal_seen) {
			CHECK(run_epoch(now_ms++));
			guard++;
		}
		CHECK(g_runtime.storage.terminal_seen);
		CHECK(!g_sd.file_open);
	}

	/* A recorder-data 200 borrows a pointer into a bank slot and takes that
	 * slot's read lease, released only by the response's completion hook. A
	 * teardown mid-stream reaches none of the hook's ordinary terminals --
	 * no send completes, nothing recycles, no close arrives -- so before
	 * aius_http_transaction_discard() the lease leaked for the rest of the
	 * boot, and the bank stopped recording once its round-robin reached the
	 * pinned slot. End-to-end through the real composition: the response is
	 * still draining when the link drops. */
	current_key = "teardown.recorder-lease-released";
	reset_world(false);
	now_ms = 1u;
	bring_online(&now_ms);
	g_app.recorder_read_ok = true;
	g_net.client_pending = true;
	g_net.rx = (const uint8_t *)recorder_request;
	g_net.rx_length = strlen(recorder_request);
	g_net.rx_offset = 0u;
	{
		unsigned guard = 0u;

		while (guard < 40u && g_app.recorder_acquires == 0u) {
			CHECK(run_epoch(now_ms++));
			guard++;
		}
	}
	CHECK(g_app.recorder_acquires == 1u);
	CHECK(g_app.recorder_releases == 0u);
	CHECK(g_runtime.app.recorder_lease_active);
	/* The acquire happens inside the DISPATCH; the transaction accepts the
	 * response on a later epoch. Drive to the point the borrowed body is
	 * actually on the wire. */
	{
		unsigned guard = 0u;

		while (guard < 40u &&
		       !g_runtime.network_http.http.response_streams_body) {
			CHECK(run_epoch(now_ms++));
			guard++;
		}
	}
	/* The borrow is genuinely mid-flight: the streamed body has not drained,
	 * so no ordinary terminal is anywhere near. */
	CHECK(g_runtime.network_http.http.response_streams_body);
	CHECK(g_runtime.network_http.http.stream_body_offset <
	      g_runtime.network_http.http.stream_body_length);
	CHECK(g_app.recorder_releases == 0u);

	g_net.link = AIUS_NETWORK_TEENSY_LINK_DOWN;
	{
		/* The teardown is applied when the adapter leaves the published
		 * generation; http_discarded itself is a one-epoch flag the runtime
		 * consumes, so it cannot be polled. */
		unsigned guard = 0u;

		while (guard < 20u && g_runtime.network_http.target.online) {
			CHECK(run_epoch(now_ms++));
			guard++;
		}
	}
	CHECK(!g_runtime.network_http.target.online);
	/* The transaction was thrown away mid-stream, so no ordinary terminal
	 * ran -- yet the borrow came back, exactly once. */
	CHECK(!g_runtime.network_http.http.response_streams_body);
	CHECK(g_app.recorder_releases == 1u);
	CHECK(!g_runtime.app.recorder_lease_active);
	/* Repaired at the teardown, so a later read finds no leak to clean up. */
	CHECK(g_runtime.app.recorder_lease_leaks == 0u);
}

/* ------------------------------------------------------------------ */
/* 9. epoch traces, validity, status                                   */
/* ------------------------------------------------------------------ */

static void test_epoch_traces(void)
{
	static const char *const keys[] = {
		"epoch.normal-trace",     "epoch.recovery-trace",
		"epoch.failure.0",        "epoch.failure.3",
		"epoch.failure.6",        "epoch.failure.7",
		"epoch.failure.audio",
		"epoch.no-partial-boot",  "epoch.completed-epochs",
		"epoch.invalid.app",      "epoch.invalid.cdc",
		"epoch.invalid.platform", "epoch.invalid.audio",
		"epoch.invalid.null",
		"epoch.status-snapshot"
	};
	AiusOpticalApp app;
	AiusCdcTeensyPrimitives cdc;
	AiusRuntimeTeensyPlatformOps platform;
	AiusRecoveryStatus status;
	uint32_t now_ms = 1u;

	validate_unique_keys("registry.epoch", keys, ARRAY_COUNT(keys), 15u);

	current_key = "epoch.normal-trace";
	reset_world(false);
	CHECK(run_epoch(now_ms++));
	/* The Phase B order (f7e770cf0): 0-7, AUDIO, SD, AUDIO, COMPLETE --
	 * the SD primitive sits alone in its inter-audio gap. */
	CHECK(g_platform.checkpoint_count == 12u);
	for (size_t i = 0; i < 8u; ++i)
		CHECK(g_platform.checkpoints[i] == (uint16_t)i);
	CHECK(g_platform.checkpoints[8] == AIUS_BOOT_CHECKPOINT_AUDIO);
	CHECK(g_platform.checkpoints[9] == AIUS_BOOT_CHECKPOINT_SD);
	CHECK(g_platform.checkpoints[10] == AIUS_BOOT_CHECKPOINT_AUDIO);
	CHECK(g_platform.checkpoints[11] ==
	      AIUS_BOOT_CHECKPOINT_EPOCH_COMPLETE);
	CHECK(g_platform.feeds == 1u);
	CHECK(g_platform.reset_capture_calls == 1u);
	CHECK(g_platform.metrics_calls == 1u);
	CHECK(g_platform.udp_calls == 1u);
	CHECK(g_platform.replay_calls == 1u);
	CHECK(g_platform.audio_calls == 2u);
	CHECK(g_runtime.last_checkpoint ==
	      AIUS_BOOT_CHECKPOINT_EPOCH_COMPLETE);
	CHECK(g_runtime.completed_epochs == 1u);

	current_key = "epoch.recovery-trace";
	reset_world(true);
	CHECK(run_epoch(now_ms++));
	CHECK(g_platform.checkpoint_count == 3u);
	CHECK(g_platform.checkpoints[0] == AIUS_BOOT_CHECKPOINT_USB_RX);
	CHECK(g_platform.checkpoints[1] == AIUS_BOOT_CHECKPOINT_USB_TX);
	CHECK(g_platform.checkpoints[2] ==
	      AIUS_BOOT_CHECKPOINT_EPOCH_COMPLETE);
	CHECK(g_platform.feeds == 1u);
	CHECK(g_platform.reset_capture_calls == 0u);
	CHECK(g_platform.metrics_calls == 0u);
	CHECK(g_platform.audio_calls == 0u);
	CHECK(g_net.stack_start_calls == 0u);
	CHECK(g_runtime.network_http.supervisor.state ==
	      AIUS_NET_STATE_RECOVERY_SAFE);

	current_key = "epoch.failure.0";
	reset_world(false);
	g_platform.reset_capture_ok = false;
	CHECK(!run_epoch(now_ms++));
	CHECK(g_platform.checkpoint_count == 1u);
	CHECK(g_platform.feeds == 0u);
	CHECK(g_runtime.completed_epochs == 0u);

	current_key = "epoch.no-partial-boot";
	CHECK(g_runtime.boot_status.boot_generation == 0u);

	current_key = "epoch.failure.3";
	reset_world(false);
	g_platform.metrics_ok = false;
	CHECK(!run_epoch(now_ms++));
	CHECK(g_platform.checkpoint_count == 4u);
	CHECK(g_platform.feeds == 0u);
	CHECK(g_platform.udp_calls == 0u);
	CHECK(g_runtime.boot_status.boot_generation == 11u);

	current_key = "epoch.failure.6";
	reset_world(false);
	g_platform.udp_ok = false;
	CHECK(!run_epoch(now_ms++));
	CHECK(g_platform.checkpoint_count == 7u);
	CHECK(g_platform.replay_calls == 0u);
	CHECK(g_platform.feeds == 0u);

	current_key = "epoch.failure.7";
	reset_world(false);
	g_platform.replay_ok = false;
	CHECK(!run_epoch(now_ms++));
	CHECK(g_platform.checkpoint_count == 8u);
	CHECK(g_platform.feeds == 0u);

	current_key = "epoch.failure.audio";
	reset_world(false);
	g_platform.audio_ok = false;
	CHECK(!run_epoch(now_ms++));
	CHECK(g_platform.checkpoint_count == 9u);
	CHECK(g_platform.checkpoints[8] == AIUS_BOOT_CHECKPOINT_AUDIO);
	CHECK(g_platform.audio_calls == 1u);
	CHECK(g_platform.feeds == 0u);

	current_key = "epoch.completed-epochs";
	reset_world(false);
	CHECK(run_epoch(now_ms++));
	CHECK(run_epoch(now_ms++));
	CHECK(g_runtime.completed_epochs == 2u);
	g_runtime.completed_epochs = UINT32_MAX;
	CHECK(run_epoch(now_ms++));
	CHECK(g_runtime.completed_epochs == UINT32_MAX);

	current_key = "epoch.invalid.app";
	app = app_table();
	cdc = cdc_primitives();
	platform = platform_ops();
	app.write_status = nullptr;
	aius_runtime_teensy_construct(&g_runtime, &app, &cdc, &platform, 0u,
	                             false);
	CHECK(!g_runtime.valid);
	CHECK(!aius_runtime_teensy_epoch(&g_runtime, 1u));

	current_key = "epoch.invalid.cdc";
	app = app_table();
	cdc = cdc_primitives();
	platform = platform_ops();
	cdc.write = nullptr;
	aius_runtime_teensy_construct(&g_runtime, &app, &cdc, &platform, 0u,
	                             false);
	CHECK(!g_runtime.valid);
	CHECK(!aius_runtime_teensy_epoch(&g_runtime, 1u));

	current_key = "epoch.invalid.platform";
	app = app_table();
	cdc = cdc_primitives();
	platform = platform_ops();
	platform.feed_watchdog = nullptr;
	aius_runtime_teensy_construct(&g_runtime, &app, &cdc, &platform, 0u,
	                             false);
	CHECK(!g_runtime.valid);

	current_key = "epoch.invalid.audio";
	platform = platform_ops();
	platform.audio = nullptr;
	aius_runtime_teensy_construct(&g_runtime, &app, &cdc, &platform, 0u,
	                             false);
	CHECK(!g_runtime.valid);
	CHECK(!aius_runtime_teensy_epoch(&g_runtime, 1u));

	platform = platform_ops();
	aius_runtime_teensy_construct(&g_runtime, &app, &cdc, nullptr, 0u,
	                             false);
	CHECK(!g_runtime.valid);
	aius_runtime_teensy_construct(&g_runtime, &app, nullptr, &platform, 0u,
	                             false);
	CHECK(!g_runtime.valid);
	aius_runtime_teensy_construct(&g_runtime, nullptr, &cdc, &platform, 0u,
	                             false);
	CHECK(!g_runtime.valid);

	current_key = "epoch.invalid.null";
	aius_runtime_teensy_construct(nullptr, &app, &cdc, &platform, 0u, false);
	CHECK(!aius_runtime_teensy_epoch(nullptr, 1u));
	aius_runtime_teensy_status(nullptr, &status);
	CHECK(status.last_checkpoint == 0u);
	aius_runtime_teensy_status(&g_runtime, nullptr);

	current_key = "epoch.status-snapshot";
	reset_world(false);
	now_ms = 1u;
	bring_online(&now_ms);
	{
		const unsigned before = g_net.link_calls + g_sd.primitive_calls +
			                g_cdc.read_calls;

		aius_runtime_teensy_status(&g_runtime, &status);
		CHECK(status.network_state == AIUS_NET_STATE_ONLINE);
		CHECK(!status.recovery_safe);
		CHECK(status.last_checkpoint ==
		      AIUS_BOOT_CHECKPOINT_EPOCH_COMPLETE);
		CHECK(status.network_generation == 1u);
		CHECK(status.boot_generation == 11u);
		CHECK(status.reset_source == 0x20u);
		CHECK(status.watchdog_early_resets == 1u);
		CHECK(status.loop_epoch == 7u);
		CHECK(status.network_restarts == 0u);
		/* Pure snapshot: no platform or adapter call. */
		CHECK(g_net.link_calls + g_sd.primitive_calls +
		          g_cdc.read_calls ==
		      before);
	}
}

int main(void)
{
	aius_producer_yield_configure(600u, AIUS_PRODUCER_YIELD_LATE_US_DEFAULT);
	test_cdc_adapter();
	test_service_budget_selectivity();
	test_usb_rx_quota();
	test_usb_tx();
	test_network_precedence();
	test_http_epoch();
	test_http_counters();
	test_storage_bridge();
	test_root_storage_job();
	test_storage_diagnostics();
	test_teardown_ordering();
	test_epoch_traces();
	printf("ok runtime-target oracle\n");
	return 0;
}
