#ifndef AIUS_HTTP_TRANSACTION_H
#define AIUS_HTTP_TRANSACTION_H

#include "aius_http.h"
#include "aius_io.h"
#include "aius_sd.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AIUS_HTTP_RX_QUOTA 256u
#define AIUS_HTTP_TX_QUOTA 512u
#define AIUS_HTTP_SD_CAPACITY_MAX 512u
#define AIUS_HTTP_SD_OUTPUT_CANCEL_FAULT UINT32_C(0x48545450)

typedef enum {
	AIUS_HTTP_TX_IDLE,
	AIUS_HTTP_TX_RECEIVE,
	AIUS_HTTP_TX_DISPATCH,
	AIUS_HTTP_TX_SD_START,
	AIUS_HTTP_TX_SD_TRANSFER,
	AIUS_HTTP_TX_SEND,
	AIUS_HTTP_TX_CLOSE
} AiusHttpTransactionState;

typedef enum {
	AIUS_HTTP_SD_PHASE_NONE,
	AIUS_HTTP_SD_PHASE_WAIT_START,
	AIUS_HTTP_SD_PHASE_UPLOAD_WAIT_INPUT,
	AIUS_HTTP_SD_PHASE_UPLOAD_SUPPLY,
	AIUS_HTTP_SD_PHASE_DOWNLOAD_ADVANCE,
	AIUS_HTTP_SD_PHASE_DOWNLOAD_WRITE,
	AIUS_HTTP_SD_PHASE_DOWNLOAD_ACK,
	AIUS_HTTP_SD_PHASE_WAIT_TERMINAL,
	AIUS_HTTP_SD_PHASE_CANCEL
} AiusHttpSdPhase;

typedef enum {
	AIUS_HTTP_APP_IMMEDIATE,
	AIUS_HTTP_APP_SD_JOB
} AiusHttpApplicationResponseKind;

typedef struct {
	AiusHttpApplicationResponseKind kind;
	unsigned status;
	const char *content_type;
	const uint8_t *body;
	size_t body_length;
	AiusSdJobRequest sd_job;
} AiusHttpApplicationResponse;

typedef enum {
	AIUS_HTTP_SD_EVENT_NONE,
	AIUS_HTTP_SD_STARTED,
	AIUS_HTTP_SD_PROGRESS,
	AIUS_HTTP_SD_NEED_UPLOAD,
	AIUS_HTTP_SD_DOWNLOAD_DATA,
	AIUS_HTTP_SD_COMPLETE,
	AIUS_HTTP_SD_FAILED,
	AIUS_HTTP_SD_CANCELLED
} AiusHttpSdEventKind;

typedef enum {
	AIUS_HTTP_ACTION_NONE,
	AIUS_HTTP_ACTION_ACCEPT,
	AIUS_HTTP_ACTION_READ,
	AIUS_HTTP_ACTION_DISPATCH,
	AIUS_HTTP_ACTION_WRITE,
	AIUS_HTTP_ACTION_SD_START,
	AIUS_HTTP_ACTION_SD_STEP,
	AIUS_HTTP_ACTION_SD_CANCEL,
	AIUS_HTTP_ACTION_CLOSE
} AiusHttpActionKind;

typedef enum {
	AIUS_HTTP_EVENT_NONE,
	AIUS_HTTP_EVENT_IO_RESULT,
	AIUS_HTTP_EVENT_APPLICATION_RESPONSE,
	AIUS_HTTP_EVENT_SD_RESULT,
	AIUS_HTTP_EVENT_PEER_CLOSED,
	AIUS_HTTP_EVENT_PEER_FAULT
} AiusHttpEventKind;

typedef struct {
	AiusHttpEventKind kind;
	uint64_t action_id;
	AiusHttpActionKind completed_action;
	AiusIoEvent io;
	const AiusHttpApplicationResponse *application_response;
	AiusHttpSdEventKind sd_kind;
	const uint8_t *sd_data;
	size_t sd_capacity;
	size_t sd_length;
	uint64_t sd_size;
	AiusSdFault sd_fault;
	uint32_t sd_driver_fault;
	uint32_t peer_fault;
} AiusHttpEvent;

typedef enum {
	AIUS_HTTP_SD_OP_NONE,
	AIUS_HTTP_SD_OP_ADVANCE,
	AIUS_HTTP_SD_OP_SUPPLY_UPLOAD,
	AIUS_HTTP_SD_OP_ACK_DOWNLOAD,
	AIUS_HTTP_SD_OP_FAIL_DOWNLOAD
} AiusHttpSdOperation;

typedef struct {
	AiusHttpActionKind kind;
	uint64_t action_id;
	uint8_t *read_buffer;
	const uint8_t *data;
	size_t length;
	const AiusHttpRequest *request;
	const AiusSdJobRequest *sd_request;
	AiusHttpSdOperation sd_operation;
	uint32_t fault;
} AiusHttpAction;

typedef enum {
	AIUS_HTTP_FAULT_NONE,
	AIUS_HTTP_FAULT_INVALID_INIT,
	AIUS_HTTP_FAULT_PROTOCOL,
	AIUS_HTTP_FAULT_REQUEST_TIMEOUT,
	AIUS_HTTP_FAULT_RESPONSE_TIMEOUT,
	AIUS_HTTP_FAULT_UPLOAD_TIMEOUT,
	AIUS_HTTP_FAULT_DOWNLOAD_TIMEOUT,
	AIUS_HTTP_FAULT_SD_TIMEOUT,
	AIUS_HTTP_FAULT_REQUEST_CAPACITY,
	AIUS_HTTP_FAULT_RESPONSE_CAPACITY,
	AIUS_HTTP_FAULT_ACTION_ID_EXHAUSTED,
	AIUS_HTTP_FAULT_TRANSPORT,
	AIUS_HTTP_FAULT_SD
} AiusHttpFault;

typedef struct {
	AiusHttpTransactionState state;
	AiusHttpSdPhase sd_phase;
	uint8_t *request;
	size_t request_capacity;
	size_t request_length;
	size_t header_length;
	uint64_t body_expected;
	size_t upload_prefix_offset;
	size_t upload_prefix_length;
	size_t upload_prefix_supplied;
	uint64_t upload_supplied;
	uint8_t *response;
	size_t response_capacity;
	size_t response_length;
	size_t response_offset;
	const uint8_t *download_data;
	size_t sd_capacity;
	uint64_t download_size;
	uint64_t download_sent;
	size_t download_length;
	size_t download_offset;
	uint32_t deadline_ms;
	uint64_t next_action_id;
	uint64_t outstanding_action_id;
	AiusHttpActionKind outstanding_action;
	size_t outstanding_length;
	AiusHttpRequest parsed_request;
	AiusSdJobRequest sd_request;
	char sd_target_path[AIUS_SD_PATH_STORAGE];
	char sd_temp_path[AIUS_SD_PATH_STORAGE];
	char sd_backup_path[AIUS_SD_PATH_STORAGE];
	char content_type[AIUS_HTTP_CONTENT_TYPE_STORAGE];
	char sd_content_type[AIUS_HTTP_CONTENT_TYPE_STORAGE];
	AiusHttpFault last_fault;
	AiusSdFault last_sd_fault;
	uint32_t last_transport_fault;
	uint32_t last_driver_fault;
	unsigned last_status;
	bool valid;
	bool client_active;
	bool transport_terminal;
	bool cleanup_requested;
	bool deadline_active;
	bool dispatch_emitted;
	bool close_emitted;
	bool response_started;
	bool response_is_download;
	bool sd_started;
	bool sd_cancel_emitted;
	bool sd_terminal;
	bool action_ids_exhausted;
} AiusHttpTransaction;

void aius_http_transaction_init(AiusHttpTransaction *transaction,
	                            uint8_t *request, size_t request_capacity,
	                            uint8_t *response, size_t response_capacity,
	                            uint32_t now_ms);

void aius_http_transaction_step(AiusHttpTransaction *transaction,
	                            uint32_t now_ms, AiusHttpEvent event,
	                            AiusHttpAction *action);

#ifdef __cplusplus
}
#endif

#endif
