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
	#include "aius_measure.h"
	#include "aius_recorder.h"
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

/* THE ONE PLACE THE OUTPUT CLOCK AND THE DSP MODEL ARE FORCED TO AGREE.
 *
 * AudioStream.h defaults AUDIO_SAMPLE_RATE_EXACT to 44100.0f behind an #ifndef,
 * and config_spdif3() (output_spdif3.cpp) derives the whole S/PDIF transmit clock
 * tree from that macro -- so it, and nothing else, is the rate this board's
 * optical output actually runs at. Every other rate in the firmware is 48 kHz:
 * the DTS Core decoder's `output_rate`, AIUS_PIPELINE_PCM_SAMPLE_RATE_HZ, this
 * file's biquad and limiter coefficients (apply_config), the meter's
 * `sample_rate_hz` and publish cadence, and the replay seam's 192,000 B/s.
 *
 * With the default left in place those two disagreed by 8.1 % and NOTHING
 * observed it: the graph's producer and its consumer are both driven by the same
 * clock, so the queue stayed level, the overrun counters stayed honestly zero,
 * and the only symptom was audio playing ~1.4 semitones flat. There is no
 * resampler anywhere in this path to absorb the difference -- see the audio
 * clock note above AiusAudioProcessor -- so the rates must simply be equal.
 *
 * A compile-time equality is the whole fix for the class of defect, not just
 * this instance (root CLAUDE.md §Convention enforcement): the build flag and the
 * config JSON can now only move together. */
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
 * names as scarcest. Written once per epoch at most, read by the status route. */
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
/* The ISR/epoch pair needs no barrier beyond the ones the nested-     */
/* interrupt argument above g_meter already makes: the audio interrupt */
/* preempts the epoch, runs its transition to completion and returns.  */
/* ------------------------------------------------------------------ */
DMAMEM static int32_t g_measure_capture_l[AIUS_MEASURE_CAPTURE_FRAMES];
DMAMEM static int32_t g_measure_capture_r[AIUS_MEASURE_CAPTURE_FRAMES];
static AiusMeasureCapture g_measure_capture;

/* ------------------------------------------------------------------ */
/* PSRAM flight recorder (aius-optical-prototype.md §PSRAM flight      */
/* recorder, prebuild-gated 2026-07-27)                                */
/*                                                                     */
/* Staging is a heap-carved OCRAM section (4 planar 512-frame slots,   */
/* 16 KiB -- the amended contract's depth after defect 26); control    */
/* state is DTCM for the boot-ROM-scribbles-OCRAM reason above. The    */
/* RING is                                                             */
/* neither: raw PSRAM at 0x70000000 sized by the boot probe, bound at  */
/* begin_pipeline() and touched ONLY by the epoch drain. No EXTMEM     */
/* static exists anywhere in this firmware and the extmem heap is      */
/* banned outright (test/recorder/test_no_extmem_heap.py) -- an        */
/* EXTMEM array links silently wrong on an 8 MB board and the smalloc  */
/* pool spans the same storage the ring claims.                        */
/*                                                                     */
/* g_recorder_present is the whole per-block cost on a bare board:     */
/* one bool test in enqueue() (the g_meters_active pattern), and the   */
/* epoch service returns on the same latch.                            */
/* ------------------------------------------------------------------ */
/* 4 slots = 16 KiB ~= 42.7 ms of staging: 4x the measured stacked epoch
 * worst case (5.5 ms SD primitive + 2 + 2 + 1 ms budgets). NOT 16 slots:
 * OCRAM's heap must keep >= ~96 KiB for NativeEthernet's boot-time mallocs
 * (64 KiB FNET stack heap + 8 x 4 KiB socket buffers) or network bring-up
 * fails and the boot dies -- defect 26. The linker now ASSERTs a 128 KiB
 * heap floor, so an over-grown staging ring fails the BUILD, not the
 * board. */
#define RECORDER_STAGING_SLOTS 4u
#define RECORDER_SLOT_FRAMES (AUDIO_BLOCK_SAMPLES * 4u)
#define RECORDER_DRAIN_BYTES_PER_EPOCH 8192u
/* In their own linker section rather than DMAMEM: the dedicated
 * .bss.aius_staging section (placed after every .bss.dma input, carved out
 * of the heap by link/aius_teensy41.ld) makes the recorder's OCRAM cost
 * visible to the linker's heap-floor ASSERT -- the defect 26 gate -- and
 * keeps a resize from ever moving a pre-existing DMAMEM address. Same OCRAM
 * bank, same cache behaviour; only packing and accountability differ. */
__attribute__((section(".aius_recorder_staging"), used, aligned(32)))
static int32_t
	g_recorder_staging[RECORDER_STAGING_SLOTS * RECORDER_SLOT_FRAMES * 2u];
__attribute__((section(".aius_recorder_staging"), used, aligned(32)))
static uint32_t g_recorder_staging_frames[RECORDER_STAGING_SLOTS];
static AiusRecorder g_recorder;
static bool g_recorder_present = false;
/* The boot probe's result (startup.c configure_external_ram), in MB. */
extern "C" uint8_t external_psram_size;
/* The previous boot's final persisted checkpoint (0xffff when the retained
 * ledger was invalid), captured by capture_boot_state() before aius_boot_init
 * folds it away. `pv=` on the USB ID line: the only after-the-fact evidence of
 * WHICH checkpoint a watchdog-killed boot died in. */
static uint16_t g_prev_boot_checkpoint = 0xffffu;
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

/* The pipeline classifies once and nothing on target ever called
 * aius_pipeline_reset(): a DTS-first boot followed by a PCM replay hunts for
 * an IEC preamble that never comes (zero blocks, a capture armed forever),
 * and PCM-first followed by DTS plays framed bursts as noise. Replay start
 * arms this flag and update() -- audio context, before any words are pushed
 * -- performs the reset, so every replay classifies its own stream. A start
 * against a live carrier is refused upstream, so the reset cannot land mid
 * live-input stream. */
static volatile bool g_pipeline_reset_pending;

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
	out.matrix.lfe_crossover_hz = model.matrix.lfe_crossover_hz;
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
 *                             count, which is fixed for the life of the boot. */
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
/* The audio clock, and the TWO block sizes that are easy to confuse   */
/*                                                                     */
/* THERE IS NO RESAMPLER ANYWHERE IN THIS PATH. Neither peripheral      */
/* converts rate: AudioInputSPDIF3 DMAs receiver words straight into    */
/* audio blocks at the incoming carrier's rate (the retained snapshot   */
/* predates the upstream asynchronous converter, and the framework copy */
/* is byte-identical to it), and the transmit DMA clocks frames out at  */
/* AUDIO_SAMPLE_RATE_EXACT. Nothing between them compares the two, and  */
/* nothing compares block->sample_rate_hz against either. A rate        */
/* disagreement is therefore not absorbed and not counted -- it is      */
/* simply heard, which is why the static_assert at the top of this file */
/* exists. The engine rate and the decoded rate must be equal by        */
/* construction; 48 kHz, because DTS Core is 48 kHz.                    */
/*                                                                     */
/* TWO BLOCK SIZES, and every per-block figure names one of them:       */
/*                                                                     */
/*   AUDIO_BLOCK_SAMPLES = 128 frames (the Teensy core default; this    */
/*   build does not override it) is the ENGINE block. It sizes the DMA  */
/*   buffers, the audio_block_t pool, update()'s word staging and the   */
/*   transmit ISR's drain of the 24-bit ring. update() therefore runs   */
/*   48000/128 = 375 times a second and moves 256 IEC words = 512 bytes */
/*   each time -- the 192,000 B/s the replay ring is sized for.         */
/*                                                                     */
/*   512 frames is the DECODED block, and it is what `blocks` counts.   */
/*   A DTS Core access unit reconstructs to 512 samples per channel and */
/*   the PCM passthrough stages the same 512                           */
/*   (AIUS_PIPELINE_PCM_BLOCK_FRAMES), so enqueue() is called once per  */
/*   four update()s, at 48000/512 = 93.75 blocks per second. The        */
/*   AUDIO_BLOCK_SAMPLES * 4 cap enqueue() applies is exactly that 512: */
/*   the two constants are coupled, so changing the engine block would  */
/*   change the decoder block this file will accept.                    */
/*                                                                     */
/* The DWT bracket in enqueue() measures the DECODED block, so its      */
/* budget is 512 frames / 48 kHz = 10,667 us (6,400,000 cycles at       */
/* 600 MHz), not 128 frames. The meter's `queue_frames` is neither of   */
/* these: it is aius_spdif_tx24_level(), the 24-bit ring's occupancy at */
/* the publish instant, which sits at 512 because one decode fills the  */
/* ring by 512 and the four following transmit interrupts drain it by   */
/* 128 each.                                                           */
/*                                                                     */
/* THE FINAL HOP IS NO LONGER audio_block_t. The transmit DMA interrupt */
/* drains the 24-bit ring straight into SPDIF_tx_buffer                */
/* (lib/aius_audio/src/aius_spdif_tx24.h), so this class has no         */
/* emit_block(), allocates no pool blocks, and is connected to no       */
/* output node. The consequence that matters here: the ring's producer  */
/* (enqueue(), reached from the software ISR that update_all() pends)   */
/* and its consumer (the transmit DMA ISR) are now at DIFFERENT NVIC    */
/* priorities, where both used to sit inside update(). That is why the  */
/* ring is a stage/commit single-producer/single-consumer structure and */
/* not the plain read/write/count triple the int16 ring could afford.   */
/* ------------------------------------------------------------------ */
class AiusAudioProcessor : public AudioStream
{
public:
	AiusAudioProcessor(void) : AudioStream(2, g_input_queue) {}

	void begin_pipeline(void)
	{
		apply_config(AIUS_OPTICAL_CONFIG);
		aius_out16_init(&g_out16, AIUS_DSP_DITHER_SEED_DEFAULT, 1);
		/* Bind the capture planes; memory only. The machine starts idle, and
		 * an idle capture costs the tap one volatile load per block. */
		(void)aius_measure_capture_init(&g_measure_capture,
		                                g_measure_capture_l,
		                                g_measure_capture_r,
		                                AIUS_MEASURE_CAPTURE_FRAMES);
		/* Bind the flight recorder before the transmit interrupt arms
		 * (its rule, same as the tx24 cursors). The probe ran in
		 * ResetHandler, so external_psram_size is settled; 0 MB arms
		 * nothing and the recorder reports absent -- a bare board must
		 * never fault, and the MPU maps the absent region RW so only
		 * this latch protects it. */
		g_recorder_present = external_psram_size != 0u &&
			aius_recorder_init(&g_recorder,
			                   reinterpret_cast<uint8_t *>(0x70000000u),
			                   (uint32_t)external_psram_size * 1048576u,
			                   external_psram_size, g_recorder_staging,
			                   g_recorder_staging_frames,
			                   RECORDER_STAGING_SLOTS,
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
		/* The replay-start pipeline reset, in audio context and before any
		 * words are pushed: the next words classify as their own stream. See
		 * g_pipeline_reset_pending's block comment. */
		if (g_pipeline_reset_pending && g_pipeline != nullptr) {
			aius_pipeline_reset(g_pipeline);
			g_pipeline_reset_pending = false;
		}
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
		/* No emit_block(): the transmit DMA interrupt drains the 24-bit ring
		 * itself, so this callback's only job is to push the epoch's input
		 * words into the pipeline. */
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
	/* The LFE fold's LR4 low-pass state, 160 B. Beside g_limiter so it lands in
	 * the same DTCM .bss as the other DSP state rather than the free-for-locals
	 * zone, and adds no stack local to the audio path. */
	AiusLfeFold g_lfe_fold = {};
	bool g_limiter_active = false;
	/* The output stage's dither streams, seeded ONCE in begin_pipeline() and
	 * deliberately not in apply_config(): re-seeding on every staged config change
	 * would restart the noise sequence and make the output a function of
	 * config-change history rather than of the sample index alone (see aius_dsp.h
	 * §DETERMINISM). Seeded even though OUTPUT_WIDTH_BITS bypasses dither, so that
	 * flipping the width is a one-line change and not also a bring-up change. */
	AiusOut16Stage g_out16 = {};
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
		if (block->frames > aius_spdif_tx24_free(&g_aius_spdif_tx24))
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
		/* Item 3 capture tap. At OUTPUT_WIDTH_BITS = 24 the words just
		 * committed ARE processed_left/right verbatim (the out stage is a
		 * copy), so tapping the planes taps the wire. After the commit and
		 * before anything else so the capture covers exactly the committed
		 * frames; the latches are this block's config generation and overrun
		 * count, read in the same interrupt that would have advanced them.
		 * Cost when no capture is in flight: one volatile load and compare
		 * inside the callee. */
		aius_measure_isr_block(&g_measure_capture, processed_left,
		                       processed_right, block->frames,
		                       g_config_generation, g_overruns);
		/* Flight-recorder tap: the same committed planes, into OCRAM
		 * staging (never EXTMEM from this context). Sits beside the
		 * measure tap so both cover exactly the committed frames. The
		 * limiter gain is read in the interrupt that just advanced it;
		 * the underrun counter is the tx24 ring's own volatile u32.
		 * Cost when no PSRAM is fitted: this one bool test. */
		if (g_recorder_present)
			aius_recorder_isr_block(&g_recorder, processed_left,
			                        processed_right, block->frames,
			                        g_overruns,
			                        g_aius_spdif_tx24.underruns,
			                        dsp.limiter != nullptr,
			                        dsp.limiter != nullptr
			                            ? (float)dsp.limiter->state.gain
			                            : 1.0f,
			                        millis());
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
				 * by apply_config(), and the level is bounded by
				 * AIUS_SPDIF_TX24_RING_FRAMES, so this is one 32x32 multiply
				 * and a shift. The level is the WHOLE transmit queue now --
				 * there is no vendor two-deep block queue behind it to be
				 * unaccounted for. */
				const uint32_t queued =
					aius_spdif_tx24_level(&g_aius_spdif_tx24);
				aius_meter_set_telemetry(&g_meter, queued,
					(uint32_t)(((uint64_t)queued *
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
};

FLASHMEM void AiusAudioProcessor::apply_config(const AiusOpticalConfig &cfg)
{
	/* features.output_eq, the other flag that was stored and never read. Zero
	 * sections IS the bypass -- aius_dsp_process() runs no cascade for an empty
	 * table -- which is why this stage appeared to work: a config that happened
	 * to carry no sections bypassed by accident, and one that carried sections
	 * ran them whatever the flag said. */
	g_eq_count = cfg.features.output_eq ? cfg.output_eq_count : 0u;
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
	/* Hoisted above the matrix block so the LFE crossover can be designed here,
	 * cold: aius_lfe_fold_init() evaluates two cos/sin pairs, which belongs
	 * beside the biquad trig below rather than in the audio ISR. The downmix
	 * would design lazily on the first block otherwise. */
	const uint32_t rate = cfg.sample_rate_hz != 0u ? cfg.sample_rate_hz
	                                               : AIUS_OPTICAL_SAMPLE_RATE_HZ;
	g_downmix = aius_downmix_config_default();
	/* features.matrix, which was parsed, stored, round-tripped and then never
	 * read -- the contract promises all five flags bypass their stage and this
	 * one had no mechanism at all.
	 *
	 * BYPASS HERE MEANS FL->L, FR->R AT UNITY, not "use the default matrix".
	 * The downmix is structurally unavoidable (six planes have to become two),
	 * so the question is what a bypassed matrix should sound like, and the
	 * editor states the purpose: unchecking a stage bypasses it "for latency
	 * and diagnostic comparisons". The comparison worth hearing is the stereo
	 * pair with nothing folded into it, which is what zero centre, zero
	 * surround, zero LFE and unity headroom reduce aius_dsp_process()'s
	 * accumulate loop to exactly -- no new machinery, no second code path, and
	 * the existing struct expresses it.
	 *
	 * Substituting the DEFAULT matrix instead was the other reading and is
	 * rejected: it would make the flag mean "ignore my settings" rather than
	 * "remove this stage", and there would then be no way to hear the input
	 * without the fold. */
	if (cfg.features.matrix) {
		g_downmix.center = cfg.matrix.center_gain_linear;
		g_downmix.surround = cfg.matrix.surround_gain_linear;
		g_downmix.lfe = cfg.matrix.lfe_gain_linear;
	} else {
		g_downmix.center = 0.0;
		g_downmix.surround = 0.0;
		g_downmix.lfe = 0.0;
	}
	/* The LFE fold's low-pass. The DTS core does not band-limit its LFE plane to
	 * the crossover -- it carries LFE decimated by 64 or 128, so up to 375 Hz of
	 * reconstruction imaging survives -- which is why this is LR4 rather than one
	 * section. If the live stream's rate differs from the config's, the downmix
	 * redesigns from the block's real rate, which is the intended behaviour. */
	g_downmix.lfe_crossover_hz = cfg.matrix.lfe_crossover_hz;
	g_downmix.lfe_fold = &g_lfe_fold;
	(void)aius_lfe_fold_init(&g_lfe_fold, (double)rate,
	                         cfg.matrix.lfe_crossover_hz);
	if (!cfg.features.matrix)
		/* Unity, completing the bypass: a headroom scale is part of the matrix
		 * stage, so a bypassed matrix must not attenuate either. Note
		 * clipfree_headroom() would return 1.0 here anyway now that the three
		 * gains are zero, but stating it is what stops a later change to that
		 * function quietly putting a gain back into the bypass path. */
		g_downmix.headroom = 1.0;
	else if (cfg.matrix.clipfree)
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
	/* The stage runs only when it would change a sample.
	 *
	 * THE DEFAULT CONFIG MADE THIS AN EXACT IDENTITY THAT STILL COST. Every
	 * channel ships at gain_db 0 and eq [], with features.input_eq true, so all
	 * six planes were copied to scratch and run through run_cascade(1.0, NULL,
	 * 0, ...) -- an int-to-double convert and an aius_quantize per sample, 3,072
	 * quantizes per block, to reproduce the input exactly.
	 *
	 * Skipping is behaviourally identical rather than an approximation: the
	 * downmix reads block->planes when dsp.input is null, and the stage's only
	 * effect in this state was to hand it a verbatim copy of those same planes.
	 *
	 * Unity is tested exactly, and that is safe because it is the value the
	 * conversion produces rather than a tolerance: aius_db_to_linear(0.0) is
	 * pow(10, 0) == 1.0 exactly, on the host and on the target. A channel with a
	 * present-but-bypassed EQ section is deliberately NOT treated as identity --
	 * eq_sections counts what the cascade will walk, and the config distinguishes
	 * "no section" from "a bypass section" on purpose. */
	bool input_stage_changes_audio = false;
	for (size_t c = 0; c < AIUS_DSP_INPUT_CHANNELS; c++) {
		const AiusInputChannelConfig &channel = g_input_stage.channels[c];
		if (channel.gain_linear != 1.0 || channel.eq_sections != 0) {
			input_stage_changes_audio = true;
			break;
		}
	}
	g_input_stage_active = cfg.features.input_eq != 0 && input_stage_changes_audio;

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
 * it. Evaluation lives in tx_gate_service() at checkpoint 7. */
static bool g_tx_gate_open;    /* latched per-epoch decision, .bss = closed */
static bool g_tx_gate_carrier; /* latched inputs, served on /api/status -- */
static bool g_tx_gate_replay;  /* never re-derived at request time */

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
	/* lfe_crossover_hz is required by the PUT validator, so omitting it here
	 * would make a GET -> PUT round-trip fail 400 on a missing key. */
	if (!http_json_append(output, capacity, &length,
	                      ",\"matrix\":{\"center_gain_linear\":%.17g,\"surround_gain_linear\":%.17g,\"lfe_gain_linear\":%.17g,\"lfe_crossover_hz\":%.17g,\"headroom_linear\":%.17g,\"clipfree\":%s},",
	                      cfg.matrix.center_gain_linear, cfg.matrix.surround_gain_linear,
	                      cfg.matrix.lfe_gain_linear, cfg.matrix.lfe_crossover_hz,
	                      headroom, cfg.matrix.clipfree ? "true" : "false"))
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
		"{\"version\":%u,\"state\":%u,\"carrier\":%u,\"format\":%u,\"sample_rate\":%lu,\"blocks\":%lu,\"overruns\":%lu,\"source\":\"%s\",\"sd_present\":%u,\"sd_filesystem\":\"%s\",\"sd_total_bytes\":%llu,\"sd_used_bytes\":%llu,\"sd_free_bytes\":%llu,\"config_valid\":%u,\"config_error_code\":%u,\"config_error\":\"%s\",\"config_error_count\":%lu,\"network_mode\":\"%s\",\"ip\":\"%u.%u.%u.%u\",\"link\":%u,\"network_restarts\":%lu,\"dhcp_pending\":%u,\"dhcp_discovers\":%lu,\"dhcp_updates\":%lu,\"reset_source\":%lu,\"http_server_rebinds\":%lu,\"cpu_temp_c\":%.2f,\"cpu_temp_valid\":%u,\"cpu_temp_max_c\":%.2f,\"cpu_temp_age_ms\":%lu,\"build_revision\":\"%s\",\"build_utc\":\"%s\",\"event_sequence\":%lu,\"event_overflows\":%lu,\"network_drops\":%lu,\"network_budget_overruns\":%lu,\"http_requests\":%lu,\"http_rejections\":%lu,\"http_client_drops\":%lu,\"http_service_overruns\":%lu,\"network_state\":%u,\"recovery_safe\":%u,\"network_last_fault\":%u,\"last_checkpoint\":%u,\"network_generation\":%lu,\"boot_generation\":%lu,\"watchdog_early_resets\":%u,\"loop_epoch\":%lu,\"tx_gate\":{\"open\":%u,\"carrier_locked\":%u,\"replay_running\":%u}}",
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
	/* The storage service's own failure record. Two SD failures that are
	 * identical on the wire -- a job refused because another owner held the
	 * volume, and a card that refused the write -- both surface as
	 * `500 sd_failure`, so the difference has to come from the device. */
	if (!aius_runtime_storage_diag_json(&g_runtime.storage, (char *)body,
	                                    capacity, &length))
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
	/* The item 3 prerequisite fix: every replay classifies its own stream.
	 * Consumed by update() in audio context before the first words land. */
	g_pipeline_reset_pending = true;
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
	/* Clearing zeroes g_overruns, the baseline the capture's arm-time latch
	 * compares against by EQUALITY -- a clear during a live capture could
	 * therefore mask a real overrun's 512-frame seam (latched 0, one overrun,
	 * then the clear restores 0). Retire the capture instead: the epoch stall
	 * watch or the ISR's next block completes the cancel, and GET /api/measure
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
	(sizeof(" nrst=") - 1u) + AIUS_ID_U32_DIGITS +
	(sizeof(" ps=") - 1u) + 3u +
	(sizeof(" pv=") - 1u) + 5u;
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
	 * persisted checkpoint (65535 = retained ledger invalid) -- the two
	 * fields a recovery-safe board most needs to explain itself. */
	int written = snprintf(payload, capacity,
		"protocol=1 revision=%s build=%s mode=optical ip=%u.%u.%u.%u rsafe=%u nstate=%u nfault=%u ckpt=%u early=%u ngen=%lu bgen=%lu rsrc=%lu epoch=%lu nrst=%lu ps=%u pv=%u",
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
		(unsigned)g_prev_boot_checkpoint);
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
/* cancels a capture the ISR provably is not feeding, the sliced       */
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
 * reclaim bound -- blocks unchanged this long after the token means the ISR
 * is provably not running (the one case the epoch may reclaim). */
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
	 * The remaining states cannot move under this function -- the ISR acts
	 * only in armed/capturing/cancelling, and this runs on the epoch side. */
	const uint32_t state = g_measure_capture.state;
	if (state != AIUS_MEASURE_IDLE && state != AIUS_MEASURE_DONE &&
	    state != AIUS_MEASURE_FAILED)
		return false;
	/* Validate the analysis parameters before the machine moves, so a bad f0
	 * or probe list leaves a retired session fully intact. */
	if (aius_measure_analysis_init(&g_measure_analysis_l,
	                               (double)AIUS_PIPELINE_PCM_SAMPLE_RATE_HZ,
	                               f0_l_hz, AIUS_MEASURE_WINDOW_FRAMES,
	                               probes_hz, probe_count) != AIUS_OK)
		return false;
	if (aius_measure_analysis_init(&g_measure_analysis_r,
	                               (double)AIUS_PIPELINE_PCM_SAMPLE_RATE_HZ,
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
		 * ISR is not feeding -- with no replay running enqueue() never
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

FLASHMEM void AiusSpdifOutputClock::set_master(bool master)
{
	update_responsibility = master;
}

/* Enforces one invariant: THE LIVE INPUT ALWAYS OWNS THE AUDIO PATH. Replay is
 * strictly the fallback for a board with no carrier, so a carrier and a replay
 * never contend -- the carrier wins, every time, and the replay ends.
 *
 * Called from checkpoint 7 AFTER the refill pass, because
 * aius_replay_service_deliver() -- reached through replay_job_route() inside
 * that pass -- is what promotes SWITCHING_TO_REPLAY to SOURCE_REPLAY once the
 * ring reaches its watermark; running after it moves the clock in the same
 * epoch the source moves, not an epoch later.
 *
 * A reconciler here rather than a pair of pokes in app_replay_start() /
 * app_replay_stop(): start only publishes PREBUFFERING, playback begins later
 * and elsewhere, and the five ways a replay ends -- an operator stop, EOF, a
 * decoder error, a cancelled job replaced by a new selection, and now a carrier
 * arriving -- all reach the same `source` field. One derived predicate covers
 * every one of them; five call sites would not, and each would be a chance to
 * leave the clock behind. It also keeps pllLocked() out of the audio ISR: this
 * runs once per epoch in thread context, and the ITCM guard's audio-block set
 * stays exactly three sections. */
static FLASHMEM void replay_clock_service(void)
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
	/* Exactly the predicate AiusAudioProcessor::update() branches on, ANDed
	 * with the absence of a carrier, so the clock owner and the audio source
	 * cannot disagree by more than the epoch that moves them.
	 *
	 * The pllLocked() term is redundant after the arm above has run -- a
	 * carrier leaves source at OPTICAL -- and it is kept because it is what
	 * makes this one function a complete statement of the invariant: the
	 * transmitter takes the clock only in the absence of a carrier, whatever
	 * any other path may have done to `source`. A carrier arriving mid-replay
	 * therefore lands the clock back on the input in the same epoch that stops
	 * the replay, not one later; a carrier going away again does nothing until
	 * a new replay is started, which is the fallback direction and belongs to
	 * an operator, not to a PLL edge. */
	const bool output_drives =
		g_replay_service.source == AIUS_REPLAY_SOURCE_REPLAY &&
		!AudioInputSPDIF3::pllLocked();
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

/* Checkpoint 7, after replay_clock_service(): both inputs are this epoch's
 * settled values -- the reconciler's source transition and any carrier
 * displacement have already landed. In recovery-safe the scheduler never
 * reaches this checkpoint, so the gate stays frozen dark from setup(), which
 * also mutes live passthrough there -- intended, and recorded in the plan. */
static FLASHMEM void tx_gate_service(void)
{
	const bool carrier = AudioInputSPDIF3::pllLocked();
	const AiusReplaySource source = g_replay_service.source;
	const bool open = aius_tx_gate_permitted(carrier, source);
	if (open != g_tx_gate_open)
		tx_gate_actuate(open);
	g_tx_gate_open = open;
	g_tx_gate_carrier = carrier;
	g_tx_gate_replay = aius_tx_gate_permitted(false, source);
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
	/* Last, so it sees this epoch's source transition rather than the previous
	 * one's. Memory only -- two byte stores under one critical section -- so it
	 * spends none of the epoch's single SD primitive. */
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

	capture_retained_boot_state();
	(void)aius_boot_teensy_watchdog_arm(&g_boot_primitives);

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
/*    in the audio ISR, and nothing new is exposed to it: the one      */
/*    value the ISR owns (g_overruns) is read through the processor's  */
/*    existing critical-section accessor, exactly as put_status() has  */
/*    always read it.                                                  */
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
/*    between two passes: g_overruns, written by the audio ISR once    */
/*    per refused block and so unbounded against an epoch that stalls, */
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
 * /api/counters clears g_overruns, and a clear must cost one missed edge rather
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
 * The epoch runs checkpoints 0..8 in order, records EPOCH_COMPLETE and feeds
 * the watchdog once; an incomplete epoch returns false without that feed, which
 * is what makes a wedged service a bounded reset rather than a hang.
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
	const bool completed = aius_runtime_teensy_epoch(&g_runtime, now_ms);

	/* After the epoch, and unconditionally: an epoch that aborted before
	 * EPOCH_COMPLETE is exactly the case whose counters explain the abort. */
	service_event_edges();
	if (completed && !g_boot_marked_healthy &&
	    (int32_t)(now_ms - g_boot_healthy_at_ms) >= 0) {
		/* Persisted by the next checkpoint, which commits the ledger. */
		aius_boot_mark_healthy(&g_boot_ledger);
		g_boot_marked_healthy = true;
	}
	if (g_reboot_requested)
		service_reboot_request(now_ms);
}
