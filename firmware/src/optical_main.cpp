/* Aius optical prototype bring-up target.
 *
 * setup() constructs the bounded runtime and loop() drives exactly one epoch of
 * it; every transport, SD and watchdog effect leaves through that runtime. What
 * remains here is the audio processor and its DSP model, the event ring, the
 * serializers behind the twenty AiusOpticalApp adapters, and the six platform
 * callbacks the runtime asks for.
 *
 * Slice S2b removed the legacy blocking network/SD path with the NativeEthernet
 * and FNET includes it needed, so src/aius_network_teensy.cpp is now the only
 * application-level owner of a vendor transport header (asserted as an exact
 * set by test/network-transport/test_vendor_source.py).
 */

#include <Arduino.h>
#include <new>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <AudioStream.h>
#include <SD.h>
#include <input_spdif3.h>
#include <output_spdif3.h>

extern "C" {
#include "aius_net.h"
	#include "aius_dsp.h"
	#include "aius_pipeline.h"
	#include "aius_http.h"
	#include "aius_usb_control.h"
	#include "aius_dsp_config.h"
}
#include "aius_replay.h"
#include "aius_optical_config.h"
#include "aius_auth.h"
#include "aius_config.h"
/* The portable dispatch seam, whose twenty callbacks this file implements. It
 * pulls in aius_sd.h (and, through aius_http_transaction.h, aius_io.h)
 * unconditionally; the legacy SD symbols that used to collide with it are
 * gone. */
#include "aius_optical_app.h"
/* The composition: the bounded runtime setup() constructs and loop() drives one
 * epoch of, the retained boot adapter that owns the ledger, the pinned 8 s
 * watchdog interval and the sole per-epoch feed, the fault adapter the
 * checkpoint callback stalls through in a fault image, the production `Serial`
 * primitive table the runtime's USB service consumes, and the network adapter
 * the UDP callback and the status surface both reach through. */
#include "aius_boot_teensy.h"
#include "aius_cdc_teensy.h"
#include "aius_fault_teensy.h"
#include "aius_network_teensy.h"
#include "aius_runtime_teensy.h"

#ifndef AIUS_BUILD_GIT_REV
#define AIUS_BUILD_GIT_REV "unknown"
#endif
#ifndef AIUS_BUILD_UTC
#define AIUS_BUILD_UTC "unknown"
#endif
/* build_metadata.py defines this for every PlatformIO image; the fallback keeps
 * the file compilable outside that build, exactly as the two stamps above do. A
 * build id change is what clears the retained early-reset count. */
#ifndef AIUS_BUILD_ID
#define AIUS_BUILD_ID 0u
#endif

/* The device MAC, reported at offset 16 of the UDP status payload. The network
 * adapter configures the identical address on the stack itself
 * (aius_network_teensy.cpp), so the reported and configured values still agree
 * after the legacy Ethernet.begin() caller was deleted. */
static byte g_mac[] = {0x02, 0x41, 0x49, 0x55, 0x01, 0x01};

/* The composed runtime, in OCRAM because aius_runtime_teensy.h requires it: the
 * object is roughly 35 kB, dominated by the HTTP request/response buffers
 * (13,312 + 8,192 B) and the model-sized application body buffer (12,288 B),
 * against a 24,412-byte DTCM free-for-locals zone. A DTCM instance could not
 * fit, and a stack one could not exist at all. aius_runtime_teensy_construct()
 * initializes every member it reads, so OCRAM's lack of startup zeroing is
 * harmless.
 *
 * Defined here rather than beside the rest of the slice S2a composition below
 * because slice S2b's status surface reads it: put_status(), http_status_json()
 * and app_write_usb_payload() all take their address, link state and DHCP state
 * from the embedded network adapter.
 *
 * Constructed into its own storage rather than declared as a plain object, to
 * keep a never-running destructor out of ITCM. AiusRuntimeTeensy embeds an SD
 * `File` (aius_sd_teensy.h:18), so its implicit destructor is non-trivial; a
 * namespace-scope object of that type makes GCC register an __aeabi_atexit
 * teardown, which odr-uses -- and therefore emits -- a 228-byte destructor in
 * ITCM. That teardown can never run: main() never returns on a Teensy and this
 * object lives for the life of the board. Placement-new default-initializes
 * exactly as the plain declaration did (File's constructor runs, the rest is
 * left to aius_runtime_teensy_construct(), which initializes every member it
 * reads) and registers nothing. */
DMAMEM static uint8_t g_runtime_storage[sizeof(AiusRuntimeTeensy)];
static_assert(alignof(AiusRuntimeTeensy) <= 32u,
	"DMAMEM aligns to 32 bytes; a stricter runtime alignment needs its own");
static AiusRuntimeTeensy &g_runtime =
	*::new (static_cast<void *>(g_runtime_storage)) AiusRuntimeTeensy;

DMAMEM static AiusReplayService g_replay_service;
static char g_replay_path[96];

/* ------------------------------------------------------------------ */
/* Replay refill -- Phase 14c                                          */
/*                                                                     */
/* The replay used to own an SD `File` and read it synchronously from   */
/* inside the refill call. Two things were wrong with that. It spent a  */
/* second blocking SD primitive in an epoch whose contract is one; and  */
/* -- worse -- every portable SD job begins at AIUS_SD_PHASE_BEGIN,     */
/* whose adapter action calls SD.begin(BUILTIN_SDCARD), so any config   */
/* PUT, download or probe re-mounted the volume underneath that open    */
/* handle. Refusing POST /api/replays/start was the interim answer.     */
/*                                                                     */
/* Both are structural now, not ordering. The replay's read position is */
/* a byte OFFSET held by the portable service, never a handle, and the  */
/* read is an AIUS_SD_JOB_REPLAY_REFILL job that opens and closes       */
/* inside itself. The job runs through the storage service's own        */
/* AiusSdTeensy adapter, which is therefore still the single owner of   */
/* the only card handle in the firmware: a foreign handle no longer     */
/* exists for a mount to happen underneath, and the adapter's file_open */
/* guard would reject a BEGIN that tried.                               */
/*                                                                     */
/* One primitive per epoch, total, is kept the same way the SD          */
/* lifecycle keeps it: this job starts only when the storage service is */
/* completely idle (sd_storage_idle()), sd_job_active() reports it so   */
/* the dispatcher keeps SD routes off the card while it runs, and       */
/* sd_lifecycle_service() yields to it for the same reason. A refusal   */
/* is free -- the plan is abandoned and re-made next epoch.             */
/*                                                                     */
/* Every definition below carries FLASHMEM on the definition itself:    */
/* the driver runs once per epoch, never per audio block.               */
/* ------------------------------------------------------------------ */

/* The refill job and its own transfer slice. The storage service's buffer
 * belongs to the storage service's job; sharing it would couple two jobs that
 * are deliberately only serialized, not merged. DMAMEM for the same reason the
 * runtime is there: ~1.2 kB has no business in the 22 kB DTCM locals zone. */
DMAMEM static AiusSdJob g_replay_job;
DMAMEM static uint8_t g_replay_transfer[AIUS_RUNTIME_STORAGE_TRANSFER];
/* The slice is what one AIUS_SD_ACTION_OUTPUT carries, and the replay service
 * decodes a delivery in one batch out of a fixed scratch array. Keep the two
 * bounds in lockstep at compile time rather than discovering a widened transfer
 * slice as a run-time NO_SPACE that faults the refill. */
static_assert(AIUS_RUNTIME_STORAGE_TRANSFER <= AIUS_REPLAY_DELIVER_MAX_BYTES,
	"a refill chunk must fit the replay service's delivery bound");
/* The adapter result awaiting delivery to the job, exactly as the storage
 * service retains its own: one primitive per epoch means the event produced by
 * this epoch's action is consumed by the next epoch's step. */
static AiusSdEvent g_replay_job_event;
static bool g_replay_job_event_present;
static bool g_replay_job_active;
/* The job outlived the selection that asked for it (a new start, or a stop), so
 * its remaining chunks are acknowledged and dropped and its terminal state is
 * not reported to the service. */
static bool g_replay_job_cancelling;
static bool g_replay_job_size_published;

/* Reported when the service refuses a delivered chunk, which can only mean the
 * job and the service disagree about the read position. Distinct from the
 * adapter's own faults so the two are told apart in a job result. */
#define AIUS_REPLAY_DELIVER_REJECT_FAULT UINT32_C(0x52504C59)

/* Abandons an in-flight refill without waiting for it. Memory only, so it is
 * legal from the dispatcher: the job drains through its own CLOSE over the next
 * epochs while the service already belongs to the new selection. */
static FLASHMEM void replay_job_cancel(void)
{
	if (!g_replay_job_active)
		return;
	g_replay_job_cancelling = true;
	aius_sd_job_cancel(&g_replay_job);
}

/* Entropy for the temporary and backup file names the dispatcher builds, mixed
 * with millis() and the boot epoch by sd_job_id(). Outlived the legacy SD job
 * it was named for; app_upload_id() is its only reader. */
static uint32_t g_sd_job_sequence;
static float g_cpu_temp_c;
static float g_cpu_temp_max_c;
static uint32_t g_cpu_temp_sample_ms;
static uint32_t g_cpu_temp_next_ms;
static bool g_cpu_temp_valid;
/* Media state and usage, published by slice S3b's root-initiated PROBE and
 * USAGE_REFRESH jobs (see §SD lifecycle below). g_sd_ready gates every SD route
 * exactly as it did before the cutover; the usage pair reads zero whenever the
 * card is absent. */
static bool g_sd_ready;
/* The volume type the last PROBE read off the mounted card, carried on that
 * job's AiusSdResult. UNKNOWN until a probe answers, and again whenever the card
 * goes away, so it can never outlive the media it describes. */
static AiusSdFilesystem g_sd_filesystem;
static uint64_t g_sd_total_bytes;
static uint64_t g_sd_used_bytes;
/* A committed write invalidates the reported usage, so the next probe tick
 * follows it with a USAGE_REFRESH. The legacy job set the same flag from its
 * upload-cleanup and delete arms. */
static bool g_sd_usage_refresh_pending;
static uint32_t g_boot_epoch;
static AudioInputSPDIF3 g_spdif_in;
static AiusConfig g_config;
/* Staged by app_store_public_config() while the dispatcher's SD job writes the
 * accepted bytes, and copied into g_config once that write commits. The legacy
 * commit arm lived inside service_sd_job(); slice S3b re-attached it to the
 * composed storage service's terminal result (sd_settle_http_write()). The
 * staging shape -- fully written, then gated -- is unchanged. */
static AiusConfig g_pending_config;
static bool g_config_upload_pending;
static uint32_t g_config_error_count;

/* Which staged commit the current HTTP-owned write carries. The legacy job
 * carried `config_upload` / `dsp_upload` booleans on itself; the portable
 * AiusSdJob has no application fields, so the same fact lives here and is
 * matched against the job's target path when its terminal result arrives. */
typedef enum {
	SD_COMMIT_NONE,
	SD_COMMIT_PUBLIC_CONFIG,
	SD_COMMIT_DSP_MODEL
} SdCommitTarget;
static SdCommitTarget g_sd_commit_target;
/* The armed write has been observed HTTP-owned and non-terminal at least once,
 * so a terminal state may now be attributed to it. Without this a commit armed
 * at checkpoint 5 would settle in the same epoch against the PREVIOUS upload's
 * still-retained DONE -- applying a model whose bytes had not been written. */
static bool g_sd_commit_writing;
/* Any HTTP-owned job has been observed in flight, so its terminal state is a
 * transition rather than a leftover. Drives the usage-refresh arming for every
 * write, which is what the legacy job did per arm. */
static bool g_sd_http_job_seen;

/* The effective DSP model (source == "default" | "sd") and its runtime-reported
 * generation counter. g_active_model mirrors AIUS_OPTICAL_CONFIG's shape so the
 * GET serializer and apply_config() work identically whether the source is the
 * const factory default or a parsed SD override. Defaults to a plain copy of
 * AIUS_OPTICAL_CONFIG (a compile-time-constant aggregate, safe to read at static
 * init). g_config_generation is a per-boot-session counter: 0 at boot for the
 * default, incremented every time ANY new model (initial SD load, hot-reinsert
 * reload, or a PUT) is applied at an audio block boundary -- it is NOT persisted
 * across reboots (the SD file's own "config_generation" field is client-supplied
 * and untrusted; only "base_generation" on a PUT is a trusted concurrency
 * token). */
static FLASHMEM AiusOpticalConfig make_default_active_model(void)
{
	return AIUS_OPTICAL_CONFIG;
}
// Off the audio path (read/written only from apply_config()'s cold callers and
// the enqueue()-side block-boundary swap, never per-block), so DMAMEM (OCRAM)
// -- keeps this ~700-byte-each pair out of the tight shared
// FlexRAM/ITCM+DTCM+stack pool.
DMAMEM static AiusOpticalConfig g_active_model = make_default_active_model();
static bool g_dsp_source_sd;
static uint32_t g_config_generation;

/* Staged model awaiting a block-boundary-safe apply. Written from loop/HTTP
 * context (a validated PUT, or an SD (re)load); consumed once, at the top of
 * AiusAudioProcessor::enqueue() (audio-ISR context), which is the actual audio
 * block boundary. g_dsp_apply_pending is the single volatile handoff flag: the
 * writer always fully populates g_pending_dsp_model BEFORE setting it, so the
 * ISR never observes a partially-written model (see docs plan "Input trim/EQ +
 * limiter wiring done" note for the equivalent apply_config() out-of-line
 * rationale). g_dsp_upload_pending is the separate SD-job-lifecycle gate
 * (mirrors g_config_upload_pending): true only while a dsp_upload SD job is in
 * flight for the currently staged model, so a failed or cancelled job never
 * arms g_dsp_apply_pending. As with the public config above, the arm on commit
 * went with the legacy job and slice S3 restores it. DMAMEM for the same reason
 * as g_active_model: written from epoch/HTTP context, read once (a plain struct
 * copy) at the block boundary, never touched per-block. */
DMAMEM static AiusOpticalConfig g_pending_dsp_model;
static bool g_dsp_upload_pending;
static volatile bool g_dsp_apply_pending;

/* The coherent meter snapshot the audio path publishes and GET /api/meters
 * serves (Phase 14b). aius_dsp.h owns the whole protocol; this file only wires
 * it.
 *
 * OCRAM, exactly as the AiusMeter contract asks ("on target the adapter places
 * this in OCRAM"). At ~1.4 kB it is far too big to spend on the 23 kB DTCM
 * free-for-locals zone when OCRAM has 416 kB spare -- §Deciding principle's
 * "memory that is genuinely free is not an argument" points the same way.
 * DMAMEM is not zeroed at startup, so apply_config() runs aius_meter_init()
 * before the first block ever reaches enqueue().
 *
 * ONE WRITER, ONE READER, NO LOCK. The writer is AiusAudioProcessor::enqueue()
 * in audio-ISR context: it owns every accumulator, both seqlock increments and
 * the published snapshot. The reader is http_meters_json() on the main thread
 * and only ever calls aius_meter_read(). The reader never makes the audio path
 * wait -- the writer takes no lock, tests no reader state and never retries;
 * retry belongs entirely to the reader's side of the seqlock.
 *
 * WHY THAT CANNOT TEAR HERE. The two sides are not concurrent, they are
 * nested: the audio interrupt preempts the main thread, runs a publish to
 * completion and returns. A reader whose 676-byte copy is straddled by a
 * publish therefore always observes seq advanced by two across the copy, fails
 * the s1 == s2 check, and retries; it can never observe the payload half-old.
 * The converse hazard -- a compiler sinking one of the writer's payload stores
 * past the closing `seq++` -- is harmless for the same reason: the sunk store
 * still retires before the exception return, so no reader can run between the
 * two. aius_meter_read() lives in another translation unit and there is no LTO
 * in this build, so its bracketing volatile reads cannot be reordered around
 * the copy from the caller's side either. */
DMAMEM static AiusMeter g_meter;

/* Publish cadence, in publishes per second. Counted in FRAMES rather than
 * blocks so the decoder's framing cannot move it: a 128-frame PCM block and a
 * 512-frame decoded block yield the same 20 Hz. Chosen against the two
 * consumers, which both poll at 10 Hz -- every poll then sees a snapshot at
 * most one window old -- and against metering quality: RMS integrated over
 * ~50 ms is a usable meter, whereas a per-block window is 2.7 ms of noise
 * sampled at random by the client. It also keeps the one non-trivial cost in
 * the tap (aius_meter_publish's 8 sqrt + 128-point copy) off 18 blocks in
 * every 19 at 48 kHz with 128-frame blocks. */
static constexpr uint32_t AIUS_METER_PUBLISH_HZ = 20u;

static FLASHMEM void dsp_model_to_optical_config(const AiusDspModel &model, AiusOpticalConfig &out)
{
	out.sample_rate_hz = model.sample_rate_hz;
	out.features.input_eq = model.features.input_eq;
	out.features.matrix = model.features.matrix;
	out.features.output_eq = model.features.output_eq;
	out.features.limiter = model.features.limiter;
	out.features.meters = model.features.meters;
	for (size_t c = 0; c < AIUS_DSP_MODEL_INPUT_CHANNELS; c++) {
		out.input_channels[c].gain_db = model.input_channels[c].gain_db;
		out.input_channels[c].eq_count = model.input_channels[c].eq_count;
		for (size_t i = 0; i < AIUS_DSP_MODEL_EQ_MAX; i++) {
			out.input_channels[c].eq[i].type = model.input_channels[c].eq[i].type;
			out.input_channels[c].eq[i].frequency_hz = model.input_channels[c].eq[i].frequency_hz;
			out.input_channels[c].eq[i].q = model.input_channels[c].eq[i].q;
			out.input_channels[c].eq[i].gain_db = model.input_channels[c].eq[i].gain_db;
		}
	}
	out.output_eq_count = model.output_eq_count;
	for (size_t i = 0; i < AIUS_DSP_MODEL_EQ_MAX; i++) {
		out.output_eq[i].type = model.output_eq[i].type;
		out.output_eq[i].frequency_hz = model.output_eq[i].frequency_hz;
		out.output_eq[i].q = model.output_eq[i].q;
		out.output_eq[i].gain_db = model.output_eq[i].gain_db;
	}
	out.matrix.center_gain_linear = model.matrix.center_gain_linear;
	out.matrix.surround_gain_linear = model.matrix.surround_gain_linear;
	out.matrix.lfe_gain_linear = model.matrix.lfe_gain_linear;
	out.matrix.headroom_linear = model.matrix.headroom_linear;
	out.matrix.clipfree = model.matrix.clipfree;
	out.limiter.enabled = model.limiter.enabled;
	out.limiter.threshold_dbfs = model.limiter.threshold_dbfs;
	out.limiter.attack_ms = model.limiter.attack_ms;
	out.limiter.release_ms = model.limiter.release_ms;
	out.limiter.lookahead_ms = model.limiter.lookahead_ms;
}

/* The SD-hosted DSP override used to be read here, a byte at a time into a
 * 12,288-byte stack array, from setup() and again from service_sd(). Slice S2b
 * deleted that read; slice S3b restored the load as a root-initiated portable
 * CONFIG_LOAD job whose bytes reach platform_publish_config() below, parsed by
 * the same aius_dsp_config_parse_put() and staged for the same block-boundary
 * apply. Nothing on this path waits on the card and no 12 KB frame exists. */

static char g_auth_token[AIUS_AUTH_TOKEN_STORAGE_SIZE];

struct AiusEvent {
	uint32_t sequence;
	uint32_t uptime_ms;
	uint8_t type;
	uint8_t state;
	uint16_t detail;
};

static AiusEvent g_events[8];
static uint32_t g_event_sequence;
static uint32_t g_event_overflows;
static uint8_t g_last_state;
static bool g_test_running;
static volatile uint32_t g_network_drops;
static volatile uint32_t g_network_budget_overruns;

static FLASHMEM uint8_t current_state(void);

/* The legacy watchdog pair is gone. The pinned 8 s interval is armed once from
 * setup() through the boot adapter and fed exactly once per complete epoch by
 * the runtime's EPOCH_COMPLETE callback; this file writes no watchdog register
 * of its own, which the optical-cutover absence group asserts file-wide. */

static FLASHMEM void service_hw_metrics(void)
{
	uint32_t now = millis();
	if ((int32_t)(now - g_cpu_temp_next_ms) < 0)
		return;
	g_cpu_temp_next_ms = now + 1000u;
	float temperature = tempmonGetTemp();
	if (temperature < -40.0f || temperature > 150.0f)
		return;
	g_cpu_temp_c = temperature;
	if (!g_cpu_temp_valid || temperature > g_cpu_temp_max_c)
		g_cpu_temp_max_c = temperature;
	g_cpu_temp_sample_ms = now;
	g_cpu_temp_valid = true;
}

/* status JSON's `sd_filesystem`, reported from what the probe found.
 *
 * Slice S3b served this as "unknown" because AiusSdResult carried no FAT type,
 * and the only other answer was the legacy SD.sdfs.vol()->fatType() reach it
 * deleted: a raw vendor call from the HTTP serializer, outside the storage
 * service that owns every SD primitive, against a volume a root or HTTP job
 * could be mid-mount on. The result now carries the type instead, read by the
 * SD adapter inside the probe's own media check while it holds the mounted
 * volume, so this serializer reads memory and touches no card -- and the raw
 * call stays deleted.
 *
 * "none" is no card. "unknown" keeps exactly the meaning it always had here: a
 * mounted volume whose type the driver does not name, exFAT included. */
static FLASHMEM const char *sd_filesystem(void)
{
	if (!g_sd_ready)
		return "none";
	switch (g_sd_filesystem) {
	case AIUS_SD_FILESYSTEM_FAT12:
		return "fat12";
	case AIUS_SD_FILESYSTEM_FAT16:
		return "fat16";
	case AIUS_SD_FILESYSTEM_FAT32:
		return "fat32";
	case AIUS_SD_FILESYSTEM_UNKNOWN:
	default:
		return "unknown";
	}
}

/* Whether the composed storage service can accept a new job this epoch. The
 * legacy predicate read the deleted LegacySdJob state; the honest post-cutover
 * source is the service that now owns every SD primitive. An HTTP-owned job, a
 * drain of one whose owner went away, or a root-initiated job all mean the same
 * thing to a caller asking to start work: busy. Reads memory only.
 *
 * The dispatcher checks this before it admits a store, so the two store
 * adapters' own checks below are the second line of defence against a job that
 * starts between the dispatcher's check and theirs. */
/* Whether the card is spoken for. The replay refill job is included because it
 * holds the shared adapter's file open across the epochs it spans: while it
 * does, an SD route admitted at checkpoint 5 would reach checkpoint 8 and start
 * a job whose BEGIN the adapter must reject. A 503 sd_busy the client retries is
 * the coherent answer, and it is the answer this callback already gives for a
 * root-initiated lifecycle job. */
static FLASHMEM bool sd_job_active(void)
{
	const AiusRuntimeStorage &storage = g_runtime.storage;

	return storage.http_owned || storage.draining ||
	       storage.root_state == AIUS_RUNTIME_STORAGE_ROOT_RUNNING ||
	       g_replay_job_active;
}

static FLASHMEM uint32_t sd_job_id(void)
{
	return ++g_sd_job_sequence ^ millis() ^ g_boot_epoch;
}

static FLASHMEM void record_event(uint8_t type, uint8_t state, uint16_t detail)
{
	AiusEvent &event = g_events[g_event_sequence % 8u];
	if (g_event_sequence >= 8u)
		g_event_overflows++;
	event.sequence = ++g_event_sequence;
	event.uptime_ms = millis();
	event.type = type;
	event.state = state;
	event.detail = detail;
}

static FLASHMEM void record_state(void)
{
	uint8_t state = current_state();
	if (state == g_last_state)
		return;
	g_last_state = state;
	record_event(1, state, 0);
}

class AiusAudioProcessor : public AudioStream
{
public:
	AiusAudioProcessor(void) : AudioStream(2, g_input_queue) {}

	void begin_pipeline(void)
	{
		apply_config(AIUS_OPTICAL_CONFIG);
		if (aius_arena_init(&g_arena, g_arena_memory, sizeof(g_arena_memory)) != 0) {
			g_create_status = AIUS_NO_SPACE;
			return;
		}
		AiusPcmSink sink = {this, consume_block};
		AiusPipelineConfig config = {AIUS_IEC61937_PACK_16, 0};
		g_create_status = aius_pipeline_create_arena(&g_pipeline, &config,
		                                             &sink, &g_arena);
	}

	AiusPipelineClass classification(void) const
	{
		return g_pipeline ? aius_pipeline_classification(g_pipeline)
		                  : AIUS_PIPELINE_UNKNOWN;
	}

	uint32_t block_count(void) const
	{
		noInterrupts();
		uint32_t value = g_blocks;
		interrupts();
		return value;
	}
	uint32_t overrun_count(void) const
	{
		noInterrupts();
		uint32_t value = g_overruns;
		interrupts();
		return value;
	}
	AiusStatus create_status(void) const { return g_create_status; }
	void clear_counters(void)
	{
		noInterrupts();
		g_blocks = 0;
		g_overruns = 0;
		/* The peak-cycle watermark is a counter like the other two: a soak that
		 * clears counters wants the worst block SINCE the clear, not since
		 * boot. Inside the same critical section because its only other writer
		 * is the audio ISR. */
		g_dsp_cycles_max = 0;
		interrupts();
	}

	/* Resolve `cfg` (either AIUS_OPTICAL_CONFIG, the const factory default in
	 * FLASH, or a RAM-resident parsed SD override with the same field shape)
	 * into the runtime output-EQ biquads, downmix matrix, input-channel
	 * trim/EQ, and limiter. Cold path -- called at bring-up and, later, at a
	 * block-boundary config apply -- so it is FLASHMEM. The default config
	 * resolves to a bypass output EQ, the standard downmix, unity input trim
	 * with no EQ, and a disabled limiter, i.e. the audio path is bit-identical
	 * to the old flat-profile + standard-mix default.
	 *
	 * Defined out-of-line (below the class) rather than in-body: an in-body
	 * definition is implicitly inline (vague linkage / comdat), which conflicts
	 * at compile time with the plain FLASHMEM free functions already sharing the
	 * .flashmem section ("section type conflict") once the function is too large
	 * to fully inline away. Out-of-line gives it ordinary linkage instead. */
	FLASHMEM void apply_config(const AiusOpticalConfig &cfg);

protected:
	void update(void) override
	{
		audio_block_t *left = receiveReadOnly(0);
		audio_block_t *right = receiveReadOnly(1);
		if (g_replay_service.source == AIUS_REPLAY_SOURCE_REPLAY &&
		    g_pipeline != nullptr) {
			uint16_t words[AUDIO_BLOCK_SAMPLES * 2];
			size_t count = aius_replay_service_pop_words(&g_replay_service, words,
			                                             AUDIO_BLOCK_SAMPLES * 2);
			if (count == AUDIO_BLOCK_SAMPLES * 2) {
				AiusStatus st = aius_pipeline_push(g_pipeline, words, count);
				if (st != AIUS_OK) g_overruns++;
			} else {
				g_overruns++;
			}
		} else if (left != nullptr && right != nullptr && g_pipeline != nullptr) {
			uint16_t words[AUDIO_BLOCK_SAMPLES * 2];
			for (unsigned i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
				words[2 * i] = (uint16_t)left->data[i];
				words[2 * i + 1] = (uint16_t)right->data[i];
			}
			AiusStatus st = aius_pipeline_push(g_pipeline, words,
			                                   AUDIO_BLOCK_SAMPLES * 2);
			if (st != AIUS_OK)
				g_overruns++;
		}
		if (left != nullptr)
			release(left);
		if (right != nullptr)
			release(right);

		emit_block();
	}

private:
	static constexpr uint32_t RING_FRAMES = 2048;
	static constexpr size_t ARENA_BYTES = 128u * 1024u;
	audio_block_t *g_input_queue[2] = {nullptr, nullptr};
	static uint8_t g_arena_memory[ARENA_BYTES] __attribute__((aligned(16)));
	AiusArena g_arena;
	AiusPipeline *g_pipeline = nullptr;
	AiusStatus g_create_status = AIUS_INVALID_ARG;
	int32_t g_left[RING_FRAMES];
	int32_t g_right[RING_FRAMES];
	uint32_t g_read = 0;
	uint32_t g_write = 0;
	uint32_t g_count = 0;
	uint32_t g_blocks = 0;
	uint32_t g_overruns = 0;
	AiusDownmixConfig g_downmix = aius_downmix_config_default();
	AiusBiquad g_eq[2];
	AiusBiquadState g_eq_state[4] = {};
	size_t g_eq_count = 0;
	/* Input stage: per-channel trim + up to two EQ sections, wired from
	 * AIUS_OPTICAL_CONFIG.input_channels[]. The scratch planes are hot per-block
	 * staging buffers input_stage_apply() copies decoded planes into before
	 * trim/EQ (see aius_dsp.h); sized to the largest block enqueue() accepts
	 * (AUDIO_BLOCK_SAMPLES * 4 frames) across all AIUS_MAX_CHANNELS planes. */
	AiusInputStage g_input_stage = aius_input_stage_default();
	AiusBiquad g_input_eq[AIUS_DSP_INPUT_CHANNELS][AIUS_DSP_INPUT_EQ_MAX] = {};
	AiusBiquadState g_input_eq_state[AIUS_DSP_INPUT_CHANNELS][AIUS_DSP_INPUT_EQ_MAX] = {};
	static constexpr uint32_t INPUT_SCRATCH_FRAMES = AUDIO_BLOCK_SAMPLES * 4;
	int32_t g_input_scratch[AIUS_MAX_CHANNELS][INPUT_SCRATCH_FRAMES];
	bool g_input_stage_active = false;
	AiusLimiter g_limiter = {};
	bool g_limiter_active = false;
	/* Meter tap state, all resolved by apply_config() so the audio path never
	 * divides and never reads the config model. g_meters_active is the whole
	 * cost of the tap when features.meters is false: one bool test per block. */
	bool g_meters_active = false;
	uint32_t g_meter_window_frames = 0;    /* frames per publish (~1/20 s) */
	uint32_t g_meter_us_per_frame_q16 = 0; /* 1e6 / fs in Q16.16, hoisted */
	uint32_t g_dsp_cycles_last = 0;
	uint32_t g_dsp_cycles_max = 0;

	static AiusStatus consume_block(void *ctx, const AiusPcmBlock *block)
	{
		return static_cast<AiusAudioProcessor *>(ctx)->enqueue(block);
	}

	AiusStatus enqueue(const AiusPcmBlock *block)
	{
		/* Block-boundary config apply: the one point in the audio path where a
		 * staged model (a validated PUT or an SD (re)load) becomes active. Cheap
		 * in the common case (a single volatile bool read); apply_config() itself
		 * is bounded but not free (biquad coefficient trig), so this only runs
		 * once per staged change, never per block. */
		if (g_dsp_apply_pending) {
			g_active_model = g_pending_dsp_model;
			apply_config(g_active_model);
			g_dsp_source_sd = true;
			g_config_generation++;
			g_dsp_apply_pending = false;
		}
		if (block == nullptr || block->frames > AUDIO_BLOCK_SAMPLES * 4 ||
		    block->planes[0] == nullptr)
			return AIUS_INVALID_ARG;
		if (block->frames > RING_FRAMES - g_count)
			return AIUS_NO_SPACE;
		int32_t processed_left[AUDIO_BLOCK_SAMPLES * 4];
		int32_t processed_right[AUDIO_BLOCK_SAMPLES * 4];
		AiusPcmBlock processed = {};
		processed.planes[0] = processed_left;
		processed.planes[1] = processed_right;
		AiusDspConfig dsp = {};
		dsp.downmix = g_downmix;
		dsp.eq = g_eq;
		dsp.eq_sections = g_eq_count;
		dsp.eq_state = g_eq_state;
		dsp.input = g_input_stage_active ? &g_input_stage : nullptr;
		dsp.limiter = g_limiter_active ? &g_limiter : nullptr;
		/* Phase 14b meter tap, opened here and closed at the bottom of this
		 * function. Everything it does is gated on one bool, so a config with
		 * features.meters false costs exactly that test; nothing publishes,
		 * and the snapshot keeps its "never published" sequence of 0, which
		 * both consumers render as "no data".
		 *
		 * The DWT cycle counter brackets the whole per-block body -- DSP,
		 * accumulate, the ring write and any publish -- so the device reports
		 * its own worst-case block cost through the snapshot it is already
		 * publishing, rather than the figure being an off-target estimate. Two
		 * volatile PPB loads per block. */
		const bool metering = g_meters_active;
		const uint32_t cycles_begin = metering ? ARM_DWT_CYCCNT : 0u;
		AiusStatus dsp_status = aius_dsp_process(&dsp, block, &processed);
		if (dsp_status != AIUS_OK)
			return dsp_status;
		/* Both taps sit after a successful process so one window always covers
		 * the same set of blocks on both sides; a block the DSP rejected
		 * contributes to neither. Bounded and allocation-free by construction:
		 * every helper walks block->frames once (already capped at
		 * AUDIO_BLOCK_SAMPLES * 4 above) over caller-owned storage, and the
		 * waveform decimation writes at most AIUS_METER_WAVEFORM_MAX points. */
		if (metering) {
			aius_meter_accumulate_input(&g_meter, block);
			aius_meter_accumulate_output(&g_meter, processed_left,
			                             processed_right, block->frames);
		}
		for (uint32_t i = 0; i < block->frames; i++) {
			int32_t l = processed_left[i] >> 8;
			int32_t r = processed_right[i] >> 8;
			if (l > 32767) l = 32767;
			if (l < -32768) l = -32768;
			if (r > 32767) r = 32767;
			if (r < -32768) r = -32768;
			g_left[g_write] = l;
			g_right[g_write] = r;
			g_write = (g_write + 1) % RING_FRAMES;
		}
		g_count += block->frames;
		g_blocks++;
		if (metering) {
			if (g_meter.out_frames >= g_meter_window_frames) {
				/* Sampled once per window, not per block: the accessor takes a
				 * libm log10, and a diagnostic field does not buy a log10 in
				 * the block callback (§Deciding principle). The reported value
				 * is therefore the instantaneous gain reduction at the publish
				 * instant, not the window's peak. */
				aius_meter_set_limiter_gr(&g_meter,
					aius_dsp_limiter_gain_reduction_db(dsp.limiter));
				/* No division: microseconds per frame was resolved to Q16.16
				 * by apply_config(), and g_count <= RING_FRAMES, so this is one
				 * 32x32 multiply and a shift. */
				aius_meter_set_telemetry(&g_meter, g_count,
					(uint32_t)(((uint64_t)g_count *
					            g_meter_us_per_frame_q16) >> 16),
					g_dsp_cycles_last, g_dsp_cycles_max);
				aius_meter_publish(&g_meter);
			}
			const uint32_t cycles = ARM_DWT_CYCCNT - cycles_begin;
			g_dsp_cycles_last = cycles;
			if (cycles > g_dsp_cycles_max)
				g_dsp_cycles_max = cycles;
		}
		return AIUS_OK;
	}

	void emit_block(void)
	{
		audio_block_t *left = allocate();
		audio_block_t *right = allocate();
		if (left == nullptr || right == nullptr) {
			if (left != nullptr) release(left);
			if (right != nullptr) release(right);
			g_overruns++;
			return;
		}
		for (unsigned i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
			if (g_count != 0) {
				left->data[i] = (int16_t)g_left[g_read];
				right->data[i] = (int16_t)g_right[g_read];
				g_read = (g_read + 1) % RING_FRAMES;
				g_count--;
			} else {
				left->data[i] = 0;
				right->data[i] = 0;
			}
		}
		transmit(left, 0);
		transmit(right, 1);
		release(left);
		release(right);
	}
};

FLASHMEM void AiusAudioProcessor::apply_config(const AiusOpticalConfig &cfg)
{
	g_eq_count = cfg.output_eq_count;
	for (size_t i = 0; i < g_eq_count && i < 2; i++) {
		const AiusOpticalEqConfig &section = cfg.output_eq[i];
		if (section.type == 0)
			g_eq[i] = aius_biquad_bypass();
		else if (section.type == 1)
			g_eq[i] = aius_biquad_highpass(cfg.sample_rate_hz,
				section.frequency_hz, section.q);
		else
			g_eq[i] = aius_biquad_peaking(cfg.sample_rate_hz,
				section.frequency_hz, section.q, section.gain_db);
	}
	memset(g_eq_state, 0, sizeof(g_eq_state));
	g_downmix = aius_downmix_config_default();
	g_downmix.center = cfg.matrix.center_gain_linear;
	g_downmix.surround = cfg.matrix.surround_gain_linear;
	g_downmix.lfe = cfg.matrix.lfe_gain_linear;
	if (cfg.matrix.clipfree)
		g_downmix.headroom = aius_downmix_clipfree_headroom(&g_downmix);
	else
		g_downmix.headroom = cfg.matrix.headroom_linear;

	for (size_t c = 0; c < AIUS_DSP_INPUT_CHANNELS; c++) {
		const AiusOpticalInputChannelConfig &ic = cfg.input_channels[c];
		AiusInputChannelConfig &channel = g_input_stage.channels[c];
		channel.gain_linear = aius_db_to_linear(ic.gain_db);
		channel.eq_sections = ic.eq_count;
		for (size_t i = 0; i < ic.eq_count && i < AIUS_DSP_INPUT_EQ_MAX; i++) {
			const AiusOpticalEqConfig &section = ic.eq[i];
			if (section.type == 0)
				g_input_eq[c][i] = aius_biquad_bypass();
			else if (section.type == 1)
				g_input_eq[c][i] = aius_biquad_highpass(cfg.sample_rate_hz,
					section.frequency_hz, section.q);
			else
				g_input_eq[c][i] = aius_biquad_peaking(cfg.sample_rate_hz,
					section.frequency_hz, section.q, section.gain_db);
		}
		channel.eq = g_input_eq[c];
		channel.eq_state = g_input_eq_state[c];
	}
	memset(g_input_eq_state, 0, sizeof(g_input_eq_state));
	for (size_t p = 0; p < AIUS_MAX_CHANNELS; p++)
		g_input_stage.scratch[p] = g_input_scratch[p];
	g_input_stage_active = cfg.features.input_eq != 0;

	g_limiter.cfg.enabled = cfg.limiter.enabled;
	g_limiter.cfg.threshold_dbfs = cfg.limiter.threshold_dbfs;
	g_limiter.cfg.attack_ms = cfg.limiter.attack_ms;
	g_limiter.cfg.release_ms = cfg.limiter.release_ms;
	g_limiter.cfg.lookahead_ms = cfg.limiter.lookahead_ms;
	memset(&g_limiter.state, 0, sizeof(g_limiter.state));
	g_limiter_active = cfg.features.limiter != 0;

	/* Meter tap, resolved here for the same reason the biquads are: this is the
	 * one cold place that already knows the model, so the audio path reads
	 * plain scalars and never divides, never reads cfg and never branches on a
	 * feature struct.
	 *
	 * aius_meter_init() also re-zeroes the published snapshot, which is what
	 * makes the three interesting transitions honest. Turning metering off
	 * takes sequence back to 0 ("never published") instead of freezing a stale
	 * window on the wire; turning it on starts a fresh window; and a sample-rate
	 * change re-bases both the reported rate and the publish cadence. It is a
	 * ~1.4 kB memset, run at a block boundary alongside the biquad trig that
	 * already dominates this function, and never per block.
	 *
	 * A reader racing that memset is safe for the same reason a reader racing a
	 * publish is: seq changes value across the copy, so the s1 == s2 check
	 * fails and the reader retries. */
	const uint32_t rate = cfg.sample_rate_hz != 0u ? cfg.sample_rate_hz
	                                               : AIUS_OPTICAL_SAMPLE_RATE_HZ;
	g_meter_window_frames = rate / AIUS_METER_PUBLISH_HZ;
	if (g_meter_window_frames == 0u)
		g_meter_window_frames = 1u;
	g_meter_us_per_frame_q16 = (uint32_t)((1000000ull << 16) / rate);
	g_meters_active = cfg.features.meters != 0;
	aius_meter_init(&g_meter, rate);
}

uint8_t AiusAudioProcessor::g_arena_memory[AiusAudioProcessor::ARENA_BYTES]
	__attribute__((aligned(16)));

static AiusAudioProcessor g_audio_processor;
static AudioOutputSPDIF3 g_spdif_out;
static AudioConnection g_spdif_left(g_spdif_in, 0, g_audio_processor, 0);
static AudioConnection g_spdif_right(g_spdif_in, 1, g_audio_processor, 1);
static AudioConnection g_audio_left(g_audio_processor, 0, g_spdif_out, 0);
static AudioConnection g_audio_right(g_audio_processor, 1, g_spdif_out, 1);

static FLASHMEM uint8_t current_state(void)
{
	AiusPipelineClass klass = g_audio_processor.classification();
	return !AudioInputSPDIF3::pllLocked() ? 2 :
	       klass == AIUS_PIPELINE_DTS ? 4 :
	       klass == AIUS_PIPELINE_PCM ? 3 : 2;
}

/* Byte 1 of the UDP status payload, and the only producer of the status JSON's
 * `network_mode` -- the JSON renders this byte rather than deriving a second
 * answer, so the two channels cannot disagree by construction.
 *
 * The byte is binary, and not by choice: aius_net_status_encode() refuses a
 * legacy_status[1] above 1 (aius_net.c:112), so a third value would silently
 * cost every 99-byte reader its recovery fields. That suits the post-cutover
 * board exactly, because this plan removed static addressing: the only two
 * reachable states are "addressed by DHCP" and "not addressed at all".
 *
 * 1 requires both halves of that claim. AIUS_NET_STATE_ONLINE is the supervisor
 * state entered on LEASE_ACQUIRED (aius_net.c:226) and left on any resource
 * fault, so it means a DHCP lease is bound on the current generation; the
 * adapter's address read then confirms the lease actually produced an address.
 * Every other supervisor state -- stack start, DHCP wait, teardown, backoff and
 * recovery-safe -- has no lease, and reports 0.
 *
 * Slice S2b served this as 0 because its legacy producer was retired, which
 * rendered "offline" next to a real IP address on a board that was demonstrably
 * online. Both inputs here are memory reads of state the runtime already
 * publishes; neither costs an SD or network primitive. */
static FLASHMEM uint8_t network_mode_byte(bool address_usable)
{
	AiusRecoveryStatus recovery;

	aius_runtime_teensy_status(&g_runtime, &recovery);
	return recovery.network_state == AIUS_NET_STATE_ONLINE && address_usable
	               ? 1u
	               : 0u;
}

/* The 71-byte UDP status payload. Its length and every field offset are exactly
 * what they were before the cutover; only the producers of four fields moved,
 * and two of those had no producer left to move (§Accepted wire-behaviour
 * changes). Field by field:
 *
 *    0      version                     unchanged
 *    1      network mode                network_mode_byte(): 1 iff a DHCP
 *                                       lease is bound and addressed, else 0
 *    2      state                       unchanged (record_state)
 *    3      flags: pll lock, test run   unchanged
 *    4..7   boot epoch                  unchanged
 *    8..11  uptime ms                   unchanged
 *   12..15  IPv4 address                aius_network_teensy_address()
 *   16..21  MAC                         unchanged (g_mac)
 *   22      NTP validity                unchanged, still 0
 *   23      pipeline classification     unchanged
 *   24      pipeline create status      unchanged
 *   25..28  sample rate                 unchanged
 *   29..32  block count                 unchanged
 *   33..36  overrun count               unchanged
 *   37/38   retired mode/mix, reserved  unchanged, still 0
 *   39..54  event + network counters    unchanged
 *   55..70  four HTTP counters          producer retired -> served as 0
 *
 * Every zero-served field is left to the memset rather than written, so no
 * offset arithmetic changed with the producers. */
static FLASHMEM void put_status(uint8_t *payload, uint16_t *length)
{
	record_state();
	noInterrupts();
	uint32_t event_sequence = g_event_sequence;
	uint32_t event_overflows = g_event_overflows;
	uint32_t network_drops = g_network_drops;
	uint32_t network_budget_overruns = g_network_budget_overruns;
	interrupts();
	uint8_t address[4];
	const bool addressed = aius_network_teensy_address(
		&g_runtime.network_http.target, address);
	memset(payload, 0, 71);
	payload[0] = 1;
	payload[1] = network_mode_byte(addressed);
	AiusPipelineClass klass = g_audio_processor.classification();
	payload[2] = g_last_state;
	if (AudioInputSPDIF3::pllLocked())
		payload[3] |= 1u;
	aius_net_put_u32(payload + 4, g_boot_epoch);
	aius_net_put_u32(payload + 8, millis());
	memcpy(payload + 12, address, sizeof(address));
	memcpy(payload + 16, g_mac, sizeof(g_mac));
	payload[22] = 0; /* NTP validity: not implemented by the network adapter */
	if (g_test_running)
		payload[3] |= 2u;
	payload[23] = (uint8_t)klass;
	payload[24] = (uint8_t)g_audio_processor.create_status();
	aius_net_put_u32(payload + 25, AudioInputSPDIF3::sampleRate());
	aius_net_put_u32(payload + 29, g_audio_processor.block_count());
	aius_net_put_u32(payload + 33, g_audio_processor.overrun_count());
	aius_net_put_u32(payload + 39, event_sequence);
	aius_net_put_u32(payload + 43, event_overflows);
	aius_net_put_u32(payload + 47, network_drops);
	aius_net_put_u32(payload + 51, network_budget_overruns);
	*length = 71;
}

/* The 99-byte UDP status payload: put_status()'s 71 bytes byte-for-byte,
 * followed by the recovery fields the normative recovery contract makes
 * observable. The extension is produced by the portable encoder
 * (aius_net_status_encode, lib/aius_net), so the device and
 * tools/net/aius_net_runner.py share one definition of the layout rather than
 * two agreeing implementations:
 *
 *   71      network_state           AiusNetState, 0..5
 *   72      recovery_safe           0 or 1
 *   73      network_last_fault      AiusNetFault, 0..10
 *   74      last_checkpoint         0..9, the last checkpoint persisted
 *   75..78  network_generation      u32 LE
 *   79..82  boot_generation         u32 LE
 *   83..86  reset_source            u32 LE, the raw retained value
 *   87      watchdog_early_resets   u8
 *   88..90  reserved                zero, and the runner rejects non-zero
 *   91..94  loop_epoch              u32 LE, completed epochs
 *   95..98  network_restarts        u32 LE
 *
 * Bytes 0..70 are exactly what put_status() wrote, so this is an extension and
 * not a wire change: a 71-byte reader is unaffected by construction and the
 * runner accepts both lengths. The encoder rejects an out-of-domain enum,
 * checkpoint or mode rather than emitting it; that answer is the legacy
 * 71-byte payload, which is the shape every reader already handles, not a
 * dropped response. */
static FLASHMEM uint16_t put_recovery_status(uint8_t *payload)
{
	uint8_t legacy[71];
	uint16_t legacy_length = 0;
	AiusRecoveryStatus recovery;

	put_status(legacy, &legacy_length);
	aius_runtime_teensy_status(&g_runtime, &recovery);
	if (aius_net_status_encode(payload, AIUS_NET_MAX_PAYLOAD, legacy,
	                           &recovery) == 99u)
		return 99u;
	memcpy(payload, legacy, legacy_length);
	return legacy_length;
}

static FLASHMEM bool http_json_append(char *body, size_t capacity, size_t *length,
	                           const char *format, ...)
{
	if (*length >= capacity)
		return false;
	va_list arguments;
	va_start(arguments, format);
	int written = vsnprintf(body + *length, capacity - *length, format,
	                        arguments);
	va_end(arguments);
	if (written < 0 || (size_t)written >= capacity - *length)
		return false;
	*length += (size_t)written;
	return true;
}

static FLASHMEM const char *http_dsp_eq_type(uint8_t type)
{
	switch (type) {
	case 0: return "bypass";
	case 1: return "highpass";
	case 2: return "peaking";
	default: return nullptr;
	}
}

/* Serialize one bounded EQ table (`sections` of `count`) as a JSON array. Cold
 * path (config read), so FLASHMEM. Returns false on capacity exhaustion or an
 * unknown section type. */
static FLASHMEM bool http_dsp_eq_array(char *output, size_t capacity, size_t *length,
                                       const AiusOpticalEqConfig *sections, uint8_t count)
{
	if (!http_json_append(output, capacity, length, "["))
		return false;
	for (uint8_t index = 0; index < count && index < 2u; index++) {
		const char *type = http_dsp_eq_type(sections[index].type);
		if (type == nullptr || !http_json_append(output, capacity, length,
			"%s{\"type\":\"%s\",\"frequency_hz\":%.17g,\"q\":%.17g,\"gain_db\":%.17g}",
			index == 0 ? "" : ",", type, sections[index].frequency_hz,
			sections[index].q, sections[index].gain_db))
			return false;
	}
	return http_json_append(output, capacity, length, "]");
}

/* Serialize the single effective DSP config: g_active_model, kept in sync with
 * whatever is actually running (the const factory default at boot, or the last
 * model applied at an audio block boundary). Cold path, fixed buffer,
 * capacity-checked; returns 0 (deterministic error) if the model cannot fit the
 * response budget. `source`/`config_generation` are runtime-reported, not part
 * of the model struct itself. */
static FLASHMEM size_t http_dsp_config_json(uint8_t *body, size_t capacity)
{
	static const char *const channel_names[6] = {"fl", "fr", "fc", "lfe", "sl", "sr"};
	const AiusOpticalConfig &cfg = g_active_model;
	if (capacity > AIUS_OPTICAL_MODEL_MAX_JSON_BYTES)
		capacity = AIUS_OPTICAL_MODEL_MAX_JSON_BYTES;
	char *output = (char *)body;
	size_t length = 0;
	if (!http_json_append(output, capacity, &length,
	                      "{\"source\":\"%s\",\"config_generation\":%lu,\"sample_rate_hz\":%lu,",
	                      g_dsp_source_sd ? "sd" : "default",
	                      (unsigned long)g_config_generation,
	                      (unsigned long)cfg.sample_rate_hz))
		return 0;
	if (!http_json_append(output, capacity, &length,
	                      "\"features\":{\"input_eq\":%s,\"matrix\":%s,\"output_eq\":%s,\"limiter\":%s,\"meters\":%s},",
	                      cfg.features.input_eq ? "true" : "false",
	                      cfg.features.matrix ? "true" : "false",
	                      cfg.features.output_eq ? "true" : "false",
	                      cfg.features.limiter ? "true" : "false",
	                      cfg.features.meters ? "true" : "false"))
		return 0;
	if (!http_json_append(output, capacity, &length, "\"input_channels\":{"))
		return 0;
	for (uint8_t channel = 0; channel < 6u; channel++) {
		const AiusOpticalInputChannelConfig &entry = cfg.input_channels[channel];
		if (!http_json_append(output, capacity, &length,
		                      "%s\"%s\":{\"gain_db\":%.17g,\"eq\":",
		                      channel == 0 ? "" : ",", channel_names[channel],
		                      entry.gain_db))
			return 0;
		if (!http_dsp_eq_array(output, capacity, &length, entry.eq, entry.eq_count))
			return 0;
		if (!http_json_append(output, capacity, &length, "}"))
			return 0;
	}
	if (!http_json_append(output, capacity, &length, "},\"output_eq\":"))
		return 0;
	if (!http_dsp_eq_array(output, capacity, &length, cfg.output_eq, cfg.output_eq_count))
		return 0;
	AiusDownmixConfig resolved = aius_downmix_config_default();
	resolved.center = cfg.matrix.center_gain_linear;
	resolved.surround = cfg.matrix.surround_gain_linear;
	resolved.lfe = cfg.matrix.lfe_gain_linear;
	double headroom = cfg.matrix.clipfree ?
		aius_downmix_clipfree_headroom(&resolved) : cfg.matrix.headroom_linear;
	if (!http_json_append(output, capacity, &length,
	                      ",\"matrix\":{\"center_gain_linear\":%.17g,\"surround_gain_linear\":%.17g,\"lfe_gain_linear\":%.17g,\"headroom_linear\":%.17g,\"clipfree\":%s},",
	                      cfg.matrix.center_gain_linear, cfg.matrix.surround_gain_linear,
	                      cfg.matrix.lfe_gain_linear, headroom,
	                      cfg.matrix.clipfree ? "true" : "false"))
		return 0;
	if (!http_json_append(output, capacity, &length,
	                      "\"limiter\":{\"enabled\":%s,\"threshold_dbfs\":%.17g,\"attack_ms\":%.17g,\"release_ms\":%.17g,\"lookahead_ms\":%.17g}}",
	                      cfg.limiter.enabled ? "true" : "false",
	                      cfg.limiter.threshold_dbfs, cfg.limiter.attack_ms,
	                      cfg.limiter.release_ms, cfg.limiter.lookahead_ms))
		return 0;
	return length;
}

/* --- GET /api/meters ------------------------------------------------------
 *
 * The wire shape is settled and normative in
 * docs/plans/aius-dsp-config-api.md §Meters endpoint: dBFS levels floored at
 * -120.0, `input` as six positional entries in AIUS_CH_* order with no channel
 * key, `output` as two-element [L, R] arrays, `waveform` as per-mille integers
 * 0..1000 whose length is its own count, and sequence 0 served as an ordinary
 * 200 meaning "never published" rather than a status-code special case.
 *
 * All of it is cold FLASHMEM. The conversion the struct's header delegates to
 * the reader is done here, in the HTTP path, precisely because it is cold: the
 * ~16 log10 calls and 128 integer scalings per snapshot cost the audio budget
 * nothing, whereas log10(0) is -Infinity, which is not JSON, and silence is a
 * meter's resting state rather than an edge case. */

/* Silence, and anything quieter than this, reports exactly this value. Every
 * emitted level is therefore finite and no client has to reinvent the clamp. */
static constexpr double AIUS_METERS_FLOOR_DBFS = -120.0;

/* Waveform points are per-mille of full scale, so the largest point is this. */
static constexpr uint32_t AIUS_METERS_WAVEFORM_FULL_SCALE = 1000u;

/* Worst-case body, so the response is fixed-size by construction rather than
 * by hope. Allowing a generous 24 characters for every "%.1f" (the real range
 * is [-120.0, +48.2]: the floor below, and a magnitude bounded by int32 above):
 *
 *   envelope + sequence + sample_rate_hz                            60
 *   6 x {"peak_dbfs":<24>,"rms_dbfs":<24>} + separators            450
 *   "output" with four [L, R] entries                              138
 *   "limiter_gain_reduction_db"                                     53
 *   "waveform" with 128 x "1000," points                           652
 *   the four uint32 telemetry fields + close                       119
 *                                                                 ----
 *                                                                 1472
 *
 * Rounded up for the NUL and for arithmetic drift. Every append is still
 * capacity-checked, so exceeding it would be a deterministic 0 (-> 500), never
 * an overrun. */
static constexpr size_t AIUS_METERS_MAX_JSON_BYTES = 1600u;
static_assert(AIUS_METERS_MAX_JSON_BYTES <= AIUS_RUNTIME_TEENSY_APP_BODY,
	"the meters body must fit the runtime's application response buffer");

/* 20*log10(v / AIUS_DSP_SAMPLE_MAX), floored. The `!(v > 0)` test also takes
 * NaN to the floor, so no non-finite value can reach the wire. */
static FLASHMEM double meter_dbfs(double magnitude)
{
	if (!(magnitude > 0.0))
		return AIUS_METERS_FLOOR_DBFS;
	const double db = 20.0 * log10(magnitude / (double)AIUS_DSP_SAMPLE_MAX);
	return db < AIUS_METERS_FLOOR_DBFS ? AIUS_METERS_FLOOR_DBFS : db;
}

/* One envelope point as a per-mille integer of full scale, rounded to nearest
 * and clamped. aius_meter_decimate() emits magnitudes, so the negative arm is
 * defensive only. */
static FLASHMEM uint32_t meter_permille(int32_t point)
{
	const uint64_t magnitude = point < 0 ? (uint64_t)-(int64_t)point
	                                     : (uint64_t)point;
	const uint64_t scaled =
		(magnitude * AIUS_METERS_WAVEFORM_FULL_SCALE +
		 (uint64_t)(AIUS_DSP_SAMPLE_MAX / 2)) / (uint64_t)AIUS_DSP_SAMPLE_MAX;
	return scaled > AIUS_METERS_WAVEFORM_FULL_SCALE
		? AIUS_METERS_WAVEFORM_FULL_SCALE : (uint32_t)scaled;
}

/* Serialize the last complete snapshot. sequence == 0 is not special-cased: a
 * board that has never published serves an ordinary 200 whose levels are all
 * at the floor and whose waveform is empty, which is exactly what "no data"
 * means, and reuses the sentinel the struct already documents instead of
 * inventing a status-code convention.
 *
 * Returning 0 is reserved for the one genuine failure: aius_meter_read()
 * refusing to hand back a torn snapshot after AIUS_METER_READ_RETRIES
 * contended attempts. That is the dispatcher's ordinary "cannot serve" answer
 * (a deterministic 500), and it needs a publish to straddle eight consecutive
 * 676-byte copies to happen at all -- against a writer that publishes 20 times
 * a second and completes in about a microsecond. */
static FLASHMEM size_t http_meters_json(uint8_t *body, size_t capacity)
{
	AiusMeterSnapshot snapshot;
	if (!aius_meter_read(&g_meter, &snapshot))
		return 0;
	if (capacity > AIUS_METERS_MAX_JSON_BYTES)
		capacity = AIUS_METERS_MAX_JSON_BYTES;
	char *output = (char *)body;
	size_t length = 0;
	if (!http_json_append(output, capacity, &length,
	                      "{\"sequence\":%lu,\"sample_rate_hz\":%lu,\"input\":[",
	                      (unsigned long)snapshot.sequence,
	                      (unsigned long)snapshot.sample_rate_hz))
		return 0;
	/* Six positional entries in AIUS_CH_* order (fl, fr, fc, lfe, sl, sr).
	 * Position is the identity; there is deliberately no channel key. */
	for (uint32_t channel = 0; channel < AIUS_METER_INPUT_CHANNELS; channel++) {
		if (!http_json_append(output, capacity, &length,
		                      "%s{\"peak_dbfs\":%.1f,\"rms_dbfs\":%.1f}",
		                      channel == 0 ? "" : ",",
		                      meter_dbfs(snapshot.input_peak[channel]),
		                      meter_dbfs(snapshot.input_rms[channel])))
			return 0;
	}
	/* The stereo output keeps both channels as [L, R]; flattening to a scalar
	 * would discard information the pipeline already has.
	 * limiter_gain_reduction_db is already dB (<= 0) and passes through
	 * unconverted. */
	if (!http_json_append(output, capacity, &length,
	                      "],\"output\":{\"peak_dbfs\":[%.1f,%.1f],\"rms_dbfs\":[%.1f,%.1f]},"
	                      "\"limiter_gain_reduction_db\":%.1f,\"waveform\":[",
	                      meter_dbfs(snapshot.output_peak[0]),
	                      meter_dbfs(snapshot.output_peak[1]),
	                      meter_dbfs(snapshot.output_rms[0]),
	                      meter_dbfs(snapshot.output_rms[1]),
	                      snapshot.limiter_gain_reduction_db))
		return 0;
	uint32_t points = snapshot.waveform_count;
	if (points > AIUS_METER_WAVEFORM_MAX)
		points = AIUS_METER_WAVEFORM_MAX;
	for (uint32_t point = 0; point < points; point++) {
		if (!http_json_append(output, capacity, &length, "%s%lu",
		                      point == 0 ? "" : ",",
		                      (unsigned long)meter_permille(snapshot.waveform[point])))
			return 0;
	}
	return http_json_append(output, capacity, &length,
	                        "],\"queue_frames\":%lu,\"estimated_latency_us\":%lu,"
	                        "\"dsp_cycles_last\":%lu,\"dsp_cycles_max\":%lu}",
	                        (unsigned long)snapshot.queue_frames,
	                        (unsigned long)snapshot.estimated_latency_us,
	                        (unsigned long)snapshot.dsp_cycles_last,
	                        (unsigned long)snapshot.dsp_cycles_max)
		? length : 0;
}

/* One generation's network measurement record, as a nested JSON object. Purely
 * diagnostic: nothing here is a supervisor input, and the adapter fills every
 * field from calls it makes once per generation in the epoch's action path.
 *
 * `netif_init` is the field the whole record exists for -- 1 says this
 * generation performed the real fnet_netif_init(), 2 says it found the
 * interface already initialised and skipped it, 0 says the probe was
 * unavailable. `stack_start_blocking_ms` is how long beginNoWait() itself took,
 * which on the initialising generation includes FNET's autonegotiation wait.
 * `tx_frames` is the frame-level discriminator across the DHCP window: zero
 * means no DISCOVER ever reached the MAC.
 *
 * Absent means absent: a generation that never started DHCP serves
 * dhcp_start_ms as null rather than 0, so a reader cannot mistake "did not
 * happen" for "happened immediately". */
static FLASHMEM bool http_net_diag_generation(char *output, size_t capacity,
	                                          size_t *length, const char *key,
	                                          const AiusNetworkTeensyDiag *diag)
{
	if (diag == nullptr)
		return http_json_append(output, capacity, length, ",\"%s\":null", key);
	if (!http_json_append(output, capacity, length,
	                      ",\"%s\":{\"generation\":%lu,\"netif_init\":%u,"
	                      "\"stack_start_blocking_ms\":%u,"
	                      "\"link_at_stack_start\":%u,\"link_up_source\":%u,",
	                      key, (unsigned long)diag->generation,
	                      (unsigned)diag->netif_init,
	                      (unsigned)diag->stack_start_blocking_ms,
	                      (unsigned)diag->link_at_stack_start,
	                      (unsigned)diag->link_up_source))
		return false;
	if (!http_json_append(output, capacity, length, "\"link_up_ms\":"))
		return false;
	if (!(diag->link_up_source != AIUS_NETWORK_TEENSY_LINK_UP_NONE
		      ? http_json_append(output, capacity, length, "%lu",
		                         (unsigned long)diag->link_up_ms)
		      : http_json_append(output, capacity, length, "null")))
		return false;
	if (!http_json_append(output, capacity, length,
	                      ",\"link_callbacks\":%u,\"link_callback_up_ms\":",
	                      (unsigned)diag->link_callbacks))
		return false;
	if (!(diag->link_callback_up_seen
		      ? http_json_append(output, capacity, length, "%lu",
		                         (unsigned long)diag->link_callback_up_ms)
		      : http_json_append(output, capacity, length, "null")))
		return false;
	if (!http_json_append(output, capacity, length, ",\"dhcp_start_ms\":"))
		return false;
	if (!(diag->dhcp_started_seen
		      ? http_json_append(output, capacity, length, "%lu",
		                         (unsigned long)diag->dhcp_start_ms)
		      : http_json_append(output, capacity, length, "null")))
		return false;
	if (!http_json_append(output, capacity, length,
	                      ",\"dhcp_state_at_start\":%u,\"end_reason\":%u,",
	                      (unsigned)diag->dhcp_state_at_start,
	                      (unsigned)diag->end_reason))
		return false;
	if (!http_json_append(output, capacity, length, "\"end_ms\":"))
		return false;
	if (!(diag->end_reason != AIUS_NETWORK_TEENSY_END_NONE
		      ? http_json_append(output, capacity, length, "%lu",
		                         (unsigned long)diag->end_ms)
		      : http_json_append(output, capacity, length, "null")))
		return false;
	if (!http_json_append(output, capacity, length,
	                      ",\"dhcp_state_at_end\":%u,\"link_at_end\":%u,"
	                      "\"link_live_at_end\":%u,\"frames_valid\":%s,",
	                      (unsigned)diag->dhcp_state_at_end,
	                      (unsigned)diag->link_at_end,
	                      (unsigned)diag->link_live_at_end,
	                      diag->frames_valid ? "true" : "false"))
		return false;
	if (!http_json_append(output, capacity, length, "\"tx_frames\":"))
		return false;
	if (!(diag->frames_valid
		      ? http_json_append(output, capacity, length, "%lu",
		                         (unsigned long)diag->tx_frames)
		      : http_json_append(output, capacity, length, "null")))
		return false;
	if (!http_json_append(output, capacity, length, ",\"rx_frames\":"))
		return false;
	if (!(diag->frames_valid
		      ? http_json_append(output, capacity, length, "%lu",
		                         (unsigned long)diag->rx_frames)
		      : http_json_append(output, capacity, length, "null")))
		return false;
	return http_json_append(output, capacity, length, "}");
}

/* The `net_diag` object appended to GET /api/status: three live reads taken
 * once per request in HTTP context, then the first generation that ever ended
 * and the live one.
 *
 * `link` beside `link_live` is the comparison this exists to make -- the first
 * is NativeEthernet's cached member, the second asks the interface. The live
 * read costs at most one MDIO transaction per 200 ms, because FNET throttles
 * the PHY register read itself and its own link service already polls at that
 * rate; the DHCP-state and frame-counter reads cost a descriptor lookup and two
 * register reads respectively, and none of the three touches a socket, waits,
 * or runs anywhere near the audio path. */
static FLASHMEM bool http_net_diag_json(char *output, size_t capacity,
	                                    size_t *length,
	                                    const AiusNetworkTeensy *network)
{
	uint32_t tx = 0;
	uint32_t rx = 0;
	const bool frames = aius_network_teensy_frames(network, &tx, &rx);

	if (!http_json_append(output, capacity, length,
	                      ",\"net_diag\":{\"link\":%u,\"link_live\":%u,"
	                      "\"dhcp_state\":%u,\"frames_valid\":%s,"
	                      "\"tx_frames\":%lu,\"rx_frames\":%lu",
	                      (unsigned)aius_network_teensy_link(network),
	                      (unsigned)aius_network_teensy_link_live(network),
	                      (unsigned)aius_network_teensy_dhcp_state(network),
	                      frames ? "true" : "false",
	                      (unsigned long)tx, (unsigned long)rx))
		return false;
	if (!http_net_diag_generation(output, capacity, length, "first",
	                              aius_network_teensy_diag_first(network)))
		return false;
	if (!http_net_diag_generation(output, capacity, length, "current",
	                              aius_network_teensy_diag(network)))
		return false;
	return http_json_append(output, capacity, length, "}");
}

/* GET /api/status. Every field keeps its key, position and type; the sources
 * that moved at slice S2b are the network ones, and they are the only lines
 * that changed:
 *
 *   ip                    put_status()'s bytes 12..15, from the adapter
 *   link                  aius_network_teensy_link(), no longer gated on the
 *                         retired network_mode -- an adapter with a link but no
 *                         lease still reports the link honestly
 *   dhcp_pending          aius_network_teensy_dhcp_pending()
 *   dhcp_updates          the adapter's own lease counter
 *   network_restarts      AiusRecoveryStatus
 *   reset_source          AiusRecoveryStatus, now the raw value rather than
 *                         the legacy low byte
 *   network_mode          the payload's own byte 1, so the JSON string and the
 *                         UDP byte are one value with two renderings. The
 *                         vocabulary is two words, not three: "static" was
 *                         reachable only through the static-addressing fallback
 *                         this plan removed, and the UDP encoder rejects any
 *                         byte above 1 anyway. "dhcp" means a bound lease with
 *                         an address; "offline" means no lease -- which is what
 *                         a reader wants from the field it reads first, and is
 *                         corroborated by ip, link and dhcp_pending beside it
 *   dhcp_discovers        no producer without a vendor patch -> 0
 *   http_server_rebinds   producer retired -> 0
 *   the four http_* counters   producer retired -> read back as 0 from the
 *                         payload's own bytes 55..70, so JSON and UDP cannot
 *                         disagree about what is being served
 *
 * The eight recovery keys appended after http_service_overruns complete
 * AiusRecoveryStatus on this channel: network_restarts and reset_source were
 * already served above, and network_state, recovery_safe, network_last_fault,
 * last_checkpoint, network_generation, boot_generation, watchdog_early_resets
 * and loop_epoch join them. They come from the same one aius_runtime_teensy_status()
 * snapshot the UDP payload encodes, so the two channels cannot disagree, and
 * they are appended rather than interleaved so no existing key moves. Every
 * value is an unsigned integer with the same domain the UDP encoder validates;
 * recovery_safe is served as 0/1 like the other booleans on this route. The
 * body grows by at most ~200 bytes against a 12,288-byte buffer
 * (AIUS_RUNTIME_TEENSY_APP_BODY), and the capacity check below is unchanged:
 * a body that does not fit is 0, which the dispatcher turns into a 500. */
static FLASHMEM size_t http_status_json(uint8_t *body, size_t capacity)
{
	uint8_t status[71];
	uint16_t status_length = 0;
	put_status(status, &status_length);
	if (capacity == 0 || status_length < 71)
		return 0;
	AiusRecoveryStatus recovery;
	aius_runtime_teensy_status(&g_runtime, &recovery);
	const AiusNetworkTeensy *network = &g_runtime.network_http.target;
	const char *network_mode = status[1] == 1u ? "dhcp" : "offline";
	int written = snprintf((char *)body, capacity,
		"{\"version\":%u,\"state\":%u,\"carrier\":%u,\"format\":%u,\"sample_rate\":%lu,\"blocks\":%lu,\"overruns\":%lu,\"source\":\"default\",\"sd_present\":%u,\"sd_filesystem\":\"%s\",\"sd_total_bytes\":%llu,\"sd_used_bytes\":%llu,\"sd_free_bytes\":%llu,\"config_valid\":%u,\"config_error_code\":%u,\"config_error\":\"%s\",\"config_error_count\":%lu,\"network_mode\":\"%s\",\"ip\":\"%u.%u.%u.%u\",\"link\":%u,\"network_restarts\":%lu,\"dhcp_pending\":%u,\"dhcp_discovers\":%lu,\"dhcp_updates\":%lu,\"reset_source\":%lu,\"http_server_rebinds\":%lu,\"cpu_temp_c\":%.2f,\"cpu_temp_valid\":%u,\"cpu_temp_max_c\":%.2f,\"cpu_temp_age_ms\":%lu,\"build_revision\":\"%s\",\"build_utc\":\"%s\",\"event_sequence\":%lu,\"event_overflows\":%lu,\"network_drops\":%lu,\"network_budget_overruns\":%lu,\"http_requests\":%lu,\"http_rejections\":%lu,\"http_client_drops\":%lu,\"http_service_overruns\":%lu,\"network_state\":%u,\"recovery_safe\":%u,\"network_last_fault\":%u,\"last_checkpoint\":%u,\"network_generation\":%lu,\"boot_generation\":%lu,\"watchdog_early_resets\":%u,\"loop_epoch\":%lu}",
		status[0], status[2], status[3] & 1u, status[23],
		(unsigned long)aius_net_get_u32(status + 25),
		(unsigned long)aius_net_get_u32(status + 29),
		(unsigned long)aius_net_get_u32(status + 33),
		g_sd_ready ? 1u : 0u, sd_filesystem(),
		(unsigned long long)g_sd_total_bytes,
		(unsigned long long)g_sd_used_bytes,
		(unsigned long long)(g_sd_total_bytes - g_sd_used_bytes),
		g_config.valid, (unsigned)g_config.error_code, g_config.error_message,
		(unsigned long)g_config_error_count,
		network_mode, status[12], status[13], status[14], status[15],
		aius_network_teensy_link(network) ==
			AIUS_NETWORK_TEENSY_LINK_UP ? 1u : 0u,
		(unsigned long)recovery.network_restarts,
		aius_network_teensy_dhcp_pending(network) ? 1u : 0u,
		0ul,
		(unsigned long)network->dhcp_updates,
		(unsigned long)recovery.reset_source,
		0ul,
		(double)g_cpu_temp_c, g_cpu_temp_valid ? 1u : 0u,
		(double)g_cpu_temp_max_c,
		g_cpu_temp_valid ? (unsigned long)(millis() - g_cpu_temp_sample_ms) : 0ul,
		AIUS_BUILD_GIT_REV, AIUS_BUILD_UTC,
		(unsigned long)aius_net_get_u32(status + 39),
		(unsigned long)aius_net_get_u32(status + 43),
		(unsigned long)aius_net_get_u32(status + 47),
		(unsigned long)aius_net_get_u32(status + 51),
		(unsigned long)aius_net_get_u32(status + 55),
		(unsigned long)aius_net_get_u32(status + 59),
		(unsigned long)aius_net_get_u32(status + 63),
		(unsigned long)aius_net_get_u32(status + 67),
		(unsigned)recovery.network_state,
		recovery.recovery_safe ? 1u : 0u,
		(unsigned)recovery.network_last_fault,
		(unsigned)recovery.last_checkpoint,
		(unsigned long)recovery.network_generation,
		(unsigned long)recovery.boot_generation,
		(unsigned)recovery.watchdog_early_resets,
		(unsigned long)recovery.loop_epoch);
	if (written <= 0 || (size_t)written >= capacity)
		return 0;
	/* The diagnostic object joins the same body rather than a second route:
	 * it is read beside link, ip and network_state, and a separate route would
	 * be a second snapshot of state that moves between requests. It is
	 * appended by rewinding over the closing brace the literal above wrote and
	 * re-closing after, so no existing key moves and the format string stays
	 * one reviewable literal. */
	size_t length = (size_t)written - 1u;
	if (!http_net_diag_json((char *)body, capacity, &length, network))
		return 0;
	/* The storage service's own failure record. Two SD failures that are
	 * identical on the wire -- a job refused because another owner held the
	 * volume, and a card that refused the write -- both surface as
	 * `500 sd_failure`, so the difference has to come from the device. */
	if (!aius_runtime_storage_diag_json(&g_runtime.storage, (char *)body,
	                                    capacity, &length))
		return 0;
	return http_json_append((char *)body, capacity, &length, "}") ? length : 0;
}

static FLASHMEM size_t http_events_json(uint32_t cursor, uint8_t *body, size_t capacity)
{
	int written = snprintf((char *)body, capacity, "{\"cursor\":%lu,\"next\":%lu,\"events\":[",
		(unsigned long)cursor, (unsigned long)g_event_sequence);
	if (written < 0 || (size_t)written >= capacity)
		return 0;
	size_t length = (size_t)written;
	uint8_t count = 0;
	uint32_t first = g_event_sequence > 8u ? g_event_sequence - 7u : 1u;
	for (uint32_t sequence = first; sequence <= g_event_sequence && count < 8; sequence++) {
		const AiusEvent &event = g_events[(sequence - 1u) % 8u];
		if (event.sequence != sequence || event.sequence <= cursor)
			continue;
		written = snprintf((char *)body + length, capacity - length,
			"%s{\"sequence\":%lu,\"uptime_ms\":%lu,\"type\":%u,\"state\":%u,\"detail\":%u}",
			count == 0 ? "" : ",", (unsigned long)event.sequence,
			(unsigned long)event.uptime_ms, event.type, event.state, event.detail);
		if (written < 0 || (size_t)written >= capacity - length)
			return 0;
		length += (size_t)written;
		count++;
	}
	if (length + 2 >= capacity)
		return 0;
	body[length++] = ']';
	body[length++] = '}';
	body[length] = 0;
	return length;
}

static FLASHMEM void reset_application(void)
{
	__disable_irq();
	SCB_AIRCR = 0x05FA0004;
	asm volatile("dsb sy" ::: "memory");
	while (true)
		asm volatile("isb" ::: "memory");
}

static FLASHMEM size_t http_home_page(uint8_t *body, size_t capacity)
{
	int length = snprintf((char *)body, capacity,
		"<!doctype html><html><head><meta charset=\"utf-8\"><title>Aius</title>"
		"<style>body{font-family:monospace;white-space:pre-wrap}code{font-family:inherit}</style></head>"
		"<body><h1>Aius optical firmware</h1><p>Git revision: %s<br>Build UTC: %s</p>"
		"<h2>API</h2><ul><li><a href=\"/api/status\">GET /api/status</a></li>"
		"<li><a href=\"/api/events\">GET /api/events</a></li>"
		"<li><code>POST /api/start {}</code></li>"
		"<li><code>POST /api/stop {}</code></li>"
		"<li><code>POST /api/clear {}</code></li>"
		"<li><code>POST /api/reboot {}</code></li>"
		"<li><a href=\"/api/sd\">GET /api/sd</a></li>"
		"<li><code>PUT /sd/www/{path}</code> raw body with Content-Length and X-Aius-SD-Auth</li>"
		"<li><code>DELETE /sd/www/{path}</code> with X-Aius-SD-Auth</li>"
		"<li><code>PUT /api/config/public</code> JSON feature flags, no auth</li></ul>"
		"<h2>DSP configuration</h2><p><a href=\"/api/dsp/config\">GET /api/dsp/config</a> "
		"reports the single effective config: source, sample_rate_hz, features, input channels, output EQ, matrix, and limiter.</p>"
		"<pre id=dsp>Loading DSP configuration...</pre>"
		"<h2>SD web upload</h2><p>Get the token over USB with <code>AIUS AUTH</code>. "
		"The picker preserves relative paths and sends authenticated standard HTTP PUT requests.</p>"
		"<label>SD auth token <input id=t size=16 maxlength=16 autocomplete=off></label> "
		"<input id=f type=file multiple webkitdirectory directory><button onclick=upload()>Upload selected files</button>"
		"<pre id=u></pre>"
		"<h2>Control</h2><p>"
		"<button title=\"Enable test processing state\" onclick=go('/api/start')>Start</button> "
		"<button title=\"Disable test processing state\" onclick=go('/api/stop')>Stop</button> "
		"<button title=\"Clear audio and event counters\" onclick=go('/api/clear')>Clear counters</button> "
		"<button title=\"Restart the application without entering HalfKay\" onclick=reboot()>Reboot</button></p>"
		"<pre id=s>Loading status...</pre><p id=e></p></body><script>"
		"const s=document.getElementById('s'),e=document.getElementById('e'),u=document.getElementById('u'),d=document.getElementById('dsp');"
		"async function get(){try{s.textContent=JSON.stringify(await (await fetch('/api/status')).json(),null,2);e.textContent=''}catch(a){e.textContent=a}}"
		"async function getDsp(){try{const r=await fetch('/api/dsp/config');if(!r.ok)throw Error('HTTP '+r.status);d.textContent=JSON.stringify(await r.json(),null,2)}catch(a){d.textContent='DSP model unavailable: '+a}}"
		"async function go(a,b){try{await fetch(a,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(b||{})});get()}catch(c){e.textContent=c}}"
		"async function reboot(){e.textContent='Rebooting...';try{await go('/api/reboot');setTimeout(get,2000)}catch(a){}}"
		"async function upload(){u.textContent='';const token=document.getElementById('t').value;const files=[...(document.getElementById('f').files||[])];for(const file of files){const rel=file.webkitRelativePath||file.name;const path=rel.split('/').slice(1).join('/')||rel;if(path.split('/').pop()==='config.json'){u.textContent+='refused '+path+'\\n';continue}try{const r=await fetch('/sd/www/'+path.split('/').map(encodeURIComponent).join('/'),{method:'PUT',headers:{'X-Aius-SD-Auth':token,'Content-Type':'application/octet-stream'},body:file});u.textContent+=r.status+' '+path+'\\n'}catch(a){u.textContent+='error '+path+' '+a+'\\n'}}}"
		"get();getDsp();setInterval(get,2000);setInterval(getDsp,2000)</script></html>",
		AIUS_BUILD_GIT_REV, AIUS_BUILD_UTC);
	return length < 0 || (size_t)length >= capacity ? 0u : (size_t)length;
}

static FLASHMEM size_t http_replay_status_json(uint8_t *body, size_t capacity)
{
	AiusReplayStatus replay = {};
	(void)aius_replay_service_status(&g_replay_service, &replay);
	const char *state = replay.state == AIUS_REPLAY_PLAYING ? "playing" :
		replay.state == AIUS_REPLAY_PREBUFFERING ? "prebuffering" :
		replay.state == AIUS_REPLAY_EOF ? "eof" :
		replay.state == AIUS_REPLAY_ERROR ? "error" : "stopped";
	const char *klass = replay.decoder.classification == AIUS_PIPELINE_DTS ? "dts" :
		replay.decoder.classification == AIUS_PIPELINE_PCM ? "pcm" : "unknown";
	int length = snprintf((char *)body, capacity,
		"{\"ok\":true,\"state\":\"%s\",\"id\":\"%s\",\"file_bytes\":%lu,\"offset_bytes\":%lu,\"duration_seconds\":%.3f,\"loop\":%s,\"loops_completed\":%lu,\"queue_words\":%lu,\"watermark_words\":%lu,\"underruns\":%lu,\"input_class\":\"%s\",\"decoder_errors\":%lu}",
		state, replay.id, (unsigned long)replay.file_bytes,
		(unsigned long)replay.offset_bytes, replay.duration_seconds,
		replay.loop ? "true" : "false", (unsigned long)replay.loops_completed,
		(unsigned long)replay.queue_words, (unsigned long)replay.watermark_words,
		(unsigned long)replay.underruns, klass, (unsigned long)replay.decoder_errors);
	return length < 0 || (size_t)length >= capacity ? 0u : (size_t)length;
}

static FLASHMEM size_t http_replay_list_json(uint8_t *body, size_t capacity)
{
	if (!g_sd_ready) return (size_t)snprintf((char *)body, capacity,
		"{\"ok\":false,\"error\":\"sd_unavailable\"}");
	File directory = SD.open("/aius/replays");
	if (!directory || !directory.isDirectory()) {
		if (directory) directory.close();
		return (size_t)snprintf((char *)body, capacity, "{\"ok\":true,\"files\":[]}");
	}
	size_t length = 0u;
	int written = snprintf((char *)body, capacity, "{\"ok\":true,\"files\":[");
	if (written < 0 || (size_t)written >= capacity) { directory.close(); return 0u; }
	length = (size_t)written;
	bool first = true;
	for (;;) {
		File entry = directory.openNextFile();
		if (!entry) break;
		if (!entry.isDirectory()) {
			const char *name = entry.name();
			const char *base = strrchr(name, '/');
			base = base == nullptr ? name : base + 1;
			size_t name_length = strlen(base);
			bool complete = name_length > 4u &&
				memcmp(base + name_length - 4u, ".iec", 4u) == 0;
			bool partial = name_length > 5u &&
				memcmp(base + name_length - 5u, ".part", 5u) == 0;
			if (complete || partial) {
				size_t suffix = complete ? 4u : 5u;
				size_t id_length = name_length - suffix;
				if (id_length <= AIUS_HTTP_MAX_REPLAY_ID) {
					char duration[24];
					snprintf(duration, sizeof(duration), complete ? "%.3f" : "null",
					         complete ? (double)entry.size() / 192000.0 : 0.0);
					written = snprintf((char *)body + length, capacity - length,
						"%s{\"id\":\"%.*s\",\"bytes\":%lu,\"duration_seconds\":%s,\"upload_next_offset\":%lu,\"complete\":%s,\"active\":%s}",
						first ? "" : ",", (int)id_length, base,
						(unsigned long)entry.size(), duration,
						(unsigned long)entry.size(), complete ? "true" : "false",
						(strlen(g_replay_service.id) == id_length &&
						 memcmp(base, g_replay_service.id, id_length) == 0 && complete) ?
						"true" : "false");
					if (written < 0 || (size_t)written >= capacity - length) {
						entry.close(); directory.close(); return 0u;
					}
					length += (size_t)written;
					first = false;
				}
			}
		}
		entry.close();
	}
	directory.close();
	if (length + 2u >= capacity) return 0u;
	body[length++] = ']'; body[length++] = '}'; body[length] = 0;
	return length;
}

/* ------------------------------------------------------------------ */
/* AiusOpticalApp adapters -- Phase 14 slices S1 and S2b               */
/*                                                                     */
/* The twenty callbacks aius_optical_app.h declares, as thin adapters  */
/* over the serializers above. Slice S1 wrote them over the legacy     */
/* handler bodies while both paths existed; slice S2b deleted the      */
/* legacy path, so these adapters and the serializers they call are    */
/* now the only implementation of every route.                         */
/*                                                                     */
/* Every definition carries FLASHMEM on the definition itself. GCC     */
/* does not inherit a `section` attribute from a forward declaration,  */
/* so a prototype-only FLASHMEM would silently leave the code in ITCM. */
/* ------------------------------------------------------------------ */

/* Records the reboot request; the runtime performs the reset after the
 * response has been written, so nothing resets inline here. volatile because
 * the store is the whole point of the callback and its reader is loop(). */
static volatile bool g_reboot_requested;

static FLASHMEM bool app_sd_ready(void *)
{
	return g_sd_ready;
}

static FLASHMEM bool app_sd_busy(void *)
{
	return sd_job_active();
}

static FLASHMEM bool app_auth_matches(void *, const uint8_t *token, size_t length)
{
	return aius_auth_token_matches(g_auth_token, (const char *)token,
	                               length) != 0;
}

static FLASHMEM size_t app_write_home(void *, uint8_t *body, size_t capacity)
{
	return http_home_page(body, capacity);
}

static FLASHMEM size_t app_write_status(void *, uint8_t *body, size_t capacity)
{
	return http_status_json(body, capacity);
}

static FLASHMEM size_t app_write_events(void *, uint32_t cursor, uint8_t *body,
                                        size_t capacity)
{
	return http_events_json(cursor, body, capacity);
}

static FLASHMEM size_t app_write_dsp_config(void *, uint8_t *body, size_t capacity)
{
	return http_dsp_config_json(body, capacity);
}

static FLASHMEM size_t app_write_meters(void *, uint8_t *body, size_t capacity)
{
	return http_meters_json(body, capacity);
}

static FLASHMEM size_t app_write_replay_status(void *, uint8_t *body,
                                               size_t capacity)
{
	return http_replay_status_json(body, capacity);
}

static FLASHMEM size_t app_write_replay_list(void *, uint8_t *body,
                                             size_t capacity)
{
	return http_replay_list_json(body, capacity);
}

/* Validate and stage; write nothing. The dispatcher persists the accepted bytes
 * through an SD job and withholds the client's 200 until that write commits, so
 * unlike http_config_public_request() this never starts an upload itself. The
 * staged model keeps its existing shape: g_pending_config is fully written
 * before g_config_upload_pending arms it, and the SD-job lifecycle remains its
 * only consumer. Refusal order follows the legacy handler -- parse first (an
 * invalid body still updates the reported config error), then media state.
 *
 * The commit target is what tells sd_settle_http_write() which staging the
 * write about to start carries, replacing the legacy job's `config_upload`
 * boolean. It is armed last, with the write not yet observed. */
static FLASHMEM AiusOpticalAppStoreResult app_store_public_config(
	void *, const uint8_t *body, size_t length)
{
	AiusConfig parsed;
	if (aius_config_parse(body, length, &parsed) != AIUS_CONFIG_OK) {
		g_config = parsed;
		g_config_error_count++;
		return AIUS_OPTICAL_APP_STORE_INVALID;
	}
	if (sd_job_active())
		return AIUS_OPTICAL_APP_STORE_BUSY;
	g_pending_config = parsed;
	g_config_upload_pending = true;
	g_sd_commit_target = SD_COMMIT_PUBLIC_CONFIG;
	g_sd_commit_writing = false;
	return AIUS_OPTICAL_APP_STORE_ACCEPTED;
}

/* Same contract for PUT /api/dsp/config: validated and staged in RAM, written
 * by the dispatcher's SD job. Authentication and media state are the
 * dispatcher's checks, so only the body's own failures remain here. The staged
 * handoff keeps its single-writer/single-reader shape -- g_pending_dsp_model is
 * fully populated, then g_dsp_upload_pending gates it for the job lifecycle;
 * g_dsp_apply_pending is armed only once the write commits, never here, so a
 * failed write can never reach the audio block boundary. */
static FLASHMEM AiusOpticalAppStoreResult app_store_dsp_config(
	void *, const uint8_t *body, size_t length)
{
	AiusDspModel parsed;
	if (aius_dsp_config_parse_put(body, length, &parsed) != AIUS_DSP_CONFIG_OK)
		return AIUS_OPTICAL_APP_STORE_INVALID;
	if (parsed.base_generation != g_config_generation)
		return AIUS_OPTICAL_APP_STORE_STALE;
	if (sd_job_active())
		return AIUS_OPTICAL_APP_STORE_BUSY;
	dsp_model_to_optical_config(parsed, g_pending_dsp_model);
	g_dsp_upload_pending = true;
	g_sd_commit_target = SD_COMMIT_DSP_MODEL;
	g_sd_commit_writing = false;
	return AIUS_OPTICAL_APP_STORE_ACCEPTED;
}

/* POST /api/replays/start, restored by Phase 14c. Memory only: it selects the
 * replay, records the path the refill job will open, and returns. No file is
 * opened, no byte is read and nothing waits, so the dispatcher's "touches no SD
 * primitive and never delays" contract holds.
 *
 * WHAT THE ROUTE ANSWERS. OK maps to the dispatcher's existing 200
 * {"ok":true,"state":"prebuffering","id":"..."} (aius_optical_app.cpp), which
 * is now literally true rather than a claim about a ring that was already full:
 * the ring reaches its watermark over the following epochs and GET
 * /api/replays/status reports the progression prebuffering -> playing. Until it
 * does, the audio path keeps taking optical input, because it gates on
 * SOURCE_REPLAY (AiusAudioProcessor::update) and start publishes only
 * SWITCHING_TO_REPLAY.
 *
 * WHAT IT NO LONGER ANSWERS. 404 replay_not_found is unreachable from here: the
 * file's existence is a property only the card can report, and asking would be
 * the very SD primitive this callback must not perform. A missing or unreadable
 * file now fails the first refill job instead, and surfaces as state "error" on
 * the status route. The dispatcher's 404 arm stays for app_replay_delete(),
 * which does still stat the card. */
static FLASHMEM AiusOpticalAppReplayResult app_replay_start(void *,
                                                            const char *id,
                                                            bool loop)
{
	int written;

	if (!g_sd_ready || !aius_replay_id_is_valid(id))
		return AIUS_OPTICAL_APP_REPLAY_FAILED;
	written = snprintf(g_replay_path, sizeof(g_replay_path),
	                   "/aius/replays/%s.iec", id);
	if (written <= 0 || (size_t)written >= sizeof(g_replay_path))
		return AIUS_OPTICAL_APP_REPLAY_FAILED;
	/* Any refill still running belongs to the previous selection. */
	replay_job_cancel();
	if (aius_replay_service_start(&g_replay_service, id, loop) != AIUS_OK)
		return AIUS_OPTICAL_APP_REPLAY_FAILED;
	return AIUS_OPTICAL_APP_REPLAY_OK;
}

/* SWITCHING_TO_REPLAY joins REPLAY in the active test because 14c made
 * prebuffering an observable state that outlives the start call: a delete
 * admitted during it would remove the file underneath the refill job that is
 * reading it. Before 14c, start returned already playing, so the window did not
 * exist. */
static FLASHMEM AiusOpticalAppReplayResult app_replay_delete(void *,
                                                             const char *id)
{
	char path[96];
	if (strcmp(id, g_replay_service.id) == 0 &&
	    (g_replay_service.source == AIUS_REPLAY_SOURCE_REPLAY ||
	     g_replay_service.source == AIUS_REPLAY_SOURCE_SWITCHING_TO_REPLAY))
		return AIUS_OPTICAL_APP_REPLAY_ACTIVE;
	snprintf(path, sizeof(path), "/aius/replays/%s.iec", id);
	if (!SD.exists(path))
		return AIUS_OPTICAL_APP_REPLAY_NOT_FOUND;
	return SD.remove(path) ? AIUS_OPTICAL_APP_REPLAY_OK :
	                         AIUS_OPTICAL_APP_REPLAY_FAILED;
}

/* Stop is memory only for the same reason start is: the in-flight refill is
 * cancelled, not waited on, and drains through its own CLOSE. */
static FLASHMEM void app_replay_stop(void *)
{
	replay_job_cancel();
	aius_replay_service_stop(&g_replay_service);
}

static FLASHMEM void app_set_test_running(void *, bool running)
{
	if (g_test_running == running)
		return;
	g_test_running = running;
	record_event(2, current_state(), running ? 1u : 0u);
}

static FLASHMEM void app_clear_counters(void *)
{
	g_audio_processor.clear_counters();
}

static FLASHMEM void app_request_reboot(void *)
{
	g_reboot_requested = true;
}

/* Worst-case length of the ID payload below, summed field by field so the
 * static_assert that follows is a proof rather than an estimate. The build
 * stamps are compile-time literals, so their contribution is exact; the counters
 * are bounded by their C type; and the four small values are bounded by the
 * domains the portable status encoder validates (AiusNetState 0..5, AiusNetFault
 * 0..10, checkpoint 0..9, and two booleans). snprintf's capacity check below is
 * still the runtime backstop: an impossible value truncates, the arm returns 0,
 * and the dispatcher answers INTERNAL rather than emitting a corrupt line. */
static constexpr size_t AIUS_ID_U32_DIGITS = 10u;   /* 4294967295 */
static constexpr size_t AIUS_ID_MAX_PAYLOAD =
	(sizeof("protocol=1 revision=") - 1u) + (sizeof(AIUS_BUILD_GIT_REV) - 1u) +
	(sizeof(" build=") - 1u) + (sizeof(AIUS_BUILD_UTC) - 1u) +
	(sizeof(" mode=optical") - 1u) +
	(sizeof(" ip=") - 1u) + 15u +
	(sizeof(" rsafe=") - 1u) + 1u +
	(sizeof(" nstate=") - 1u) + 1u +
	(sizeof(" nfault=") - 1u) + 2u +
	(sizeof(" ckpt=") - 1u) + 1u +
	(sizeof(" early=") - 1u) + 3u +
	(sizeof(" ngen=") - 1u) + AIUS_ID_U32_DIGITS +
	(sizeof(" bgen=") - 1u) + AIUS_ID_U32_DIGITS +
	(sizeof(" rsrc=") - 1u) + AIUS_ID_U32_DIGITS +
	(sizeof(" epoch=") - 1u) + AIUS_ID_U32_DIGITS +
	(sizeof(" nrst=") - 1u) + AIUS_ID_U32_DIGITS;
static_assert(AIUS_ID_MAX_PAYLOAD <= AIUS_OPTICAL_APP_USB_PAYLOAD_MAX,
	"the ID payload must fit the dispatcher's bound, or every ID answer is "
	"a truncation-driven INTERNAL error");

/* The legacy USB service's ID and AUTH arms. The dispatcher answers PROTOCOL
 * and BOOTLOADER itself and asks for no payload, so any other kind is a
 * refusal. AUTH copies rather than formats so no new format literal is
 * introduced.
 *
 * ID carries the four documented identity fields, the leased address, and all
 * ten AiusRecoveryStatus fields. Recovery-safe mode services bounded USB only
 * -- Ethernet stays down -- so this is the one channel that must expose the
 * recovery state when the board is actually in it, and Phase 16a's live smoke
 * reads it here.
 *
 * The ten fields do not fit alongside everything slice S2b put on this line:
 * AIUS_OPTICAL_APP_USB_PAYLOAD_MAX is 256 bytes, the identity fields plus the
 * address already spend 118 of them, and ten keyed values cost 125 even with
 * the short keys below. `link`, `dhcp_pending`, `dhcp_discovers` and
 * `dhcp_updates` are therefore no longer on this line. That is a deliberate
 * trade and not only a budget one: all four are served by GET /api/status,
 * `dhcp_discovers` has had no producer since S2b and was always zero, all four
 * are meaningless in recovery-safe mode where the stack is down, and `nstate` +
 * `nfault` say strictly more about a stalled lease (DHCP_WAIT vs BACKOFF, and
 * which fault caused it) than `link` and `dhcp_pending` ever did. The four
 * fields the USB reference documents -- protocol, revision, build, mode -- are
 * all still here.
 *
 * Keys are abbreviated because nothing longer fits; they map onto the JSON and
 * UDP names one-for-one:
 *
 *   rsafe   recovery_safe          ngen    network_generation
 *   nstate  network_state          bgen    boot_generation
 *   nfault  network_last_fault     rsrc    reset_source
 *   ckpt    last_checkpoint        epoch   loop_epoch
 *   early   watchdog_early_resets  nrst    network_restarts
 */
static FLASHMEM size_t app_write_usb_payload(void *, AiusUsbCommandKind kind,
                                             char *payload, size_t capacity)
{
	if (kind == AIUS_USB_COMMAND_AUTH) {
		size_t length = strlen(g_auth_token);
		if (length == 0u || length >= capacity)
			return 0u;
		memcpy(payload, g_auth_token, length + 1u);
		return length;
	}
	if (kind != AIUS_USB_COMMAND_ID)
		return 0u;
	const AiusNetworkTeensy *network = &g_runtime.network_http.target;
	uint8_t address[4];
	AiusRecoveryStatus recovery;
	(void)aius_network_teensy_address(network, address);
	aius_runtime_teensy_status(&g_runtime, &recovery);
	int written = snprintf(payload, capacity,
		"protocol=1 revision=%s build=%s mode=optical ip=%u.%u.%u.%u rsafe=%u nstate=%u nfault=%u ckpt=%u early=%u ngen=%lu bgen=%lu rsrc=%lu epoch=%lu nrst=%lu",
		AIUS_BUILD_GIT_REV, AIUS_BUILD_UTC,
		address[0], address[1], address[2], address[3],
		recovery.recovery_safe ? 1u : 0u,
		(unsigned)recovery.network_state,
		(unsigned)recovery.network_last_fault,
		(unsigned)recovery.last_checkpoint,
		(unsigned)recovery.watchdog_early_resets,
		(unsigned long)recovery.network_generation,
		(unsigned long)recovery.boot_generation,
		(unsigned long)recovery.reset_source,
		(unsigned long)recovery.loop_epoch,
		(unsigned long)recovery.network_restarts);
	return written > 0 && (size_t)written < capacity ? (size_t)written : 0u;
}

/* The dispatcher combines this with its own per-dispatch sequence to name the
 * temporary and backup files, so the entropy source stays the one the legacy
 * upload paths used: g_sd_job_sequence mixed with millis() and the boot epoch.
 * Reading it through sd_job_id() also keeps that counter monotonic across both
 * paths while the legacy one is still live. */
static FLASHMEM uint32_t app_upload_id(void *)
{
	return sd_job_id();
}

/* Directory enumeration for GET /sd/<dir>/. Raw SD access, like
 * http_replay_list_json above it and for the same reason: the portable engine
 * has no directory job kind, so there is nothing bounded to drive. Two things
 * keep it honest until one exists. The dispatcher calls this only when
 * app_sd_busy() is false, so it cannot run while a job holds the volume -- a
 * gate the replay listing does not have. And the scan is capped at
 * AIUS_OPTICAL_APP_INDEX_SCAN_MAX reads rather than running to the end of an
 * unknown directory, so the epoch stays bounded even on a large card.
 *
 * The handle is never retained past the call, and `entries`/`capacity` belong
 * to the caller. */
static FLASHMEM AiusOpticalAppDirResult app_read_directory(
	void *, const char *path, AiusOpticalAppDirBatch *batch)
{
	if (!g_sd_ready)
		return AIUS_OPTICAL_APP_DIR_FAILED;
	File directory = SD.open(path);
	if (!directory)
		return AIUS_OPTICAL_APP_DIR_NOT_FOUND;
	if (!directory.isDirectory()) {
		directory.close();
		return AIUS_OPTICAL_APP_DIR_NOT_DIRECTORY;
	}
	unsigned scanned = 0u;
	while (batch->count < batch->capacity &&
	       scanned < AIUS_OPTICAL_APP_INDEX_SCAN_MAX) {
		File entry = directory.openNextFile();
		if (!entry) {
			directory.close();
			return AIUS_OPTICAL_APP_DIR_OK;
		}
		scanned++;
		const char *name = entry.name();
		const char *base = strrchr(name, '/');
		base = base == nullptr ? name : base + 1;
		if (strlen(base) > AIUS_OPTICAL_APP_INDEX_NAME_MAX) {
			batch->hidden++;
		} else {
			AiusOpticalAppDirEntry *slot = &batch->entries[batch->count];
			snprintf(slot->name, sizeof(slot->name), "%s", base);
			slot->is_directory = entry.isDirectory();
			slot->size_bytes = slot->is_directory ?
				0u : (uint64_t)entry.size();
			batch->count++;
		}
		entry.close();
	}
	/* Stopped on a bound rather than at the end: is anything left? */
	File extra = directory.openNextFile();
	if (extra) {
		batch->more = true;
		extra.close();
	}
	directory.close();
	return AIUS_OPTICAL_APP_DIR_OK;
}

/* Handed to aius_optical_app_init() by slice S2a. Deliberately external
 * linkage: an internal-linkage table nothing references yet is exactly the
 * shape -Wunused-variable reports, whereas an extern definition is never
 * diagnosed, and --gc-sections still drops it (and, with it, every callback
 * above) until S2a creates the reference -- so this costs no FLASH, no DTCM and
 * no ITCM in the current image. Members are positional and in header order. */
extern const AiusOpticalApp g_aius_optical_app;
const AiusOpticalApp g_aius_optical_app = {
	nullptr,                  /* context: the adapters read file statics */
	app_sd_ready,
	app_sd_busy,
	app_auth_matches,
	app_write_home,
	app_write_status,
	app_write_events,
	app_write_dsp_config,
	app_write_meters,
	app_write_replay_status,
	app_write_replay_list,
	app_store_public_config,
	app_store_dsp_config,
	app_replay_start,
	app_replay_delete,
	app_replay_stop,
	app_set_test_running,
	app_clear_counters,
	app_request_reboot,
	app_write_usb_payload,
	app_upload_id,
	app_read_directory
};

/* ------------------------------------------------------------------ */
/* Runtime composition -- Phase 14 slices S2a and S2b                  */
/*                                                                     */
/* setup() constructs the delivered AiusRuntimeTeensy and returns;     */
/* loop() runs exactly one epoch through it. Slice S2b deleted the     */
/* legacy path this replaced, so the runtime is now the only route     */
/* from this file to any transport, SD or watchdog effect.             */
/*                                                                     */
/* g_runtime itself is defined near the top of the file, because the   */
/* status surface reads its network adapter.                           */
/*                                                                     */
/* Every definition below carries FLASHMEM on the definition itself.   */
/* None of it runs per audio block -- the platform callbacks run once  */
/* per runtime epoch and the construction helpers exactly once -- so a */
/* prototype-only FLASHMEM (which GCC does not inherit) would be a     */
/* silent regression.                                                  */
/* ------------------------------------------------------------------ */

/* Retained boot state, captured once in setup() and published every epoch by
 * the RESET_CAPTURE platform callback. The ledger is the boot adapter's, so
 * the checkpoint callback persists it through the same double-slot storage the
 * Phase 13 target oracle covers, and its mode is what selects recovery-safe
 * operation for the whole runtime. */
static AiusBootTeensyPrimitives g_boot_primitives;
static AiusBootStorageTeensy g_boot_storage;
static AiusBootLedger g_boot_ledger;
static bool g_recovery_safe;

/* Ten seconds of completed normal epochs clears the retained early-reset count
 * (§Normative recovery contract). Without it a second watchdog reset any time
 * later in the board's life would latch recovery-safe mode permanently, since
 * its only exit is a new image. */
static constexpr uint32_t AIUS_BOOT_HEALTHY_MS = 10000u;
static uint32_t g_boot_healthy_at_ms;
static bool g_boot_marked_healthy;

/* UDP scratch in OCRAM rather than on the epoch stack. The epoch call chain
 * (scheduler -> service -> HTTP transaction -> dispatcher) is deeper than the
 * legacy loop's, and these three buffers are 1,576 B of the 22,684-byte DTCM
 * free-for-locals zone. Every byte sent is written before it is read: the
 * receive buffer by the receive itself, the status payload by
 * put_recovery_status() up to the length it returns, the events payload up to
 * the count it reports, and the frame by the encoder up to the length it
 * returns. g_udp_packet must stay live while the
 * frame is encoded because AiusNetFrame::payload points into it. */
DMAMEM static uint8_t g_udp_packet[AIUS_NET_MAX_FRAME];
DMAMEM static uint8_t g_udp_frame[AIUS_NET_MAX_FRAME];
DMAMEM static uint8_t g_udp_payload[AIUS_NET_MAX_PAYLOAD];

/* send_response()'s body against the network adapter instead of the legacy
 * EthernetUDP object. The adapter latches the peer on every receive and sends
 * back to it, which is exactly the beginPacket(remoteIP(), remotePort())
 * request/response pairing the legacy path had, so the encoded frame and its
 * destination are both unchanged. One attempt, no retry: a WOULD_BLOCK is this
 * epoch's answer, not something to wait out. */
static FLASHMEM bool udp_send_frame(uint8_t type, uint32_t request_id,
                                    const uint8_t *payload, uint16_t length)
{
	const size_t frame_length = aius_net_encode(g_udp_frame,
	                                            sizeof(g_udp_frame), type, 0,
	                                            request_id, payload, length);
	if (frame_length == 0u)
		return false;
	const AiusIoEvent event = aius_network_teensy_udp_send_once(
		&g_runtime.network_http.target, g_udp_frame, frame_length);
	return event.result == AIUS_IO_PROGRESS && event.bytes == frame_length;
}

/* send_nack()'s body. */
static FLASHMEM bool udp_send_nack(uint32_t request_id, uint8_t error)
{
	const uint8_t payload[1] = {error};

	return udp_send_frame(AIUS_NET_RSP_NACK, request_id, payload,
	                      sizeof(payload));
}

/* service_network()'s GET_EVENTS arm verbatim, writing into the OCRAM payload
 * buffer instead of a stack one. Returns the payload length. */
static FLASHMEM uint16_t udp_put_events(const AiusNetFrame *request)
{
	const uint32_t cursor = request->payload_len >= 4 ?
		aius_net_get_u32(request->payload) : 0u;
	uint8_t count = 0;
	uint32_t first = g_event_sequence > 8u ? g_event_sequence - 7u : 1u;

	for (uint32_t sequence = first;
	     sequence <= g_event_sequence && count < 4; sequence++) {
		const AiusEvent &event = g_events[(sequence - 1u) % 8u];
		if (event.sequence != sequence || event.sequence <= cursor)
			continue;
		uint8_t *dst = g_udp_payload + 1 + count * 12;
		aius_net_put_u32(dst, event.sequence);
		aius_net_put_u32(dst + 4, event.uptime_ms);
		dst[8] = event.type;
		dst[9] = event.state;
		aius_net_put_u16(dst + 10, event.detail);
		count++;
	}
	g_udp_payload[0] = count;
	return (uint16_t)(1 + count * 12);
}

/* service_network()'s command switch: the same nine command types with the
 * same response types, the status payload extended from 71 to 99 bytes by
 * put_recovery_status() (bytes 0..70 are still put_status()'s, unchanged), and
 * the same AIUS_NET_ERR_TYPE nack for the retired mode/mix commands and any
 * unknown type. The three arms that mutate state go through slice S1's
 * adapters, which are the legacy bodies, so UDP and HTTP keep one producer
 * each. REBOOT is the one ordering change and it is the contract's: the
 * dispatcher records the request and the runtime performs the reset after the
 * epoch, so the ack still leaves before the board resets. */
static FLASHMEM void udp_dispatch(const AiusNetFrame *request)
{
	uint16_t length = 0;

	switch (request->type) {
	case AIUS_NET_CMD_DISCOVER:
		length = put_recovery_status(g_udp_payload);
		(void)udp_send_frame(AIUS_NET_RSP_DISCOVERY, request->request_id,
		                     g_udp_payload, length);
		break;
	case AIUS_NET_CMD_GET_STATUS:
		length = put_recovery_status(g_udp_payload);
		(void)udp_send_frame(AIUS_NET_RSP_STATUS, request->request_id,
		                     g_udp_payload, length);
		break;
	case AIUS_NET_CMD_CLEAR_COUNTERS:
		app_clear_counters(nullptr);
		(void)udp_send_frame(AIUS_NET_RSP_ACK, request->request_id,
		                     nullptr, 0);
		break;
	case AIUS_NET_CMD_SET_MODE:
	case AIUS_NET_CMD_SET_MIX:
		/* Mode/mix selection is retired by the single-config model. The
		 * command IDs remain in the binary protocol, but there is no
		 * selection to set, so the device rejects them rather than
		 * silently no-op-acking. */
		(void)udp_send_nack(request->request_id, AIUS_NET_ERR_TYPE);
		break;
	case AIUS_NET_CMD_START_TEST:
		app_set_test_running(nullptr, true);
		(void)udp_send_frame(AIUS_NET_RSP_ACK, request->request_id,
		                     nullptr, 0);
		break;
	case AIUS_NET_CMD_STOP_TEST:
		app_set_test_running(nullptr, false);
		(void)udp_send_frame(AIUS_NET_RSP_ACK, request->request_id,
		                     nullptr, 0);
		break;
	case AIUS_NET_CMD_REBOOT:
		(void)udp_send_frame(AIUS_NET_RSP_ACK, request->request_id,
		                     nullptr, 0);
		app_request_reboot(nullptr);
		break;
	case AIUS_NET_CMD_GET_EVENTS:
		length = udp_put_events(request);
		(void)udp_send_frame(AIUS_NET_RSP_EVENTS, request->request_id,
		                     g_udp_payload, length);
		break;
	default:
		(void)udp_send_nack(request->request_id, AIUS_NET_ERR_TYPE);
		break;
	}
}

/* Checkpoint 0. The retained facts are captured once, in setup(); this
 * publishes them, so a failed capture can never be half-published and the read
 * is not repeated against a register the capture already cleared. */
static FLASHMEM bool platform_reset_capture(void *,
                                            const AiusRuntimeBudget *budget,
                                            uint32_t now_ms,
                                            AiusRuntimeTeensyBootStatus *status)
{
	(void)now_ms;
	if (budget == nullptr ||
	    budget->checkpoint != AIUS_BOOT_CHECKPOINT_RESET_CAPTURE ||
	    status == nullptr)
		return false;
	status->boot_generation = g_boot_ledger.boot_generation;
	status->reset_source = g_boot_ledger.reset_source;
	status->watchdog_early_resets = g_boot_ledger.early_watchdog_resets;
	return true;
}

/* Checkpoint 3. service_hw_metrics() is already its own one-sample-per-second
 * rate limiter, so the budget's "one sample" is what it does. */
static FLASHMEM bool platform_metrics(void *, const AiusRuntimeBudget *budget,
                                      uint32_t now_ms)
{
	(void)now_ms;
	if (budget == nullptr ||
	    budget->checkpoint != AIUS_BOOT_CHECKPOINT_METRICS)
		return false;
	service_hw_metrics();
	return true;
}

/* Checkpoint 6: one frame in, at most one frame out, inside the budget's 2 ms.
 *
 * The boolean is the scheduler's "this service call was valid", not "the
 * datagram went out" -- returning false aborts the epoch before the single
 * watchdog feed, which is a reset, so no transport condition may produce it.
 * A quiet socket, a would-block send and a decode failure are all ordinary
 * outcomes of an epoch that did its work, exactly as the delivered network and
 * HTTP services treat theirs. An oversized datagram is truncated by the
 * receive and then fails decode, so it still lands in g_network_drops as the
 * legacy explicit length check did. */
static FLASHMEM bool platform_udp(void *, const AiusRuntimeBudget *budget,
                                  uint32_t now_ms)
{
	(void)now_ms;
	if (budget == nullptr || budget->checkpoint != AIUS_BOOT_CHECKPOINT_UDP)
		return false;

	const uint32_t started = micros();
	const size_t capacity = budget->max_rx_bytes < sizeof(g_udp_packet) ?
		budget->max_rx_bytes : sizeof(g_udp_packet);
	const AiusIoEvent received = aius_network_teensy_udp_receive_once(
		&g_runtime.network_http.target, g_udp_packet, capacity);
	AiusNetFrame request;

	if (received.result != AIUS_IO_PROGRESS)
		return true;
	if (aius_net_decode(g_udp_packet, received.bytes, &request) !=
	    AIUS_NET_OK) {
		g_network_drops++;
		return true;
	}
	udp_dispatch(&request);
	if ((uint32_t)(micros() - started) > budget->service_budget_ms * 1000u)
		g_network_budget_overruns++;
	return true;
}

/* ------------------------------------------------------------------ */
/* SD lifecycle -- Phase 14 slice S3b                                  */
/*                                                                     */
/* The legacy service_sd() drove the card directly: SD.begin() while    */
/* unmounted, SD.mediaPresent() at 1 Hz once mounted, SD.totalSize()/  */
/* usedSize() when usage went stale, and byte-at-a-time blocking reads  */
/* of /aius/config.json and /aius/dsp.json from setup() and again on a  */
/* hot re-insert. All four are now root-initiated portable jobs started */
/* through R1's aius_runtime_teensy_start_storage_job() and advanced by */
/* exactly one SD primitive per epoch at checkpoint 8. Nothing below    */
/* waits on the card and nothing runs from setup().                    */
/*                                                                     */
/* One primitive per epoch, total, is kept by construction: the driver  */
/* holds a single owed-step slot, starts a job only when the storage    */
/* service is idle, and never starts the next step before the previous  */
/* one is terminal. The four steps chain probe -> public config -> DSP  */
/* model -> usage, which is the order service_sd() ran them in.        */
/*                                                                     */
/* Every definition carries FLASHMEM on the definition itself; the      */
/* driver runs once per epoch and the publication consumer once per     */
/* loaded file, never per audio block.                                 */
/* ------------------------------------------------------------------ */

/* The two SD-hosted configs, spelled exactly as the dispatcher's own
 * build_root_path() spells them, so the commit arm can match a write's
 * canonicalized target_path against the staging it carries. */
#define AIUS_SD_PUBLIC_CONFIG_PATH "/aius/config.json"
#define AIUS_SD_DSP_MODEL_PATH "/aius/dsp.json"
/* service_sd()'s cadence, unchanged: one probe per second, which is what makes
 * a hot insert and a hot removal observable. */
#define AIUS_SD_PROBE_INTERVAL_MS 1000u

typedef enum {
	SD_STEP_NONE,
	SD_STEP_PROBE,
	SD_STEP_LOAD_CONFIG,
	SD_STEP_LOAD_DSP,
	SD_STEP_USAGE
} SdLifecycleStep;

/* The one owed-or-running step. g_sd_step_running distinguishes "the storage
 * service accepted it" from "it is still owed because the service was busy". */
static SdLifecycleStep g_sd_step;
static bool g_sd_step_running;
static uint32_t g_sd_probe_at_ms;

/* Re-attaches the config-commit arm that died with service_sd_job().
 *
 * The staging is unchanged -- app_store_public_config() / app_store_dsp_config()
 * still fill g_pending_config / g_pending_dsp_model and arm their upload-pending
 * flags -- and this is the consumer the legacy job's UPLOAD_CLEANUP arm used to
 * be, moved onto the composed storage service's terminal result.
 *
 * Two facts are needed and neither is on the portable job: which staging the
 * write carries (g_sd_commit_target, matched against the job's target path) and
 * whether this terminal state belongs to that write rather than to the previous
 * one (g_sd_commit_writing, set only after the job has been seen HTTP-owned and
 * non-terminal). A drained job -- one whose HTTP owner was discarded -- reaches
 * its terminal cancelled, so committed is false and the staging is disarmed
 * without applying, which is exactly what the legacy FAIL_CLEANUP arm did.
 *
 * Reads memory only, and runs before any root job so the terminal state cannot
 * be overwritten by a restart of the shared job object. */
static FLASHMEM void sd_settle_http_write(void)
{
	const AiusRuntimeStorage &storage = g_runtime.storage;
	bool committed;

	/* `draining` is included so a write whose HTTP owner was discarded is
	 * still seen: its terminal state then disarms the staging instead of
	 * leaving it armed forever. */
	if ((storage.http_owned || storage.draining) && !storage.terminal_seen) {
		g_sd_http_job_seen = true;
		if (g_sd_commit_target != SD_COMMIT_NONE &&
		    storage.job.kind == AIUS_SD_JOB_UPLOAD &&
		    strcmp(storage.job.target_path,
		           g_sd_commit_target == SD_COMMIT_PUBLIC_CONFIG ?
		                   AIUS_SD_PUBLIC_CONFIG_PATH :
		                   AIUS_SD_DSP_MODEL_PATH) == 0)
			g_sd_commit_writing = true;
		return;
	}
	if (!g_sd_http_job_seen || !storage.terminal_seen)
		return;
	g_sd_http_job_seen = false;
	committed = storage.job.state == AIUS_SD_DONE;
	/* A committed write or delete moves the usage figures; a download does
	 * not. The legacy job armed the same refresh from the same two arms. */
	if (committed && storage.job.kind != AIUS_SD_JOB_DOWNLOAD)
		g_sd_usage_refresh_pending = true;
	if (!g_sd_commit_writing)
		return;
	if (g_sd_commit_target == SD_COMMIT_DSP_MODEL) {
		/* The SD write is durable; the audio-side apply happens at the
		 * next block boundary (AiusAudioProcessor::enqueue), not here. */
		if (committed && g_dsp_upload_pending)
			g_dsp_apply_pending = true;
		g_dsp_upload_pending = false;
	} else {
		if (committed && g_config_upload_pending)
			g_config = g_pending_config;
		g_config_upload_pending = false;
	}
	g_sd_commit_writing = false;
	g_sd_commit_target = SD_COMMIT_NONE;
}

/* Whether a root-initiated job may start this epoch.
 *
 * The first three terms mirror aius_runtime_storage_start_root_job()'s own
 * rejection conditions, so a refusal is anticipated rather than discovered. The
 * fourth is the one only checkpoint 7 can see: checkpoint 5 produces at most one
 * SD-backed HTTP action and checkpoint 8 consumes it, so a root job started on
 * top of an undelivered action would have the storage service refuse an
 * already-admitted route in-band with a correlated PROTOCOL fault. Yielding
 * turns that into a start this driver simply does not make; the dispatcher's
 * sd_busy callback then keeps the next epoch's routes off a running root job. */
static FLASHMEM bool sd_storage_idle(void)
{
	const AiusRuntimeStorage &storage = g_runtime.storage;

	return !storage.http_owned && !storage.http_authority &&
	       !storage.http_event_present && !storage.draining &&
	       storage.root_state != AIUS_RUNTIME_STORAGE_ROOT_RUNNING &&
	       !g_runtime.storage_action_present && !g_replay_job_active;
}

/* Memory only: builds the step's request and hands it to the runtime. Every
 * request here is compile-time constant and satisfies aius_sd.c's validation --
 * PROBE and USAGE_REFRESH carry no path and no publication, CONFIG_LOAD carries
 * a canonical path, a content type and the caller's own publication bound. */
static FLASHMEM bool sd_start_step(SdLifecycleStep step)
{
	AiusSdJobRequest request;

	memset(&request, 0, sizeof(request));
	switch (step) {
	case SD_STEP_PROBE:
		request.kind = AIUS_SD_JOB_PROBE;
		break;
	case SD_STEP_USAGE:
		request.kind = AIUS_SD_JOB_USAGE_REFRESH;
		break;
	case SD_STEP_LOAD_CONFIG:
		request.kind = AIUS_SD_JOB_CONFIG_LOAD;
		request.target_path = AIUS_SD_PUBLIC_CONFIG_PATH;
		request.content_type = "application/json";
		request.publish = true;
		/* Per caller: the feature-flag config's own bound, so an
		 * oversized file faults at CONFIG_SIZE exactly as the legacy
		 * 256-byte read buffer refused it. */
		request.publish_capacity = AIUS_CONFIG_MAX_JSON_BYTES;
		break;
	case SD_STEP_LOAD_DSP:
		request.kind = AIUS_SD_JOB_CONFIG_LOAD;
		request.target_path = AIUS_SD_DSP_MODEL_PATH;
		request.content_type = "application/json";
		request.publish = true;
		/* Per caller: the model bound slice S3a's publication buffer is
		 * sized for. SD primitives stay 512 bytes either way -- the
		 * engine reads a transfer slice at a time into that buffer. */
		request.publish_capacity = AIUS_DSP_MODEL_MAX_JSON_BYTES;
		break;
	case SD_STEP_NONE:
	default:
		return false;
	}
	return aius_runtime_teensy_start_storage_job(&g_runtime, &request);
}

/* Consumes the terminal result of the step that was running and chooses the
 * next. The result pointer is valid only until another job of either owner
 * starts, which is why this runs before any start in the same pass. */
static FLASHMEM void sd_step_complete(uint32_t now_ms)
{
	const AiusSdResult *result =
		aius_runtime_storage_root_result(&g_runtime.storage);
	const bool ok = result != nullptr &&
	                result->kind != AIUS_SD_RESULT_FAULT;
	const SdLifecycleStep step = g_sd_step;

	g_sd_step = SD_STEP_NONE;
	g_sd_step_running = false;

	switch (step) {
	case SD_STEP_PROBE: {
		/* PROBE is BEGIN then MEDIA_PRESENT, so a card that will not
		 * mount fails the job and a mounted-but-ejected one reports
		 * media_present false; both mean absent. */
		const bool present = ok && result->media_present;

		/* Republished on every probe tick, so a card swap re-reports the
		 * new volume's type rather than the old one's. */
		g_sd_filesystem = present ? result->filesystem
		                          : AIUS_SD_FILESYSTEM_UNKNOWN;
		g_sd_probe_at_ms = now_ms + AIUS_SD_PROBE_INTERVAL_MS;
		if (present && !g_sd_ready) {
			/* Boot and hot re-insert are the same transition, and
			 * both reload the two SD-hosted configs -- the second
			 * trigger service_sd() had. No mkdir is needed: the
			 * portable UPLOAD job creates /aius itself, and a
			 * CONFIG_LOAD against a missing directory simply fails
			 * its open, which is the missing-file case. */
			g_sd_ready = true;
			g_sd_step = SD_STEP_LOAD_CONFIG;
		} else if (!present && g_sd_ready) {
			g_sd_ready = false;
			g_sd_total_bytes = 0u;
			g_sd_used_bytes = 0u;
			g_sd_usage_refresh_pending = false;
		} else if (present && g_sd_usage_refresh_pending) {
			g_sd_step = SD_STEP_USAGE;
		}
		return;
	}
	case SD_STEP_LOAD_CONFIG:
		/* On success the bytes were already parsed into g_config by the
		 * publication consumer. A job that never got that far -- missing
		 * file, empty or oversized file, IO fault -- leaves the defaults
		 * and counts one config error, which is what load_sd_config()
		 * did for its own missing-file case. The one surface difference
		 * is that an empty or oversized file now reports "not loaded"
		 * rather than "empty input" / "input too large", because the
		 * bytes never reach a parser to say so. */
		if (!ok) {
			aius_config_set_defaults(&g_config);
			g_config_error_count++;
		}
		g_sd_step = SD_STEP_LOAD_DSP;
		return;
	case SD_STEP_LOAD_DSP:
		/* A missing or invalid /aius/dsp.json is the common case and is
		 * not an error: the active model is left alone, exactly as
		 * stage_dsp_sd_override() did. */
		g_sd_step = SD_STEP_USAGE;
		return;
	case SD_STEP_USAGE:
		if (ok) {
			g_sd_total_bytes = result->total_bytes;
			g_sd_used_bytes = result->used_bytes;
		}
		/* Cleared either way: a failed refresh is re-armed by the next
		 * committed write, not retried by spinning on the card. */
		g_sd_usage_refresh_pending = false;
		return;
	case SD_STEP_NONE:
	default:
		return;
	}
}

/* One SD lifecycle pass, at checkpoint 7. Memory only -- every card access it
 * causes happens at checkpoint 8, one primitive at a time.
 *
 * A refused start is not an error and costs nothing: the step stays owed, no
 * state moves, and the next epoch retries it. That is the whole of the
 * contention answer in this direction -- a boot load that lands while a route is
 * mid-upload simply waits for the upload, which is why /aius/config.json and
 * /aius/dsp.json can be late but never lost. */
static FLASHMEM void sd_lifecycle_service(uint32_t now_ms)
{
	/* First, because a root job start would restart the shared job object
	 * and destroy the terminal state the commit arm reads. */
	sd_settle_http_write();

	if (g_runtime.storage.root_state ==
	    AIUS_RUNTIME_STORAGE_ROOT_RUNNING)
		return;
	if (g_sd_step_running)
		sd_step_complete(now_ms);
	if (g_sd_step == SD_STEP_NONE) {
		if ((int32_t)(now_ms - g_sd_probe_at_ms) < 0)
			return;
		g_sd_step = SD_STEP_PROBE;
	}
	if (!sd_storage_idle())
		return;
	if (sd_start_step(g_sd_step))
		g_sd_step_running = true;
	else
		/* sd_storage_idle() already excluded every busy case, so only a
		 * request the portable engine rejected outright can land here.
		 * Every request above is constant, so retrying would spin: drop
		 * the step and let the probe tick bring the next one. */
		g_sd_step = SD_STEP_NONE;
}

/* The runtime's optional publication consumer, wired in at construction and
 * called in SD-checkpoint context. The only publisher is this file's own
 * CONFIG_LOAD -- both config PUTs write with publish false -- so the running
 * step says which file the bytes are, and the payload is parsed in place from
 * the delivered pointer rather than copied anywhere.
 *
 * Returning true acknowledges the payload and lets the job complete; a parse
 * failure is a property of the file, not of the job, so it is recorded exactly
 * as the legacy loaders recorded it rather than faulting the read. */
static FLASHMEM bool platform_publish_config(void *, const uint8_t *data,
                                             size_t length)
{
	if (data == nullptr || !g_sd_step_running)
		return false;

	if (g_sd_step == SD_STEP_LOAD_CONFIG) {
		(void)aius_config_parse(data, length, &g_config);
		if (!g_config.valid)
			g_config_error_count++;
		return true;
	}
	if (g_sd_step == SD_STEP_LOAD_DSP) {
		AiusDspModel parsed;

		/* A staged PUT owns g_pending_dsp_model until its commit arm
		 * settles; a reinsert load must not write across it. */
		if (g_dsp_upload_pending)
			return true;
		if (aius_dsp_config_parse_put(data, length, &parsed) !=
		    AIUS_DSP_CONFIG_OK)
			return true;
		dsp_model_to_optical_config(parsed, g_pending_dsp_model);
		/* Staged, not applied: the block-boundary swap in
		 * AiusAudioProcessor::enqueue() remains the only place the
		 * active model changes, where config_generation increments and
		 * source becomes "sd". */
		g_dsp_apply_pending = true;
		return true;
	}
	/* No load is in flight, so these bytes belong to a job this file did not
	 * start. Refusing faults that job (AIUS_SD_FAULT_PUBLISH) rather than
	 * completing it as though it had published. */
	return false;
}

/* Builds one event for the refill job, in the adapter's own shape. */
static FLASHMEM AiusSdEvent replay_job_make_event(AiusSdEventKind kind,
                                                  uint64_t action_id,
                                                  uint32_t fault)
{
	AiusSdEvent event;

	memset(&event, 0, sizeof(event));
	event.kind = kind;
	event.action_id = action_id;
	event.fault = fault;
	return event;
}

/* The refill job's action sink, and the counterpart of the storage service's
 * route_action(). Only two action kinds are handled here rather than executed:
 * the download chunk, which is the whole point of the job and goes to the
 * replay service instead of to an HTTP client, and the terminal pair. Everything
 * else is exactly one adapter primitive, whose result is retained for the next
 * epoch's step. */
static FLASHMEM void replay_job_route(const AiusSdAction *action)
{
	switch (action->kind) {
	case AIUS_SD_ACTION_NONE:
	case AIUS_SD_ACTION_COMPLETE:
	case AIUS_SD_ACTION_FAIL:
		return;

	case AIUS_SD_ACTION_OUTPUT: {
		/* A cancelled job's chunk is acknowledged and dropped: the
		 * service has already moved on to another selection, and
		 * delivering into it would be the stale read the offset check
		 * exists to refuse. */
		const bool taken =
			!g_replay_job_cancelling &&
			aius_replay_service_deliver(
				&g_replay_service, (uint32_t)action->offset,
				action->buffer, action->length) == AIUS_OK;

		g_replay_job_event = replay_job_make_event(
			taken || g_replay_job_cancelling ? AIUS_SD_EVENT_OK :
			                                   AIUS_SD_EVENT_FAULT,
			action->action_id,
			taken || g_replay_job_cancelling ?
				0u : AIUS_REPLAY_DELIVER_REJECT_FAULT);
		g_replay_job_event_present = true;
		return;
	}

	default:
		break;
	}

	/* The one SD primitive this epoch, through the storage service's adapter
	 * -- the single owner of the only card handle in the firmware. */
	g_replay_job_event =
		aius_sd_teensy_execute(&g_runtime.storage.target, action);
	g_replay_job_event_present = true;
}

/* One bounded job advance: deliver at most one retained adapter result, take at
 * most one emitted action, spend at most one SD primitive. */
static FLASHMEM void replay_job_advance(uint32_t now_ms)
{
	AiusSdAction action;
	AiusSdEvent event;

	if (g_replay_job_event_present) {
		event = g_replay_job_event;
		g_replay_job_event_present = false;
		memset(&g_replay_job_event, 0, sizeof(g_replay_job_event));
	} else {
		event = replay_job_make_event(AIUS_SD_EVENT_NONE, 0u, 0u);
	}

	memset(&action, 0, sizeof(action));
	aius_sd_job_step(&g_replay_job, now_ms, event, &action);
	replay_job_route(&action);

	/* The job's own FILE_SIZE is the only place the service can learn how
	 * long the file is, now that nothing else stats the card. Published once
	 * per job; an odd or shrinking size is refused by the service, which
	 * then falls back to finding the end from a short refill. */
	if (!g_replay_job_size_published && !g_replay_job_cancelling &&
	    g_replay_job.captured_size != 0u &&
	    g_replay_job.captured_size <= UINT32_MAX) {
		(void)aius_replay_service_set_file_bytes(
			&g_replay_service,
			(uint32_t)g_replay_job.captured_size);
		g_replay_job_size_published = true;
	}

	if (action.kind != AIUS_SD_ACTION_COMPLETE &&
	    action.kind != AIUS_SD_ACTION_FAIL)
		return;

	/* Terminal: the handle is closed and the card is free again. A cancelled
	 * job reports nothing -- its selection no longer exists. */
	if (!g_replay_job_cancelling)
		aius_replay_service_refill_complete(
			&g_replay_service,
			action.kind == AIUS_SD_ACTION_COMPLETE);
	g_replay_job_active = false;
	g_replay_job_cancelling = false;
	g_replay_job_size_published = false;
	g_replay_job_event_present = false;
	memset(&g_replay_job_event, 0, sizeof(g_replay_job_event));
}

/* Memory only: the request is the planned range, against the path
 * app_replay_start() built. The engine clamps the range to the file's real
 * length for AIUS_SD_JOB_REPLAY_REFILL, so a plan that runs past the end
 * completes short rather than faulting -- which is how the service finds EOF. */
static FLASHMEM void replay_job_start(const AiusReplayRefill *refill,
                                      uint32_t now_ms)
{
	AiusSdJobRequest request;

	memset(&request, 0, sizeof(request));
	request.kind = AIUS_SD_JOB_REPLAY_REFILL;
	request.target_path = g_replay_path;
	request.total = refill->byte_count;
	request.initial_offset = refill->offset_bytes;
	g_replay_job_active = true;
	g_replay_job_cancelling = false;
	g_replay_job_size_published = false;
	g_replay_job_event_present = false;
	memset(&g_replay_job_event, 0, sizeof(g_replay_job_event));
	aius_sd_job_start(&g_replay_job, &request, now_ms, 0u);
}

/* One replay refill pass, at checkpoint 7. Spends at most one SD primitive.
 *
 * A running job is advanced. Otherwise the service is asked whether it wants a
 * read -- the call that also settles the drained-EOF transition, so it is made
 * every epoch -- and the plan is turned into a job only if the storage service
 * is completely idle. It is not idle when an HTTP action admitted at checkpoint
 * 5 is waiting for checkpoint 8, which is what keeps a refill from preempting a
 * route, and the abandoned plan costs nothing. */
static FLASHMEM void replay_refill_service(size_t budget_words, uint32_t now_ms)
{
	AiusReplayRefill refill;

	if (g_replay_job_active) {
		replay_job_advance(now_ms);
		return;
	}
	if (!aius_replay_service_plan_refill(&g_replay_service, budget_words,
	                                     &refill))
		return;
	if (!sd_storage_idle()) {
		aius_replay_service_abandon_refill(&g_replay_service);
		return;
	}
	replay_job_start(&refill, now_ms);
	replay_job_advance(now_ms);
}

/* Checkpoint 7: the SD lifecycle and the replay refill, in that order, with the
 * refill quota taken from the budget rather than the legacy loop's literal
 * 2,048.
 *
 * This checkpoint is the only slot either can use: it is the last application
 * callback before the SD checkpoint consumes the epoch's HTTP action, so it is
 * the only point at which a job can be started and still yield to a route
 * admitted two checkpoints earlier (sd_storage_idle()). In recovery-safe mode
 * the scheduler runs only the two USB checkpoints, so neither runs at all --
 * which is the intended behaviour for a board recovering from repeated watchdog
 * resets.
 *
 * The lifecycle goes first because it is rate-limited -- one probe per second,
 * and the two config loads only on a card transition -- so giving it the first
 * refusal cannot starve replay, whereas the reverse could starve the probe that
 * makes a hot card removal observable. Each yields to the other's running job,
 * so the epoch still spends at most one SD primitive between them. */
static FLASHMEM bool platform_replay(void *, const AiusRuntimeBudget *budget,
                                     uint32_t now_ms)
{
	if (budget == nullptr ||
	    budget->checkpoint != AIUS_BOOT_CHECKPOINT_REPLAY)
		return false;
	sd_lifecycle_service(now_ms);
	replay_refill_service(budget->max_rx_bytes, now_ms);
	return true;
}

/* The last checkpoint is persisted before each service call, through the boot
 * adapter's double-slot retained storage. A torn write cannot be read back as
 * valid: the slot's committed marker is cleared first and restored last, and
 * the CRC covers sequence and ledger together.
 *
 * aius_fault_checkpoint() is then given the same checkpoint. In a production
 * image AIUS_FAULT_STALL_CHECKPOINT is undefined and it compiles to a return,
 * so it costs one call; in a teensy41-optical-fault image built for checkpoint
 * N it stalls forever the first time N is reached. The call belongs AFTER the
 * commit, so the retained ledger records the checkpoint that stalled and the
 * watchdog reset that follows is attributable -- which is exactly what the
 * Phase 16a live recovery smoke measures. Without this call every fault image
 * would be incapable of failing and that proof would be vacuous. */
static FLASHMEM void platform_checkpoint(void *, uint16_t checkpoint)
{
	aius_boot_record_checkpoint(&g_boot_ledger, checkpoint);
	(void)aius_boot_storage_teensy_commit(&g_boot_storage, &g_boot_ledger);
	aius_fault_checkpoint(checkpoint);
}

/* The one watchdog feed, reached only from the scheduler's EPOCH_COMPLETE, and
 * the only place the file writes the watchdog service register at all. */
static FLASHMEM void platform_feed_watchdog(void *)
{
	(void)aius_boot_teensy_watchdog_feed(&g_boot_primitives);
}

/* Returned by value rather than held as a file-scope const: Teensy 4 places
 * rodata in DTCM, and the epoch reads this table through the copy
 * aius_runtime_teensy_construct() takes, so a static one would cost DTCM for
 * nothing. publish_config is the one optional entry, filled by slice S3b now
 * that a root-initiated CONFIG_LOAD produces a payload to consume. */
static FLASHMEM AiusRuntimeTeensyPlatformOps platform_ops(void)
{
	AiusRuntimeTeensyPlatformOps ops;

	memset(&ops, 0, sizeof(ops));
	ops.context = nullptr;
	ops.reset_capture = platform_reset_capture;
	ops.metrics = platform_metrics;
	ops.udp = platform_udp;
	ops.replay = platform_replay;
	ops.checkpoint = platform_checkpoint;
	ops.feed_watchdog = platform_feed_watchdog;
	ops.publish_config = platform_publish_config;
	return ops;
}

/* Retained reset state, captured exactly once. The adapter reads SRC_SRSR and
 * clears the bits it read in one call, so this must not be repeated per epoch:
 * a second read would report zero. The retained ledger then decides this boot's
 * generation, its early-reset count and its mode -- a watchdog reset whose last
 * checkpoint was not EPOCH_COMPLETE increments the count, and the second
 * consecutive one selects recovery-safe. A changed build id clears both. */
static FLASHMEM void capture_retained_boot_state(void)
{
	AiusBootLedger retained;
	AiusBootInput input;

	g_boot_primitives = aius_boot_teensy_hardware_primitives();
	/* Read once: the adapter clears the bits it reports. The status surfaces
	 * take the value back out of AiusRecoveryStatus, which carries the raw
	 * word rather than the legacy low byte. */
	const uint32_t raw_reset_source =
		aius_boot_teensy_reset_source(&g_boot_primitives);

	aius_boot_storage_teensy_init(&g_boot_storage, &g_boot_primitives);
	memset(&retained, 0, sizeof(retained));
	(void)aius_boot_storage_teensy_load(&g_boot_storage, &retained);

	memset(&input, 0, sizeof(input));
	input.build_id = (uint32_t)AIUS_BUILD_ID;
	input.raw_reset_source = raw_reset_source;
	input.reset_kind = aius_boot_teensy_reset_kind(raw_reset_source);
	/* A zeroed or corrupt retained ledger fails validation inside
	 * aius_boot_init(), which then starts a normal generation-1 boot. */
	aius_boot_init(&g_boot_ledger, &retained, input);
	g_recovery_safe = g_boot_ledger.mode == AIUS_BOOT_MODE_RECOVERY_SAFE;
}

/* The per-device auth token, derived from the fused MAC words. Memory only. */
static FLASHMEM void derive_auth_token(void)
{
	uint8_t auth_material[AIUS_AUTH_DEVICE_MATERIAL_SIZE];
	uint32_t mac0 = HW_OCOTP_MAC0;
	uint32_t mac1 = HW_OCOTP_MAC1;
	uint32_t mac2 = HW_OCOTP_MAC2;

	memcpy(auth_material, &mac0, sizeof(mac0));
	memcpy(auth_material + 4u, &mac1, sizeof(mac1));
	memcpy(auth_material + 8u, &mac2, sizeof(mac2));
	memcpy(auth_material + 12u, &mac0, sizeof(mac0));
	(void)aius_auth_derive_token(auth_material, g_auth_token);
}

/* Construction only, per §Normative recovery contract: capture retained reset
 * state, arm the watchdog, place audio output in its safe state, construct the
 * service objects, return. No SD probe, no link or DHCP wait, no USB wait, no
 * peer transaction, and no loop waiting on anything external. Everything the
 * legacy setup() did beyond that is now epoch work: the network comes up
 * through the supervisor's retained initial action at checkpoint 4, and the SD
 * probe plus both config loads are root-initiated portable jobs the checkpoint-7
 * driver starts from the first epoch (sd_lifecycle_service()), never from here.
 * g_sd_probe_at_ms is left at its static zero so that first epoch probes
 * immediately rather than a second late.
 *
 * FLASHMEM because it runs once, before the first audio block: ITCM belongs to
 * code that runs per block (§Deciding principle). `wiring.h` declares setup()
 * and loop() without a section, and GCC does not merge a section attribute from
 * a declaration into a later definition -- so the attribute has to sit here, on
 * the definition, exactly as it does on every other cold definition in this
 * file. */
FLASHMEM void setup(void)
{
	capture_retained_boot_state();
	(void)aius_boot_teensy_watchdog_arm(&g_boot_primitives);

	AudioMemory(24);
	g_audio_processor.begin_pipeline();

	/* No parser is initialized here: the runtime's USB service owns its own
	 * AiusUsbRuntime, and the legacy one died with service_usb_control(). */
	Serial.begin(115200);
	derive_auth_token();
	aius_replay_service_init(&g_replay_service);
	/* Memory only, like every other construction here: the refill job's
	 * transfer slice is registered now so checkpoint 7 can start a job
	 * without allocating or touching the card. */
	aius_sd_job_init(&g_replay_job, g_replay_transfer,
	                 sizeof(g_replay_transfer));
	aius_config_set_defaults(&g_config);
	g_boot_epoch = micros();

	const AiusCdcTeensyPrimitives cdc = aius_cdc_teensy_serial_primitives();
	const AiusRuntimeTeensyPlatformOps platform = platform_ops();
	aius_runtime_teensy_construct(&g_runtime, &g_aius_optical_app, &cdc,
	                              &platform, millis(), g_recovery_safe);
	g_boot_healthy_at_ms = millis() + AIUS_BOOT_HEALTHY_MS;
}

/* Exactly one runtime epoch, then the two things the epoch itself must not do.
 *
 * The epoch runs checkpoints 0..8 in order, records EPOCH_COMPLETE and feeds
 * the watchdog once; an incomplete epoch returns false without that feed, which
 * is what makes a wedged service a bounded reset rather than a hang.
 *
 * request_reboot() only records (aius_optical_app.cpp), because the dispatcher
 * never resets or delays, so the reset happens here -- after the epoch, so the
 * HTTP response or UDP ack that asked for it has already been written.
 *
 * FLASHMEM for the same reason as setup(): loop() is the epoch driver, not
 * per-block work. The audio path reaches the processor through AudioStream's
 * interrupt, never through here, so nothing in this function runs per block. */
FLASHMEM void loop(void)
{
	const uint32_t now_ms = millis();
	const bool completed = aius_runtime_teensy_epoch(&g_runtime, now_ms);

	if (completed && !g_boot_marked_healthy &&
	    (int32_t)(now_ms - g_boot_healthy_at_ms) >= 0) {
		/* Persisted by the next checkpoint, which commits the ledger. */
		aius_boot_mark_healthy(&g_boot_ledger);
		g_boot_marked_healthy = true;
	}
	if (g_reboot_requested)
		reset_application();
}
