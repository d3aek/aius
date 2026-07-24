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
	AIUS_SD_JOB_REPLAY_REFILL
} AiusSdJobKind;

typedef enum {
	AIUS_SD_INSTALL_REPLACE,
	AIUS_SD_INSTALL_APPEND_PART,
	AIUS_SD_INSTALL_FINALIZE_PART
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
	AIUS_SD_PHASE_MKDIR,
	AIUS_SD_PHASE_PART_SIZE_OPEN,
	AIUS_SD_PHASE_PART_SIZE,
	AIUS_SD_PHASE_PART_SIZE_CLOSE,
	AIUS_SD_PHASE_UPLOAD_OPEN,
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
	AIUS_SD_PHASE_INSTALL_EXISTS,
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
	AIUS_SD_PHASE_TERMINAL
} AiusSdPhase;

typedef enum {
	AIUS_SD_EVENT_NONE,
	AIUS_SD_EVENT_OK,
	AIUS_SD_EVENT_BYTES,
	AIUS_SD_EVENT_BOOL,
	AIUS_SD_EVENT_SIZE,
	AIUS_SD_EVENT_FAULT
} AiusSdEventKind;

typedef struct {
	AiusSdEventKind kind;
	uint64_t action_id;
	size_t bytes;
	uint64_t size;
	bool value;
	uint32_t fault;
} AiusSdEvent;

typedef enum {
	AIUS_SD_ACTION_NONE,
	AIUS_SD_ACTION_BEGIN,
	AIUS_SD_ACTION_MEDIA_PRESENT,
	AIUS_SD_ACTION_MKDIR,
	AIUS_SD_ACTION_OPEN_READ,
	AIUS_SD_ACTION_OPEN_WRITE_TRUNCATE,
	AIUS_SD_ACTION_OPEN_APPEND,
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
	AIUS_SD_ACTION_FAIL
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
	bool publish_config;
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
	AIUS_SD_FAULT_RANGE = 17
} AiusSdFault;

typedef struct {
	AiusSdResultKind kind;
	bool media_present;
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
	char target_path[AIUS_SD_PATH_STORAGE];
	char temp_path[AIUS_SD_PATH_STORAGE];
	char backup_path[AIUS_SD_PATH_STORAGE];
	char directory_path[AIUS_SD_PATH_STORAGE];
	const char *content_type;
	uint8_t *buffer;
	size_t capacity;
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
	AiusSdResult result;
	bool target_existed;
	bool backup_moved;
	bool install_failed;
	bool installed;
	bool file_open;
	bool publish_config;
	bool close_emitted;
	bool cancel_requested;
	bool abort_requested;
	bool action_ids_exhausted;
	bool terminal_emitted;
} AiusSdJob;

void aius_sd_job_init(AiusSdJob *job, uint8_t *buffer, size_t capacity);

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
