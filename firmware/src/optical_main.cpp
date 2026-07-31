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
	#include "aius_dsp_runtime_config.h"
	#include "aius_measure.h"
	#include "aius_recorder.h"
	#include "aius_audio_ingress.h"
	#include "aius_audio_service.h"
	#include "aius_audio_clock.h"
	#include "aius_clock.h"
	#include "aius_pipeline.h"
	#include "aius_http.h"
	#include "aius_usb_control.h"
	#include "aius_dsp_config.h"
}
#include "aius_replay.h"
#include "aius_tx_gate.h"
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
/* The 24-bit output path: our own route from the playback ring into the S/PDIF
 * transmit DMA buffer, alongside the retained AudioOutputSPDIF3 instead of
 * through its int16 audio_block_t queue. The header sits beside its
 * implementation in lib/aius_audio/src/ rather than in lib/aius_audio/include/,
 * which holds the retained snapshot's own headers and is not ours to add to;
 * the path is relative for the same reason aius_spdif_sources.cpp's is. */
#include "../lib/aius_audio/src/aius_spdif_tx24.h"
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

/* THE BOOT/REPLAY CLOCK AND GENERATED DEFAULT ARE FORCED TO AGREE.
 *
 * AudioStream.h defaults AUDIO_SAMPLE_RATE_EXACT to 44100.0f behind an #ifndef,
 * and config_spdif3() (output_spdif3.cpp) derives the whole S/PDIF transmit clock
 * tree from that macro at boot. PCM and both decoders now carry runtime nominal
 * rates, while apply_config() derives filters and meters from each accepted
 * stream. The generated model and replay format remain 48 kHz, so the boot
 * default must remain equal to those two compile-time contracts.
 *
 * With the default left in place those two disagreed by 8.1 % and NOTHING
 * observed it: the graph's producer and its consumer are both driven by the same
 * clock, so the queue stayed level, the overrun counters stayed honestly zero,
 * and the only symptom was audio playing ~1.4 semitones flat. There is no
 * resampler anywhere in this path to absorb the difference.
 *
 * The compile-time equality continues to protect boot and replay. Live optical
 * plans are reconciled separately before the physical gate may open. */
static_assert((uint32_t)AUDIO_SAMPLE_RATE_EXACT == AIUS_OPTICAL_SAMPLE_RATE_HZ,
	"the Teensy audio engine clock (AUDIO_SAMPLE_RATE_EXACT, set in "
	"platformio.ini) must equal the configured DSP sample rate "
	"(config/optical_dsp.json -> AIUS_OPTICAL_SAMPLE_RATE_HZ)");

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

/* The audio software ISR publishes exact 256-word engine chunks here; AUDIO
 * consumes them. Storage is OCRAM, while cursors and diagnostics stay in DTCM
 * because the boot ROM scribbles DMAMEM at reset. */
DMAMEM static uint16_t
	g_audio_ingress_words[AIUS_AUDIO_INGRESS_CAPACITY_WORDS];
static_assert(AIUS_AUDIO_INGRESS_CAPACITY_WORDS == 8192u,
	"target ingress must contain exactly 8192 words");
static_assert(sizeof(g_audio_ingress_words) == 16384u,
	"target ingress must occupy exactly 16 KiB of OCRAM");
static AiusAudioIngress g_audio_ingress;
static AiusAudioService g_audio_service;
static bool g_audio_ingress_ready;
static bool g_audio_service_ready;
static AiusReplaySource g_audio_ingress_source =
	AIUS_REPLAY_SOURCE_OPTICAL;
/* Epoch-owned reset request. Replay start and a debounced carrier relock each
 * advance it; AUDIO acknowledges the exact value through AiusAudioService. */
static uint32_t g_audio_reset_sequence;
/* Whole normal-epoch timing. AUDIO owns its narrower 8 ms diagnostics. */
static uint32_t g_runtime_epoch_max_us;
static uint32_t g_runtime_epoch_budget_overruns;
static uint32_t g_audio_checkpoint_max_cycles;
static uint32_t g_tx_high_water_frames;
static constexpr uint32_t AIUS_RUNTIME_EPOCH_BUDGET_US = 25000u;

/* Portable rate truth is updated once at checkpoint 7 and consumed by AUDIO,
 * the bounded target adapter and the physical-gate decision later in the
 * epoch. The desired plan is checkpoint-owned; applied-plan and boundary
 * truth come back through the adapter's race-safe snapshots. */
static AiusRateTracker g_rate_tracker;
static AiusSpdifClockPlan g_clock_desired_plan;
static uint32_t g_clock_carrier_rate_millihz;
static uint32_t g_clock_nominal_rate_hz;
static uint32_t g_clock_stream_rate_hz;
static uint32_t g_clock_output_rate_millihz = UINT32_C(48000000);
static uint32_t g_pipeline_pcm_rate_hz = AIUS_OPTICAL_SAMPLE_RATE_HZ;
static uint32_t g_clock_rate_changes;
static uint32_t g_clock_trim_updates;
static uint32_t g_clock_structural_replans;
static uint32_t g_clock_invalid_windows;
static uint32_t g_clock_faults;
static uint32_t g_clock_last_fault;
static uint32_t g_clock_mismatch_blocks;
static uint32_t g_clock_unsupported_blocks;
static uint32_t g_clock_unsupported_episodes;
static uint32_t g_clock_tracker_noted_target_millihz;
static uint32_t g_clock_last_apply_attempt_ms;
static bool g_clock_carrier_ready;
static bool g_clock_desired_plan_valid;
static bool g_clock_plan_applied;
static bool g_clock_dark_transition_pending;
static bool g_clock_replay_ready;
static bool g_clock_apply_attempted;
static bool g_clock_fault_episode;
static bool g_clock_live_ready;
/* AUDIO's producer-side admission latch. A supported nominal can be known
 * while the receiver's physical-rate estimate is still settling; publishing
 * that interval would overflow ingress or run new-rate words through the old
 * plan. Main closes this before reset and reopens only after the accepted plan
 * and dark transition are complete. */
static volatile bool g_clock_live_ingress_open;
static bool g_clock_carrier_unsupported;
static bool g_clock_stream_unsupported_episode;
static uint32_t g_clock_relock_reset_nominal_hz;
static bool g_tx_gate_open;    /* latched decision, .bss = closed */
static bool g_tx_gate_carrier; /* latched checkpoint-7 carrier input */
static bool g_tx_gate_replay;  /* never re-derived at request time */

static FLASHMEM void tx_gate_actuate(bool open);
static FLASHMEM bool clock_output_is_master(void);

static inline bool supported_program_rate(uint32_t rate_hz)
{
	return rate_hz == 32000u || rate_hz == 44100u || rate_hz == 48000u;
}

static inline void close_stream_rate_gate(void)
{
	g_clock_live_ready = false;
	if (g_tx_gate_open)
		tx_gate_actuate(false);
	g_tx_gate_open = false;
}

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
/* 512 BYTES BECAUSE THE ENGINE SAYS SO, and this is the sentence that cost a
 * flash. aius_sd_job_init() takes this capacity as the job's per-primitive read
 * length, and aius_sd.c's validate_request() refuses any job whose capacity
 * exceeds AIUS_SD_MAX_TRANSFER (512) with AIUS_SD_FAULT_CAPACITY -- BEFORE the
 * first action is emitted, so the job never mounts, never opens and never reads
 * a byte. An attempt to read a whole refill in one primitive by sizing this
 * array to 8 KB therefore did not slow the replay down, it killed it outright:
 * every refill was rejected at validation, the service went to ERROR on the
 * first one, and plan_refill() never asks again from ERROR.
 *
 * AIUS_SD_MAX_TRANSFER is private to aius_sd.c, so the compiler cannot see it
 * from here. The two guards that remain are the ones it CAN see, plus the
 * cross-library text check in test/optical-cutover, which reads the engine's
 * own #define and fails when this array outgrows it. */
DMAMEM static uint8_t g_replay_transfer[AIUS_RUNTIME_STORAGE_TRANSFER];
/* The slice is what one AIUS_SD_ACTION_OUTPUT carries, and the replay service
 * decodes a delivery in one batch out of a fixed scratch array. Keep the two
 * bounds in lockstep at compile time rather than discovering a widened transfer
 * slice as a run-time NO_SPACE that faults the refill. */
static_assert(sizeof(g_replay_transfer) <= AIUS_REPLAY_DELIVER_MAX_BYTES,
	"a refill chunk must fit the replay service's delivery bound");
/* And the bound the flashed regression walked straight past: the replay job's
 * transfer slice is the SAME primitive every other consumer spends, so it is
 * held to the same number rather than to a private one. AIUS_RUNTIME_STORAGE_
 * TRANSFER is already asserted equal to AIUS_HTTP_SD_CAPACITY_MAX, so this ties
 * all three to one value. */
static_assert(sizeof(g_replay_transfer) <= AIUS_RUNTIME_STORAGE_TRANSFER,
	"the replay job spends the same bounded SD primitive as every other "
	"consumer; a bigger one is refused by the engine at validation");
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

/* THE REFILL JOB'S OWN DIAGNOSTICS, because nothing else has them.
 *
 * The refill runs on g_replay_job, not on the storage service's job, so its
 * failures never reach AiusRuntimeStorage's `last_failure` and never appear in
 * sd_diag. When the first live loop-boundary fault landed, sd_diag showed
 * driver_faults going 1 -> 2 and a `last_failure` still describing a CONFIG_LOAD
 * from boot: the count moved and the record did not, so the one fault anyone
 * wanted was the one fault nobody could see.
 *
 * The adapter's own duration figures have the same problem in the other
 * direction. `longest_primitive_us` is a single running maximum across every
 * primitive from every job, with no attribution: a 1,016,348 us reading proves
 * something blocked for a second and says nothing about which action, which job
 * or when. So this file times its own primitives, around its own call into the
 * adapter, and keeps the worst one WITH the action that caused it.
 *
 * All of it lives here rather than in the storage service because the storage
 * service is not this job's owner and src/aius_runtime_storage.* is outside this
 * change's surface. Served as a `replay_diag` object beside `sd_diag` on
 * GET /api/status. Everything below is a store on a path that runs at most once
 * per epoch; nothing is sampled per audio block. */
typedef struct {
	uint32_t jobs;          /* refill jobs started */
	uint32_t faults;        /* refill jobs that ended in FAIL */
	uint32_t mounts;        /* AIUS_SD_ACTION_BEGIN performed by the refill */
	uint32_t last_primitive_us;
	uint32_t longest_primitive_us;
	uint8_t longest_action;   /* AiusSdActionKind of the worst primitive */
	uint8_t last_fault;       /* AiusSdFault of the last failed job */
	uint8_t last_fault_phase; /* AiusSdPhase it landed in -- which primitive */
	uint8_t last_fault_action;
	uint32_t last_fault_driver;
	uint32_t last_fault_offset_bytes;
	uint32_t last_fault_at_ms;
	uint32_t last_fault_loops;
} ReplayJobDiag;
/* DMAMEM like the job and the service it describes: a diagnostic record has no
 * claim on the DTCM free-for-locals zone, which is the bank §Deciding principle
 * names as scarcest. Written once per epoch at most, read by the status route.
 * .bss.dma is NOLOAD, so setup() must clear this before the first status read. */
DMAMEM static ReplayJobDiag g_replay_diag;

/* Why the current selection is not playing, so /api/replays/status can tell a
 * replay the operator stopped from one a carrier displaced (replay_clock_service
 * below). Without it both read as plain `stopped`, which is the same class of
 * silent ambiguity as a replay that reports `playing` and processes no blocks.
 * One byte, not an enum object: a plain `enum` would be int-sized in .bss, and
 * DTCM is the bank the deciding principle names as scarcest. */
typedef enum {
	REPLAY_STOP_NONE = 0, /* never stopped, or a selection is live */
	REPLAY_STOP_REQUEST,  /* DELETE-ish: the operator asked, via app_replay_stop */
	REPLAY_STOP_CARRIER,  /* the optical input came up and took the audio path */
	/* The card failed AIUS_REPLAY_REFILL_FAULT_LIMIT refills in a row. Its own
	 * reason because it is the one ending an operator cannot infer: a single
	 * fault is now survivable, so reaching this means the card stopped
	 * answering rather than hiccupped. Appended, so no value moves. */
	REPLAY_STOP_FAULT
} ReplayStopReason;
static uint8_t g_replay_stop_reason;

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
 * AiusAudioProcessor::enqueue() (AUDIO checkpoint context), the accepted
 * block boundary. g_dsp_apply_pending is the single volatile handoff flag: the
 * writer always fully populates g_pending_dsp_model BEFORE setting it, so AUDIO
 * never observes a partially-written model (see docs plan "Input trim/EQ +
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
 * this in OCRAM"). At ~9.5 kB (the analysis ring and its published window are
 * most of it) it is far too big to spend on the ~48 kB DTCM
 * free-for-locals zone when OCRAM has ~128 kB spare -- §Deciding principle's
 * "memory that is genuinely free is not an argument" points the same way.
 * DMAMEM is not zeroed at startup, so apply_config() runs aius_meter_init()
 * before the first block ever reaches enqueue().
 *
 * ONE WRITER, ONE READER. The writer is AiusAudioProcessor::enqueue() in the
 * AUDIO checkpoint: it owns every accumulator, both seqlock increments and the
 * published snapshot. The reader is http_meters_json() at the serial HTTP
 * checkpoint and only ever calls aius_meter_read().
 *
 * WHY THAT CANNOT TEAR HERE. Runtime checkpoints do not overlap: a publish or
 * read completes before the next checkpoint begins. The seqlock remains the
 * portable snapshot contract and makes any future concurrent reader retry
 * rather than accept a straddled copy. */
DMAMEM static AiusMeter g_meter;

/* ------------------------------------------------------------------ */
/* Measurement loopback capture -- item 3 slice 3                      */
/* (aius-dsp-config-api.md §Item 3 design; the portable machine and    */
/* the analysis live in lib/aius_dsp/aius_measure.*)                   */
/*                                                                     */
/* The 128 KiB sample buffer is DMAMEM (OCRAM, ~293 kB free): like the */
/* transmit ring it needs no init -- every frame is written before the */
/* buffer is ever read, because analysis reads only `captured` buffers */
/* and captured means all 16,384 frames were copied. KNOWN CONSEQUENCE */
/* carried from item 1's delivery note: a new 128 KiB DMAMEM object    */
/* moves g_aius_boot_slots, so the retained ledger restarts            */
/* boot_generation at the first flash of this build. Expected, not a   */
/* regression.                                                         */
/*                                                                     */
/* The CONTROL state is DTCM, never DMAMEM: .bss.dma is NOLOAD and the */
/* boot ROM scribbles OCRAM at reset -- the ring's own documented      */
/* hazard -- and this struct must read as idle before the first arm.   */
/* Recorder producer and drain are serial AUDIO/epoch work; its portable */
/* token protocol remains the ownership boundary.                      */
/* ------------------------------------------------------------------ */
DMAMEM static int32_t g_measure_capture_l[AIUS_MEASURE_CAPTURE_FRAMES];
DMAMEM static int32_t g_measure_capture_r[AIUS_MEASURE_CAPTURE_FRAMES];
static AiusMeasureCapture g_measure_capture;

/* ------------------------------------------------------------------ */
/* PSRAM flight recorder (aius-optical-prototype.md §PSRAM flight      */
/* recorder, prebuild-gated 2026-07-27)                                */
/*                                                                     */
/* Staging is the first 32 KiB of fitted PSRAM (8 planar 512-frame     */
/* slots); the circular ring begins after it at a power-of-two byte    */
/* capacity. Both producer and drain are now serial epoch work, so no  */
/* interrupt or DMA observes this storage. Control state remains DTCM. */
/* No EXTMEM                                                           */
/* static exists anywhere in this firmware and the extmem heap is      */
/* banned outright (test/recorder/test_no_extmem_heap.py) -- an        */
/* EXTMEM array links silently wrong on an 8 MB board and the smalloc  */
/* pool spans the same storage the ring claims.                        */
/*                                                                     */
/* g_recorder_present is the whole per-block cost on a bare board:     */
/* one bool test in enqueue() (the runtime meters_active field), and  */
/* epoch service returns on the same latch.                            */
/* ------------------------------------------------------------------ */
/* AUDIO may commit eight output steps after REPLAY already drained for this
 * epoch. Eight slots make that exact burst gap-free. Removing the prior 16 KiB
 * OCRAM staging offsets the new 16 KiB ingress ring and preserves the linker's
 * 128 KiB NativeEthernet heap floor. */
#define RECORDER_STAGING_SLOTS 8u
#define RECORDER_SLOT_FRAMES (AUDIO_BLOCK_SAMPLES * 4u)
#define RECORDER_STAGING_BYTES \
	(RECORDER_STAGING_SLOTS * RECORDER_SLOT_FRAMES * 2u * sizeof(int32_t))
#define RECORDER_DRAIN_BYTES_PER_EPOCH RECORDER_STAGING_BYTES
static_assert(RECORDER_STAGING_BYTES == 32768u,
	"eight planar 512-frame recorder slots must occupy 32 KiB");
static uint32_t g_recorder_staging_frames[RECORDER_STAGING_SLOTS];
static AiusRecorder g_recorder;
static bool g_recorder_present = false;
/* The boot probe's result (startup.c configure_external_ram), in MB. */
extern "C" uint8_t external_psram_size;
/* Best-known PSRAM on the SECOND pad (FlexSPI2 chip-select A2, silkscreened
 * FLASH), in MB. The core's probe never interrogates pad 2 while pad 1 is
 * silent, so a lone chip soldered there is invisible to `external_psram_size`
 * -- exactly the 2026-07-27 bench state. probe_second_pad_psram() closes that
 * gap; `psram_pad2_mb` on GET /api/status is its reader. */
static uint8_t g_psram_pad2_mb = 0u;

/* The IP-command primitives below are the startup probe's own
 * (cores/teensy4/startup.c flexspi2_command / flexspi2_psram_id), replicated
 * because they are static there. The LUT indices are the ones startup
 * programmed and left behind: 0 exit-QPI, 1 reset-enable, 2 reset, 3 read-ID. */
static FLASHMEM void flexspi2_ip_command(uint32_t index, uint32_t address)
{
	FLEXSPI2_IPCR0 = address;
	FLEXSPI2_IPCR1 = FLEXSPI_IPCR1_ISEQID(index);
	FLEXSPI2_IPCMD = FLEXSPI_IPCMD_TRG;
	while (!(FLEXSPI2_INTR & FLEXSPI_INTR_IPCMDDONE))
		;
	FLEXSPI2_INTR = FLEXSPI_INTR_IPCMDDONE;
}

static FLASHMEM uint32_t flexspi2_ip_read_id(uint32_t address)
{
	FLEXSPI2_IPCR0 = address;
	FLEXSPI2_IPCR1 = FLEXSPI_IPCR1_ISEQID(3) | FLEXSPI_IPCR1_IDATSZ(4);
	FLEXSPI2_IPCMD = FLEXSPI_IPCMD_TRG;
	while (!(FLEXSPI2_INTR & FLEXSPI_INTR_IPCMDDONE))
		;
	const uint32_t id = FLEXSPI2_RFDR0;
	FLEXSPI2_INTR = FLEXSPI_INTR_IPCMDDONE | FLEXSPI_INTR_IPRXWA;
	return id;
}

/* Pad 2's IP-command window base when NO pad-1 chip was found: startup never
 * writes FLSHA1CR0 on that path, so it holds its reset default of 64 MB
 * (0x10000 KB) and chip-select A2's window begins at 64 MB. (On the found
 * path startup re-bases A1 to its real size, which is why ITS pad-2 probe
 * uses `size1 << 20` instead.) */
static constexpr uint32_t AIUS_PSRAM_PAD2_PROBE_BASE = 0x04000000u;

/* Runs once from setup(), construction-cheap (four bounded IP commands on an
 * otherwise idle FlexSPI2). Gated to the no-pad-1 case: on a 16 MB board the
 * pad-2 chip is LIVE mapped memory that a reset command would corrupt, and on
 * an 8 MB board startup's own second probe already asked pad 2 and heard
 * nothing -- both answers derive from `external_psram_size`. */
static FLASHMEM void probe_second_pad_psram(void)
{
	if (external_psram_size != 0u) {
		g_psram_pad2_mb = external_psram_size > 8u ? 8u : 0u;
		return;
	}

	flexspi2_ip_command(0u, AIUS_PSRAM_PAD2_PROBE_BASE); /* exit QPI */
	flexspi2_ip_command(1u, AIUS_PSRAM_PAD2_PROBE_BASE); /* reset enable */
	flexspi2_ip_command(2u, AIUS_PSRAM_PAD2_PROBE_BASE); /* reset */
	const uint32_t id = flexspi2_ip_read_id(AIUS_PSRAM_PAD2_PROBE_BASE);
	switch (id & 0xFFFFu) {
	case 0x5D0Du: /* AP / Ipus / ESP / Lyontek KGD */
		g_psram_pad2_mb = 8u;
		break;
	case 0x5D9Du: /* ISSI: size in ID bits 23:21 */
		switch ((id >> 21) & 0x7u) {
		case 0x3u:
			g_psram_pad2_mb = 8u;
			break;
		case 0x4u:
			g_psram_pad2_mb = 16u;
			break;
		default:
			g_psram_pad2_mb = 0u;
			break;
		}
		break;
	default:
		g_psram_pad2_mb = 0u;
		break;
	}
}
/* The previous boot's final persisted checkpoint (0xffff when the retained
 * ledger was invalid), captured by capture_boot_state() before aius_boot_init
 * folds it away. `pv=` on the USB ID line: the only after-the-fact evidence of
 * WHICH checkpoint a watchdog-killed boot died in. */
static uint16_t g_prev_boot_checkpoint = 0xffffu;
/* The previous boot's FIRST aborting checkpoint (one-shot retained latch,
 * defect 25's instrument). `pv=` names wherever the final epoch had walked to;
 * this names the service whose `false` return started the starvation. `fa=`
 * on the USB ID line: 0-9 a checkpoint, 254 no abort, 255 latch invalid. */
static uint8_t g_prev_first_abort = AIUS_ABORT_INVALID;
/* The dispatcher's mask mirror must equal the recorder's own bits -- the
 * measure probe-cap rule, applied to four constants. */
static_assert(AIUS_OPTICAL_APP_RECORDER_MASK_OVERRUN ==
              AIUS_RECORDER_MASK_OVERRUN, "recorder mask mirror drifted");
static_assert(AIUS_OPTICAL_APP_RECORDER_MASK_UNDERRUN ==
              AIUS_RECORDER_MASK_UNDERRUN, "recorder mask mirror drifted");
static_assert(AIUS_OPTICAL_APP_RECORDER_MASK_CARRIER_LOSS ==
              AIUS_RECORDER_MASK_CARRIER_LOSS, "recorder mask mirror drifted");
static_assert(AIUS_OPTICAL_APP_RECORDER_MASK_LIMITER_GR ==
              AIUS_RECORDER_MASK_LIMITER_GR, "recorder mask mirror drifted");

/* Reset requests stay in epoch context. AUDIO flushes queued ingress and resets
 * the pipeline before acknowledging the sequence; update() only publishes
 * words and discontinuity generations. */
static FLASHMEM void request_audio_reset(void)
{
	g_audio_reset_sequence++;
}

/* The only sample walk left in the software audio ISR. Keep it out of
 * AiusAudioProcessor::update() so that callback remains an auditable sequence
 * of ingress operations; noinline gives the ITCM placement gate a stable,
 * separately pinned producer section. */
static __attribute__((noinline)) void
audio_ingress_publish_live(uint16_t *slot, const audio_block_t *left,
	                       const audio_block_t *right)
{
	for (unsigned i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
		slot[2u * i] = (uint16_t)left->data[i];
		slot[2u * i + 1u] = (uint16_t)right->data[i];
	}
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

/* The ring's `type` byte is a wire contract: GET /api/events serves it verbatim,
 * tools/net/aius_net_runner.py reads it, and the status compatibility contract
 * pins it. Values are allocated once and never renumbered -- a reader meeting an
 * unknown type has one field it cannot name, a reader meeting a moved one is
 * silently wrong about every field beside it. Naming them here rather than
 * writing the digits at the call sites is what makes that checkable.
 *
 *    1  STATE                 the S/PDIF lock or pipeline classification moved.
 *                             `state` carries the new value; detail is 0.
 *    2  TEST_TONE             the test tone was toggled. detail 1 running, 0 not.
 *    3  REPLAY_STOP           a replay ended by itself. detail is the stop reason.
 *
 * 4..10 are counter edges, emitted by service_event_edges() at the bottom of this
 * file. Each says "a counter the status payload already carries advanced", with
 * the delta or the code that explains it in `detail` -- the fact a poller could
 * previously only infer from two samples of a number, and never order against
 * anything else. Their bound (at most one event per counter per epoch, carrying
 * the delta rather than one event per increment) belongs to that function and is
 * documented there.
 *
 *    4  OVERRUN               audio blocks the pipeline refused. detail: delta.
 *    5  NETWORK_DROP          datagrams that failed decode. detail: delta.
 *    6  NETWORK_BUDGET        UDP service calls over their budget. detail: delta.
 *    7  CONFIG_ERROR          a config parse failed. detail: AiusConfigError.
 *    8  NETWORK_RESTART       the supervisor restarted the stack. detail: the
 *                             AiusNetFault that caused it.
 *    9  DHCP_UPDATE           the adapter bound a new lease. detail: delta.
 *   10  WATCHDOG_EARLY_RESET  the retained ledger's early-reset count, reported
 *                             on the first epoch that observes it. detail: the
 *                             count, which is fixed for the life of the boot.
 *   11  RATE_CHANGE            an accepted live nominal rate. detail: Hz.
 *   12  RATE_UNSUPPORTED       one refused-rate episode. detail: measured Hz,
 *                              or zero for an unsupported decoded block.
 *   13  CLOCK_FAULT            one failed apply episode. detail:
 *                              AiusClockApplyResult. */
enum AiusEventType : uint8_t {
	AIUS_EVENT_STATE = 1,
	AIUS_EVENT_TEST_TONE = 2,
	AIUS_EVENT_REPLAY_STOP = 3,
	AIUS_EVENT_OVERRUN = 4,
	AIUS_EVENT_NETWORK_DROP = 5,
	AIUS_EVENT_NETWORK_BUDGET = 6,
	AIUS_EVENT_CONFIG_ERROR = 7,
	AIUS_EVENT_NETWORK_RESTART = 8,
	AIUS_EVENT_DHCP_UPDATE = 9,
	AIUS_EVENT_WATCHDOG_EARLY_RESET = 10,
	AIUS_EVENT_RATE_CHANGE = 11,
	AIUS_EVENT_RATE_UNSUPPORTED = 12,
	AIUS_EVENT_CLOCK_FAULT = 13,
};

static AiusEvent g_events[8];
static uint32_t g_event_sequence;
static uint32_t g_event_overflows;
static uint8_t g_last_state;
static bool g_test_running;
static volatile uint32_t g_network_drops;
static volatile uint32_t g_network_budget_overruns;
/* The HTTP checkpoint's wall-clock overruns, measured by platform_checkpoint()
 * between the HTTP stamp and the UDP checkpoint's arrival -- the scheduler
 * calls the checkpoint hook immediately before each service, so that interval
 * is one HTTP service call and nothing else. The bound is the checkpoint's
 * declared service_budget_ms (2 ms), which carries the legacy service_http()
 * 2,000 us threshold forward. The other three http_* counters live where the
 * transaction is observable: AiusRuntimeNetworkHttp. */
static uint32_t g_http_checkpoint_entered_us;
static volatile uint32_t g_http_service_overruns;

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
	record_event(AIUS_EVENT_STATE, state, 0);
}

/* ------------------------------------------------------------------ */
/* The audio clock, and the block sizes that are easy to confuse       */
/*                                                                     */
/* THERE IS NO RESAMPLER ANYWHERE IN THIS PATH. Neither peripheral      */
/* converts rate: AudioInputSPDIF3 DMAs receiver words straight into    */
/* audio blocks at the incoming carrier's rate (the retained snapshot   */
/* predates the upstream asynchronous converter, and the framework copy */
/* is byte-identical to it). The tracker accepts 32/44.1/48 kHz as       */
/* runtime data and the target adapter follows the accepted live clock.  */
/* enqueue() compares every decoded/passthrough block rate with that     */
/* accepted carrier and applied output plan before config, DSP or output;*/
/* a mismatch is muted and counted.                                      */
/*                                                                     */
/* THREE BLOCK SIZES, and every per-block figure names one of them:     */
/*                                                                     */
/*   AUDIO_BLOCK_SAMPLES = 128 frames (the Teensy core default; this    */
/*   build does not override it) is the ENGINE block. It sizes the DMA  */
/*   buffers, the audio_block_t pool, update()'s word staging and the   */
/*   transmit ISR's drain of the 24-bit ring. At 48 kHz update() runs   */
/*   375 times a second and moves 256 IEC words = 512 bytes each time;  */
/*   live input cadence instead follows the accepted carrier.           */
/*                                                                     */
/*   AC-3 emits 256-frame decoded slices. DTS Core and PCM passthrough   */
/*   emit 512-frame blocks. Both pass through enqueue(), whose           */
/*   AUDIO_BLOCK_SAMPLES * 4 cap is the largest accepted block.         */
/*                                                                     */
/* The DWT bracket in enqueue() measures one decoded/passthrough block, */
/* not the 128-frame engine block. The meter's `queue_frames` is        */
/* aius_spdif_tx24_level(), the 24-bit ring occupancy at publication.   */
/*                                                                     */
/* THE FINAL HOP IS NO LONGER audio_block_t. The transmit DMA interrupt */
/* drains the 24-bit ring straight into SPDIF_tx_buffer                */
/* (lib/aius_audio/src/aius_spdif_tx24.h), so this class has no         */
/* emit_block(), allocates no pool blocks, and is connected to no       */
/* output node. The consequence that matters here: the ring's producer  */
/* (enqueue(), reached from the runtime AUDIO checkpoint)               */
/* and its consumer (the transmit DMA ISR) are now at DIFFERENT NVIC    */
/* priorities, where both used to sit inside update(). That is why the  */
/* ring is a stage/commit single-producer/single-consumer structure and */
/* not the plain read/write/count triple the int16 ring could afford.   */
/* ------------------------------------------------------------------ */

class AiusAudioProcessor : public AudioStream
{
public:
	AiusAudioProcessor(void) : AudioStream(2, g_input_queue) {}

	/* Bring-up only, FLASHMEM, and out-of-line for apply_config()'s reason
	 * (see its comment): the arena carve grew this past what the compiler
	 * inlines away, and the ITCM placement gate rightly rejects cold code in
	 * the hot bank. Definition follows the class. */
	FLASHMEM void begin_pipeline(void);

	AiusPipelineClass classification(void) const
	{
		return g_pipeline ? aius_pipeline_classification(g_pipeline)
		                  : AIUS_PIPELINE_UNKNOWN;
	}

	uint32_t block_count(void) const
	{
		return g_blocks;
	}
	uint32_t overrun_count(void) const
	{
		/* One aligned producer-owned u32. The atomic load avoids masking the
		 * software audio IRQ on every committed output block. */
		return __atomic_load_n(&g_audio_ingress.dropped_chunks,
		                       __ATOMIC_ACQUIRE);
	}
	AiusStatus create_status(void) const { return g_create_status; }
	/* Pipeline state is AUDIO-owned and HTTP runs serially in the same epoch. */
	AiusStatus pipeline_metrics(AiusMetrics *out) const
	{
		return g_pipeline != nullptr
			? aius_pipeline_get_metrics(g_pipeline, out)
			: AIUS_INVALID_ARG;
	}
	uint16_t pipeline_codec_pc(void) const
	{
		return g_pipeline != nullptr ? aius_pipeline_codec_pc(g_pipeline) : 0u;
	}
	AiusStatus pipeline_codec_metrics(uint16_t codec_pc,
	                                 AiusMetrics *out) const
	{
		return g_pipeline != nullptr
			? aius_pipeline_get_codec_metrics(g_pipeline, codec_pc, out)
			: AIUS_INVALID_ARG;
	}
	void pipeline_service_diag(AiusPipelineServiceDiag *out) const
	{
		aius_pipeline_get_service_diag(g_pipeline, out);
	}
	AiusStatus parser_diag(AiusIecDiag *out) const
	{
		return g_pipeline != nullptr
			? aius_pipeline_get_parser_diag(g_pipeline, out)
			: AIUS_INVALID_ARG;
	}
	AiusStatus reclass_counters(uint32_t *retype, uint32_t *reclassify) const
	{
		return g_pipeline != nullptr
			? aius_pipeline_get_reclass_counters(g_pipeline, retype, reclassify)
			: AIUS_INVALID_ARG;
	}
	/* The confirmation squelch's lifetime dropped-frame count and its LIVE
	 * gate state -- the pair that makes "by-ear silence at the seam" a reading
	 * rather than an inference. AUDIO is the sole writer. */
	AiusStatus squelch_diag(uint32_t *squelched_frames, int *pcm_confirmed) const
	{
		return g_pipeline != nullptr
			? aius_pipeline_get_squelch_diag(g_pipeline, squelched_frames,
			                                 pcm_confirmed)
			: AIUS_INVALID_ARG;
	}
	void clear_counters(void)
	{
		NVIC_DISABLE_IRQ(IRQ_SOFTWARE);
		aius_audio_ingress_clear_producer_diag(&g_audio_ingress);
		NVIC_ENABLE_IRQ(IRQ_SOFTWARE);
		g_blocks = 0;
		g_dsp_cycles_max = 0;
		g_tx_high_water_frames = aius_spdif_tx24_level(&g_aius_spdif_tx24);
		g_runtime_epoch_max_us = 0u;
		g_runtime_epoch_budget_overruns = 0u;
		g_audio_checkpoint_max_cycles = 0u;
		if (g_audio_service_ready)
			aius_audio_service_clear_diag(&g_audio_service);
		if (g_pipeline != nullptr)
			aius_pipeline_clear_service_diag(g_pipeline);
		g_clock_rate_changes = 0u;
		g_clock_trim_updates = 0u;
		g_clock_structural_replans = 0u;
		g_clock_invalid_windows = 0u;
		g_clock_faults = 0u;
		g_clock_last_fault = 0u;
		g_clock_mismatch_blocks = 0u;
		g_clock_unsupported_blocks = 0u;
		g_clock_unsupported_episodes = 0u;
	}

	/* Cold wrapper over the public production-model resolver. The implementation
	 * is shared with the native latency renderer; this class owns only placement
	 * of the meter and process scratch. */
	FLASHMEM void apply_config(const AiusOpticalConfig &cfg,
	                           uint32_t stream_rate_hz);

protected:
	void update(void) override
	{
		audio_block_t *left = receiveReadOnly(0);
		audio_block_t *right = receiveReadOnly(1);
		const AiusReplaySource source =
			g_replay_service.source == AIUS_REPLAY_SOURCE_REPLAY
				? AIUS_REPLAY_SOURCE_REPLAY
				: AIUS_REPLAY_SOURCE_OPTICAL;
		if (g_audio_ingress_ready && source != g_audio_ingress_source) {
			aius_audio_ingress_record_source_change(&g_audio_ingress);
			g_audio_ingress_source = source;
		}
		if (g_audio_ingress_ready && source == AIUS_REPLAY_SOURCE_REPLAY) {
			uint16_t *slot = nullptr;
			if (aius_audio_ingress_reserve(&g_audio_ingress, &slot) ==
			    AIUS_OK) {
				const size_t count = aius_replay_service_pop_words(
					&g_replay_service, slot,
					AIUS_AUDIO_INGRESS_CHUNK_WORDS);
				if (count == AIUS_AUDIO_INGRESS_CHUNK_WORDS)
					(void)aius_audio_ingress_commit(&g_audio_ingress);
				else
					aius_audio_ingress_cancel_gap(
						&g_audio_ingress,
						AIUS_AUDIO_INGRESS_GAP_SHORT_REPLAY);
			} else {
				(void)aius_replay_service_discard_words(
					&g_replay_service,
					AIUS_AUDIO_INGRESS_CHUNK_WORDS);
			}
		} else if (g_audio_ingress_ready &&
		           !g_clock_live_ingress_open) {
			/* Deliberate acquisition quiescence: release the AudioStream
			 * blocks without publishing and without recording a gap. The
			 * pending reset owns discontinuity accounting once, while the
			 * one-second physical-rate stability gate may safely outlive
			 * every bounded audio ring. */
		} else if (g_audio_ingress_ready && left != nullptr &&
		           right != nullptr) {
			uint16_t *slot = nullptr;
			if (aius_audio_ingress_reserve(&g_audio_ingress, &slot) ==
			    AIUS_OK) {
				audio_ingress_publish_live(slot, left, right);
				(void)aius_audio_ingress_commit(&g_audio_ingress);
			}
		} else if (g_audio_ingress_ready) {
			aius_audio_ingress_record_gap(
				&g_audio_ingress,
				AIUS_AUDIO_INGRESS_GAP_MISSING_LIVE);
		}
		if (left != nullptr)
			release(left);
		if (right != nullptr)
			release(right);
		/* No emit_block(): the transmit DMA interrupt drains the 24-bit ring
		 * itself; this callback only publishes one bounded ingress chunk. */
	}

private:
	static constexpr size_t ARENA_BYTES = 128u * 1024u;
	/* THE OUTPUT WORD WIDTH, and the one place it is chosen.
	 *
	 * 24 bits is the transmit register's width and the DSP's own output width,
	 * so this selection is lossless end to end and aius_out16_* is bypassed --
	 * present, tested and reachable through aius_out_stage_run()'s other arm,
	 * but not on the shipped path. A constexpr rather than a config field
	 * because nothing on the device has a reason to ask for 16-bit output: the
	 * sink is a 24-bit S/PDIF register either way, so selecting 16 would only
	 * throw away 8 bits. Promoting it to AiusOpticalConfig is a config-surface
	 * change and belongs with whoever next widens that model. */
	static constexpr int OUTPUT_WIDTH_BITS = AIUS_DSP_OUT_WIDTH_24;
	audio_block_t *g_input_queue[2] = {nullptr, nullptr};
	static uint8_t g_arena_memory[ARENA_BYTES] __attribute__((aligned(16)));
	AiusArena g_arena;
	AiusPipeline *g_pipeline = nullptr;
	AiusStatus g_create_status = AIUS_INVALID_ARG;
	/* The playback ring is NOT a member any more: it is g_aius_spdif_tx24, 24-bit
	 * and in DMAMEM, owned by the transmit path that drains it
	 * (lib/aius_audio/src/aius_spdif_tx24.h). Moving it out took 8,192 bytes of
	 * int16 ring off DTCM's free-for-locals zone -- the scarcest bank on this
	 * board -- and put 16,384 bytes of int32 ring into OCRAM, of which 310 KB was
	 * free. The int16 ring could afford DTCM; a 24-bit one could not, and the
	 * transmit ISR's read is strictly sequential so the L1 D-cache amortises the
	 * OCRAM latency over eight words per line. */
	uint32_t g_blocks = 0;
	/* Production model resolution lives in the public adapter so firmware and
	 * the native latency renderer cannot diverge on bypass, coefficient or
	 * state-reset semantics. */
	AiusDspRuntimeConfig g_dsp_runtime = {};
	/* The double staging planes, carved from g_arena in begin_pipeline() (see
	 * the placement rationale there). Pointer members only: the storage is
	 * arena DTCM, sized to the largest block enqueue() accepts. */
	static constexpr uint32_t DSP_STAGE_FRAMES = AUDIO_BLOCK_SAMPLES * 4;
	double *g_dsp_scratch_d[AIUS_MAX_CHANNELS] = {};
	double *g_dsp_work_l = nullptr;
	double *g_dsp_work_r = nullptr;
	/* The output stage's dither streams, seeded ONCE in begin_pipeline() and
	 * deliberately not in apply_config(): re-seeding on every staged config change
	 * would restart the noise sequence and make the output a function of
	 * config-change history rather than of the sample index alone (see aius_dsp.h
	 * §DETERMINISM). Seeded even though OUTPUT_WIDTH_BITS bypasses dither, so that
	 * flipping the width is a one-line change and not also a bring-up change. */
	AiusOut16Stage g_out16 = {};
	uint32_t g_dsp_cycles_last = 0;
	uint32_t g_dsp_cycles_max = 0;
	uint32_t g_applied_stream_rate_hz = AIUS_OPTICAL_SAMPLE_RATE_HZ;

	static __attribute__((section(".flashmem.audio_consume")))
	AiusStatus consume_block(void *ctx, const AiusPcmBlock *block)
	{
		return static_cast<AiusAudioProcessor *>(ctx)->enqueue(block);
	}

	__attribute__((section(".flashmem.audio_enqueue")))
	AiusStatus enqueue(const AiusPcmBlock *block)
	{
		if (block == nullptr || block->frames > DSP_STAGE_FRAMES ||
		    block->planes[0] == nullptr)
			return AIUS_INVALID_ARG;
		if (block->frames > aius_spdif_tx24_free(&g_aius_spdif_tx24))
			return AIUS_NO_SPACE;
		/* Capacity preflight is first: a refused block applies no staged model
		 * and advances no DSP, meter, recorder, measurement or output state. */
		g_clock_stream_rate_hz = block->sample_rate_hz;
		if (!supported_program_rate(block->sample_rate_hz)) {
			g_clock_unsupported_blocks++;
			close_stream_rate_gate();
			if (!g_clock_stream_unsupported_episode) {
				g_clock_stream_unsupported_episode = true;
				g_clock_unsupported_episodes++;
				record_event(AIUS_EVENT_RATE_UNSUPPORTED,
				             current_state(), 0u);
			}
			return AIUS_REFUSED;
		}
		g_clock_stream_unsupported_episode = false;
		const bool replay =
			g_replay_service.source == AIUS_REPLAY_SOURCE_REPLAY;
		const uint32_t expected_rate_hz =
			replay ? AIUS_OPTICAL_SAMPLE_RATE_HZ
			       : g_clock_nominal_rate_hz;
		const bool output_plan_ready =
			replay ? g_clock_replay_ready
			       : g_clock_plan_applied;
		if (!output_plan_ready) {
			close_stream_rate_gate();
			return AIUS_NO_SPACE;
		}
		if (block->sample_rate_hz != expected_rate_hz) {
			g_clock_mismatch_blocks++;
			close_stream_rate_gate();
			return AIUS_REFUSED;
		}
		g_clock_live_ready = !replay && g_clock_carrier_ready;
		const bool model_pending = g_dsp_apply_pending;
		if (model_pending)
			g_active_model = g_pending_dsp_model;
		if (model_pending ||
		    g_applied_stream_rate_hz != block->sample_rate_hz) {
			apply_config(g_active_model, block->sample_rate_hz);
			g_applied_stream_rate_hz = block->sample_rate_hz;
			if (!model_pending)
				g_config_generation++;
		}
		if (model_pending) {
			g_dsp_source_sd = true;
			g_config_generation++;
			g_dsp_apply_pending = false;
		}
		int32_t processed_left[AUDIO_BLOCK_SAMPLES * 4];
		int32_t processed_right[AUDIO_BLOCK_SAMPLES * 4];
		AiusPcmBlock processed = {};
		processed.planes[0] = processed_left;
		processed.planes[1] = processed_right;
		AiusDspConfig dsp = {};
		aius_dsp_runtime_config_bind(&g_dsp_runtime, &dsp);
		for (size_t p = 0; p < AIUS_MAX_CHANNELS; p++)
			dsp.scratch[p] = g_dsp_scratch_d[p];
		dsp.work_l = g_dsp_work_l;
		dsp.work_r = g_dsp_work_r;
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
		const bool metering = g_dsp_runtime.meters_active;
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
		/* Into the transmit ring, at OUTPUT_WIDTH_BITS.
		 *
		 * At 24 bits -- the shipped selection -- this is a copy and nothing else:
		 * the DSP's word IS the S/PDIF transmit word, so there is no dither, no
		 * rounding and no saturation left to do, and the ~-93 dBFS floor the old
		 * int16 hop imposed is gone. The stage that used to live here
		 * (aius_out16_run: TPDF dither, round to nearest, saturate) is not deleted
		 * -- aius_out_stage_run()'s 16-bit arm still runs it, and the dsp suite
		 * still pins it -- it is bypassed, which is the correct demotion once the
		 * path stops narrowing the word.
		 *
		 * Two calls because the destination is a ring the block can wrap.
		 * aius_spdif_tx24_stage() returns the contiguous run available at each
		 * offset and the free-space check above bounds frames, so there is at most
		 * one wrap; committing once at the end is what publishes the whole block to
		 * the transmit interrupt atomically. */
		int32_t *ring_left = nullptr;
		int32_t *ring_right = nullptr;
		uint32_t first = aius_spdif_tx24_stage(&g_aius_spdif_tx24, 0,
		                                       &ring_left, &ring_right);
		if (first > block->frames)
			first = block->frames;
		aius_out_stage_run(&g_out16, OUTPUT_WIDTH_BITS, processed_left,
		                   processed_right, ring_left, ring_right, first);
		if (first < block->frames) {
			(void)aius_spdif_tx24_stage(&g_aius_spdif_tx24, first,
			                            &ring_left, &ring_right);
			aius_out_stage_run(&g_out16, OUTPUT_WIDTH_BITS,
			                   processed_left + first, processed_right + first,
			                   ring_left, ring_right, block->frames - first);
		}
		aius_spdif_tx24_commit(&g_aius_spdif_tx24, block->frames);
		const uint32_t tx_level =
			aius_spdif_tx24_level(&g_aius_spdif_tx24);
		if (tx_level > g_tx_high_water_frames)
			g_tx_high_water_frames = tx_level;
		const uint32_t dropped_chunks = overrun_count();
		/* Item 3 capture tap. At OUTPUT_WIDTH_BITS = 24 the words just
		 * committed ARE processed_left/right verbatim (the out stage is a
		 * copy), so tapping the planes taps the wire. After the commit and
		 * before anything else so the capture covers exactly the committed
		 * frames; the latches are this block's config generation and overrun
		 * count, snapshotted from the ingress producer.
		 * Cost when no capture is in flight: one volatile load and compare
		 * inside the callee. */
		aius_measure_producer_block(&g_measure_capture, processed_left,
		                            processed_right, block->frames,
		                            g_config_generation, dropped_chunks);
		/* Flight-recorder tap: the same committed planes, into PSRAM staging.
		 * Producer and drain are serial runtime work. Sits beside the
		 * measure tap so both cover exactly the committed frames. The
		 * limiter gain is read in the call that just advanced it;
		 * the underrun counter is the tx24 ring's own volatile u32.
		 * Cost when no PSRAM is fitted: this one bool test. */
		if (g_recorder_present)
			aius_recorder_producer_block(
				&g_recorder, processed_left, processed_right,
				block->frames, dropped_chunks,
				g_aius_spdif_tx24.underruns,
				dsp.limiter != nullptr,
				dsp.limiter != nullptr
					? (float)dsp.limiter->state.gain
					: 1.0f,
				millis());
		g_blocks++;
		if (metering) {
			if (g_meter.out_frames >=
			    g_dsp_runtime.meter_window_frames) {
				/* Sampled once per window, not per block: the accessor takes a
				 * libm log10, and a diagnostic field does not buy a log10 in
				 * the block callback (§Deciding principle). The reported value
				 * is therefore the instantaneous gain reduction at the publish
				 * instant, not the window's peak. */
				aius_meter_set_limiter_gr(&g_meter,
					aius_dsp_limiter_gain_reduction_db(dsp.limiter));
				/* No division: microseconds per frame was resolved to Q16.16
				 * by apply_config(), and the level is bounded by
				 * AIUS_SPDIF_TX24_RING_FRAMES, so this is one 32x32 multiply
				 * and a shift. The level is the WHOLE transmit queue now --
				 * there is no vendor two-deep block queue behind it to be
				 * unaccounted for. */
				const uint32_t queued =
					aius_spdif_tx24_level(&g_aius_spdif_tx24);
				aius_meter_set_telemetry(&g_meter, queued,
					(uint32_t)(((uint64_t)queued *
					            g_dsp_runtime.meter_us_per_frame_q16) >> 16),
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
};

static FLASHMEM uint32_t audio_clock_us(void *)
{
	return micros();
}

static uint32_t codec_cycle_clock(void *)
{
	return ARM_DWT_CYCCNT;
}

static FLASHMEM void audio_enter_reset_critical(void *)
{
	NVIC_DISABLE_IRQ(IRQ_SOFTWARE);
}

static FLASHMEM void audio_exit_reset_critical(void *)
{
	NVIC_ENABLE_IRQ(IRQ_SOFTWARE);
}

FLASHMEM void AiusAudioProcessor::begin_pipeline(void)
{
	apply_config(AIUS_OPTICAL_CONFIG, AIUS_OPTICAL_SAMPLE_RATE_HZ);
	aius_out16_init(&g_out16, AIUS_DSP_DITHER_SEED_DEFAULT, 1);
	/* Bind the capture planes; memory only. The machine starts idle, and
	 * an idle capture costs the tap one volatile load per block. */
	(void)aius_measure_capture_init(&g_measure_capture,
	                                g_measure_capture_l,
	                                g_measure_capture_r,
	                                AIUS_MEASURE_CAPTURE_FRAMES);
	/* The first 32 KiB of fitted PSRAM is eight producer staging slots. The
	 * circular ring uses the largest power-of-two byte extent that fits after
	 * the carve: 4 MiB on an 8 MiB board, 8 MiB on a 16 MiB board. */
	uint8_t *const psram = reinterpret_cast<uint8_t *>(0x70000000u);
	const uint32_t psram_bytes =
		(uint32_t)external_psram_size * 1048576u;
	const uint32_t recorder_ring_bytes =
		aius_recorder_ring_bytes_after_staging(
			psram_bytes, RECORDER_STAGING_BYTES);
	g_recorder_present = recorder_ring_bytes >= AIUS_RECORDER_FRAME_BYTES &&
		aius_recorder_init(
			&g_recorder, psram + RECORDER_STAGING_BYTES,
			recorder_ring_bytes, external_psram_size,
			reinterpret_cast<int32_t *>(psram),
			g_recorder_staging_frames, RECORDER_STAGING_SLOTS,
			RECORDER_SLOT_FRAMES) == AIUS_OK;
	if (!g_recorder_present)
		(void)aius_recorder_init(&g_recorder, nullptr, 0u, 0u,
		                         nullptr, nullptr, 0u, 0u);
	/* Take the S/PDIF transmit DMA interrupt from the retained snapshot's
	 * 16-bit filler. After this the wire carries our 24-bit ring verbatim;
	 * before it, the vendor ISR transmits block_silent, which is the same
	 * silence the board idles in. Here rather than in setup() because this
	 * is the one function that owns bringing the audio path up, and it runs
	 * after the static AudioOutputSPDIF3 constructor has allocated the DMA
	 * channel. */
	aius_spdif_tx24_begin();
	aius_audio_clock_initialize();
	if (aius_arena_init(&g_arena, g_arena_memory, sizeof(g_arena_memory)) != 0) {
		g_create_status = AIUS_NO_SPACE;
		return;
	}
	/* The single-quantize chain's double staging, carved from the SAME
	 * DTCM arena the pipeline decodes in: six input planes (24,576 B) +
	 * the stereo work pair (8,192 B), once, before the pipeline claims
	 * the rest. OCRAM was the contracted home but the build's heap-floor
	 * gate (defect 26: NativeEthernet needs 128 KiB of boot heap) vetoed
	 * it -- OCRAM sat 13 KiB above the floor, this needs 32. The arena
	 * has the headroom instead: decode high-water 62,560 of 131,072, so
	 * the carve leaves ~36 KiB of margin, everything stays in DTCM (the
	 * fastest bank for the four per-block passes over the work pair),
	 * and DTCM free-for-locals still improves because the old 12,288 B
	 * int32 scratch member is gone. */
	for (size_t p = 0; p < AIUS_MAX_CHANNELS; p++) {
		g_dsp_scratch_d[p] = (double *)aius_arena_alloc(
			&g_arena, DSP_STAGE_FRAMES * sizeof(double), 16);
		if (g_dsp_scratch_d[p] == nullptr) {
			g_create_status = AIUS_NO_SPACE;
			return;
		}
	}
	g_dsp_work_l = (double *)aius_arena_alloc(
		&g_arena, DSP_STAGE_FRAMES * sizeof(double), 16);
	g_dsp_work_r = (double *)aius_arena_alloc(
		&g_arena, DSP_STAGE_FRAMES * sizeof(double), 16);
	if (g_dsp_work_l == nullptr || g_dsp_work_r == nullptr) {
		g_create_status = AIUS_NO_SPACE;
		return;
	}
	AiusPcmSink sink = {this, consume_block};
	AiusPipelineConfig config = {
		AIUS_IEC61937_PACK_16,
		0,
		{nullptr, codec_cycle_clock},
	};
	g_create_status = aius_pipeline_create_arena(&g_pipeline, &config,
	                                             &sink, &g_arena);
	if (g_create_status == AIUS_OK && g_audio_ingress_ready) {
		g_audio_service_ready =
			aius_audio_service_init(
				&g_audio_service, &g_audio_ingress, g_pipeline,
				audio_clock_us, nullptr,
				audio_enter_reset_critical,
				audio_exit_reset_critical, nullptr) == AIUS_OK;
		if (!g_audio_service_ready)
			g_create_status = AIUS_INVALID_ARG;
	}
}

FLASHMEM void AiusAudioProcessor::apply_config(const AiusOpticalConfig &cfg,
	                                            uint32_t stream_rate_hz)
{
	(void)aius_dsp_runtime_config_apply(
		&g_dsp_runtime, &cfg, stream_rate_hz, &g_meter);
}

uint8_t AiusAudioProcessor::g_arena_memory[AiusAudioProcessor::ARENA_BYTES]
	__attribute__((aligned(16)));

static AiusAudioProcessor g_audio_processor;
/* STILL CONSTRUCTED, DELIBERATELY, AND DELIBERATELY NOT CONNECTED.
 *
 * Its constructor is what runs begin() -> config_spdif3(): the audio PLL, the
 * whole transmit clock tree, CORE_PIN14 as SPDIF_OUT, the transmit DMA channel
 * and the double-buffered SPDIF_tx_buffer. All of that is still exactly what we
 * want; it is only the ISR's int16 source that the 24-bit path replaces, and
 * aius_spdif_tx24_begin() replaces it by pointing the same DMA channel's
 * interrupt somewhere else.
 *
 * THE TWO CONNECTIONS INTO IT ARE GONE, and that is load-bearing rather than
 * tidying. AudioConnection::connect() is the only thing that sets
 * AudioStream::active, so with no connection AudioOutputSPDIF3::update() is
 * never called -- which is required, because it would queue audio_block_t into
 * block_left_1st/2nd that nothing now releases, exhausting the 24-block pool
 * within a second and turning every subsequent allocate() into an overrun.
 *
 * update_setup() still ran inside begin(), so the clock-responsibility contract
 * is untouched: g_spdif_in is constructed above and claims it first,
 * g_spdif_out gets false, and replay_clock_service() moves it as before. */
static AudioOutputSPDIF3 g_spdif_out;
static AudioConnection g_spdif_left(g_spdif_in, 0, g_audio_processor, 0);
static AudioConnection g_spdif_right(g_spdif_in, 1, g_audio_processor, 1);

/* ---- optical transmit gate (bench actuator) --------------------------------
 *
 * The final product holds the transmitter dark electronically (TX_ENABLE into
 * the 1G126's OE); the bench build, wired jack-direct, enforces the same
 * policy -- no input carrier and no replay, no output carrier -- by routing
 * pad 14 away from SPDIF_OUT. The predicate lives in aius_tx_gate.h so the
 * carrier firmware later drives pad 39 from the identical boolean.
 *
 * THE ACTUATOR IS THE PAD MUX, NOT THE PERIPHERAL. SPDIF3, its PLL, the
 * transmit DMA and the tx24 ring all keep running into a dark pad (the same
 * drain the on-stop analysis at replay stop describes), so opening the gate
 * is one register store with no peripheral state to rebuild. Dark is GPIO
 * driven-low via direct register writes -- data first, then direction, then
 * mux -- because the OTJ-5's Vin low is "LED off". Bare pinMode() is
 * DELIBERATELY not used: digital.c's pinMode rewrites the pad's PADCONFIG
 * (DSE(7) + SION) that S/PDIF has run on at reset-default since the item 3
 * measurements; these three stores touch mux, direction and data only. The
 * SPDIF_OUT routing value 3 is what AudioOutputSPDIF3::begin() wrote above
 * (output_spdif3.cpp:94, "3:SPDIF_OUT"); the gate is the pad's only writer
 * after boot.
 *
 * A ~0.1-2 ms silence-carrier blip at static construction (~300 ms after
 * every reset, begin() starting DMA before setup() can run) is unavoidable
 * and recorded in the plan; setup()'s first act is the dark-set that ends
 * it. Routine evaluation lives in tx_gate_service() at checkpoint 7; an AUDIO
 * block whose rate contract fails closes it immediately before returning. */

static FLASHMEM void tx_gate_actuate(bool open)
{
	if (open) {
		CORE_PIN14_CONFIG = 3; /* SPDIF_OUT, as begin() routed it */
		return;
	}
	CORE_PIN14_PORTCLEAR = CORE_PIN14_BITMASK;
	CORE_PIN14_DDRREG |= CORE_PIN14_BITMASK;
	CORE_PIN14_CONFIG = 5; /* ALT5 GPIO, now driving low */
}

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
 *   55..70  four HTTP counters          reinstated: the first three are
 *                                       observed at the runtime seam
 *                                       (AiusRuntimeNetworkHttp, where the
 *                                       transaction's actions and faults are
 *                                       visible), the fourth is the root's
 *                                       own checkpoint timing
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
	aius_net_put_u32(payload + 55, g_runtime.network_http.http_requests);
	aius_net_put_u32(payload + 59, g_runtime.network_http.http_rejections);
	aius_net_put_u32(payload + 63,
	                 g_runtime.network_http.http_client_drops);
	aius_net_put_u32(payload + 67, g_http_service_overruns);
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
 *   74      last_checkpoint         0..10, the last checkpoint persisted
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
	/* lfe_crossover_hz is required by the PUT validator, so omitting it here
	 * would make a GET -> PUT round-trip fail 400 on a missing key.
	 *
	 * layout_aware is the mirror hazard and the reason it is emitted even though
	 * the validator would accept its absence: parse-without-emit makes every
	 * GET-modify-PUT silently reset a hand-set false back to true, because absent
	 * reads ON. The rule for this endpoint is that the two key sets match. */
	if (!http_json_append(output, capacity, &length,
	                      ",\"matrix\":{\"center_gain_linear\":%.17g,\"surround_gain_linear\":%.17g,\"lfe_gain_linear\":%.17g,\"lfe_crossover_hz\":%.17g,\"headroom_linear\":%.17g,\"clipfree\":%s,\"layout_aware\":%s},",
	                      cfg.matrix.center_gain_linear, cfg.matrix.surround_gain_linear,
	                      cfg.matrix.lfe_gain_linear, cfg.matrix.lfe_crossover_hz,
	                      headroom, cfg.matrix.clipfree ? "true" : "false",
	                      cfg.matrix.layout_aware ? "true" : "false"))
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
 * The raw-sample analysis window adds the rest. A sample is at worst
 * "-8388608" -- 8 characters, because the window carries 24-bit sample units,
 * NOT int32 -- and the separators are one fewer than the count:
 *
 *   ,"analysis_window":[                                            20
 *   1024 x 8 characters + 1023 separators                          9215
 *   ],"analysis_full_scale":8388607                                  31
 *                                                                 -----
 *                                                                  9266
 *
 * 1472 + 9266 = 10,738 against the 12,288-byte AIUS_RUNTIME_TEENSY_APP_BODY,
 * so the full window fits on the existing route with ~1.5 kB spare and needs
 * neither a shorter window nor a second endpoint. AIUS_METER_ANALYSIS_WINDOW
 * is pinned below because that headroom is the reason it can be 1024: at 2048
 * the body would be ~20 kB and the arithmetic stops working.
 *
 * Rounded up for the NUL and for arithmetic drift. Every append is still
 * capacity-checked, so exceeding it would be a deterministic 0 (-> 500), never
 * an overrun. */
static constexpr size_t AIUS_METERS_MAX_JSON_BYTES = 10880u;
static_assert(AIUS_METERS_MAX_JSON_BYTES <= AIUS_RUNTIME_TEENSY_APP_BODY,
	"the meters body must fit the runtime's application response buffer");
static_assert(AIUS_METER_ANALYSIS_WINDOW == 1024u,
	"the meters body arithmetic above is computed for a 1024-sample window");
/* 9 bytes is the widest one sample can serialize to: a separator plus
 * "-8388608". The window's own share of the body, checked against the total. */
static_assert(1472u + 20u + AIUS_METER_ANALYSIS_WINDOW * 9u + 31u <=
		AIUS_METERS_MAX_JSON_BYTES,
	"the analysis window does not fit the meters body budget");

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

/* One analysis-window sample, optionally comma-separated, appended as a plain
 * decimal integer. Same contract as http_json_append(): false on capacity
 * exhaustion, and the body stays NUL-terminated either way.
 *
 * WHY NOT http_json_append(). It is one vsnprintf() per call, and this array
 * is 1024 elements against the HTTP checkpoint's 2 ms service_budget_ms --
 * enough to spend the whole budget on formatting alone and tick
 * http_service_overruns on every poll of a route the SD editor polls at 10 Hz.
 * Emitting the digits directly is a few tens of instructions per sample. The
 * envelope loop below is left on http_json_append(): 128 points is not worth
 * a second spelling. */
static FLASHMEM bool meter_append_sample(char *body, size_t capacity,
                                         size_t *length, int32_t sample,
                                         bool separator)
{
	/* Widest output: ',' + '-' + the 10 digits of any uint32. */
	char text[12];
	size_t written = 0;
	if (separator)
		text[written++] = ',';
	/* Widened before the negation so INT32_MIN has no undefined behaviour. */
	uint32_t magnitude;
	if (sample < 0) {
		text[written++] = '-';
		magnitude = (uint32_t)-(int64_t)sample;
	} else {
		magnitude = (uint32_t)sample;
	}
	char digits[10];
	size_t count = 0;
	do {
		digits[count++] = (char)('0' + (magnitude % 10u));
		magnitude /= 10u;
	} while (magnitude != 0u);
	while (count > 0)
		text[written++] = digits[--count];
	/* +1 for the NUL, so the body a caller may still printf into is valid. */
	if (*length + written + 1u > capacity)
		return false;
	memcpy(body + *length, text, written);
	*length += written;
	body[*length] = '\0';
	return true;
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
 * snapshot copies to happen at all -- against a writer that publishes 20 times
 * a second and completes in about a microsecond.
 *
 * The raw-sample analysis window rides on this same response rather than a
 * second route: it fits the body budget (see the arithmetic above), it is the
 * same snapshot at the same instant as the levels beside it, and a separate
 * GET would let a client pair a window with levels from a different publish.
 * It is FEATURE-DETECTED by its consumer, so a board that has never published
 * serves "analysis_window":[] and the analyser simply does not light up --
 * which is also exactly what features.meters false produces, with no
 * special case anywhere on this path. */
static FLASHMEM size_t http_meters_json(uint8_t *body, size_t capacity)
{
	/* STATIC, not automatic: the snapshot is ~4.7 kB now that it carries the
	 * analysis window, and the DTCM free-for-locals zone is ~48 kB. Safe
	 * because this runs only from app_write_meters() on the main thread, one
	 * request at a time and never reentrantly; DMAMEM keeps it off DTCM for
	 * the reason g_meter is there, and it needs no startup zeroing because
	 * aius_meter_read() overwrites it before any read. */
	DMAMEM static AiusMeterSnapshot snapshot;
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
	/* The raw mono window, oldest first, in 24-bit sample units -- unscaled
	 * and unconverted, because a client that wants a spectrum needs the
	 * signal, not a rendering of it. analysis_full_scale states what "0 dBFS"
	 * is for these integers; without it a consumer would have to guess, and
	 * the obvious guess is int16. */
	if (!http_json_append(output, capacity, &length, "],\"analysis_window\":["))
		return 0;
	uint32_t samples = snapshot.analysis_count;
	if (samples > AIUS_METER_ANALYSIS_WINDOW)
		samples = AIUS_METER_ANALYSIS_WINDOW;
	for (uint32_t sample = 0; sample < samples; sample++) {
		if (!meter_append_sample(output, capacity, &length,
		                         snapshot.analysis_window[sample], sample != 0))
			return 0;
	}
	return http_json_append(output, capacity, &length,
	                        "],\"analysis_full_scale\":%ld,"
	                        "\"queue_frames\":%lu,\"estimated_latency_us\":%lu,"
	                        "\"dsp_cycles_last\":%lu,\"dsp_cycles_max\":%lu}",
	                        (long)AIUS_DSP_SAMPLE_MAX,
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
	                      ",\"%s\":{\"generation\":%lu,"
	                      "\"stack_started_ms\":%lu,\"netif_init\":%u,"
	                      "\"stack_start_blocking_ms\":%u,"
	                      "\"link_at_stack_start\":%u,\"link_up_source\":%u,",
	                      key, (unsigned long)diag->generation,
	                      /* The record's one absolute value, and the only key
	                       * that anchors every *_ms offset below to board
	                       * uptime. Without it a reader can time the phases of
	                       * a generation relative to each other but cannot say
	                       * when the generation itself began -- which is what
	                       * separates "the stack came up slowly" from "the
	                       * stack started late", and those have different
	                       * causes and different fixes. */
	                      (unsigned long)diag->stack_started_ms_absolute,
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
	/* What released the receive-readiness gate, and when. This is the field
	 * that says whether the gate did its job -- COUNTER means a frame arrived
	 * and DHCP started on evidence, DEADLINE means nothing arrived inside the
	 * bound and DHCP started anyway -- and without it the gap between link-up
	 * and dhcp_start_ms is unattributed. */
	if (!http_json_append(output, capacity, length,
	                      ",\"rx_ready_source\":%u,\"rx_ready_ms\":",
	                      (unsigned)diag->rx_ready_source))
		return false;
	if (!((diag->rx_ready_source == AIUS_NETWORK_TEENSY_RX_READY_COUNTER ||
	       diag->rx_ready_source == AIUS_NETWORK_TEENSY_RX_READY_DEADLINE)
		      ? http_json_append(output, capacity, length, "%lu",
		                         (unsigned long)diag->rx_ready_ms)
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
/* The refill job's diagnostics (see ReplayJobDiag). Every field is attribution
 * the adapter's single running maximum and the storage service's single failure
 * record could not carry:
 *
 *   longest_primitive_us + longest_action   WHICH card action blocked longest
 *   last_fault{,_phase,_action,_driver}     WHICH primitive failed, and why
 *   last_fault_offset_bytes + _loops        WHERE in the file, and on which lap
 *   mounts                                  REAL remounts this job caused
 *   mount_reuses                            BEGINs the adapter answered from
 *                                           its cached mount (adapter-global)
 *
 * The first run of this block is what identified the defect it now measures:
 * longest_action came back AIUS_SD_ACTION_BEGIN with mounts equal to jobs, which
 * said the 1.02 s stall was the per-job remount and not a read.
 *
 * `fault_phase` is the load-bearing one: a terminal FAILED has already moved
 * `phase` on to the cleanup arm, so without the latched copy an open failure and
 * a read failure are indistinguishable after the fact. */
/* AUDIO checkpoint diagnostics. Producer-owned ingress fields are copied under
 * the same brief mask used by the legacy overrun projection; service, pipeline
 * and epoch counters are main-thread-owned. */
static FLASHMEM bool http_audio_diag_json(char *output, size_t capacity,
	                                      size_t *length)
{
	AiusAudioIngressDiag ingress;
	AiusAudioServiceDiag service;
	AiusPipelineServiceDiag pipeline;

	memset(&ingress, 0, sizeof(ingress));
	NVIC_DISABLE_IRQ(IRQ_SOFTWARE);
	aius_audio_ingress_get_diag(&g_audio_ingress, &ingress);
	NVIC_ENABLE_IRQ(IRQ_SOFTWARE);
	aius_audio_service_get_diag(
		g_audio_service_ready ? &g_audio_service : nullptr, &service);
	aius_pipeline_get_service_diag(
		g_audio_processor.create_status() == AIUS_OK
			? g_audio_service.pipeline
			: nullptr,
		&pipeline);

	return http_json_append(
		output, capacity, length,
		",\"audio_diag\":{"
		"\"ingress_level_words\":%lu,"
		"\"ingress_high_water_words\":%lu,"
		"\"ingress_generation\":%lu,"
		"\"ingress_dropped_chunks\":%lu,"
		"\"ingress_dropped_words\":%lu,"
		"\"ingress_drop_overflow\":%lu,"
		"\"ingress_drop_missing_live\":%lu,"
		"\"ingress_drop_short_replay\":%lu,"
		"\"source_changes\":%lu,"
		"\"source_flushes\":%lu,"
		"\"source_flush_words\":%lu,"
		"\"gap_flushes\":%lu,"
		"\"gap_flush_words\":%lu,"
		"\"checkpoint_calls\":%lu,"
		"\"input_words_consumed\":%lu,"
		"\"service_steps_attempted\":%lu,"
		"\"service_steps_committed\":%lu,"
		"\"service_generation_resets\":%lu,"
		"\"service_requested_resets\":%lu,"
		"\"service_max_us\":%lu,"
		"\"service_max_cycles\":%lu,"
		"\"service_budget_overruns\":%lu,"
		"\"service_last_failure\":%d,"
		"\"transmit_stops\":%lu,"
		"\"decode_units\":%lu,"
		"\"sink_attempts\":%lu,"
		"\"committed_output_blocks\":%lu,"
		"\"transmit_stalls\":%lu,"
		"\"retained_retries\":%lu,"
		"\"parse_rejects\":%lu,"
		"\"decode_rejects\":%lu,"
		"\"output_drops\":%lu,"
		"\"au_abandons\":%lu,"
		"\"pipeline_last_failure\":%d,"
		"\"tx_level_frames\":%lu,"
		"\"tx_high_water_frames\":%lu,"
		"\"runtime_epoch_max_us\":%lu,"
		"\"runtime_epoch_budget_overruns\":%lu}",
		(unsigned long)ingress.level_words,
		(unsigned long)ingress.high_water_words,
		(unsigned long)ingress.generation,
		(unsigned long)ingress.dropped_chunks,
		(unsigned long)ingress.dropped_words,
		(unsigned long)ingress.dropped_by_cause[
			AIUS_AUDIO_INGRESS_GAP_OVERFLOW],
		(unsigned long)ingress.dropped_by_cause[
			AIUS_AUDIO_INGRESS_GAP_MISSING_LIVE],
		(unsigned long)ingress.dropped_by_cause[
			AIUS_AUDIO_INGRESS_GAP_SHORT_REPLAY],
		(unsigned long)ingress.source_changes,
		(unsigned long)ingress.source_flushes,
		(unsigned long)ingress.source_flush_words,
		(unsigned long)ingress.gap_flushes,
		(unsigned long)ingress.gap_flush_words,
		(unsigned long)service.checkpoint_calls,
		(unsigned long)service.input_words_consumed,
		(unsigned long)service.service_steps_attempted,
		(unsigned long)service.service_steps_committed,
		(unsigned long)service.generation_resets,
		(unsigned long)service.requested_resets,
		(unsigned long)service.max_service_us,
		(unsigned long)g_audio_checkpoint_max_cycles,
		(unsigned long)service.budget_overruns,
		(int)service.last_failure,
		(unsigned long)service.transmit_stops,
		(unsigned long)pipeline.decode_units,
		(unsigned long)pipeline.sink_attempts,
		(unsigned long)pipeline.committed_blocks,
		(unsigned long)pipeline.transmit_stalls,
		(unsigned long)pipeline.retained_retries,
		(unsigned long)pipeline.parse_rejects,
		(unsigned long)pipeline.decode_rejects,
		(unsigned long)pipeline.output_drops,
		(unsigned long)pipeline.au_abandons,
		(int)pipeline.last_failure,
		(unsigned long)aius_spdif_tx24_level(&g_aius_spdif_tx24),
		(unsigned long)g_tx_high_water_frames,
		(unsigned long)g_runtime_epoch_max_us,
		(unsigned long)g_runtime_epoch_budget_overruns);
}

static FLASHMEM bool http_clock_json(char *output, size_t capacity,
	                                 size_t *length)
{
	AiusAudioIngressDiag ingress;
	const bool output_master = clock_output_is_master();
	const uint32_t status_nominal_rate_hz =
		output_master ? AIUS_OPTICAL_SAMPLE_RATE_HZ
		              : g_clock_nominal_rate_hz;
	int32_t trim_ppm = 0;
	if (status_nominal_rate_hz != 0u) {
		const int64_t nominal_millihz =
			(int64_t)status_nominal_rate_hz * INT64_C(1000);
		trim_ppm = (int32_t)(
			((int64_t)g_clock_output_rate_millihz -
			 nominal_millihz) *
			INT64_C(1000000) / nominal_millihz);
	}
	const char *state =
		output_master ? "replay" :
		g_clock_fault_episode ? "fault" :
		!g_tx_gate_carrier ? "unlocked" :
		g_clock_carrier_unsupported ? "unsupported" :
		g_clock_carrier_ready
			? (g_clock_plan_applied ? "tracking" : "refused")
			: "acquiring";

	memset(&ingress, 0, sizeof(ingress));
	NVIC_DISABLE_IRQ(IRQ_SOFTWARE);
	aius_audio_ingress_get_diag(&g_audio_ingress, &ingress);
	NVIC_ENABLE_IRQ(IRQ_SOFTWARE);

	return http_json_append(
		output, capacity, length,
		",\"clock\":{"
		"\"carrier_locked\":%s,"
		"\"carrier_ready\":%s,"
		"\"carrier_rate_millihz\":%lu,"
		"\"nominal_rate_hz\":%lu,"
		"\"stream_rate_hz\":%lu,"
		"\"output_rate_millihz\":%lu,"
		"\"trim_ppm\":%ld,"
		"\"master\":\"%s\","
		"\"state\":\"%s\","
		"\"rate_changes\":%lu,"
		"\"trim_updates\":%lu,"
		"\"structural_replans\":%lu,"
		"\"invalid_windows\":%lu,"
		"\"clock_faults\":%lu,"
		"\"last_clock_fault\":%lu,"
		"\"mismatch_blocks\":%lu,"
		"\"unsupported_blocks\":%lu,"
		"\"unsupported_episodes\":%lu,"
		"\"tx_level_frames\":%lu,"
		"\"tx_underrun_frames\":%lu,"
		"\"ingress_level_words\":%lu,"
		"\"ingress_high_water_words\":%lu}",
		g_tx_gate_carrier ? "true" : "false",
		g_clock_carrier_ready ? "true" : "false",
		(unsigned long)g_clock_carrier_rate_millihz,
		(unsigned long)status_nominal_rate_hz,
		(unsigned long)g_clock_stream_rate_hz,
		(unsigned long)g_clock_output_rate_millihz,
		(long)trim_ppm, output_master ? "output" : "input", state,
		(unsigned long)g_clock_rate_changes,
		(unsigned long)g_clock_trim_updates,
		(unsigned long)g_clock_structural_replans,
		(unsigned long)g_clock_invalid_windows,
		(unsigned long)g_clock_faults,
		(unsigned long)g_clock_last_fault,
		(unsigned long)g_clock_mismatch_blocks,
		(unsigned long)g_clock_unsupported_blocks,
		(unsigned long)g_clock_unsupported_episodes,
		(unsigned long)aius_spdif_tx24_level(&g_aius_spdif_tx24),
		(unsigned long)g_aius_spdif_tx24.underruns,
		(unsigned long)ingress.level_words,
		(unsigned long)ingress.high_water_words);
}

/* The pipeline's own record -- the classifier's verdict and the decode path's
 * swallowed faults. Deferred service skips a faulted burst and keeps
 * streaming (a decode error must not gap the wire), which left defect 29's
 * silent decoder invisible from every surface: state 4, zero output, and
 * `last_status` trapped in RAM. AUDIO and HTTP are serial epoch work;
 * `metrics_valid` is 0 only when the pipeline never constructed. */
static FLASHMEM bool http_pipeline_diag_json(char *output, size_t capacity,
	                                         size_t *length)
{
	AiusMetrics metrics;
	AiusMetrics dts_metrics;
	AiusMetrics ac3_metrics;
	AiusIecDiag iec;
	AiusPipelineServiceDiag service;

	memset(&metrics, 0, sizeof(metrics));
	memset(&dts_metrics, 0, sizeof(dts_metrics));
	memset(&ac3_metrics, 0, sizeof(ac3_metrics));
	memset(&iec, 0, sizeof(iec));
	memset(&service, 0, sizeof(service));
	const AiusPipelineClass klass = g_audio_processor.classification();
	const uint16_t codec_pc = g_audio_processor.pipeline_codec_pc();
	const AiusStatus got = g_audio_processor.pipeline_metrics(&metrics);
	(void)g_audio_processor.pipeline_codec_metrics(11u, &dts_metrics);
	(void)g_audio_processor.pipeline_codec_metrics(1u, &ac3_metrics);
	const uint32_t shared_arena_high_water =
		dts_metrics.arena_high_water > ac3_metrics.arena_high_water
			? dts_metrics.arena_high_water
			: ac3_metrics.arena_high_water;
	(void)g_audio_processor.parser_diag(&iec);
	g_audio_processor.pipeline_service_diag(&service);
	/* Defect 28: lifetime re-classification counters. On a live board a
	 * non-zero retype_count is what distinguishes a mid-stream re-type from
	 * a reboot that happened to classify DTS. */
	uint32_t retype = 0;
	uint32_t reclassify = 0;
	(void)g_audio_processor.reclass_counters(&retype, &reclassify);
	/* The confirmation squelch: squelched_frames advancing with pcm_confirmed
	 * false is the signature of a mis-typed run being dropped rather than
	 * played -- the seam that used to be audible as periodic buzz. */
	uint32_t squelched_frames = 0;
	int pcm_confirmed = 0;
	(void)g_audio_processor.squelch_diag(&squelched_frames, &pcm_confirmed);

	return http_json_append(output, capacity, length,
	                        ",\"pipeline_diag\":{\"class\":%u,"
	                        "\"codec_pc\":%u,"
	                        "\"metrics_valid\":%u,\"last_status\":%d,"
	                        "\"create_status\":%d,"
	                        "\"decode_cycles\":%llu,"
	                        "\"arena_high_water\":%lu,"
	                        "\"alloc_count\":%lu,"
	                        "\"dts_decode_cycles\":%llu,"
	                        "\"dts_alloc_count\":%lu,"
	                        "\"ac3_decode_cycles\":%llu,"
	                        "\"ac3_alloc_count\":%lu,"
	                        "\"decode_rejects\":%lu,"
	                        "\"bursts_ok\":%lu,"
	                        "\"rejects_no_space\":%lu,"
	                        "\"rejects_malformed\":%lu,"
	                        "\"rejects_unsupported\":%lu,"
	                        "\"resyncs\":%lu,"
	                        "\"last_reject_pc\":%u,"
	                        "\"last_reject_pd\":%u,"
	                        "\"retype_count\":%lu,"
	                        "\"reclassify_count\":%lu,"
	                        "\"squelched_frames\":%lu,"
	                        "\"pcm_confirmed\":%u}",
	                        (unsigned)klass,
	                        (unsigned)codec_pc,
	                        got == AIUS_OK ? 1u : 0u,
	                        (int)metrics.last_status,
	                        (int)g_audio_processor.create_status(),
	                        (unsigned long long)metrics.decode_cycles,
	                        (unsigned long)shared_arena_high_water,
	                        (unsigned long)metrics.alloc_count,
	                        (unsigned long long)dts_metrics.decode_cycles,
	                        (unsigned long)dts_metrics.alloc_count,
	                        (unsigned long long)ac3_metrics.decode_cycles,
	                        (unsigned long)ac3_metrics.alloc_count,
	                        (unsigned long)service.decode_rejects,
	                        (unsigned long)iec.bursts_ok,
	                        (unsigned long)iec.rejects_no_space,
	                        (unsigned long)iec.rejects_malformed,
	                        (unsigned long)iec.rejects_unsupported,
	                        (unsigned long)iec.resyncs,
	                        (unsigned)iec.last_reject_pc,
	                        (unsigned)iec.last_reject_pd,
	                        (unsigned long)retype,
	                        (unsigned long)reclassify,
	                        (unsigned long)squelched_frames,
	                        (unsigned)(pcm_confirmed ? 1 : 0));
}

static FLASHMEM bool http_replay_diag_json(char *output, size_t capacity,
	                                       size_t *length)
{
	return http_json_append(output, capacity, length,
	                        ",\"replay_diag\":{\"jobs\":%lu,\"faults\":%lu,"
	                        "\"mounts\":%lu,\"mount_reuses\":%lu,"
	                        "\"consecutive_faults\":%lu,"
	                        "\"last_primitive_us\":%lu,"
	                        "\"longest_primitive_us\":%lu,"
	                        "\"longest_action\":%u,\"last_fault\":%u,"
	                        "\"last_fault_phase\":%u,"
	                        "\"last_fault_action\":%u,"
	                        "\"last_fault_driver\":%lu,"
	                        "\"last_fault_offset_bytes\":%lu,"
	                        "\"last_fault_loops\":%lu,"
	                        "\"last_fault_at_ms\":%lu}",
	                        (unsigned long)g_replay_diag.jobs,
	                        (unsigned long)g_replay_diag.faults,
	                        (unsigned long)g_replay_diag.mounts,
	                        /* Adapter-global, not replay-scoped: every
	                         * consumer's BEGINs share one cache, and a
	                         * per-job attribution would claim a precision
	                         * this counter does not have. */
	                        (unsigned long)
					g_runtime.storage.target.mount_reuses,
	                        (unsigned long)g_replay_service.refill_faults,
	                        (unsigned long)g_replay_diag.last_primitive_us,
	                        (unsigned long)g_replay_diag.longest_primitive_us,
	                        (unsigned)g_replay_diag.longest_action,
	                        (unsigned)g_replay_diag.last_fault,
	                        (unsigned)g_replay_diag.last_fault_phase,
	                        (unsigned)g_replay_diag.last_fault_action,
	                        (unsigned long)g_replay_diag.last_fault_driver,
	                        (unsigned long)
					g_replay_diag.last_fault_offset_bytes,
	                        (unsigned long)g_replay_diag.last_fault_loops,
	                        (unsigned long)g_replay_diag.last_fault_at_ms);
}

/* The boot-configuration fuse word, hex-encoded with its two decoded fields.
 *
 * This is prerequisite 4 of docs/plans/aius-ota-upgrade.md brought within reach
 * of a browser: whether PJRC blows 0x6E0[23:16] on a Teensy 4.1 is the SINGLE
 * gating fact for hardware A/B image selection on this part, it is documented
 * nowhere PJRC publishes, and it cannot be answered by reading anything -- only
 * by asking a board. Serving it beside boot_generation makes the answer a URL
 * rather than a bench session with a debugger.
 *
 * The full word is served as hex rather than as a number because it is a bit
 * field, not a quantity, and because the fields this firmware does not decode
 * are still in there -- a later reader who learns what 0x6E0[31:24] means can
 * recover it from an old capture. The two decoded fields are served beside it
 * so nobody has to shift by hand:
 *
 *   sec_image_offset  [23:16]  secondary image base = 256 KiB x this. ZERO on
 *                              a shipped part, and zero is exactly the finding
 *                              that kills hardware A/B: the secondary image
 *                              would live at the primary's address.
 *   sec_image_size    [15:12]
 *
 * `read` is false on a host build, where `word` is AIUS_BOOT_FUSE_UNREAD and
 * decodes to values no board can produce. A client must check it before
 * concluding anything, and cannot be misled if it forgets.
 *
 * READ ONLY. Nothing on this path -- here, aius_boot_teensy_boot_fuse(), or
 * anything either of them calls -- writes OCOTP in any form. The burn is
 * irreversible and per board. */
static FLASHMEM bool http_boot_fuse_json(char *output, size_t capacity,
	                                     size_t *length)
{
	const AiusBootFuse fuse = aius_boot_teensy_boot_fuse();

	return http_json_append(output, capacity, length,
	                        ",\"boot_fuse_0x6e0\":{\"word\":\"0x%08lx\","
	                        "\"read\":%s,\"sec_image_offset\":%u,"
	                        "\"sec_image_size\":%u}",
	                        (unsigned long)fuse.word,
	                        fuse.read ? "true" : "false",
	                        (unsigned)fuse.sec_image_offset,
	                        (unsigned)fuse.sec_image_size);
}

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
 *   http_server_rebinds   0 for good: the concept it counted -- an Ethernet
 *                         server object dying and being re-begun mid-run --
 *                         does not exist post-cutover, where the listener is
 *                         bound once per network generation. The nearest
 *                         real fact is network_restarts, already served
 *   the four http_* counters   reinstated (see put_status()), still read
 *                         back from the payload's own bytes 55..70, so JSON
 *                         and UDP cannot disagree about what is being served
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
		"{\"version\":%u,\"state\":%u,\"carrier\":%u,\"format\":%u,\"sample_rate\":%lu,\"blocks\":%lu,\"overruns\":%lu,\"source\":\"%s\",\"sd_present\":%u,\"sd_filesystem\":\"%s\",\"sd_total_bytes\":%llu,\"sd_used_bytes\":%llu,\"sd_free_bytes\":%llu,\"config_valid\":%u,\"config_error_code\":%u,\"config_error\":\"%s\",\"config_error_count\":%lu,\"network_mode\":\"%s\",\"ip\":\"%u.%u.%u.%u\",\"link\":%u,\"network_restarts\":%lu,\"dhcp_pending\":%u,\"dhcp_discovers\":%lu,\"dhcp_updates\":%lu,\"reset_source\":%lu,\"http_server_rebinds\":%lu,\"cpu_temp_c\":%.2f,\"cpu_temp_valid\":%u,\"cpu_temp_max_c\":%.2f,\"cpu_temp_age_ms\":%lu,\"build_revision\":\"%s\",\"build_utc\":\"%s\",\"event_sequence\":%lu,\"event_overflows\":%lu,\"network_drops\":%lu,\"network_budget_overruns\":%lu,\"http_requests\":%lu,\"http_rejections\":%lu,\"http_client_drops\":%lu,\"http_service_overruns\":%lu,\"network_state\":%u,\"recovery_safe\":%u,\"network_last_fault\":%u,\"last_checkpoint\":%u,\"network_generation\":%lu,\"boot_generation\":%lu,\"watchdog_early_resets\":%u,\"loop_epoch\":%lu,\"psram_mb\":%u,\"psram_pad2_mb\":%u,\"tx_gate\":{\"open\":%u,\"carrier_locked\":%u,\"replay_running\":%u}}",
		status[0], status[2], status[3] & 1u, status[23],
		(unsigned long)aius_net_get_u32(status + 25),
		(unsigned long)aius_net_get_u32(status + 29),
		(unsigned long)aius_net_get_u32(status + 33),
		/* The effective DSP model's source, the same fact and the same
		 * spelling GET /api/dsp/config serves (http_dsp_config_json) -- not
		 * the audio source, which is `state`. */
		g_dsp_source_sd ? "sd" : "default",
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
		(unsigned long)recovery.loop_epoch,
		(unsigned)external_psram_size,
		(unsigned)g_psram_pad2_mb,
		/* The LATCHED checkpoint-7 decision and inputs, never re-derived
		 * here: re-reading pllLocked() at request time could disagree with
		 * the pad within one poll. */
		g_tx_gate_open ? 1u : 0u, g_tx_gate_carrier ? 1u : 0u,
		g_tx_gate_replay ? 1u : 0u);
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
	/* The storage service's own failure record, and the live upload progress
	 * beside it. Two SD failures that are identical on the wire -- a job
	 * refused because another owner held the volume, and a card that refused
	 * the write -- both surface as `500 sd_failure`, so the difference has to
	 * come from the device.
	 *
	 * The transaction is passed because it owns `upload_supplied` and the
	 * storage service does not; this is the composition root, so it is the one
	 * place both are in scope. Reading it here is a plain load of one field
	 * outside any transaction step, exactly as reboot_drain_complete() reads
	 * `state`, so it consumes no event and emits no action. */
	if (!aius_runtime_storage_diag_json(&g_runtime.storage,
	                                    &g_runtime.network_http.http,
	                                    (char *)body, capacity, &length))
		return 0;
	/* The refill job's own record, BESIDE sd_diag rather than inside it. The
	 * two are different job streams with different owners: sd_diag belongs to
	 * AiusRuntimeStorage, which never sees g_replay_job, so a refill fault has
	 * always been absent from it -- visible only as driver_faults ticking while
	 * `last_failure` described something else entirely. Folding this into that
	 * object would mean writing src/aius_runtime_storage.*, and conflating two
	 * job streams in one `last_failure` would make the next fault as hard to
	 * attribute as this one was. */
	if (!http_replay_diag_json((char *)body, capacity, &length))
		return 0;
	if (!http_audio_diag_json((char *)body, capacity, &length))
		return 0;
	if (!http_clock_json((char *)body, capacity, &length))
		return 0;
	if (!http_pipeline_diag_json((char *)body, capacity, &length))
		return 0;
	/* Last, and a plain register read rather than a latched counter: the
	 * OCOTP shadow is loaded once by the boot ROM and never changes, so
	 * reading it at request time cannot disagree with anything else in this
	 * body the way a re-derived live value could. */
	if (!http_boot_fuse_json((char *)body, capacity, &length))
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
		/* THE CONTROL SURFACE IS THE SD-HOSTED TREE, NOT THIS PAGE, and until
		 * this section existed nothing said so. Both SD pages already link back
		 * here ("Device status"), so the relationship was one-way: a browser
		 * that landed on / had no route to the editor at all, and the only
		 * SD-shaped link on the page was /api/sd, which serves {"ready":true}
		 * and lists nothing. That is the whole defect -- the files were served
		 * correctly the entire time and simply could not be reached.
		 *
		 * The subtree root is named explicitly because /sd/ is NOT a route: the
		 * dispatcher serves /sd/<dir>/, so /sd/ alone is a 404 and a link to it
		 * would look like the same bug from the other side. */
		"<h2>Control surface</h2>"
		"<p>The user interface is served from the SD card. This page is the "
		"fallback for when the card is absent.</p>"
		"<ul><li><a href=\"/sd/www/index.html\">DSP editor</a> - input trim and "
		"EQ, downmix, output EQ, limiter, live meters</li>"
		"<li><a href=\"/sd/www/replays.html\">Replay manager</a></li>"
		"<li><a href=\"/sd/www/status.html\">Device status</a></li>"
		"<li><a href=\"/sd/www/\">Browse /sd/www/</a> - the served SD web tree</li></ul>"
		"<p id=sd></p>"
		"<h2>API</h2><ul><li><a href=\"/api/status\">GET /api/status</a></li>"
		"<li><a href=\"/api/events\">GET /api/events</a></li>"
		"<li><code>POST /api/start {}</code></li>"
		"<li><code>POST /api/stop {}</code></li>"
		"<li><code>POST /api/clear {}</code></li>"
		"<li><code>POST /api/reboot {}</code></li>"
		"<li>Those four POST routes require <code>X-Aius-SD-Auth</code>.</li>"
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
		"<h2>Control</h2><p>These four send the token from the field above.</p><p>"
		"<button title=\"Enable test processing state\" onclick=go('/api/start')>Start</button> "
		"<button title=\"Disable test processing state\" onclick=go('/api/stop')>Stop</button> "
		"<button title=\"Clear audio and event counters\" onclick=go('/api/clear')>Clear counters</button> "
		"<button title=\"Restart the application without entering HalfKay\" onclick=reboot()>Reboot</button></p>"
		"<pre id=s>Loading status...</pre><p id=e></p></body><script>"
		"const s=document.getElementById('s'),e=document.getElementById('e'),u=document.getElementById('u'),d=document.getElementById('dsp');"
		"async function get(){try{const j=await (await fetch('/api/status')).json();s.textContent=JSON.stringify(j,null,2);document.getElementById('sd').textContent=j.sd_present?'':'No SD card is mounted, so the three links above will not resolve.';e.textContent=''}catch(a){e.textContent=a}}"
		"async function getDsp(){try{const r=await fetch('/api/dsp/config');if(!r.ok)throw Error('HTTP '+r.status);d.textContent=JSON.stringify(await r.json(),null,2)}catch(a){d.textContent='DSP model unavailable: '+a}}"
		"async function go(a,b){try{await fetch(a,{method:'POST',headers:{'Content-Type':'application/json','X-Aius-SD-Auth':document.getElementById('t').value||'-'},body:JSON.stringify(b||{})});get()}catch(c){e.textContent=c}}"
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
	/* Served alongside `state` rather than folded into it: `stopped` is the
	 * portable service's own vocabulary and stays exactly what it was, while
	 * this says who stopped it. A client that never looks reads the same
	 * document it read before. */
	const char *stop_reason =
		g_replay_stop_reason == REPLAY_STOP_CARRIER ? "carrier" :
		g_replay_stop_reason == REPLAY_STOP_REQUEST ? "request" :
		g_replay_stop_reason == REPLAY_STOP_FAULT ? "fault" : "none";
	int length = snprintf((char *)body, capacity,
		"{\"ok\":true,\"state\":\"%s\",\"stop_reason\":\"%s\",\"id\":\"%s\",\"file_bytes\":%lu,\"offset_bytes\":%lu,\"duration_seconds\":%.3f,\"loop\":%s,\"loops_completed\":%lu,\"queue_words\":%lu,\"watermark_words\":%lu,\"underruns\":%lu,\"input_class\":\"%s\",\"decoder_errors\":%lu,\"refill_faults\":%lu}",
		state, stop_reason, replay.id, (unsigned long)replay.file_bytes,
		(unsigned long)replay.offset_bytes, replay.duration_seconds,
		replay.loop ? "true" : "false", (unsigned long)replay.loops_completed,
		(unsigned long)replay.queue_words, (unsigned long)replay.watermark_words,
		(unsigned long)replay.underruns, klass, (unsigned long)replay.decoder_errors,
		(unsigned long)replay.refill_faults);
	return length < 0 || (size_t)length >= capacity ? 0u : (size_t)length;
}

/* Longest tail this serializer can emit, held back from the row loop so the
 * truncation verdict can ALWAYS be stated: `],"truncated":false}` plus its NUL
 * is 21 bytes. A listing that could not say whether it was complete would be
 * exactly the wrong answer this bound exists to prevent. */
#define REPLAY_LIST_TAIL_RESERVE 24u

/* GET /api/replays. Raw SD access, like app_read_directory() below and for the
 * same reason: the portable engine has no directory job kind, so there is
 * nothing bounded to drive the enumeration onto. It is therefore held to the
 * two rules app_read_directory() already establishes, and the Phase 14d audit
 * found it holding neither (open defect 6).
 *
 * BOUNDED. At most AIUS_OPTICAL_APP_INDEX_SCAN_MAX raw reads, so one epoch's
 * enumeration costs a constant rather than however many files the card happens
 * to hold. What this replaces is a `for (;;) openNextFile()` that ran to the end
 * of an unknown directory inside a single epoch -- the most plausible
 * unexplained 8 s watchdog reset in the image, since a large replays directory
 * makes that walk arbitrarily long and the SD library is uncancellable.
 *
 * GATED. The dispatcher calls this only when app_sd_busy() is false
 * (aius_optical_app.cpp), so it can never touch the volume while a job owns it
 * -- refill, upload, download or probe. A refused call answers 503 sd_busy: the
 * distinguishable, already-documented refusal every other SD route uses, and a
 * refusal rather than a stale or partial answer.
 *
 * HONEST WHEN IT STOPS EARLY. Either bound -- the scan cap or the response
 * buffer -- reports "truncated":true, and the tail is reserved before the first
 * row so that verdict can always be written. A truncated listing is never
 * mistakable for a complete one. */
static FLASHMEM size_t http_replay_list_json(uint8_t *body, size_t capacity)
{
	if (!g_sd_ready) return (size_t)snprintf((char *)body, capacity,
		"{\"ok\":false,\"error\":\"sd_unavailable\"}");
	File directory = SD.open("/aius/replays");
	if (!directory || !directory.isDirectory()) {
		if (directory) directory.close();
		return (size_t)snprintf((char *)body, capacity,
			"{\"ok\":true,\"files\":[],\"truncated\":false}");
	}
	size_t length = 0u;
	int written = snprintf((char *)body, capacity, "{\"ok\":true,\"files\":[");
	if (written < 0 ||
	    (size_t)written + REPLAY_LIST_TAIL_RESERVE >= capacity) {
		directory.close();
		return 0u;
	}
	length = (size_t)written;
	/* Everything the rows may spend, with the tail already deducted. */
	const size_t row_limit = capacity - REPLAY_LIST_TAIL_RESERVE;
	bool first = true;
	bool truncated = false;
	unsigned scanned = 0u;
	while (scanned < AIUS_OPTICAL_APP_INDEX_SCAN_MAX) {
		File entry = directory.openNextFile();
		if (!entry) break;
		scanned++;
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
					written = snprintf((char *)body + length, row_limit - length,
						"%s{\"id\":\"%.*s\",\"bytes\":%lu,\"duration_seconds\":%s,\"upload_next_offset\":%lu,\"complete\":%s,\"active\":%s}",
						first ? "" : ",", (int)id_length, base,
						(unsigned long)entry.size(), duration,
						(unsigned long)entry.size(), complete ? "true" : "false",
						(strlen(g_replay_service.id) == id_length &&
						 memcmp(base, g_replay_service.id, id_length) == 0 && complete) ?
						"true" : "false");
					/* Out of room is a truncation, not a 500: the rows
					 * already written are correct and the tail says the
					 * listing stopped early. */
					if (written < 0 ||
					    (size_t)written >= row_limit - length) {
						truncated = true;
						entry.close();
						break;
					}
					length += (size_t)written;
					first = false;
				}
			}
		}
		entry.close();
	}
	/* Stopped on the scan cap rather than at the end of the directory: one
	 * further read says whether anything was left behind, exactly as
	 * app_read_directory() decides its own `more`. */
	if (!truncated && scanned >= AIUS_OPTICAL_APP_INDEX_SCAN_MAX) {
		File extra = directory.openNextFile();
		if (extra) {
			truncated = true;
			extra.close();
		}
	}
	directory.close();
	written = snprintf((char *)body + length, capacity - length,
	                   "],\"truncated\":%s}", truncated ? "true" : "false");
	if (written < 0 || (size_t)written >= capacity - length) return 0u;
	return length + (size_t)written;
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

/* Records the reboot request; the reset itself happens in loop(), gated on the
 * HTTP transaction draining (see reboot_drain_complete()). volatile because the
 * store is the whole point of the callback and its reader is loop().
 *
 * This deliberately records nothing else -- no timestamp, no transaction
 * snapshot. The dispatcher's contract is that it never delays and reads no
 * clock, and this adapter runs inside it; loop() already has the epoch's now_ms
 * and arms the ceiling from there. */
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
	g_pending_dsp_model = parsed;
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

	/* The live input always owns the audio path (replay_clock_service), so a
	 * start against a carrier would be admitted and then reversed at the next
	 * checkpoint. Refuse it here instead, with its own status. */
	if (AudioInputSPDIF3::pllLocked())
		return AIUS_OPTICAL_APP_REPLAY_CARRIER_PRESENT;
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
	/* Every replay classifies its own stream. AUDIO consumes the request in
	 * epoch context; the later effective-source transition is independently
	 * generation-tagged by update(). */
	request_audio_reset();
	g_replay_stop_reason = REPLAY_STOP_NONE;
	return AIUS_OPTICAL_APP_REPLAY_OK;
}

/* SWITCHING_TO_REPLAY joins REPLAY in the active test because 14c made
 * prebuffering an observable state that outlives the start call: a delete
 * admitted during it would remove the file underneath the refill job that is
 * reading it. Before 14c, start returned already playing, so the window did not
 * exist.
 *
 * Two card primitives, and both are already bounded by being constants -- there
 * is no walk here to cap. What the Phase 14d audit found missing was the
 * exclusion: this ran while an SD job could own the volume. The dispatcher now
 * calls it only when app_sd_busy() is false (aius_optical_app.cpp), the same
 * gate app_read_directory() has always had, and a refused call answers
 * 503 sd_busy rather than a wrong 404. */
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
	g_replay_stop_reason = REPLAY_STOP_REQUEST;
}

static FLASHMEM void app_set_test_running(void *, bool running)
{
	if (g_test_running == running)
		return;
	g_test_running = running;
	record_event(AIUS_EVENT_TEST_TONE, current_state(), running ? 1u : 0u);
}

static FLASHMEM void app_clear_counters(void *)
{
	g_audio_processor.clear_counters();
	/* Clearing zeroes the ingress-drop projection used by the capture latch
	 * compares against by EQUALITY -- a clear during a live capture could
	 * therefore mask a real overrun's 512-frame seam (latched 0, one overrun,
	 * then the clear restores 0). Retire the capture instead: the epoch stall
	 * watch or AUDIO's next producer block completes the cancel, and GET /api/measure
	 * says why. A no-op in every non-live state. */
	(void)aius_measure_cancel(&g_measure_capture);
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
 * 0..10, checkpoint 0..10, and two booleans). snprintf's capacity check below is
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
	(sizeof(" nrst=") - 1u) + AIUS_ID_U32_DIGITS +
	(sizeof(" ps=") - 1u) + 3u +
	(sizeof(" pv=") - 1u) + 5u +
	(sizeof(" fa=") - 1u) + 3u;
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
	/* `ps` is the fitted PSRAM in MB, `pv` the PREVIOUS boot's final
	 * persisted checkpoint (65535 = retained ledger invalid), `fa` the
	 * PREVIOUS boot's FIRST aborting checkpoint (254 = none, 255 = latch
	 * invalid) -- the fields a recovery-safe board most needs to explain
	 * itself. */
	int written = snprintf(payload, capacity,
		"protocol=1 revision=%s build=%s mode=optical ip=%u.%u.%u.%u rsafe=%u nstate=%u nfault=%u ckpt=%u early=%u ngen=%lu bgen=%lu rsrc=%lu epoch=%lu nrst=%lu ps=%u pv=%u fa=%u",
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
		(unsigned long)recovery.network_restarts,
		(unsigned)external_psram_size,
		(unsigned)g_prev_boot_checkpoint,
		(unsigned)g_prev_first_abort);
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

/* The measurement callbacks are DEFINED beside the measurement session (after
 * this table -- their bodies read state that lives with the METRICS-checkpoint
 * glue), and declared here so the positional aggregate can name them. FLASHMEM
 * sits on the definitions, per the rule that a prototype-only attribute
 * silently lands the code in ITCM. */
static size_t app_write_measure(void *, uint8_t *body, size_t capacity);
static bool app_measure_start(void *, const char *vector_id, uint32_t f0_l_hz,
                              uint32_t f0_r_hz, const uint32_t *probes_hz,
                              size_t probe_count);
static size_t app_write_recorder(void *, uint8_t *body, size_t capacity);
static bool app_recorder_freeze(void *);
static bool app_recorder_thaw(void *);
static bool app_recorder_config(void *, uint32_t mask, float gr_threshold_db,
                                uint32_t gr_hold_blocks,
                                uint32_t postroll_blocks);
static AiusOpticalAppRecorderReadResult app_recorder_read(
	void *, uint32_t offset_bytes, uint32_t length_bytes,
	uint32_t generation, const uint8_t **data, uint32_t *length);

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
	app_read_directory,
	app_write_measure,
	app_measure_start,
	app_write_recorder,
	app_recorder_freeze,
	app_recorder_thaw,
	app_recorder_config,
	app_recorder_read
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

/* ------------------------------------------------------------------ */
/* Measurement session -- the epoch side of the item 3 capture         */
/*                                                                     */
/* The portable machine owns every transition (aius_measure.h); this   */
/* glue owns WHEN the epoch-side ones fire: the stall watch that       */
/* cancels a capture the AUDIO producer provably is not feeding, the   */
/* analysis pass over a captured buffer, and the parameters a start    */
/* request supplies. All of it runs at the METRICS checkpoint inside   */
/* its declared 1 ms budget; the overrun counter beside it is the      */
/* evidence when an epoch does not (the HTTP checkpoint's pattern).    */
/* ------------------------------------------------------------------ */

/* The analysis accumulators are epoch-only and re-initialised by
 * aius_measure_analysis_init() before every use, so DMAMEM's no-init hazard
 * cannot reach them; ~600 B each stays off DTCM. */
DMAMEM static AiusMeasureAnalysis g_measure_analysis_l;
DMAMEM static AiusMeasureAnalysis g_measure_analysis_r;

/* Session parameters and results. DTCM .bss deliberately: GET /api/measure
 * reads this before any session ever starts, so it must read as "none" on a
 * cold boot, and .bss zeroing is what guarantees that. ~600 B. */
typedef struct {
	bool started;              /* a session exists (live or retired) */
	char vector_id[AIUS_REPLAY_MAX_ID_LENGTH + 1u];
	double f0_l_hz;
	double f0_r_hz;
	double probes_hz[AIUS_MEASURE_PROBES_MAX];
	uint32_t probe_count;
	uint32_t analysis_offset;  /* frames fed so far, 0..WINDOW */
	uint32_t watch_blocks;     /* stall watch: last block count seen */
	uint32_t watch_since_ms;   /* ...and when it last moved */
	bool figures_valid;        /* both channels' figures are published */
	AiusMeasureFigures figures_l;
	AiusMeasureFigures figures_r;
} MeasureSession;
static MeasureSession g_measure_session;

/* The stall watch's bounds, wall-clock rather than epoch-count so they do not
 * drift with epoch rate. ARMED tolerates the operator gap between POST
 * /api/measure/start and POST /api/replays/start (arm-then-start is the
 * natural order; replay start includes an SD prebuffer); CAPTURING only ever
 * stalls when the replay stopped underneath the capture; CANCELLING is the
 * reclaim bound -- blocks unchanged this long after the token means the AUDIO
 * producer is provably not running (the one case the epoch may reclaim). */
static constexpr uint32_t MEASURE_ARM_STALL_MS = 10000u;
static constexpr uint32_t MEASURE_CAPTURE_STALL_MS = 2000u;
static constexpr uint32_t MEASURE_RECLAIM_MS = 1000u;

/* METRICS-checkpoint wall-clock overruns, on g_http_service_overruns's
 * pattern: nothing aborts on one -- the counter is the evidence that the
 * declared 1 ms budget was exceeded. */
static volatile uint32_t g_metrics_service_overruns;

/* Arm a measurement session (epoch/HTTP context; the dispatcher calls this).
 * Analysis parameters are validated BEFORE the machine moves, so a bad f0 or
 * probe list refuses without disturbing a retired session's figures... the
 * capture is armed last, latching the config generation and overrun counter
 * as of this instant. A retired session (done/failed) is released and
 * replaced; a live one refuses. */
static FLASHMEM bool measure_start(const char *vector_id, double f0_l_hz,
                                   double f0_r_hz, const double *probes_hz,
                                   uint32_t probe_count)
{
	if (vector_id == nullptr || probe_count > AIUS_MEASURE_PROBES_MAX)
		return false;
	/* Refuse a live capture FIRST: mid-analysis the accumulators below are in
	 * use, and an init would corrupt the in-flight pass before the refusal.
	 * The remaining states cannot move under this function -- the AUDIO
	 * producer acts only in armed/capturing/cancelling. */
	const uint32_t state = g_measure_capture.state;
	if (state != AIUS_MEASURE_IDLE && state != AIUS_MEASURE_DONE &&
	    state != AIUS_MEASURE_FAILED)
		return false;
	/* Validate the analysis parameters before the machine moves, so a bad f0
	 * or probe list leaves a retired session fully intact. */
	const double output_rate_hz =
		(double)g_clock_output_rate_millihz / 1000.0;
	if (aius_measure_analysis_init(&g_measure_analysis_l,
	                               output_rate_hz,
	                               f0_l_hz, AIUS_MEASURE_WINDOW_FRAMES,
	                               probes_hz, probe_count) != AIUS_OK)
		return false;
	if (aius_measure_analysis_init(&g_measure_analysis_r,
	                               output_rate_hz,
	                               f0_r_hz, AIUS_MEASURE_WINDOW_FRAMES,
	                               probes_hz, probe_count) != AIUS_OK)
		return false;
	if (state != AIUS_MEASURE_IDLE &&
	    aius_measure_release(&g_measure_capture) != AIUS_OK)
		return false;

	MeasureSession &s = g_measure_session;
	memset(&s, 0, sizeof(s));
	s.started = true;
	strncpy(s.vector_id, vector_id, sizeof(s.vector_id) - 1u);
	s.f0_l_hz = f0_l_hz;
	s.f0_r_hz = f0_r_hz;
	if (probe_count > 0)
		memcpy(s.probes_hz, probes_hz, probe_count * sizeof(double));
	s.probe_count = probe_count;
	s.watch_blocks = g_audio_processor.block_count();
	s.watch_since_ms = millis();
	return aius_measure_arm(&g_measure_capture, g_config_generation,
	                        g_audio_processor.overrun_count()) == AIUS_OK;
}

/* One epoch of measurement work, at the METRICS checkpoint. Idle cost: one
 * DTCM load and a switch. The analysis feeds one declared slice per epoch
 * into BOTH channel accumulators (~2 x 170 us worst case against the 1 ms
 * budget), so a full window completes in WINDOW/SLICE = 32 epochs. */
static FLASHMEM void measure_epoch_service(uint32_t now_ms)
{
	AiusMeasureCapture &cap = g_measure_capture;
	MeasureSession &s = g_measure_session;
	switch (cap.state) {
	case AIUS_MEASURE_ARMED:
	case AIUS_MEASURE_CAPTURING:
	case AIUS_MEASURE_CANCELLING: {
		/* The stall watch. Only the epoch side can time out a capture the
		 * AUDIO producer is not feeding -- with no replay running enqueue() never
		 * executes (the race the one-writer machine exists to close). */
		const uint32_t blocks = g_audio_processor.block_count();
		if (blocks != s.watch_blocks) {
			s.watch_blocks = blocks;
			s.watch_since_ms = now_ms;
			break;
		}
		const uint32_t stalled_ms = now_ms - s.watch_since_ms;
		if (cap.state == AIUS_MEASURE_CANCELLING) {
			if (stalled_ms > MEASURE_RECLAIM_MS)
				(void)aius_measure_reclaim(&cap);
		} else if (stalled_ms > (cap.state == AIUS_MEASURE_ARMED ?
		                                 MEASURE_ARM_STALL_MS :
		                                 MEASURE_CAPTURE_STALL_MS)) {
			(void)aius_measure_cancel(&cap);
			s.watch_since_ms = now_ms; /* restart the clock for the reclaim */
		}
		break;
	}
	case AIUS_MEASURE_CAPTURED:
		if (aius_measure_begin_analysis(&cap) == AIUS_OK)
			s.analysis_offset = 0;
		break;
	case AIUS_MEASURE_ANALYSING: {
		const uint32_t start =
			AIUS_MEASURE_CAPTURE_FRAMES - AIUS_MEASURE_WINDOW_FRAMES;
		uint32_t n = AIUS_MEASURE_WINDOW_FRAMES - s.analysis_offset;
		if (n > AIUS_MEASURE_SLICE_FRAMES)
			n = AIUS_MEASURE_SLICE_FRAMES;
		const uint32_t at = start + s.analysis_offset;
		(void)aius_measure_analysis_slice(&g_measure_analysis_l,
		                                  g_measure_capture_l + at, n);
		(void)aius_measure_analysis_slice(&g_measure_analysis_r,
		                                  g_measure_capture_r + at, n);
		s.analysis_offset += n;
		if (s.analysis_offset >= AIUS_MEASURE_WINDOW_FRAMES) {
			const bool ok_l = aius_measure_analysis_finish(
				&g_measure_analysis_l, &s.figures_l) == AIUS_OK;
			const bool ok_r = aius_measure_analysis_finish(
				&g_measure_analysis_r, &s.figures_r) == AIUS_OK;
			s.figures_valid = ok_l && ok_r;
			(void)aius_measure_finish_analysis(&cap);
		}
		break;
	}
	default:
		break; /* idle, done, failed: nothing owed this epoch */
	}
}

static_assert(AIUS_OPTICAL_APP_MEASURE_PROBES_MAX == AIUS_MEASURE_PROBES_MAX,
              "the dispatcher's probe cap mirrors aius_measure.h's; change both");

static FLASHMEM const char *measure_state_name(uint32_t state)
{
	switch (state) {
	case AIUS_MEASURE_ARMED: return "armed";
	case AIUS_MEASURE_CAPTURING: return "capturing";
	case AIUS_MEASURE_CAPTURED: return "captured";
	case AIUS_MEASURE_ANALYSING: return "analysing";
	case AIUS_MEASURE_DONE: return "done";
	case AIUS_MEASURE_CANCELLING: return "cancelling";
	case AIUS_MEASURE_FAILED: return "failed";
	default: return "idle";
	}
}

static FLASHMEM const char *measure_fail_name(uint32_t reason)
{
	switch (reason) {
	case AIUS_MEASURE_FAIL_GENERATION: return "generation";
	case AIUS_MEASURE_FAIL_OVERRUN: return "overrun";
	case AIUS_MEASURE_FAIL_CANCELLED: return "cancelled";
	case AIUS_MEASURE_FAIL_RECLAIMED: return "reclaimed";
	default: return "none";
	}
}

/* One channel's figures as a nested object. Ratios to 0.01 dB (the analysis
 * resolves far finer than any stated floor); harmonic and probe arrays are
 * positional, like the meters' input array. */
static FLASHMEM bool measure_figures_json(char *output, size_t capacity,
                                          size_t *length, const char *key,
                                          const AiusMeasureFigures *fig)
{
	if (!http_json_append(output, capacity, length,
	                      "\"%s\":{\"f0_hz\":%.1f,\"fundamental_dbfs\":%.2f,"
	                      "\"dc_dbfs\":%.1f,\"thd_n_db\":%.2f,\"snr_db\":%.2f,"
	                      "\"thd_db\":%.2f,\"harmonics_dbc\":[",
	                      key, fig->f0_hz, fig->fundamental_dbfs, fig->dc_dbfs,
	                      fig->thd_n_db, fig->snr_db, fig->thd_db))
		return false;
	for (uint32_t h = 0; h < fig->harmonics; h++)
		if (!http_json_append(output, capacity, length, "%s%.1f",
		                      h == 0 ? "" : ",", fig->harmonic_dbc[h]))
			return false;
	if (!http_json_append(output, capacity, length, "],\"probes_dbfs\":["))
		return false;
	for (uint32_t p = 0; p < fig->probes; p++)
		if (!http_json_append(output, capacity, length, "%s%.2f",
		                      p == 0 ? "" : ",", fig->probe_dbfs[p]))
			return false;
	return http_json_append(output, capacity, length, "]}");
}

/* GET /api/measure: the state machine, the figures once published, and the
 * resolvable floors beside them, as the design requires ("the figures, each
 * with its resolvable floor, and the vector id they were measured against").
 * The floors are properties of the 16-bit STIMULUS, not of the analysis:
 * THD+N and SNR bottom out at the replay vectors' quantization floor
 * (~-96 dB; the analysis itself proves ~-140 dB on 24-bit synthesis), Goertzel
 * harmonic THD stays usable to ~-95 dBc, and the -140 dB inter-stage floor is
 * not verifiable on-device by any 16-bit transport -- it stays host-proven.
 * SNR's harmonic exclusion additionally inherits the harmonic estimate's
 * leakage-limited accuracy (~-90 dB of the subtracted harmonic). */
static FLASHMEM size_t http_measure_json(uint8_t *body, size_t capacity)
{
	const MeasureSession &s = g_measure_session;
	const uint32_t state = g_measure_capture.state;
	char *output = (char *)body;
	size_t length = 0;
	if (!http_json_append(output, capacity, &length,
	                      "{\"state\":\"%s\",\"fail_reason\":\"%s\",",
	                      measure_state_name(state),
	                      measure_fail_name(g_measure_capture.fail_reason)))
		return 0;
	if (s.started) {
		if (!http_json_append(output, capacity, &length,
		                      "\"vector_id\":\"%s\",", s.vector_id))
			return 0;
	} else if (!http_json_append(output, capacity, &length,
	                             "\"vector_id\":null,")) {
		return 0;
	}
	if (!http_json_append(output, capacity, &length,
	                      "\"captured_frames\":%lu,\"capture_frames\":%lu,"
	                      "\"analysed_frames\":%lu,\"window_frames\":%lu,",
	                      (unsigned long)g_measure_capture.cursor,
	                      (unsigned long)AIUS_MEASURE_CAPTURE_FRAMES,
	                      (unsigned long)s.analysis_offset,
	                      (unsigned long)AIUS_MEASURE_WINDOW_FRAMES))
		return 0;
	if (s.figures_valid) {
		if (!http_json_append(output, capacity, &length, "\"figures\":{") ||
		    !measure_figures_json(output, capacity, &length, "l",
		                          &s.figures_l) ||
		    !http_json_append(output, capacity, &length, ",") ||
		    !measure_figures_json(output, capacity, &length, "r",
		                          &s.figures_r) ||
		    !http_json_append(output, capacity, &length, "},"))
			return 0;
	} else if (!http_json_append(output, capacity, &length,
	                             "\"figures\":null,")) {
		return 0;
	}
	return http_json_append(output, capacity, &length,
	                        "\"floors\":{\"thd_n_db\":-96.0,\"snr_db\":96.0,"
	                        "\"thd_dbc\":-95.0},"
	                        "\"metrics_service_overruns\":%lu}",
	                        (unsigned long)g_metrics_service_overruns)
		? length : 0;
}

static FLASHMEM size_t app_write_measure(void *, uint8_t *body, size_t capacity)
{
	return http_measure_json(body, capacity);
}

static FLASHMEM bool app_measure_start(void *, const char *vector_id,
                                       uint32_t f0_l_hz, uint32_t f0_r_hz,
                                       const uint32_t *probes_hz,
                                       size_t probe_count)
{
	double probes[AIUS_MEASURE_PROBES_MAX];
	if (probe_count > AIUS_MEASURE_PROBES_MAX)
		return false;
	for (size_t p = 0; p < probe_count; p++)
		probes[p] = (double)probes_hz[p];
	return measure_start(vector_id, (double)f0_l_hz, (double)f0_r_hz, probes,
	                     (uint32_t)probe_count);
}

/* ------------------------------------------------------------------ */
/* Flight-recorder application callbacks (dispatch context, epoch)     */
/* ------------------------------------------------------------------ */

static FLASHMEM const char *recorder_state_name(uint32_t state)
{
	switch (state) {
	case AIUS_RECORDER_RECORDING: return "recording";
	case AIUS_RECORDER_POSTROLL: return "postroll";
	case AIUS_RECORDER_DRAINING: return "draining";
	case AIUS_RECORDER_FROZEN: return "frozen";
	case AIUS_RECORDER_ABSENT:
	default: return "absent";
	}
}

static FLASHMEM const char *recorder_trigger_name(uint32_t trigger)
{
	switch (trigger) {
	case AIUS_RECORDER_TRIGGER_OVERRUN: return "overrun";
	case AIUS_RECORDER_TRIGGER_UNDERRUN: return "underrun";
	case AIUS_RECORDER_TRIGGER_CARRIER_LOSS: return "carrier_loss";
	case AIUS_RECORDER_TRIGGER_LIMITER_GR: return "limiter_gr";
	case AIUS_RECORDER_TRIGGER_OPERATOR: return "operator";
	case AIUS_RECORDER_TRIGGER_NONE:
	default: return "none";
	}
}

/* GET /api/recorder: status + the frozen header in one document -- the
 * contract's WAV-sufficient field list (encoding, geometry, valid extent and
 * oldest position, trigger metadata, gaps, generations, the armed config and
 * the chunk cap). Served from a coherent epoch-context snapshot, never
 * re-derived mid-serialize. */
static FLASHMEM size_t app_write_recorder(void *, uint8_t *body,
                                          size_t capacity)
{
	AiusRecorderSnapshot s;
	aius_recorder_snapshot(&g_recorder, &s);
	int length = snprintf((char *)body, capacity,
		"{\"state\":\"%s\",\"fitted_mb\":%lu,"
		"\"sample_rate_hz\":%lu,"
		"\"encoding\":\"s32le\",\"significant_bits\":24,"
		"\"interleave\":\"lr\",\"frame_bytes\":%u,"
		"\"ring_frames\":%lu,\"valid_frames\":%lu,"
		"\"oldest_frame\":%lu,\"freeze_generation\":%lu,"
		"\"boot_generation\":%lu,"
		"\"gap_count\":%lu,\"last_gap_frame\":%lu,"
		"\"suppressed_triggers\":%lu,"
		"\"postroll_captured\":%lu,"
		"\"chunk_max_bytes\":%lu,"
		"\"trigger\":{\"which\":\"%s\",\"at_frame\":%lu,"
		"\"at_block\":%lu,\"at_ms\":%lu},"
		"\"config\":{\"overrun\":%s,\"underrun\":%s,"
		"\"carrier_loss\":%s,\"limiter_gr\":%s,"
		"\"gr_threshold_db\":%.1f,\"gr_hold_blocks\":%lu,"
		"\"postroll_blocks\":%lu}}",
		recorder_state_name(s.state), (unsigned long)s.fitted_mb,
		(unsigned long)AIUS_OPTICAL_SAMPLE_RATE_HZ,
		(unsigned)AIUS_RECORDER_FRAME_BYTES,
		(unsigned long)s.ring_frames, (unsigned long)s.valid_frames,
		(unsigned long)s.oldest_frame,
		(unsigned long)s.freeze_generation,
		(unsigned long)g_boot_ledger.boot_generation,
		(unsigned long)s.gap_count, (unsigned long)s.last_gap_frame,
		(unsigned long)s.suppressed_triggers,
		(unsigned long)s.postroll_captured,
		(unsigned long)AIUS_RECORDER_READ_MAX_BYTES,
		recorder_trigger_name(s.trigger.trigger),
		(unsigned long)s.trigger.at_frame,
		(unsigned long)s.trigger.at_block,
		(unsigned long)s.trigger.at_ms,
		(s.trigger_mask & AIUS_RECORDER_MASK_OVERRUN) ? "true" : "false",
		(s.trigger_mask & AIUS_RECORDER_MASK_UNDERRUN) ? "true" : "false",
		(s.trigger_mask & AIUS_RECORDER_MASK_CARRIER_LOSS) ? "true"
		                                                   : "false",
		(s.trigger_mask & AIUS_RECORDER_MASK_LIMITER_GR) ? "true"
		                                                 : "false",
		(double)s.gr_threshold_db, (unsigned long)s.gr_hold_blocks,
		(unsigned long)s.postroll_blocks);
	return length > 0 && (size_t)length < capacity ? (size_t)length : 0u;
}

static FLASHMEM bool app_recorder_freeze(void *)
{
	return g_recorder_present && aius_recorder_freeze(&g_recorder) == AIUS_OK;
}

static FLASHMEM bool app_recorder_thaw(void *)
{
	return g_recorder_present && aius_recorder_thaw(&g_recorder) == AIUS_OK;
}

static FLASHMEM bool app_recorder_config(void *, uint32_t mask,
                                         float gr_threshold_db,
                                         uint32_t gr_hold_blocks,
                                         uint32_t postroll_blocks)
{
	/* The contract's absent rule: every route but the status read refuses
	 * cleanly on a bare board, config included. */
	return g_recorder_present &&
	       aius_recorder_set_config(&g_recorder, mask, gr_threshold_db,
	                                gr_hold_blocks,
	                                postroll_blocks) == AIUS_OK;
}

static FLASHMEM AiusOpticalAppRecorderReadResult app_recorder_read(
	void *, uint32_t offset_bytes, uint32_t length_bytes,
	uint32_t generation, const uint8_t **data, uint32_t *length)
{
	/* The 400/409 split the dispatcher serves: state and generation are
	 * REFUSED (retry after freeze / re-read the header), argument shape is
	 * INVALID (fix the request). The lib folds both into INVALID_ARG, so
	 * the split lives here where both facts are readable. */
	if (!g_recorder_present || g_recorder.state != AIUS_RECORDER_FROZEN ||
	    generation != g_recorder.freeze_generation)
		return AIUS_OPTICAL_APP_RECORDER_READ_REFUSED;
	return aius_recorder_read(&g_recorder, offset_bytes, length_bytes,
	                          generation, data, length) == AIUS_OK
		? AIUS_OPTICAL_APP_RECORDER_READ_OK
		: AIUS_OPTICAL_APP_RECORDER_READ_INVALID;
}

/* Checkpoint 3. service_hw_metrics() is already its own one-sample-per-second
 * rate limiter, so the budget's "one sample" is what it does; the measurement
 * session's epoch work shares the checkpoint and its declared 1 ms budget,
 * with the overrun counter as the evidence when an epoch exceeds it. */
static FLASHMEM bool platform_metrics(void *, const AiusRuntimeBudget *budget,
                                      uint32_t now_ms)
{
	if (budget == nullptr ||
	    budget->checkpoint != AIUS_BOOT_CHECKPOINT_METRICS)
		return false;
	const uint32_t started = micros();
	service_hw_metrics();
	measure_epoch_service(now_ms);
	if ((uint32_t)(micros() - started) > budget->service_budget_ms * 1000u)
		g_metrics_service_overruns++;
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
			/* Belt and braces against the adapter's own
			 * invalidation. The adapter drops its cached mount when
			 * a media check reports the card gone, which is the
			 * same event this arm reacts to -- but a probe whose
			 * JOB failed never reaches that check at all (BEGIN
			 * faulted, or the step was cancelled), and this arm
			 * fires for that case too. Saying it explicitly here
			 * means the card-absent transition invalidates by
			 * construction rather than by both paths happening to
			 * agree. Memory only. */
			aius_sd_teensy_invalidate_mount(
				&g_runtime.storage.target);
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
		g_pending_dsp_model = parsed;
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
	 * -- the single owner of the only card handle in the firmware.
	 *
	 * Timed here rather than read from the adapter because the adapter keeps
	 * ONE running maximum across every job it serves and does not say which
	 * action set it. That is how a 1.02 s primitive at a loop boundary stayed
	 * unattributable: the number was real and named nothing. Two micros() reads
	 * on a path that already blocks on the card. */
	const uint32_t mounts_before = g_runtime.storage.target.mounts;
	const uint32_t started_us = micros();
	g_replay_job_event =
		aius_sd_teensy_execute(&g_runtime.storage.target, action);
	const uint32_t elapsed_us = micros() - started_us;
	g_replay_job_event_present = true;

	/* REAL mounts, not BEGIN actions. Counting the action was right when
	 * every BEGIN mounted; now that the adapter answers a BEGIN from a
	 * cached mount, counting actions would report one per job forever and
	 * hide the whole point of the change. The adapter's own counter only
	 * moves when SD.begin() actually ran, so its delta is the honest
	 * refill-attributed figure. */
	g_replay_diag.mounts +=
		g_runtime.storage.target.mounts - mounts_before;
	g_replay_diag.last_primitive_us = elapsed_us;
	if (elapsed_us > g_replay_diag.longest_primitive_us) {
		g_replay_diag.longest_primitive_us = elapsed_us;
		g_replay_diag.longest_action = (uint8_t)action->kind;
	}
}

/* Does routing this action cost the epoch's one SD primitive?
 *
 * OUTPUT hands an already-read buffer to the replay service (a memcpy into the
 * ring); COMPLETE and FAIL are bookkeeping; NONE is nothing. Every other action
 * this job can emit -- BEGIN, OPEN_READ, FILE_SIZE, SEEK, READ, CLOSE -- goes
 * through aius_sd_teensy_execute() and blocks on the card. */
static FLASHMEM bool replay_action_spends_primitive(AiusSdActionKind kind)
{
	return kind != AIUS_SD_ACTION_OUTPUT &&
	       kind != AIUS_SD_ACTION_COMPLETE &&
	       kind != AIUS_SD_ACTION_FAIL &&
	       kind != AIUS_SD_ACTION_NONE;
}

/* The most steps one epoch may take. The machine can only ever put ONE
 * memory-only action (an OUTPUT) in front of a primitive, so two is the real
 * maximum and this is slack; it exists so a machine change can never turn this
 * loop into an unbounded one. Falling out of the loop is harmless -- the next
 * epoch resumes exactly where this one stopped. */
#define REPLAY_JOB_STEPS_PER_EPOCH 4u

/* One bounded job advance: SPEND AT MOST ONE SD PRIMITIVE, but do not spend the
 * epoch on a step that touches no card.
 *
 * The bound this keeps is unchanged and is the one that matters: an epoch
 * blocks on the card at most once, so the worst case the watchdog must absorb
 * is one primitive, exactly as §Bounded SD job ABI requires. What changed is
 * what a step that performs NO primitive costs. It used to cost a whole epoch,
 * because the loop advanced the job once per epoch regardless: a refill of N
 * bytes cost 6 + 2 * N/512 epochs, of which N/512 were AIUS_SD_ACTION_OUTPUT --
 * a memcpy into the ring, paid for at the price of a card transaction. That is
 * why the refill ran at 65 % of realtime with the DSP at 6 % of budget: at 512
 * bytes per primitive the seam needs ~375 reads per second to sustain 192 kB/s,
 * and there are only ~672 epochs in a second to spend.
 *
 * Stepping until a primitive has actually been spent removes exactly the epochs
 * that were doing no I/O and changes nothing about blocking time. A refill now
 * costs 6 + N/512 epochs. Everything else about the seam -- the plan/deliver
 * contract, the offset checks, the cancellation drain -- is untouched. */
static FLASHMEM void replay_job_advance(uint32_t now_ms)
{
	for (unsigned step = 0u; step < REPLAY_JOB_STEPS_PER_EPOCH; step++) {
		AiusSdAction action;
		AiusSdEvent event;

		if (g_replay_job_event_present) {
			event = g_replay_job_event;
			g_replay_job_event_present = false;
			memset(&g_replay_job_event, 0,
			       sizeof(g_replay_job_event));
		} else {
			event = replay_job_make_event(AIUS_SD_EVENT_NONE, 0u,
			                              0u);
		}

		memset(&action, 0, sizeof(action));
		aius_sd_job_step(&g_replay_job, now_ms, event, &action);
		replay_job_route(&action);

		/* The job's own FILE_SIZE is the only place the service can
		 * learn how long the file is, now that nothing else stats the
		 * card. Published once per job; an odd or shrinking size is
		 * refused by the service, which then falls back to finding the
		 * end from a short refill. */
		if (!g_replay_job_size_published && !g_replay_job_cancelling &&
		    g_replay_job.captured_size != 0u &&
		    g_replay_job.captured_size <= UINT32_MAX) {
			(void)aius_replay_service_set_file_bytes(
				&g_replay_service,
				(uint32_t)g_replay_job.captured_size);
			g_replay_job_size_published = true;
		}

		if (action.kind == AIUS_SD_ACTION_COMPLETE ||
		    action.kind == AIUS_SD_ACTION_FAIL) {
			/* Latched BEFORE the service is told, and before the
			 * job is reused, because everything that attributes the
			 * failure lives on the job and is gone next epoch. This
			 * is the record sd_diag could not hold: the phase names
			 * which primitive failed, and the offset and lap number
			 * say where in the file it happened -- which is how a
			 * fault that only ever appears at a loop boundary
			 * becomes a fact rather than a suspicion. */
			if (action.kind == AIUS_SD_ACTION_FAIL &&
			    !g_replay_job_cancelling) {
				g_replay_diag.faults++;
				g_replay_diag.last_fault =
					(uint8_t)g_replay_job.last_fault;
				g_replay_diag.last_fault_phase =
					(uint8_t)g_replay_job.fault_phase;
				g_replay_diag.last_fault_action =
					(uint8_t)g_replay_job.outstanding_action;
				g_replay_diag.last_fault_driver =
					g_replay_job.last_driver_fault;
				g_replay_diag.last_fault_offset_bytes =
					g_replay_service.offset_bytes;
				g_replay_diag.last_fault_loops =
					g_replay_service.loops_completed;
				g_replay_diag.last_fault_at_ms = now_ms;
			}
			/* Terminal: the handle is closed and the card is free
			 * again. A cancelled job reports nothing -- its
			 * selection no longer exists. */
			if (!g_replay_job_cancelling)
				aius_replay_service_refill_complete(
					&g_replay_service,
					action.kind ==
						AIUS_SD_ACTION_COMPLETE);
			/* A fault run that reached the service's limit ends the
			 * replay, and says so the way a carrier does: its own
			 * reason on the status route and a transition in the
			 * event ring, rather than a silent `stopped`. Read after
			 * refill_complete(), which is what applies the limit. */
			if (!g_replay_job_cancelling &&
			    g_replay_service.state == AIUS_REPLAY_ERROR &&
			    g_replay_stop_reason != REPLAY_STOP_FAULT) {
				g_replay_stop_reason = REPLAY_STOP_FAULT;
				record_event(AIUS_EVENT_REPLAY_STOP,
				             current_state(),
				             REPLAY_STOP_FAULT);
			}
			g_replay_job_active = false;
			g_replay_job_cancelling = false;
			g_replay_job_size_published = false;
			g_replay_job_event_present = false;
			memset(&g_replay_job_event, 0,
			       sizeof(g_replay_job_event));
			return;
		}
		/* NONE means the machine emitted nothing to perform, so routing
		 * retained no event and another step would emit NONE again. */
		if (action.kind == AIUS_SD_ACTION_NONE)
			return;
		/* The epoch's one card transaction has been spent. */
		if (replay_action_spends_primitive(action.kind))
			return;
	}
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
	g_replay_diag.jobs++;
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
 * route, and the abandoned plan costs nothing.
 *
 * IT NO LONGER TAKES THE RUNTIME BUDGET, and that is a fix rather than a
 * simplification. It used to be called as
 * `replay_refill_service(budget->max_rx_bytes, ...)` and to pass that straight
 * into plan_refill()'s `budget_words`, so a field named and documented in BYTES
 * (and consumed as bytes at the UDP checkpoint, :2412) was read as WORDS here
 * and nowhere else. The replay row's 2,048 therefore silently capped every plan
 * at 4,096 bytes, invisibly to this seam's own constants -- which is how a
 * refill sized for 32 KB and a slice sized for 8 KB could both be "right" and
 * still disagree with the thing that gated them.
 *
 * A network receive quota is the wrong bound for a disk read in any case. The
 * refill's real bounds are AIUS_REPLAY_REFILL_MAX_BYTES and the ring's own free
 * space, both of which plan_refill() already applies, and the per-epoch cost is
 * fixed at one SD primitive whatever the plan size is -- a larger plan spans
 * more epochs, it does not make any epoch longer. So the quota is taken from
 * the seam that owns it. See lib/aius_runtime/include/aius_runtime.h for the
 * unit contract this leaves behind. */
static FLASHMEM void replay_refill_service(uint32_t now_ms)
{
	AiusReplayRefill refill;

	if (g_replay_job_active) {
		replay_job_advance(now_ms);
		return;
	}
	if (!aius_replay_service_plan_refill(&g_replay_service,
	                                     AIUS_REPLAY_REFILL_MAX_BYTES / 2u,
	                                     &refill))
		return;
	if (!sd_storage_idle()) {
		aius_replay_service_abandon_refill(&g_replay_service);
		return;
	}
	replay_job_start(&refill, now_ms);
	replay_job_advance(now_ms);
}

/* ------------------------------------------------------------------ */
/* Replay clock handover                                               */
/*                                                                     */
/* AudioStream::update_setup() grants update responsibility to the     */
/* FIRST claimant, and g_spdif_in is constructed above g_spdif_out, so */
/* the INPUT holds it. input_spdif3.cpp drives AudioStream::update_all */
/* from its DMA ISR, and that DMA is clocked by the incoming carrier.  */
/* With the optical input unsoldered there is no carrier, so that ISR  */
/* never fires, update_all() is never called, AiusAudioProcessor::     */
/* update() never runs, and a replay reports `playing` while processing */
/* exactly zero blocks -- no underruns, no overruns, nothing.          */
/*                                                                     */
/* The transmitter has no such dependency, which is what makes a       */
/* runtime fix possible. config_spdif3() (output_spdif3.cpp) points    */
/* SPDIF0_CLK_ROOT at PLL4 -- CCM_CDCDR_SPDIF0_CLK_SEL(0), the audio   */
/* PLL set_audioClock() programs three lines earlier -- and sets       */
/* SPDIF_STC_TXCLK_SOURCE(1), so the tx bit clock is that PLL divided  */
/* down. SPDIF_SCR_TXSEL(5) is "Tx normal operation", not the          */
/* 1:feed-through-from-SPDIFIN mode, so the tx DATAPATH is not slaved  */
/* to the receiver either. The dependency in fact runs the other way:  */
/* SPDIF_SRPC_CLKSRC_SEL(1) makes the RECEIVER fall back to tx_clk     */
/* whenever its DPLL is unlocked. The tx DMA is triggered by           */
/* DMAMUX_SOURCE_SPDIF_TX over a 4 * AUDIO_BLOCK_SAMPLES int32 buffer  */
/* with INTHALF | INTMAJOR, i.e. one interrupt every                   */
/* AUDIO_BLOCK_SAMPLES frames -- exactly the rx side's rate. So the    */
/* transmit DMA ISR (ours since the 24-bit path landed; see            */
/* lib/aius_audio/src/aius_spdif_tx24.h) already runs at the audio     */
/* block rate with no input present; it simply declines to call        */
/* update_all(). Replacing the handler changed nothing here: the same  */
/* DMA channel, the same trigger, the same rate, the same             */
/* update_responsibility test at the bottom of the handler.           */
/*                                                                     */
/* THE LIVE INPUT ALWAYS OWNS THE AUDIO PATH. Replay is strictly the   */
/* fallback for a board with no carrier, so production is unchanged:   */
/* the input remains clock master for passthrough, the handover is     */
/* refused while the receiver's DPLL is locked, and a carrier arriving */
/* mid-replay STOPS the replay and takes the clock back in the same    */
/* epoch -- reported as stop_reason "carrier" on                       */
/* GET /api/replays/status and as a type-3 event on /api/events, so a  */
/* replay that ends by itself is never an unexplained `stopped`.       */
/* See replay_clock_service() below.                                   */
/*                                                                     */
/* ON STOP the clock goes back to the input, deliberately. With no     */
/* carrier that means the graph stops again: `blocks` and `overruns`   */
/* freeze at their final values, and the transmitter -- whose ISR      */
/* keeps running -- drains the 24-bit ring to empty and then writes    */
/* zeros, so the optical output carries digital silence. That is the   */
/* same idle state the board boots into with nothing plugged in, and   */
/* it is the right one: a passthrough device with no source has        */
/* nothing to pass through. Nothing leaks across the transition -- the */
/* ring holds only committed frames and AudioInputSPDIF3::update()     */
/* releases the pair it allocates each pass while block_offset stays 0.*/
/* The silence is now COUNTED as well as heard: the substituted frames */
/* advance g_aius_spdif_tx24.underruns, where the vendor path's        */
/* block_silent substitution advanced nothing at all.                 */
/* ------------------------------------------------------------------ */

/* update_responsibility is `protected static` on both peripherals
 * (input_spdif3.h, output_spdif3.h), so a DERIVED CLASS -- never an object --
 * is what reaches it; [class.protected]'s "through an object of the derived
 * type" restriction applies to non-static members only. Private inheritance and
 * a deleted default constructor because these are access shims, not audio
 * nodes: neither is an AudioStream anyone may connect, and constructing one
 * would call the base begin() a second time and re-run config_spdif3().
 *
 * Defined out-of-line, and FLASHMEM on both the declaration and the definition,
 * for the reason AiusAudioProcessor::apply_config() records above: an in-body
 * definition is implicitly inline (vague linkage), which conflicts with the
 * plain FLASHMEM free functions sharing .flashmem, and a COMDAT copy of one in
 * .text would fail the hot-object ITCM guard by name. Nothing here runs per
 * audio block -- the whole handover is checkpoint-7 work. */
class AiusSpdifInputClock : private AudioInputSPDIF3 {
public:
	AiusSpdifInputClock(void) = delete;
	static FLASHMEM void set_master(bool master);
};

class AiusSpdifOutputClock : private AudioOutputSPDIF3 {
public:
	AiusSpdifOutputClock(void) = delete;
	static FLASHMEM bool is_master(void);
	static FLASHMEM void set_master(bool master);
};

FLASHMEM void AiusSpdifInputClock::set_master(bool master)
{
	update_responsibility = master;
}

FLASHMEM bool AiusSpdifOutputClock::is_master(void)
{
	return update_responsibility;
}

static FLASHMEM bool clock_output_is_master(void)
{
	return AiusSpdifOutputClock::is_master();
}

FLASHMEM void AiusSpdifOutputClock::set_master(bool master)
{
	update_responsibility = master;
}

/* Settle source ownership before any clock decision. Replay is strictly the
 * fallback for a board with no carrier, so a carrier and a replay never
 * contend: the carrier wins and the replay ends.
 *
 * Called from checkpoint 7 AFTER the refill pass, because
 * aius_replay_service_deliver() -- reached through replay_job_route() inside
 * that pass -- is what promotes SWITCHING_TO_REPLAY to SOURCE_REPLAY once the
 * ring reaches its watermark; running after it moves the clock in the same
 * epoch the source moves, not an epoch later.
 *
 * Source settlement is separate from graph-master reconciliation because the
 * exact 48 kHz replay plan and its four-period validity hold must finish
 * between them. */
static FLASHMEM void replay_source_service(void)
{
	/* A carrier displaces any replay that is prebuffering or playing. The
	 * refill is cancelled and the service stopped exactly as app_replay_stop()
	 * does it -- memory only, the job drains through its own CLOSE -- and the
	 * reason is recorded so /api/replays/status does not report this as an
	 * indistinguishable `stopped`, and so /api/events carries the transition
	 * rather than leaving it to be inferred from a counter. */
	if ((g_replay_service.source == AIUS_REPLAY_SOURCE_REPLAY ||
	     g_replay_service.source == AIUS_REPLAY_SOURCE_SWITCHING_TO_REPLAY) &&
	    AudioInputSPDIF3::pllLocked()) {
		replay_job_cancel();
		aius_replay_service_stop(&g_replay_service);
		g_replay_stop_reason = REPLAY_STOP_CARRIER;
		record_event(AIUS_EVENT_REPLAY_STOP, current_state(),
		             REPLAY_STOP_CARRIER);
	}
}

/* The only graph-master writer. Called after sample_rate_service(), so output
 * can take responsibility only after the production 48 kHz plan is applied
 * and its exact 512-frame invalid hold has completed. A failed apply leaves
 * the input master, the pad dark and replay data unconsumed. */
static FLASHMEM void replay_clock_service(void)
{
	const bool output_drives =
		g_replay_service.source == AIUS_REPLAY_SOURCE_REPLAY &&
		!AudioInputSPDIF3::pllLocked() && g_clock_replay_ready;
	if (output_drives == AiusSpdifOutputClock::is_master())
		return;
	/* Both flags are read from a DMA ISR -- input_spdif3.cpp's at the top of
	 * its handler, output_spdif3.cpp's at the bottom of its. This runs in
	 * thread context, so an ISR cannot be mid-flight when it starts: one that
	 * had already begun would have run to completion before the epoch
	 * resumed. The only interleaving left is an ISR entering BETWEEN the two
	 * stores, and one critical section spanning both removes it -- every ISR
	 * observes either the whole before-state or the whole after-state, so
	 * exactly one peripheral drives update_all() at every instant an ISR can
	 * see. That makes the store order irrelevant, which is the point: neither
	 * the both-drive window (a double-processed block) nor the neither-drives
	 * window (a stalled one) can exist. __disable_irq() carries a memory
	 * clobber, so the compiler cannot hoist a store out either.
	 *
	 * Two byte stores, at most once per epoch and in practice once per replay
	 * transition -- shorter than the critical sections block_count() and
	 * AudioOutputSPDIF3::update() already take, and it adds nothing to the
	 * per-block work of either ISR. */
	noInterrupts();
	AiusSpdifOutputClock::set_master(output_drives);
	AiusSpdifInputClock::set_master(!output_drives);
	interrupts();
}

/* Checkpoint 7, after replay_source_service(): both inputs are this epoch's
 * settled values -- refill promotion and any carrier displacement have
 * already landed. In recovery-safe the scheduler never
 * reaches this checkpoint, so the gate stays frozen dark from setup(), which
 * also mutes live passthrough there -- intended, and recorded in the plan. */
/* Defect 28 (C3): a debounced carrier-lock rising edge re-classifies the
 * pipeline through the same block-boundary reset replay start uses, so every
 * NEW carrier types its own stream and neither latch hazard survives a source
 * swap. Debounced on the unlocked-run length: a 1-2 epoch PLL micro-flap
 * mid-programme keeps the latched class (the parser's per-burst re-lock is
 * the right response there); only a drop of AIUS_CARRIER_RESET_DEBOUNCE_EPOCHS
 * or more (~27 ms at ~115 epoch/s) arms the reset on relock. Born saturated:
 * the box HAS been unlocked since power-on, so the first-ever lock edge
 * behaves like any post-drop relock -- pre-lock garbage words may already
 * have typed the fresh pipeline. Epoch-thread writer beside the replay-start
 * writer (same thread, no race); the pre-existing benign ISR set/clear race
 * self-heals in-pipeline (re-type or starvation re-classify). */
static constexpr uint8_t AIUS_CARRIER_RESET_DEBOUNCE_EPOCHS = 3;
static uint8_t g_carrier_unlocked_epochs = AIUS_CARRIER_RESET_DEBOUNCE_EPOCHS;

static FLASHMEM void pipeline_pcm_rate_service(uint32_t rate_hz)
{
	if (!g_audio_service_ready || rate_hz == g_pipeline_pcm_rate_hz)
		return;
	if (aius_pipeline_set_pcm_sample_rate(g_audio_service.pipeline, rate_hz) ==
	    AIUS_OK)
		g_pipeline_pcm_rate_hz = rate_hz;
}

static FLASHMEM bool replay_rate_requested(AiusReplaySource source)
{
	return source == AIUS_REPLAY_SOURCE_SWITCHING_TO_REPLAY ||
	       source == AIUS_REPLAY_SOURCE_REPLAY;
}

static FLASHMEM bool clock_plan_exact_equal(
	const AiusSpdifClockPlan &left,
	const AiusSpdifClockPlan &right)
{
	return left.nominal_rate_hz == right.nominal_rate_hz &&
	       left.target_rate_millihz == right.target_rate_millihz &&
	       left.pll_div_select == right.pll_div_select &&
	       left.pll_numerator == right.pll_numerator &&
	       left.pll_denominator == right.pll_denominator &&
	       left.root_predivider == right.root_predivider &&
	       left.root_postdivider == right.root_postdivider &&
	       left.tx_clock_divider == right.tx_clock_divider &&
	       left.channel_status_rate_bits ==
		       right.channel_status_rate_bits;
}

static FLASHMEM void clock_select_desired_plan(
	const AiusSpdifClockPlan &plan)
{
	if (g_clock_desired_plan_valid &&
	    clock_plan_exact_equal(g_clock_desired_plan, plan))
		return;
	g_clock_desired_plan = plan;
	g_clock_desired_plan_valid = true;
	g_clock_apply_attempted = false;
}

static FLASHMEM void clock_record_fault(
	AiusClockApplyResult fault,
	bool reset_already_requested)
{
	g_clock_live_ingress_open = false;
	close_stream_rate_gate();
	aius_audio_clock_begin_dark_transition();
	if (fault == AIUS_CLOCK_APPLY_INVALID_PLAN)
		g_clock_desired_plan_valid = false;
	g_clock_last_fault = (uint32_t)fault;
	g_clock_plan_applied = false;
	g_clock_replay_ready = false;
	g_clock_dark_transition_pending = true;
	if (g_clock_fault_episode)
		return;
	if (!reset_already_requested)
		request_audio_reset();
	g_clock_fault_episode = true;
	g_clock_faults++;
	record_event(AIUS_EVENT_CLOCK_FAULT, current_state(), (uint16_t)fault);
}

static FLASHMEM void clock_flush_ring_preserve_diag(void)
{
	/* The TX ISR is the sole cursor reader. The critical section is only two
	 * stores and preserves its lifetime underrun diagnostic. */
	noInterrupts();
	aius_spdif_tx24_flush_preserve_diag(&g_aius_spdif_tx24);
	interrupts();
}

static FLASHMEM bool clock_apply_desired_plan(
	uint32_t now_ms,
	bool flush_ring,
	bool reset_audio,
	bool fault_reset_owned)
{
	if (!g_clock_desired_plan_valid)
		return false;

	close_stream_rate_gate();
	aius_audio_clock_begin_dark_transition();
	g_clock_dark_transition_pending = true;
	if (flush_ring)
		clock_flush_ring_preserve_diag();
	if (reset_audio)
		request_audio_reset();
	/* fault_reset_owned covers resets established earlier in the same
	 * acquisition episode: locked-rate quiescence/relock credit and replay
	 * start. It is distinct from reset_audio, which describes only this call.
	 * A same-nominal structural replan passes false for both and lets the first
	 * fault own the one reset needed before producer admission recovers. */

	g_clock_apply_attempted = true;
	g_clock_last_apply_attempt_ms = now_ms;
	const AiusClockApplyResult applied =
		aius_audio_clock_apply(&g_clock_desired_plan);
	if (applied != AIUS_CLOCK_APPLY_OK) {
		clock_record_fault(
			applied, reset_audio || fault_reset_owned);
		return false;
	}
	g_clock_invalid_windows++;
	g_clock_fault_episode = false;
	return true;
}

static FLASHMEM void clock_refresh_applied(
	AiusReplaySource source,
	uint32_t now_ms)
{
	AiusAudioClockBoundaryStatus boundary = {};
	AiusSpdifClockPlan applied = {};
	const bool have_boundary =
		aius_audio_clock_get_boundary_status(&boundary);
	const bool have_applied =
		aius_audio_clock_get_applied_plan(&applied);

	if (have_applied)
		g_clock_output_rate_millihz = applied.target_rate_millihz;
	if (g_clock_dark_transition_pending && have_boundary &&
	    boundary.transition_complete)
		g_clock_dark_transition_pending = false;

	const bool desired_structure_applied =
		have_applied && g_clock_desired_plan_valid &&
		applied.nominal_rate_hz == g_clock_desired_plan.nominal_rate_hz &&
		aius_spdif_clock_plan_structurally_equal(
			&applied, &g_clock_desired_plan);
	const bool replay_selected = replay_rate_requested(source);
	g_clock_plan_applied =
		!replay_selected && desired_structure_applied &&
		applied.nominal_rate_hz == g_clock_nominal_rate_hz &&
		!g_clock_dark_transition_pending;
	g_clock_replay_ready =
		replay_selected && desired_structure_applied &&
		applied.nominal_rate_hz == AIUS_OPTICAL_SAMPLE_RATE_HZ &&
		applied.target_rate_millihz == UINT32_C(48000000) &&
		have_boundary && boundary.transition_complete &&
		!g_clock_dark_transition_pending;

	if (!replay_selected && have_applied &&
	    applied.nominal_rate_hz == g_clock_nominal_rate_hz &&
	    applied.target_rate_millihz !=
		    g_clock_tracker_noted_target_millihz &&
	    aius_rate_tracker_note_target_applied(
		    &g_rate_tracker, applied.target_rate_millihz, now_ms)) {
		g_clock_tracker_noted_target_millihz =
			applied.target_rate_millihz;
	}

	/* A full apply is synchronously bounded. A transient lock miss retries at
	 * most once per second, while one fault episode remains one counter/event. */
	if (g_clock_dark_transition_pending && !desired_structure_applied &&
	    (!g_clock_apply_attempted ||
	     (uint32_t)(now_ms - g_clock_last_apply_attempt_ms) >=
		     UINT32_C(1000)))
		(void)clock_apply_desired_plan(now_ms, false, false, true);
}

static FLASHMEM void sample_rate_service(uint32_t now_ms)
{
	const bool carrier = AudioInputSPDIF3::pllLocked();
	const AiusReplaySource source = g_replay_service.source;
	uint32_t measured_millihz = 0u;
	if (carrier &&
	    !aius_spdif_input_rate_millihz(&measured_millihz))
		measured_millihz =
			AudioInputSPDIF3::sampleRate() * UINT32_C(1000);
	AiusRateTrackerSample sample = {
		carrier,
		measured_millihz,
		replay_rate_requested(source)
			? AIUS_CLOCK_SOURCE_REPLAY
			: AIUS_CLOCK_SOURCE_LIVE,
		now_ms,
	};
	const AiusRateTrackerResult result =
		aius_rate_tracker_step(&g_rate_tracker, &sample);
	const uint32_t observed_nominal_hz =
		carrier ? aius_clock_snap_nominal_rate(measured_millihz) : 0u;
	if (g_clock_relock_reset_nominal_hz != 0u &&
	    observed_nominal_hz != g_clock_relock_reset_nominal_hz)
		g_clock_relock_reset_nominal_hz = 0u;

	if (replay_rate_requested(source))
		pipeline_pcm_rate_service(AIUS_OPTICAL_SAMPLE_RATE_HZ);
	g_clock_carrier_rate_millihz = sample.carrier_rate_millihz;
	g_clock_carrier_ready = result.carrier_ready;
	if (!result.carrier_ready) {
		g_clock_live_ingress_open = false;
		g_clock_live_ready = false;
	}
	if (result.quiesce_requested && carrier &&
	    !replay_rate_requested(source)) {
		/* Close the physical and producer gates on the first changed sample,
		 * then flush/reset the old-rate software state at the next AUDIO
		 * checkpoint. Matching acceptance consumes this reset marker instead
		 * of issuing a second classification reset. Carrier loss remains
		 * owned by the existing debounced relock/replay reset paths. */
		g_clock_live_ingress_open = false;
		close_stream_rate_gate();
		request_audio_reset();
		g_clock_relock_reset_nominal_hz = observed_nominal_hz;
	}

	if (result.nominal_accepted) {
		const uint32_t old_nominal = g_clock_nominal_rate_hz;
		const bool reset_for_acceptance =
			g_clock_relock_reset_nominal_hz !=
				result.nominal_rate_hz;
		AiusSpdifClockPlan plan = {};
		g_clock_nominal_rate_hz = result.nominal_rate_hz;
		g_clock_stream_rate_hz = 0u;
		g_clock_tracker_noted_target_millihz = 0u;
		g_clock_relock_reset_nominal_hz = 0u;
		g_clock_live_ready = false;
		g_clock_carrier_unsupported = false;
		pipeline_pcm_rate_service(result.nominal_rate_hz);
		if (old_nominal != result.nominal_rate_hz) {
			g_clock_rate_changes++;
			record_event(AIUS_EVENT_RATE_CHANGE, current_state(),
			             (uint16_t)result.nominal_rate_hz);
		}
		if (!aius_spdif_clock_plan(
			    result.nominal_rate_hz,
			    result.target_rate_millihz, &plan)) {
			clock_record_fault(AIUS_CLOCK_APPLY_INVALID_PLAN, true);
		} else {
			clock_select_desired_plan(plan);
			(void)clock_apply_desired_plan(
				now_ms, true, reset_for_acceptance, true);
		}
	} else if (result.target_update) {
		AiusSpdifClockPlan plan = {};
		AiusSpdifClockPlan applied = {};
		if (!aius_spdif_clock_plan(
			    g_clock_nominal_rate_hz,
			    result.target_rate_millihz, &plan)) {
			clock_record_fault(AIUS_CLOCK_APPLY_INVALID_PLAN, false);
		} else if (
			aius_audio_clock_get_applied_plan(&applied) &&
			aius_spdif_clock_plan_structurally_equal(&applied,
			                                         &plan)) {
			if (aius_audio_clock_request_trim(&plan)) {
				g_clock_desired_plan = plan;
				g_clock_desired_plan_valid = true;
				g_clock_trim_updates++;
				g_clock_invalid_windows++;
			}
		} else {
			clock_select_desired_plan(plan);
			g_clock_structural_replans++;
			(void)clock_apply_desired_plan(
				now_ms, false, false, false);
		}
	}

	if (result.unsupported_accepted) {
		g_clock_carrier_unsupported = true;
		g_clock_unsupported_episodes++;
		record_event(AIUS_EVENT_RATE_UNSUPPORTED, current_state(),
		             measured_millihz / UINT32_C(1000) > UINT16_MAX
			             ? UINT16_MAX
			             : (uint16_t)(
				             measured_millihz /
				             UINT32_C(1000)));
	}
	if (!carrier)
		g_clock_carrier_unsupported = false;

	if (replay_rate_requested(source) && !carrier) {
		AiusSpdifClockPlan replay_plan = {};
		AiusSpdifClockPlan applied = {};
		if (aius_spdif_clock_plan(
			    AIUS_OPTICAL_SAMPLE_RATE_HZ,
			    UINT32_C(48000000), &replay_plan)) {
			const bool already_applied =
				aius_audio_clock_get_applied_plan(&applied) &&
				clock_plan_exact_equal(applied, replay_plan);
			clock_select_desired_plan(replay_plan);
			if (!already_applied && !g_clock_apply_attempted) {
				const bool rate_changed =
					applied.nominal_rate_hz != 0u &&
					applied.nominal_rate_hz !=
						AIUS_OPTICAL_SAMPLE_RATE_HZ;
				(void)clock_apply_desired_plan(
					now_ms, rate_changed, rate_changed, true);
			}
		} else {
			clock_record_fault(AIUS_CLOCK_APPLY_INVALID_PLAN, true);
		}
	}

	clock_refresh_applied(source, now_ms);
	if (replay_rate_requested(source) || !g_clock_carrier_ready)
		g_clock_live_ingress_open = false;
	else if (g_clock_plan_applied)
		/* Monotonic within one accepted live episode: a same-nominal
		 * structural hold makes plan_applied temporarily false but must keep
		 * buffering because it deliberately preserves decoder generation. */
		g_clock_live_ingress_open = true;
}

static FLASHMEM void tx_gate_service(void)
{
	const bool carrier = AudioInputSPDIF3::pllLocked();
	if (!carrier) {
		if (g_carrier_unlocked_epochs < UINT8_MAX)
			g_carrier_unlocked_epochs++;
	} else {
		if (g_carrier_unlocked_epochs >=
		    AIUS_CARRIER_RESET_DEBOUNCE_EPOCHS) {
			request_audio_reset();
			g_clock_relock_reset_nominal_hz =
				aius_clock_snap_nominal_rate(
					g_clock_carrier_rate_millihz);
		}
		g_carrier_unlocked_epochs = 0;
	}
	const AiusReplaySource source = g_replay_service.source;
	const bool open =
		aius_tx_gate_permitted(carrier, g_clock_live_ready,
		                       g_clock_replay_ready, source);
	if (open != g_tx_gate_open)
		tx_gate_actuate(open);
	g_tx_gate_open = open;
	g_tx_gate_carrier = carrier;
	g_tx_gate_replay =
		aius_tx_gate_permitted(false, false, g_clock_replay_ready,
		                       source);
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
	replay_refill_service(now_ms);
	/* Settle carrier displacement before selecting a target clock. */
	replay_source_service();
	/* Apply or verify the settled source's live plan or exact 48 kHz replay
	 * plan before graph ownership can move. */
	sample_rate_service(now_ms);
	/* Memory only -- two byte stores under one critical section. Output takes
	 * graph ownership only after replay plan/hold acknowledgement. */
	replay_clock_service();
	/* After the clock reconciler for the same reason it runs last: the gate
	 * must judge this epoch's settled source and carrier, not the previous
	 * one's. A boolean and at most three register stores; no budget row. */
	tx_gate_service();
	/* Flight recorder, after tx_gate_service() by contract: the epoch
	 * trigger evaluation consumes this epoch's LATCHED gate and carrier
	 * (underrun qualification at both ends of the delta; carrier falling
	 * edge), and the drain quota rides this REPLAY budget row -- 8 KiB x
	 * ~115 epoch/s is 2x the 384 KiB/s fill rate, so a post-stall backlog
	 * clears. The drain is a pure copy/interleave into write-back-cached
	 * EXTMEM: nothing DMAs the PSRAM, so there is deliberately NO cache
	 * maintenance here. */
	if (g_recorder_present) {
		aius_recorder_epoch(&g_recorder, g_tx_gate_carrier,
		                    g_tx_gate_open,
		                    g_aius_spdif_tx24.underruns, now_ms);
		(void)aius_recorder_drain(&g_recorder,
		                          RECORDER_DRAIN_BYTES_PER_EPOCH);
	}
	return true;
}

static FLASHMEM bool platform_audio(void *, const AiusRuntimeBudget *budget,
                                    uint32_t now_ms)
{
	(void)now_ms;
	if (budget == nullptr ||
	    budget->checkpoint != AIUS_BOOT_CHECKPOINT_AUDIO ||
	    budget->max_calls != 1u || budget->max_rx_bytes != 0u ||
	    budget->max_tx_bytes != 0u ||
	    budget->service_budget_ms * 1000u !=
		    AIUS_AUDIO_SERVICE_BUDGET_US ||
	    !g_audio_service_ready)
		return false;

	const uint32_t cycles_started = ARM_DWT_CYCCNT;
	const AiusStatus status =
		aius_audio_service_run(&g_audio_service, g_audio_reset_sequence);
	const uint32_t cycles = ARM_DWT_CYCCNT - cycles_started;
	if (cycles > g_audio_checkpoint_max_cycles)
		g_audio_checkpoint_max_cycles = cycles;
	return status == AIUS_OK;
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
	/* Close the HTTP measurement before this checkpoint's own ledger
	 * commit joins the interval; open it after the HTTP commit for the
	 * same reason. Recovery-safe epochs run neither checkpoint, and an
	 * epoch that aborts inside HTTP never reaches UDP, so a stale stamp
	 * is unreadable by construction. */
	if (checkpoint == AIUS_BOOT_CHECKPOINT_UDP &&
	    (uint32_t)(micros() - g_http_checkpoint_entered_us) >
	            aius_runtime_budget(AIUS_BOOT_CHECKPOINT_HTTP)
	                            ->service_budget_ms *
	                    1000u)
		g_http_service_overruns++;
	aius_boot_record_checkpoint(&g_boot_ledger, checkpoint);
	(void)aius_boot_storage_teensy_commit(&g_boot_storage, &g_boot_ledger);
	aius_fault_checkpoint(checkpoint);
	if (checkpoint == AIUS_BOOT_CHECKPOINT_HTTP)
		g_http_checkpoint_entered_us = micros();
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
	ops.audio = platform_audio;
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
	/* The previous boot's final persisted checkpoint, captured HERE because
	 * nothing else keeps it: aius_boot_init() folds the retained ledger into
	 * this boot's and the stalled checkpoint of a watchdog-killed boot is
	 * otherwise unrecoverable (the recovery smoke documents exactly that
	 * gap). Surfaced as `pv=` on the USB ID line, which recovery-safe
	 * still serves. */
	g_prev_boot_checkpoint = aius_boot_is_valid(&retained)
		? retained.last_checkpoint : 0xffffu;
	/* Same capture-or-never window as `pv=`, same reader (`fa=`). Also
	 * re-arms the latch for this boot, before the watchdog exists. */
	g_prev_first_abort = aius_boot_teensy_abort_capture();
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
	/* FIRST: end the static-construction carrier blip. g_spdif_out's
	 * constructor ran begin() ~300 ms after reset with DMA already streaming
	 * zeroed buffers; this dark-set is the earliest firmware-controlled
	 * instant that can close the gate (no startup hook precedes constructors,
	 * and begin() would overwrite a hook's mux anyway). g_tx_gate_open is
	 * .bss false, so the latched state and the pad agree from here until
	 * checkpoint 7 first evaluates the policy. */
	tx_gate_actuate(false);
	aius_rate_tracker_init(&g_rate_tracker);

	capture_retained_boot_state();
	(void)aius_boot_teensy_watchdog_arm(&g_boot_primitives);
	probe_second_pad_psram();

	/* Initialise both ISR-visible handoffs before AudioMemory can let the audio
	 * graph run. DMAMEM is NOLOAD and cannot be trusted before these calls.
	 * The replay job diagnostics share that bank and require an explicit clear. */
	memset(&g_replay_diag, 0, sizeof(g_replay_diag));
	aius_replay_service_init(&g_replay_service);
	g_audio_ingress_ready =
		aius_audio_ingress_init(
			&g_audio_ingress, g_audio_ingress_words,
			AIUS_AUDIO_INGRESS_CAPACITY_WORDS) == AIUS_OK;
	AudioMemory(24);
	g_audio_processor.begin_pipeline();

	/* No parser is initialized here: the runtime's USB service owns its own
	 * AiusUsbRuntime, and the legacy one died with service_usb_control().
	 *
	 * There is no Serial.begin() either, and its absence is load-bearing.
	 * usb_serial_class::begin() ignores the baud rate entirely on Teensy 4 --
	 * the CDC endpoint is already up from the core's usb_init() -- and its whole
	 * body is a wait for the host to assert DTR: up to 2,000 ms once enumerated,
	 * 750 ms if the host has not begun enumeration (cores/teensy4/usb_serial.h).
	 * That is a wait on something external in a construction-only setup(), which
	 * §Normative recovery contract forbids, and it was costing 2,001 ms of every
	 * boot before the network stack was even started, because nothing holds DTR
	 * on this board. It was also the bring-up measurement's bimodality: with a
	 * host program holding the port open the wait ends early, which is the 1.66 s
	 * step between the two clusters. The CDC adapter needs none of it -- it reads
	 * and writes Serial directly and uses the same DTR bool as its connected
	 * predicate (aius_cdc_teensy.cpp serial_connected). */
	derive_auth_token();
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
	/* The retained boot facts are published HERE, before the first epoch, and
	 * not only from checkpoint 0.
	 *
	 * They are setup-time constants: capture_retained_boot_state() above has
	 * already read SRC_SRSR once and decided this boot's generation, early-reset
	 * count and mode, and nothing changes any of them afterwards. Publishing
	 * them from a per-epoch checkpoint made them a function of which checkpoints
	 * run -- and recovery-safe mode runs only 1 and 2 (aius_runtime.c), so
	 * checkpoint 0 never ran there and boot_generation, reset_source and
	 * watchdog_early_resets all read 0 on every status channel at exactly the
	 * moment the recovery boundary engaged and a reader needs them most.
	 *
	 * Two alternatives were rejected. Making reset capture unconditional, or
	 * adding checkpoint 0 to the recovery-safe set, buys the same three fields
	 * at the cost of the property that mode exists for: recovery-safe must run
	 * almost nothing, and each added checkpoint is a platform callback plus a
	 * ledger record-and-commit per epoch. Having the status serializers read
	 * g_boot_ledger directly would leave runtime->boot_status as a second,
	 * sometimes-stale copy of the same three values and put the retained facts
	 * on a different path per channel.
	 *
	 * The platform callback is reused rather than reimplemented so there is
	 * exactly one expression of what the three fields are; checkpoint 0 keeps
	 * republishing the identical values in normal mode, which costs nothing and
	 * keeps it an injectable checkpoint with a budget the scheduler checks. */
	(void)platform_reset_capture(
		nullptr,
		aius_runtime_budget(AIUS_BOOT_CHECKPOINT_RESET_CAPTURE),
		millis(), &g_runtime.boot_status);
	g_boot_healthy_at_ms = millis() + AIUS_BOOT_HEALTHY_MS;
}

/* ------------------------------------------------------------------ */
/* Counter edges -> events                                             */
/*                                                                     */
/* Before this, record_event() had three reachable call sites and the  */
/* ring answered almost nothing: every other observable fact on the    */
/* device -- overruns, dropped datagrams, budget overruns, config      */
/* errors, network restarts, DHCP updates, early watchdog resets --    */
/* was a counter in the status payload and nothing else, so a poller   */
/* saw a number that had changed but not when, and could not order it  */
/* against anything. These seven edges are the fix, and every          */
/* constraint below is load-bearing:                                   */
/*                                                                     */
/*  - Epoch-loop context only. loop() calls this once per pass, after  */
/*    the epoch that may have advanced the counters. Nothing here runs */
/*    in the audio ISR. The ingress drop projection is read through    */
/*    the processor's brief critical-section accessor.                 */
/*                                                                     */
/*  - One comparison per counter per epoch, against a scalar static.   */
/*    Seven uint32_t of .bss, no bank, no array, no queue, no          */
/*    transport. DTCM's free-for-locals zone is the whole stack for    */
/*    both the main loop and the audio ISR, so anything larger than a  */
/*    scalar per counter would be paid for out of it.                  */
/*                                                                     */
/*  - At most one event per counter per epoch, carrying the delta      */
/*    rather than one event per increment, so no burst can outrun the  */
/*    eight-entry ring. Two counters really can advance many times     */
/*    between two passes: ingress drops, written by the audio ISR once */
/*    per missing chunk and unbounded against an epoch that stalls,    */
/*    and g_config_error_count, which has one HTTP producer and two SD */
/*    ones. The remaining five are bounded to +1 by their producers    */
/*    (one datagram received per epoch, one budget check per epoch,    */
/*    one supervisor step, one lease, one retained ledger read) and    */
/*    are collapsed the same way regardless, so the bound is a         */
/*    property of this function and not of a producer that may change. */
/*                                                                     */
/*  - The ring's accounting stays exactly as truthful as it was. A     */
/*    burst overwrites the oldest entry and increments                 */
/*    g_event_overflows in record_event(); that counter is what the    */
/*    soak reads to know what it missed, and nothing here grows to     */
/*    avoid incrementing it.                                           */
/*                                                                     */
/*  - current_state() is called per emitted event rather than per      */
/*    epoch, because it reads the S/PDIF peripheral. Events are rare;  */
/*    epochs are not.                                                  */
/*                                                                     */
/* Not included, deliberately: the four http_* counters. Their         */
/* producers are reinstated (see put_status()), but every HTTP failure */
/* they count is already ordered by the request/response exchange the  */
/* client saw fail, so a poller diffing status learns the same fact an */
/* edge would carry; adopt them here only if defect 24's investigation */
/* proves it needs device-side ordering. And record_state()            */
/* is NOT called from here: a state edge is already an event, and a    */
/* flapping carrier detected per epoch rather than per poll would fill */
/* the ring with the one thing it already reported.                    */
/* ------------------------------------------------------------------ */

static uint32_t g_prev_overruns;
static uint32_t g_prev_network_drops;
static uint32_t g_prev_network_budget_overruns;
static uint32_t g_prev_config_errors;
static uint32_t g_prev_network_restarts;
static uint32_t g_prev_dhcp_updates;
static uint32_t g_prev_watchdog_early_resets;

/* One counter's edge in one comparison: the advance since the previous epoch,
 * clamped to the ring's 16-bit detail field, or 0 for "nothing to report".
 *
 * The static is resynchronised on every call, including a decrease: POST
 * /api/counters clears the ingress-drop interval, and a clear must cost one
 * missed edge rather
 * than a permanent four-billion delta on the next real one. */
static FLASHMEM uint16_t counter_edge(uint32_t current, uint32_t *previous)
{
	const uint32_t delta = current - *previous;

	*previous = current;
	if (delta == 0u || delta > 0x7FFFFFFFu)
		return 0u;
	return delta > 0xFFFFu ? (uint16_t)0xFFFFu : (uint16_t)delta;
}

static FLASHMEM void service_event_edges(void)
{
	const AiusNetworkTeensy *network = &g_runtime.network_http.target;
	const AiusNetSupervisor *supervisor = &g_runtime.network_http.supervisor;
	uint16_t delta;

	delta = counter_edge(g_audio_processor.overrun_count(), &g_prev_overruns);
	if (delta != 0u)
		record_event(AIUS_EVENT_OVERRUN, current_state(), delta);

	delta = counter_edge(g_network_drops, &g_prev_network_drops);
	if (delta != 0u)
		record_event(AIUS_EVENT_NETWORK_DROP, current_state(), delta);

	delta = counter_edge(g_network_budget_overruns,
	                     &g_prev_network_budget_overruns);
	if (delta != 0u)
		record_event(AIUS_EVENT_NETWORK_BUDGET, current_state(), delta);

	/* detail is the error code, not the delta: which parse failed is the
	 * diagnostic, and the count itself is already in the status payload. */
	delta = counter_edge(g_config_error_count, &g_prev_config_errors);
	if (delta != 0u)
		record_event(AIUS_EVENT_CONFIG_ERROR, current_state(),
		             (uint16_t)g_config.error_code);

	/* Likewise: a restart's delta is always 1, and the fault that caused it
	 * is the only thing a reader of this event wants. */
	delta = counter_edge(supervisor->restarts, &g_prev_network_restarts);
	if (delta != 0u)
		record_event(AIUS_EVENT_NETWORK_RESTART, current_state(),
		             (uint16_t)supervisor->last_fault);

	delta = counter_edge(network->dhcp_updates, &g_prev_dhcp_updates);
	if (delta != 0u)
		record_event(AIUS_EVENT_DHCP_UPDATE, current_state(), delta);

	/* Fixed for the life of the boot, so this fires at most once: on the
	 * first epoch after the RESET_CAPTURE checkpoint publishes it. That is
	 * the point -- it puts the retained early-reset count into the same
	 * ordered stream as everything that happened after it. */
	delta = counter_edge(g_runtime.boot_status.watchdog_early_resets,
	                     &g_prev_watchdog_early_resets);
	if (delta != 0u)
		record_event(AIUS_EVENT_WATCHDOG_EARLY_RESET, current_state(),
		             delta);
}

/* ------------------------------------------------------------------ */
/* Requested reset -- POST /api/reboot                                 */
/*                                                                     */
/* Ceiling on how long a requested reset waits for its own             */
/* acknowledgement to drain. 500 ms, the same figure and the same      */
/* reasoning as AIUS_USB_BOOTLOADER_DELAY_MS: a terminal action may    */
/* wait on a genuine retirement signal, but it may never be prevented  */
/* by a peer, and the 8 s watchdog window comfortably covers it.       */
/*                                                                     */
/* It is five times the transaction's own RESPONSE_TIMEOUT_MS (100 ms, */
/* aius_http_transaction.c:25), so a peer that stops reading is        */
/* drained by the transaction's own fault path -- record the timeout,  */
/* close, restart_idle -- long before this expires. The ceiling is the */
/* backstop for the case the transaction cannot resolve at all: the    */
/* HTTP checkpoint does not run while the network generation is        */
/* offline (aius_runtime_network_http.cpp:205), so a link or DHCP      */
/* failure between the dispatch and the drain would otherwise leave    */
/* the transaction parked forever with a reset outstanding.            */
/* ------------------------------------------------------------------ */

static constexpr uint32_t AIUS_REBOOT_DRAIN_MS = 500u;
static uint32_t g_reboot_deadline_ms;
static bool g_reboot_deadline_armed;

/* True once the HTTP transaction is back at its drained terminal state.
 *
 * AIUS_HTTP_TX_IDLE is reached only through restart_idle()
 * (aius_http_transaction.c:1536), and on the response path that is strictly
 * after the last WRITE was accepted by the socket and the CLOSE completed, so
 * observing it means the acknowledgement is on the wire rather than merely
 * buffered. That is the whole difference from the retired behaviour: the
 * dispatching epoch retains a pending application event, and format_response()
 * has not yet run, so at the end of that epoch not one byte of the 200 exists.
 *
 * outstanding_action is required NONE as well so no transport primitive is in
 * flight when the core resets. In IDLE the only one that can be outstanding is
 * an ACCEPT, and the transaction alternates ACCEPT / NONE every other epoch, so
 * this costs at most one further epoch and cannot deadlock.
 *
 * A teardown that discards the transaction (http_init() on
 * AIUS_NET_ACTION_TEARDOWN) also lands here, which is correct: the response can
 * no longer be delivered, so there is nothing left to wait for.
 *
 * Reading the state is not a step: this is a plain load of two fields outside
 * the epoch, so it cannot consume an event, emit an action, stall a service or
 * make one return false. */
static FLASHMEM bool reboot_drain_complete(void)
{
	const AiusHttpTransaction *http = &g_runtime.network_http.http;

	return http->state == AIUS_HTTP_TX_IDLE &&
	       http->outstanding_action == AIUS_HTTP_ACTION_NONE;
}

/* One epoch's decision about a reset that has been asked for and not yet
 * performed. Arms the ceiling on the first epoch that sees the request -- which
 * is the epoch the dispatch happened in, where the transaction is still in
 * AIUS_HTTP_TX_DISPATCH -- then resets on the first epoch where the transaction
 * has drained, or once the ceiling expires, whichever comes first.
 *
 * On expiry the reset happens regardless. A requested reset is honoured; only
 * its acknowledgement is best-effort. The alternative -- waiting indefinitely --
 * would let a wedged or vanished peer veto the one operation that recovers the
 * board. */
static FLASHMEM void service_reboot_request(uint32_t now_ms)
{
	if (!g_reboot_deadline_armed) {
		g_reboot_deadline_ms = now_ms + AIUS_REBOOT_DRAIN_MS;
		g_reboot_deadline_armed = true;
		return;
	}
	if (reboot_drain_complete() ||
	    (int32_t)(now_ms - g_reboot_deadline_ms) >= 0) {
		/* "Reboot with TX disabled first": a clean driven LOW until the ROM
		 * takes the pad back to its undriven reset default, rather than a
		 * carrier that dies mid-frame at reset. */
		tx_gate_actuate(false);
		reset_application();
	}
}

/* Exactly one runtime epoch, then the three things the epoch itself must not do.
 *
 * The normal epoch runs checkpoints 0..7, AUDIO 9, then SD 8, records
 * EPOCH_COMPLETE 10 and feeds the watchdog once; an incomplete epoch returns
 * false without that feed, which is what makes a wedged service a bounded
 * reset rather than a hang.
 *
 * request_reboot() only records (aius_optical_app.cpp), because the dispatcher
 * never resets or delays, so the reset happens here -- but NOT at the end of the
 * epoch that dispatched. That epoch has only retained a pending application
 * response: format_response() runs inside accept_application(), which executes
 * only when the NEXT step consumes that event, so at the end of the dispatching
 * epoch not one byte of the 200 exists. Counted in epochs, with N the dispatch:
 * N+1 formats and performs the first <=512-byte socket write, N+2 consumes its
 * result, and the close plus restart_idle() are later still. Resetting here
 * unconditionally is exactly the defect the B6 reboots gate hit: boot_generation
 * advanced and the client timed out. service_reboot_request() gates it on the
 * transaction reaching AIUS_HTTP_TX_IDLE, bounded at AIUS_REBOOT_DRAIN_MS.
 *
 * service_event_edges() is the epoch's one observability step and the sole call
 * site of the counter edges above; it belongs here rather than at a checkpoint
 * because an aborted epoch must still get its counters read.
 *
 * FLASHMEM for the same reason as setup(): loop() is the epoch driver, not
 * per-block work. The audio path reaches the processor through AudioStream's
 * interrupt, never through here, so nothing in this function runs per block. */
FLASHMEM void loop(void)
{
	const uint32_t now_ms = millis();
	const uint32_t epoch_started_us = micros();
	const bool completed = aius_runtime_teensy_epoch(&g_runtime, now_ms);
	const uint32_t epoch_us = micros() - epoch_started_us;
	if (completed && !g_runtime.recovery_safe) {
		if (epoch_us > g_runtime_epoch_max_us)
			g_runtime_epoch_max_us = epoch_us;
		if (epoch_us > AIUS_RUNTIME_EPOCH_BUDGET_US)
			g_runtime_epoch_budget_overruns++;
	}

	/* After the epoch, and unconditionally: an epoch that aborted before
	 * EPOCH_COMPLETE is exactly the case whose counters explain the abort. */
	service_event_edges();
	if (!completed)
		/* One-shot into retained RAM: the checkpoint persisted before the
		 * service that just returned false -- the defect-25 datum `pv=`
		 * structurally cannot carry. */
		aius_boot_teensy_abort_note(g_runtime.last_checkpoint);
	if (completed && !g_boot_marked_healthy &&
	    (int32_t)(now_ms - g_boot_healthy_at_ms) >= 0) {
		/* Persisted by the next checkpoint, which commits the ledger. */
		aius_boot_mark_healthy(&g_boot_ledger);
		g_boot_marked_healthy = true;
	}
	if (g_reboot_requested)
		service_reboot_request(now_ms);
}
