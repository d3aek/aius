#ifndef AIUS_SD_H
#define AIUS_SD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AIUS_SD_PATH_STORAGE 128u

typedef enum {
	AIUS_SD_JOB_PROBE,
	AIUS_SD_JOB_CONFIG_LOAD,
	AIUS_SD_JOB_USAGE_REFRESH,
	AIUS_SD_JOB_UPLOAD,
	AIUS_SD_JOB_DOWNLOAD,
	AIUS_SD_JOB_DELETE,
	AIUS_SD_JOB_REPLAY_REFILL,
	/* Reads one file from initial_offset to its end (total 0, no
	 * content_type), one capacity-sized chunk per READ/OUTPUT primitive
	 * pair. initial_offset is 0 or a multiple of
	 * AIUS_SD_CACHE_FILL_ALIGN (a cancelled fill resuming at its next
	 * chunk, seeking as a ranged DOWNLOAD does; past the end is a RANGE
	 * fault). Each OUTPUT action is the sink: the caller consumes
	 * buffer/length/offset (absolute) and acknowledges with OK, or with a
	 * FAULT event to fail the job. Read only: a cancel closes the file and
	 * issues no write. */
	AIUS_SD_JOB_CACHE_FILL
} AiusSdJobKind;

/* A CACHE_FILL's initial_offset granularity: the web cache's chunk. */
#define AIUS_SD_CACHE_FILL_ALIGN 512u

typedef enum {
	AIUS_SD_INSTALL_REPLACE,
	AIUS_SD_INSTALL_APPEND_PART,
	AIUS_SD_INSTALL_FINALIZE_PART,
	/* A settings save (Phase 4h H4): two full copies, the target and its
	 * fixed backup_path, each overwritten in place with no temp file, no
	 * RENAME and no REMOVE. A pass of an N-chunk file (N > 1) is
	 * OPEN_WRITE_BEGIN, a WRITE of a zero chunk 0 the job fills itself, one
	 * NEED_INPUT + WRITE per chunk 1 to N - 1, TRUNCATE to the length, SEEK
	 * 0, NEED_INPUT + WRITE of the real chunk 0, CLOSE (N + 5 primitives);
	 * a one-chunk file is OPEN_WRITE_BEGIN, WRITE, TRUNCATE, CLOSE (4). The
	 * passes run backup then target, or target then backup with
	 * target_first, or the target alone with skip_backup; then one
	 * CACHE_DROP and the target's read-back (OPEN_READ, FILE_SIZE, a READ
	 * per chunk, CLOSE), whose CRC-32 must equal expected_crc32. UPLOAD
	 * only; backup_path required, temp_path NULL, total > 0, no publish.
	 * A chunk is the job's transfer capacity (512 B in every caller). */
	AIUS_SD_INSTALL_COPY_BEFORE_WRITE,
	/* A www PUT into the PSRAM store (Phase 4h S8, NS8). Expressible in a
	 * request so the HTTP transaction's SD_START can carry it, and routed
	 * to the store before the engine; aius_sd_job_start rejects it with
	 * AIUS_SD_FAULT_INVALID_REQUEST and issues no primitive, so it can
	 * never reach a card even if that routing fails. */
	AIUS_SD_INSTALL_STORE
} AiusSdInstallMode;

typedef enum {
	AIUS_SD_IDLE,
	AIUS_SD_PREPARE,
	AIUS_SD_MKDIR,
	AIUS_SD_OPEN,
	AIUS_SD_NEED_INPUT,
	AIUS_SD_READ,
	AIUS_SD_WRITE,
	AIUS_SD_CHECK_TARGET,
	AIUS_SD_BACKUP,
	AIUS_SD_INSTALL,
	AIUS_SD_ROLLBACK,
	AIUS_SD_CLEANUP,
	AIUS_SD_PUBLISH,
	AIUS_SD_CANCEL_CLOSE,
	AIUS_SD_CANCEL_CLEANUP,
	AIUS_SD_DONE,
	AIUS_SD_FAILED
} AiusSdState;

typedef enum {
	AIUS_SD_PHASE_NONE,
	AIUS_SD_PHASE_BEGIN,
	AIUS_SD_PHASE_PROBE_MEDIA,
	AIUS_SD_PHASE_CONFIG_OPEN,
	AIUS_SD_PHASE_CONFIG_SIZE,
	AIUS_SD_PHASE_CONFIG_READ,
	AIUS_SD_PHASE_CONFIG_CLOSE,
	AIUS_SD_PHASE_CONFIG_PUBLISH,
	AIUS_SD_PHASE_USAGE_TOTAL,
	AIUS_SD_PHASE_USAGE_USED,
	AIUS_SD_PHASE_PATH_EXISTS,
	AIUS_SD_PHASE_MKDIR,
	AIUS_SD_PHASE_PART_SIZE_OPEN,
	AIUS_SD_PHASE_PART_SIZE,
	AIUS_SD_PHASE_PART_SIZE_CLOSE,
	AIUS_SD_PHASE_UPLOAD_OPEN,
	AIUS_SD_PHASE_UPLOAD_TRUNCATE,
	AIUS_SD_PHASE_UPLOAD_INPUT,
	AIUS_SD_PHASE_UPLOAD_WRITE,
	AIUS_SD_PHASE_UPLOAD_CLOSE,
	AIUS_SD_PHASE_FINAL_SIZE_OPEN,
	AIUS_SD_PHASE_FINAL_SIZE,
	AIUS_SD_PHASE_FINAL_SIZE_CLOSE,
	AIUS_SD_PHASE_DOWNLOAD_OPEN,
	AIUS_SD_PHASE_DOWNLOAD_SIZE,
	AIUS_SD_PHASE_DOWNLOAD_SEEK,
	AIUS_SD_PHASE_DOWNLOAD_READ,
	AIUS_SD_PHASE_DOWNLOAD_OUTPUT,
	AIUS_SD_PHASE_DOWNLOAD_CLOSE,
	AIUS_SD_PHASE_DELETE_REMOVE,
	AIUS_SD_PHASE_INSTALL_REMOVE_OLD_BACKUP,
	AIUS_SD_PHASE_INSTALL_BACKUP,
	AIUS_SD_PHASE_INSTALL_RENAME,
	AIUS_SD_PHASE_INSTALL_REMOVE_BACKUP,
	AIUS_SD_PHASE_INSTALL_PUBLISH_OPEN,
	AIUS_SD_PHASE_INSTALL_PUBLISH_SIZE,
	AIUS_SD_PHASE_INSTALL_PUBLISH_READ,
	AIUS_SD_PHASE_INSTALL_PUBLISH_CLOSE,
	AIUS_SD_PHASE_INSTALL_PUBLISH,
	AIUS_SD_PHASE_ROLLBACK,
	AIUS_SD_PHASE_CLEANUP_CLOSE,
	AIUS_SD_PHASE_CLEANUP_TEMP,
	AIUS_SD_PHASE_TERMINAL,
	/* Copy-before-write (Phase 4h), appended after TERMINAL so every
	 * earlier value, published as fault_phase, keeps its number. */
	AIUS_SD_PHASE_CBW_OPEN,
	AIUS_SD_PHASE_CBW_ZERO_WRITE,
	AIUS_SD_PHASE_CBW_INPUT,
	AIUS_SD_PHASE_CBW_WRITE,
	AIUS_SD_PHASE_CBW_TRUNCATE,
	AIUS_SD_PHASE_CBW_SEEK,
	AIUS_SD_PHASE_CBW_CLOSE,
	AIUS_SD_PHASE_CACHE_DROP,
	AIUS_SD_PHASE_VERIFY_OPEN,
	AIUS_SD_PHASE_VERIFY_SIZE,
	AIUS_SD_PHASE_VERIFY_READ,
	AIUS_SD_PHASE_VERIFY_CLOSE
} AiusSdPhase;

typedef enum {
	AIUS_SD_EXISTS_NONE,
	AIUS_SD_EXISTS_MKDIR,
	AIUS_SD_EXISTS_DELETE,
	AIUS_SD_EXISTS_INSTALL_TARGET,
	AIUS_SD_EXISTS_REMOVE_OLD_BACKUP,
	AIUS_SD_EXISTS_REMOVE_COMMITTED_BACKUP,
	AIUS_SD_EXISTS_CLEANUP_TEMP
} AiusSdExistencePurpose;

typedef enum {
	AIUS_SD_EVENT_NONE,
	AIUS_SD_EVENT_OK,
	AIUS_SD_EVENT_BYTES,
	AIUS_SD_EVENT_BOOL,
	AIUS_SD_EVENT_SIZE,
	AIUS_SD_EVENT_FAULT
} AiusSdEventKind;

/* The volume type a PROBE found, as a plain enumerated value rather than a
 * driver type: the portable engine stays free of vendor headers, and the driver
 * is the only layer that knows what its own FAT-type numbering means.
 *
 * UNKNOWN is the honest answer for three distinct cases the wire deliberately
 * does not distinguish -- no card, a card that would not mount, and a mounted
 * volume whose type the driver does not recognise (exFAT is one: the legacy
 * status surface this restores recognised 12, 16 and 32 only, and widening the
 * vocabulary is a wire change, not a bug fix). Zero is UNKNOWN so a
 * zero-initialized event or result reports it without anyone setting it. */
typedef enum {
	AIUS_SD_FILESYSTEM_UNKNOWN = 0,
	AIUS_SD_FILESYSTEM_FAT12 = 1,
	AIUS_SD_FILESYSTEM_FAT16 = 2,
	AIUS_SD_FILESYSTEM_FAT32 = 3
} AiusSdFilesystem;

typedef struct {
	AiusSdEventKind kind;
	uint64_t action_id;
	size_t bytes;
	uint64_t size;
	bool value;
	uint32_t fault;
	/* Only an AIUS_SD_ACTION_MEDIA_PRESENT event may carry a value other
	 * than UNKNOWN; the engine faults the job with AIUS_SD_FAULT_PROTOCOL
	 * for any other event that does, exactly as it does for a stray `bytes`
	 * or `size`. The driver reports it from the volume the same primitive
	 * already mounted, so it costs no extra SD primitive. */
	AiusSdFilesystem filesystem;
} AiusSdEvent;

typedef enum {
	AIUS_SD_ACTION_NONE,
	AIUS_SD_ACTION_BEGIN,
	AIUS_SD_ACTION_MEDIA_PRESENT,
	AIUS_SD_ACTION_MKDIR,
	AIUS_SD_ACTION_OPEN_READ,
	AIUS_SD_ACTION_OPEN_WRITE_BEGIN,
	AIUS_SD_ACTION_OPEN_APPEND,
	AIUS_SD_ACTION_TRUNCATE,
	AIUS_SD_ACTION_SEEK,
	AIUS_SD_ACTION_NEED_INPUT,
	AIUS_SD_ACTION_READ,
	AIUS_SD_ACTION_WRITE,
	AIUS_SD_ACTION_OUTPUT,
	AIUS_SD_ACTION_FILE_SIZE,
	AIUS_SD_ACTION_EXISTS,
	AIUS_SD_ACTION_RENAME,
	AIUS_SD_ACTION_REMOVE,
	AIUS_SD_ACTION_CLOSE,
	AIUS_SD_ACTION_TOTAL_SIZE,
	AIUS_SD_ACTION_USED_SIZE,
	AIUS_SD_ACTION_PUBLISH_CONFIG,
	AIUS_SD_ACTION_COMPLETE,
	AIUS_SD_ACTION_FAIL,
	/* Syncs the mounted volume's dirty cached sector, then drops the
	 * cache, so the read-back that follows reads the card, not SdFat's
	 * cache (Phase 4h H1, CR4-B2). No payload; answered with OK. Appended
	 * so every earlier action keeps its number. */
	AIUS_SD_ACTION_CACHE_DROP
} AiusSdActionKind;

typedef struct {
	AiusSdActionKind kind;
	uint64_t action_id;
	const char *path;
	const char *path_to;
	uint8_t *buffer;
	size_t length;
	uint64_t offset;
	uint32_t fault;
} AiusSdAction;

typedef struct {
	AiusSdJobKind kind;
	AiusSdInstallMode install_mode;
	const char *target_path;
	const char *temp_path;
	const char *backup_path;
	const char *content_type;
	uint64_t total;
	uint64_t initial_offset;
	/* When true, the job re-opens the installed/loaded file and hands its
	 * complete bytes to the caller via one AIUS_SD_ACTION_PUBLISH_CONFIG
	 * action (mechanism is generic, not feature-config-specific despite the
	 * action's name). publish_capacity is the caller-supplied maximum byte
	 * count for that publish (e.g. AIUS_CONFIG_MAX_JSON_BYTES for the
	 * feature-flag config, or a different caller-owned limit for another
	 * config-like file); required and validated against the job's publication
	 * capacity when publish is true, must be zero otherwise. */
	bool publish;
	size_t publish_capacity;
	bool require_existing;
	/* AIUS_SD_INSTALL_COPY_BEFORE_WRITE only; ignored by every other mode.
	 * target_first: write the target before the backup (the path's safe
	 * copy is the backup, so the backup is written last); false writes the
	 * backup first. skip_backup: write the target alone (a restore-created
	 * entry whose backup already holds these bytes); backup_path is still
	 * required and target_first is then immaterial. expected_crc32: the
	 * zlib CRC-32 of the accepted bytes, which the read-back must match. */
	bool target_first;
	bool skip_backup;
	uint32_t expected_crc32;
} AiusSdJobRequest;

typedef enum {
	AIUS_SD_RESULT_NONE,
	AIUS_SD_RESULT_PROBE,
	AIUS_SD_RESULT_USAGE,
	AIUS_SD_RESULT_BYTES,
	AIUS_SD_RESULT_COMPLETE,
	AIUS_SD_RESULT_FAULT
} AiusSdResultKind;

typedef enum {
	AIUS_SD_FAULT_NONE = 0,
	AIUS_SD_FAULT_INVALID_REQUEST = 1,
	AIUS_SD_FAULT_PROTOCOL = 2,
	AIUS_SD_FAULT_CAPACITY = 3,
	AIUS_SD_FAULT_PATH = 4,
	AIUS_SD_FAULT_OVERFLOW = 5,
	AIUS_SD_FAULT_TIMEOUT = 6,
	AIUS_SD_FAULT_SHORT_IO = 7,
	AIUS_SD_FAULT_MEDIA = 8,
	AIUS_SD_FAULT_OPEN = 9,
	AIUS_SD_FAULT_SEEK = 10,
	AIUS_SD_FAULT_IO = 11,
	AIUS_SD_FAULT_INSTALL = 12,
	AIUS_SD_FAULT_ROLLBACK = 13,
	AIUS_SD_FAULT_PUBLISH = 14,
	AIUS_SD_FAULT_CANCELLED = 15,
	AIUS_SD_FAULT_CLEANUP = 16,
	AIUS_SD_FAULT_RANGE = 17,
	AIUS_SD_FAULT_NOT_FOUND = 18,
	/* A copy-before-write read-back whose size or CRC-32 differs from the
	 * bytes accepted. */
	AIUS_SD_FAULT_VERIFY = 19
} AiusSdFault;

typedef struct {
	AiusSdResultKind kind;
	bool media_present;
	/* AIUS_SD_RESULT_PROBE only, and meaningful only with media_present: the
	 * volume type the probe's own media check read off the mounted card. A
	 * probe that found no media, or any other job kind, leaves it UNKNOWN. */
	AiusSdFilesystem filesystem;
	/* Copy-before-write only, placed in the padding before total_bytes so
	 * the result keeps its pre-4h size on the ARM ABI. passes_closed: how
	 * many passes reached a successful CLOSE (0 to 2), so the caller knows
	 * which file holds the pinned version complete: the first pass's file
	 * after 1, both after 2. readback_mismatch: both passes closed and the
	 * read-back's size or CRC-32 disagreed (AIUS_SD_FAULT_VERIFY). */
	uint8_t passes_closed;
	bool readback_mismatch;
	uint64_t total_bytes;
	uint64_t used_bytes;
	uint64_t transferred_bytes;
	AiusSdFault fault;
	uint32_t driver_fault;
} AiusSdResult;

typedef struct {
	AiusSdState state;
	AiusSdPhase phase;
	AiusSdJobKind kind;
	AiusSdInstallMode install_mode;
	AiusSdExistencePurpose existence_purpose;
	char target_path[AIUS_SD_PATH_STORAGE];
	char temp_path[AIUS_SD_PATH_STORAGE];
	char backup_path[AIUS_SD_PATH_STORAGE];
	char directory_path[AIUS_SD_PATH_STORAGE];
	const char *content_type;
	uint8_t *buffer;
	size_t capacity;
	/* Optional accumulation buffer for a publishing job, and the only field
	 * that may exceed `capacity`. Null (the default) means the publication
	 * accumulates into `buffer` bounded by `capacity`, which is what every
	 * caller that never calls aius_sd_job_set_publish_buffer() gets. The
	 * transfer slice is unaffected either way: a publishing read still asks
	 * the driver for at most `capacity` bytes per action, so the per-epoch SD
	 * primitive stays bounded however large the publication is. */
	uint8_t *publish_buffer;
	size_t publish_buffer_capacity;
	size_t buffer_offset;
	uint64_t initial_offset;
	uint64_t end_offset;
	uint64_t captured_size;
	uint64_t transferred;
	uint64_t total;
	size_t config_bytes;
	size_t requested;
	size_t pending;
	uint32_t deadline_ms;
	uint64_t next_action_id;
	uint64_t outstanding_action_id;
	AiusSdActionKind outstanding_action;
	uint8_t mkdir_path_index;
	uint8_t mkdir_offset;
	AiusSdFault last_fault;
	uint32_t last_driver_fault;
	/* The phase the authoritative fault landed in, latched beside last_fault
	 * by the same replace rule and cleared with it. A terminal FAILED is
	 * otherwise unattributable: `phase` has moved on to the cleanup arm and
	 * then to TERMINAL by the time anyone reads the result, so an upload that
	 * failed its write and one that failed its install rename are
	 * indistinguishable. NONE while no fault has been recorded. */
	AiusSdPhase fault_phase;
	/* Copy-before-write state (Phase 4h), in the padding before `result`
	 * and at the tail, so AiusSdJob keeps its pre-4h 696 B on the ARM ABI:
	 * OCRAM holds two jobs and its floor is fixed (static assert in
	 * aius_sd.c). copy_flags: target first, skip backup, the real chunk 0
	 * being written, a read-back mismatch. copy_pass: the pass index, which
	 * is also how many passes' CLOSE succeeded. The pass cursor (the next
	 * source offset) and the read-back cursor reuse `transferred`. */
	uint8_t copy_flags;
	uint8_t copy_pass;
	uint32_t expected_crc32;
	AiusSdResult result;
	bool target_existed;
	bool backup_moved;
	bool install_failed;
	bool installed;
	bool file_open;
	bool publish;
	size_t publish_capacity;
	bool require_existing;
	bool close_emitted;
	bool cancel_requested;
	bool abort_requested;
	bool action_ids_exhausted;
	bool terminal_emitted;
	/* The running read-back CRC-32 (copy-before-write), in the tail
	 * padding. */
	uint32_t verify_crc32;
} AiusSdJob;

void aius_sd_job_init(AiusSdJob *job, uint8_t *buffer, size_t capacity);

/* Points a publishing job's accumulation at a caller-owned buffer that may be
 * larger than the transfer slice, so a file bigger than `capacity` can be
 * published in one AIUS_SD_ACTION_PUBLISH_CONFIG while each read action stays
 * bounded by `capacity`. A null `buffer` or a zero `capacity` restores the
 * default (accumulate into the job's own buffer). Construction-time wiring:
 * aius_sd_job_init() clears it, so call this after init, and a call while a job
 * is in flight is ignored rather than swapping the buffer under it. */
void aius_sd_job_set_publish_buffer(AiusSdJob *job,
	                           uint8_t *buffer,
	                           size_t capacity);

void aius_sd_job_start(AiusSdJob *job,
	               const AiusSdJobRequest *request,
	               uint32_t now_ms,
	               uint32_t deadline_ms);

void aius_sd_job_cancel(AiusSdJob *job);

bool aius_sd_job_supply(AiusSdJob *job,
	                const uint8_t *data,
	                size_t length);

void aius_sd_job_step(AiusSdJob *job,
	              uint32_t now_ms,
	              AiusSdEvent event,
	              AiusSdAction *action);

const AiusSdResult *aius_sd_job_result(const AiusSdJob *job);

#ifdef __cplusplus
}
#endif

#endif
