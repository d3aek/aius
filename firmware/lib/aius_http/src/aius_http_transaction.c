#include "aius_http_transaction.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

#define REQUEST_TIMEOUT_MS UINT32_C(150)
#define RESPONSE_TIMEOUT_MS UINT32_C(100)
#define SD_TIMEOUT_MS UINT32_C(2000)
#define DOWNLOAD_TIMEOUT_MS UINT32_C(10000)
#define CONFIG_PUBLICATION_MAX UINT64_C(256)

static const uint8_t bad_request_body[] =
	"{\"ok\":false,\"error\":\"bad_request\"}";
static const uint8_t method_body[] =
	"{\"ok\":false,\"error\":\"method_not_allowed\"}";
static const uint8_t route_body[] =
	"{\"ok\":false,\"error\":\"not_found\"}";
static const uint8_t request_timeout_body[] =
	"{\"ok\":false,\"error\":\"request_timeout\"}";
static const uint8_t request_large_body[] =
	"{\"ok\":false,\"error\":\"request_too_large\"}";
static const uint8_t protocol_body[] =
	"{\"ok\":false,\"error\":\"protocol\"}";
static const uint8_t response_large_body[] =
	"{\"ok\":false,\"error\":\"response_too_large\"}";
static const uint8_t sd_not_found_body[] =
	"{\"ok\":false,\"error\":\"sd_not_found\"}";
static const uint8_t sd_range_body[] =
	"{\"ok\":false,\"error\":\"sd_range\"}";
static const uint8_t sd_unavailable_body[] =
	"{\"ok\":false,\"error\":\"sd_unavailable\"}";
static const uint8_t sd_capacity_body[] =
	"{\"ok\":false,\"error\":\"sd_capacity\"}";
static const uint8_t sd_failure_body[] =
	"{\"ok\":false,\"error\":\"sd_failure\"}";

static void clear_action(AiusHttpAction *action)
{
	memset(action, 0, sizeof(*action));
	action->kind = AIUS_HTTP_ACTION_NONE;
	action->sd_operation = AIUS_HTTP_SD_OP_NONE;
}

static bool deadline_expired(uint32_t now_ms, uint32_t deadline_ms)
{
	return (int32_t)(now_ms - deadline_ms) >= 0;
}

static void arm_deadline(AiusHttpTransaction *transaction, uint32_t now_ms,
	                     uint32_t interval)
{
	transaction->deadline_ms = now_ms + interval;
	transaction->deadline_active = true;
}

static void clear_outstanding(AiusHttpTransaction *transaction)
{
	transaction->outstanding_action = AIUS_HTTP_ACTION_NONE;
	transaction->outstanding_action_id = 0u;
	transaction->outstanding_length = 0u;
}

static bool emit_action(AiusHttpTransaction *transaction,
	                    AiusHttpAction *action, AiusHttpActionKind kind)
{
	uint64_t id = transaction->next_action_id;
	if (id == 0u) {
		transaction->last_fault = AIUS_HTTP_FAULT_ACTION_ID_EXHAUSTED;
		transaction->action_ids_exhausted = true;
		transaction->valid = false;
		return false;
	}
	transaction->next_action_id = id + UINT64_C(1);
	transaction->outstanding_action = kind;
	transaction->outstanding_action_id = id;
	action->kind = kind;
	action->action_id = id;
	return true;
}

static bool supported_status(unsigned status)
{
	return aius_http_reason(status) != NULL;
}

static bool copy_string(char *output, size_t capacity, const char *input,
	                    bool required)
{
	size_t length = 0u;
	if (input == NULL)
		return !required;
	while (length < capacity && input[length] != '\0') {
		unsigned char value = (unsigned char)input[length];
		if (value < 0x20u || value > 0x7eu)
			return false;
		length++;
	}
	if (length == 0u || length >= capacity)
		return false;
	memcpy(output, input, length + 1u);
	return true;
}

static bool valid_path(const char *path)
{
	size_t length = 0u;
	size_t segment = 0u;
	if (path == NULL || path[0] != '/')
		return false;
	while (length < AIUS_SD_PATH_STORAGE && path[length] != '\0') {
		unsigned char value = (unsigned char)path[length];
		if (value < 0x20u || value > 0x7eu || value == '\\')
			return false;
		if (length != 0u && value == '/') {
			if (segment == 0u ||
			    (segment == 1u && path[length - 1u] == '.') ||
			    (segment == 2u && path[length - 1u] == '.' &&
			     path[length - 2u] == '.'))
				return false;
			segment = 0u;
		} else if (length != 0u) {
			segment++;
		}
		length++;
	}
	if (length <= 1u || length >= AIUS_SD_PATH_STORAGE ||
	    path[length - 1u] == '/' || segment == 0u ||
	    (segment == 1u && path[length - 1u] == '.') ||
	    (segment == 2u && path[length - 1u] == '.' &&
	     path[length - 2u] == '.'))
		return false;
	return true;
}

static bool paths_equal(const char *left, const char *right)
{
	return left != NULL && right != NULL && strcmp(left, right) == 0;
}

static bool sd_request_fields_valid(const AiusSdJobRequest *request)
{
	bool target = request->target_path != NULL;
	bool temp = request->temp_path != NULL;
	bool backup = request->backup_path != NULL;
	if ((unsigned)request->kind > (unsigned)AIUS_SD_JOB_REPLAY_REFILL ||
	    (unsigned)request->install_mode >
		    (unsigned)AIUS_SD_INSTALL_FINALIZE_PART)
		return false;
	if (request->kind != AIUS_SD_JOB_DELETE &&
	    request->require_existing)
		return false;
	if (request->total > (uint64_t)INT64_MAX ||
	    request->initial_offset > (uint64_t)INT64_MAX ||
	    request->total >
		    (uint64_t)INT64_MAX - request->initial_offset)
		return false;
	if ((target && !valid_path(request->target_path)) ||
	    (temp && !valid_path(request->temp_path)) ||
	    (backup && !valid_path(request->backup_path)) ||
	    paths_equal(request->target_path, request->temp_path) ||
	    paths_equal(request->target_path, request->backup_path) ||
	    paths_equal(request->temp_path, request->backup_path))
		return false;

	switch (request->kind) {
	case AIUS_SD_JOB_PROBE:
	case AIUS_SD_JOB_USAGE_REFRESH:
		return request->install_mode == AIUS_SD_INSTALL_REPLACE &&
		       !target && !temp && !backup && request->content_type == NULL &&
		       request->total == 0u && request->initial_offset == 0u &&
		       !request->publish_config;
	case AIUS_SD_JOB_CONFIG_LOAD:
		return request->install_mode == AIUS_SD_INSTALL_REPLACE &&
		       target && !temp && !backup && request->content_type != NULL &&
		       request->total == 0u && request->initial_offset == 0u &&
		       request->publish_config;
	case AIUS_SD_JOB_DELETE:
		return request->install_mode == AIUS_SD_INSTALL_REPLACE &&
		       target && !temp && !backup && request->content_type == NULL &&
		       request->total == 0u && request->initial_offset == 0u &&
		       !request->publish_config;
	case AIUS_SD_JOB_DOWNLOAD:
		return request->install_mode == AIUS_SD_INSTALL_REPLACE &&
		       target && !temp && !backup && request->content_type != NULL &&
		       !request->publish_config;
	case AIUS_SD_JOB_REPLAY_REFILL:
		return false;
	case AIUS_SD_JOB_UPLOAD:
		switch (request->install_mode) {
		case AIUS_SD_INSTALL_REPLACE:
			return target && temp && backup &&
			       request->initial_offset == 0u &&
			       (!request->publish_config ||
			        (request->total != 0u &&
			         request->total <= CONFIG_PUBLICATION_MAX &&
			         request->content_type != NULL)) &&
			       (request->publish_config ||
			        request->content_type == NULL);
		case AIUS_SD_INSTALL_APPEND_PART:
			return target && !temp && !backup &&
			       request->content_type == NULL &&
			       !request->publish_config;
		case AIUS_SD_INSTALL_FINALIZE_PART:
			return target && temp && backup &&
			       (!request->publish_config ||
			        (request->content_type != NULL &&
			         request->initial_offset + request->total != 0u &&
			         request->initial_offset + request->total <=
				         CONFIG_PUBLICATION_MAX)) &&
			       (request->publish_config ||
			        request->content_type == NULL);
		default:
			return false;
		}
	default:
		return false;
	}
}

static bool copy_sd_request(AiusHttpTransaction *transaction,
	                        const AiusSdJobRequest *source)
{
	if (!sd_request_fields_valid(source))
		return false;
	memset(&transaction->sd_request, 0, sizeof(transaction->sd_request));
	transaction->sd_request = *source;
	if (source->target_path != NULL) {
		if (!copy_string(transaction->sd_target_path,
		                 sizeof(transaction->sd_target_path),
		                 source->target_path, true))
			return false;
		transaction->sd_request.target_path = transaction->sd_target_path;
	}
	if (source->temp_path != NULL) {
		if (!copy_string(transaction->sd_temp_path,
		                 sizeof(transaction->sd_temp_path),
		                 source->temp_path, true))
			return false;
		transaction->sd_request.temp_path = transaction->sd_temp_path;
	}
	if (source->backup_path != NULL) {
		if (!copy_string(transaction->sd_backup_path,
		                 sizeof(transaction->sd_backup_path),
		                 source->backup_path, true))
			return false;
		transaction->sd_request.backup_path = transaction->sd_backup_path;
	}
	if (source->content_type != NULL) {
		if (!copy_string(transaction->sd_content_type,
		                 sizeof(transaction->sd_content_type),
		                 source->content_type, true))
			return false;
		transaction->sd_request.content_type =
			transaction->sd_content_type;
	}
	return true;
}

static bool zero_sd_request(const AiusSdJobRequest *request)
{
	return request->kind == AIUS_SD_JOB_PROBE &&
	       request->install_mode == AIUS_SD_INSTALL_REPLACE &&
	       request->target_path == NULL && request->temp_path == NULL &&
	       request->backup_path == NULL && request->content_type == NULL &&
	       request->total == 0u && request->initial_offset == 0u &&
	       !request->publish_config && !request->require_existing;
}

static bool event_common_zero(const AiusHttpEvent *event)
{
	return event->action_id == 0u &&
	       event->completed_action == AIUS_HTTP_ACTION_NONE &&
	       event->io.result == AIUS_IO_PROGRESS && event->io.bytes == 0u &&
	       event->io.fault == 0u && event->application_response == NULL &&
	       event->sd_kind == AIUS_HTTP_SD_EVENT_NONE &&
	       event->sd_data == NULL && event->sd_capacity == 0u &&
	       event->sd_length == 0u && event->sd_size == 0u &&
	       event->sd_fault == AIUS_SD_FAULT_NONE &&
	       event->sd_driver_fault == 0u && event->peer_fault == 0u;
}

static bool io_payload_valid(const AiusHttpTransaction *transaction,
	                         const AiusHttpEvent *event)
{
	AiusIoResult result = event->io.result;
	if (event->application_response != NULL ||
	    event->sd_kind != AIUS_HTTP_SD_EVENT_NONE ||
	    event->sd_data != NULL || event->sd_capacity != 0u ||
	    event->sd_length != 0u || event->sd_size != 0u ||
	    event->sd_fault != AIUS_SD_FAULT_NONE ||
	    event->sd_driver_fault != 0u || event->peer_fault != 0u ||
	    event->action_id == 0u ||
	    event->completed_action != transaction->outstanding_action ||
	    event->action_id != transaction->outstanding_action_id)
		return false;
	if (event->completed_action != AIUS_HTTP_ACTION_ACCEPT &&
	    event->completed_action != AIUS_HTTP_ACTION_READ &&
	    event->completed_action != AIUS_HTTP_ACTION_WRITE &&
	    event->completed_action != AIUS_HTTP_ACTION_CLOSE)
		return false;
	if ((unsigned)result > (unsigned)AIUS_IO_FAULT)
		return false;
	if (result == AIUS_IO_PROGRESS) {
		if (event->io.fault != 0u)
			return false;
		if (event->completed_action == AIUS_HTTP_ACTION_READ ||
		    event->completed_action == AIUS_HTTP_ACTION_WRITE)
			return event->io.bytes != 0u &&
			       event->io.bytes <= transaction->outstanding_length;
		return event->io.bytes == 0u;
	}
	if (event->io.bytes != 0u)
		return false;
	if (result == AIUS_IO_FAULT)
		return event->io.fault != 0u;
	if (event->io.fault != 0u)
		return false;
	return !(event->completed_action == AIUS_HTTP_ACTION_ACCEPT &&
	         result == AIUS_IO_CLOSED);
}

static bool app_event_valid(const AiusHttpTransaction *transaction,
	                        const AiusHttpEvent *event)
{
	return event->action_id != 0u &&
	       event->action_id == transaction->outstanding_action_id &&
	       event->completed_action == AIUS_HTTP_ACTION_DISPATCH &&
	       transaction->outstanding_action == AIUS_HTTP_ACTION_DISPATCH &&
	       event->io.result == AIUS_IO_PROGRESS && event->io.bytes == 0u &&
	       event->io.fault == 0u && event->application_response != NULL &&
	       event->sd_kind == AIUS_HTTP_SD_EVENT_NONE &&
	       event->sd_data == NULL && event->sd_capacity == 0u &&
	       event->sd_length == 0u && event->sd_size == 0u &&
	       event->sd_fault == AIUS_SD_FAULT_NONE &&
	       event->sd_driver_fault == 0u && event->peer_fault == 0u;
}

static bool sd_payload_shape_valid(const AiusHttpEvent *event)
{
	switch (event->sd_kind) {
	case AIUS_HTTP_SD_STARTED:
		return event->sd_data == NULL && event->sd_capacity != 0u &&
		       event->sd_capacity <= AIUS_HTTP_SD_CAPACITY_MAX &&
		       event->sd_length == 0u && event->sd_size == 0u &&
		       event->sd_fault == AIUS_SD_FAULT_NONE &&
		       event->sd_driver_fault == 0u;
	case AIUS_HTTP_SD_PROGRESS:
		return event->sd_data == NULL && event->sd_capacity == 0u &&
		       event->sd_length == 0u && event->sd_size == 0u &&
		       event->sd_fault == AIUS_SD_FAULT_NONE &&
		       event->sd_driver_fault == 0u;
	case AIUS_HTTP_SD_NEED_UPLOAD:
		return event->sd_data == NULL && event->sd_capacity == 0u &&
		       event->sd_length != 0u && event->sd_size == 0u &&
		       event->sd_fault == AIUS_SD_FAULT_NONE &&
		       event->sd_driver_fault == 0u;
	case AIUS_HTTP_SD_DOWNLOAD_DATA:
		return event->sd_data != NULL && event->sd_capacity == 0u &&
		       event->sd_length != 0u &&
		       event->sd_fault == AIUS_SD_FAULT_NONE &&
		       event->sd_driver_fault == 0u;
	case AIUS_HTTP_SD_COMPLETE:
		return event->sd_data == NULL && event->sd_capacity == 0u &&
		       event->sd_length == 0u &&
		       event->sd_fault == AIUS_SD_FAULT_NONE &&
		       event->sd_driver_fault == 0u;
	case AIUS_HTTP_SD_FAILED:
		return event->sd_data == NULL && event->sd_capacity == 0u &&
		       event->sd_length == 0u && event->sd_size == 0u &&
		       event->sd_fault != AIUS_SD_FAULT_NONE &&
		       (unsigned)event->sd_fault <=
			       (unsigned)AIUS_SD_FAULT_NOT_FOUND;
	case AIUS_HTTP_SD_CANCELLED:
		return event->sd_data == NULL && event->sd_capacity == 0u &&
		       event->sd_length == 0u && event->sd_size == 0u &&
		       (unsigned)event->sd_fault <=
			       (unsigned)AIUS_SD_FAULT_NOT_FOUND &&
		       (event->sd_fault != AIUS_SD_FAULT_NONE ||
		        event->sd_driver_fault == 0u);
	default:
		return false;
	}
}

static bool sd_event_correlated(const AiusHttpTransaction *transaction,
	                            const AiusHttpEvent *event)
{
	if (event->action_id == 0u ||
	    event->action_id != transaction->outstanding_action_id ||
	    event->completed_action != transaction->outstanding_action ||
	    (transaction->outstanding_action != AIUS_HTTP_ACTION_SD_START &&
	     transaction->outstanding_action != AIUS_HTTP_ACTION_SD_STEP &&
	     transaction->outstanding_action != AIUS_HTTP_ACTION_SD_CANCEL) ||
	    event->io.result != AIUS_IO_PROGRESS || event->io.bytes != 0u ||
	    event->io.fault != 0u || event->application_response != NULL ||
	    event->peer_fault != 0u || !sd_payload_shape_valid(event))
		return false;
	return true;
}

static bool peer_event_valid(const AiusHttpEvent *event)
{
	if (event->action_id != 0u ||
	    event->completed_action != AIUS_HTTP_ACTION_NONE ||
	    event->io.result != AIUS_IO_PROGRESS || event->io.bytes != 0u ||
	    event->io.fault != 0u || event->application_response != NULL ||
	    event->sd_kind != AIUS_HTTP_SD_EVENT_NONE ||
	    event->sd_data != NULL || event->sd_capacity != 0u ||
	    event->sd_length != 0u || event->sd_size != 0u ||
	    event->sd_fault != AIUS_SD_FAULT_NONE ||
	    event->sd_driver_fault != 0u)
		return false;
	if (event->kind == AIUS_HTTP_EVENT_PEER_CLOSED)
		return event->peer_fault == 0u;
	return event->peer_fault != 0u;
}

static bool is_sd_action(AiusHttpActionKind action)
{
	return action == AIUS_HTTP_ACTION_SD_START ||
	       action == AIUS_HTTP_ACTION_SD_STEP ||
	       action == AIUS_HTTP_ACTION_SD_CANCEL;
}

static bool is_transport_action(AiusHttpActionKind action)
{
	return action == AIUS_HTTP_ACTION_READ ||
	       action == AIUS_HTTP_ACTION_WRITE ||
	       action == AIUS_HTTP_ACTION_CLOSE;
}

static void reset_client(AiusHttpTransaction *transaction, uint32_t now_ms)
{
	transaction->state = AIUS_HTTP_TX_RECEIVE;
	transaction->sd_phase = AIUS_HTTP_SD_PHASE_NONE;
	transaction->request_length = 0u;
	transaction->header_length = 0u;
	transaction->body_expected = 0u;
	transaction->upload_prefix_offset = 0u;
	transaction->upload_prefix_length = 0u;
	transaction->upload_prefix_supplied = 0u;
	transaction->upload_supplied = 0u;
	transaction->response_length = 0u;
	transaction->response_offset = 0u;
	transaction->download_data = NULL;
	transaction->sd_capacity = 0u;
	transaction->download_size = 0u;
	transaction->download_sent = 0u;
	transaction->download_length = 0u;
	transaction->download_offset = 0u;
	memset(&transaction->parsed_request, 0,
	       sizeof(transaction->parsed_request));
	memset(&transaction->sd_request, 0, sizeof(transaction->sd_request));
	memset(transaction->sd_target_path, 0,
	       sizeof(transaction->sd_target_path));
	memset(transaction->sd_temp_path, 0, sizeof(transaction->sd_temp_path));
	memset(transaction->sd_backup_path, 0,
	       sizeof(transaction->sd_backup_path));
	memset(transaction->content_type, 0, sizeof(transaction->content_type));
	memset(transaction->sd_content_type, 0,
	       sizeof(transaction->sd_content_type));
	transaction->last_fault = AIUS_HTTP_FAULT_NONE;
	transaction->last_sd_fault = AIUS_SD_FAULT_NONE;
	transaction->last_transport_fault = 0u;
	transaction->last_driver_fault = 0u;
	transaction->last_status = 0u;
	transaction->client_active = true;
	transaction->transport_terminal = false;
	transaction->cleanup_requested = false;
	transaction->deadline_active = true;
	transaction->dispatch_emitted = false;
	transaction->close_emitted = false;
	transaction->response_started = false;
	transaction->response_is_download = false;
	transaction->sd_started = false;
	transaction->sd_cancel_emitted = false;
	transaction->sd_terminal = false;
	clear_outstanding(transaction);
	arm_deadline(transaction, now_ms, REQUEST_TIMEOUT_MS);
}

static void restart_idle(AiusHttpTransaction *transaction)
{
	transaction->state = AIUS_HTTP_TX_IDLE;
	transaction->sd_phase = AIUS_HTTP_SD_PHASE_NONE;
	transaction->request_length = 0u;
	transaction->header_length = 0u;
	transaction->body_expected = 0u;
	transaction->upload_prefix_offset = 0u;
	transaction->upload_prefix_length = 0u;
	transaction->upload_prefix_supplied = 0u;
	transaction->upload_supplied = 0u;
	transaction->response_length = 0u;
	transaction->response_offset = 0u;
	transaction->download_data = NULL;
	transaction->sd_capacity = 0u;
	transaction->download_size = 0u;
	transaction->download_sent = 0u;
	transaction->download_length = 0u;
	transaction->download_offset = 0u;
	memset(&transaction->parsed_request, 0,
	       sizeof(transaction->parsed_request));
	memset(&transaction->sd_request, 0, sizeof(transaction->sd_request));
	transaction->client_active = false;
	transaction->deadline_active = false;
	transaction->cleanup_requested = false;
	transaction->transport_terminal = false;
	transaction->dispatch_emitted = false;
	transaction->close_emitted = false;
	transaction->response_started = false;
	transaction->response_is_download = false;
	transaction->sd_started = false;
	transaction->sd_cancel_emitted = false;
	transaction->sd_terminal = false;
	clear_outstanding(transaction);
}

static bool format_response(AiusHttpTransaction *transaction, unsigned status,
	                        const char *content_type, const uint8_t *body,
	                        size_t body_length)
{
	size_t length = aius_http_write_response(
		transaction->response, transaction->response_capacity, status,
		content_type, body, body_length);
	if (length == 0u)
		return false;
	transaction->response_length = length;
	transaction->response_offset = 0u;
	transaction->last_status = status;
	return true;
}

static void request_cleanup(AiusHttpTransaction *transaction)
{
	transaction->cleanup_requested = true;
	transaction->deadline_active = false;
}

static void record_fault(AiusHttpTransaction *transaction,
	                     AiusHttpFault fault)
{
	if (transaction->last_fault == AIUS_HTTP_FAULT_NONE)
		transaction->last_fault = fault;
}

static void record_transport_driver_fault(
	AiusHttpTransaction *transaction, uint32_t fault)
{
	if (fault != 0u && transaction->last_transport_fault == 0u)
		transaction->last_transport_fault = fault;
}

static void select_fixed(AiusHttpTransaction *transaction, unsigned status,
	                     const uint8_t *body, size_t body_length,
	                     AiusHttpFault fault)
{
	record_fault(transaction, fault);
	if (transaction->response_started ||
	    transaction->last_transport_fault != 0u) {
		request_cleanup(transaction);
		return;
	}
	if (!format_response(transaction, status, "application/json", body,
	                     body_length - 1u)) {
		transaction->last_fault = AIUS_HTTP_FAULT_RESPONSE_CAPACITY;
		transaction->last_status = 0u;
		transaction->response_length = 0u;
	}
	request_cleanup(transaction);
}

static void protocol_fault(AiusHttpTransaction *transaction)
{
	select_fixed(transaction, 400u, protocol_body, sizeof(protocol_body),
	             AIUS_HTTP_FAULT_PROTOCOL);
}

static void transport_fault(AiusHttpTransaction *transaction, uint32_t fault)
{
	record_fault(transaction, AIUS_HTTP_FAULT_TRANSPORT);
	record_transport_driver_fault(transaction, fault);
	transaction->response_length = 0u;
	request_cleanup(transaction);
}

static bool streamed_request(const AiusHttpRequest *request)
{
	return request->method == AIUS_HTTP_PUT &&
	       (request->route == AIUS_HTTP_ROUTE_SD_FILE ||
	        request->route == AIUS_HTTP_ROUTE_SD_REPLAY);
}

static size_t find_header_end(const uint8_t *data, size_t length,
	                          size_t old_length)
{
	size_t start = old_length > 3u ? old_length - 3u : 0u;
	for (size_t i = start; i + 4u <= length; i++)
		if (memcmp(data + i, "\r\n\r\n", 4u) == 0)
			return i + 4u;
	return 0u;
}

static void parse_ready_request(AiusHttpTransaction *transaction)
{
	AiusHttpResult result =
		aius_http_parse(transaction->request, transaction->request_length,
		                &transaction->parsed_request);
	if (transaction->parsed_request.header_length != 0u) {
		transaction->header_length = transaction->parsed_request.header_length;
		transaction->body_expected =
			transaction->parsed_request.content_length;
	}
	if (result == AIUS_HTTP_ERR_BODY &&
	    transaction->parsed_request.header_length != 0u &&
	    !transaction->parsed_request.body_complete &&
	    (uint64_t)transaction->parsed_request.body_length <
		    transaction->parsed_request.content_length) {
		uint64_t required =
			(uint64_t)transaction->parsed_request.header_length +
			transaction->parsed_request.content_length;
		if (required > transaction->request_capacity ||
		    required > AIUS_HTTP_MAX_REQUEST ||
		    transaction->parsed_request.content_length >
			    AIUS_HTTP_MAX_BODY) {
			select_fixed(transaction, 413u, request_large_body,
			             sizeof(request_large_body),
			             AIUS_HTTP_FAULT_REQUEST_CAPACITY);
		}
		return;
	}
	if (result != AIUS_HTTP_OK) {
		switch (result) {
		case AIUS_HTTP_ERR_TOO_LARGE:
			select_fixed(transaction, 413u, request_large_body,
			             sizeof(request_large_body),
			             AIUS_HTTP_FAULT_REQUEST_CAPACITY);
			break;
		case AIUS_HTTP_ERR_METHOD:
			select_fixed(transaction, 405u, method_body,
			             sizeof(method_body), AIUS_HTTP_FAULT_PROTOCOL);
			break;
		case AIUS_HTTP_ERR_ROUTE:
			select_fixed(transaction, 404u, route_body,
			             sizeof(route_body), AIUS_HTTP_FAULT_PROTOCOL);
			break;
		default:
			select_fixed(transaction, 400u, bad_request_body,
			             sizeof(bad_request_body),
			             AIUS_HTTP_FAULT_PROTOCOL);
			break;
		}
		return;
	}
	if (!streamed_request(&transaction->parsed_request) &&
	    !transaction->parsed_request.body_complete)
		return;
	if (streamed_request(&transaction->parsed_request)) {
		transaction->upload_prefix_offset =
			transaction->parsed_request.header_length;
		transaction->upload_prefix_length =
			transaction->parsed_request.body_length;
	}
	transaction->state = AIUS_HTTP_TX_DISPATCH;
	transaction->deadline_active = false;
}

static void discard_request_slices(AiusHttpTransaction *transaction)
{
	transaction->parsed_request.body = NULL;
	transaction->parsed_request.resource = NULL;
	transaction->parsed_request.replay_id = NULL;
	transaction->parsed_request.sd_auth = NULL;
}

static bool application_fields_valid(const AiusHttpApplicationResponse *reply)
{
	char ignored[AIUS_HTTP_CONTENT_TYPE_STORAGE];
	if ((unsigned)reply->kind > (unsigned)AIUS_HTTP_APP_SD_JOB ||
	    !supported_status(reply->status) ||
	    !copy_string(ignored, sizeof(ignored), reply->content_type, true) ||
	    (reply->body_length != 0u && reply->body == NULL))
		return false;
	if (reply->kind == AIUS_HTTP_APP_IMMEDIATE)
		return zero_sd_request(&reply->sd_job);
	return true;
}

static void response_overflow(AiusHttpTransaction *transaction)
{
	transaction->last_fault = AIUS_HTTP_FAULT_RESPONSE_CAPACITY;
	if (!format_response(transaction, 500u, "application/json",
	                     response_large_body,
	                     sizeof(response_large_body) - 1u)) {
		transaction->last_status = 0u;
		transaction->response_length = 0u;
	}
	request_cleanup(transaction);
}

static bool accept_application(AiusHttpTransaction *transaction,
	                           const AiusHttpApplicationResponse *reply,
	                           uint32_t now_ms)
{
	bool was_streamed = streamed_request(&transaction->parsed_request);
	if (!application_fields_valid(reply))
		return false;
	if (reply->kind == AIUS_HTTP_APP_IMMEDIATE) {
		if (transaction->cleanup_requested) {
			discard_request_slices(transaction);
			return true;
		}
		if (!format_response(transaction, reply->status, reply->content_type,
		                     reply->body, reply->body_length))
			response_overflow(transaction);
		if (!transaction->cleanup_requested) {
			transaction->state = AIUS_HTTP_TX_SEND;
			arm_deadline(transaction, now_ms, RESPONSE_TIMEOUT_MS);
		}
		discard_request_slices(transaction);
		return true;
	}
	if (!copy_string(transaction->content_type,
	                 sizeof(transaction->content_type),
	                 reply->content_type, true) ||
	    !copy_sd_request(transaction, &reply->sd_job))
		return false;
	if (transaction->sd_request.kind == AIUS_SD_JOB_UPLOAD) {
		if (!was_streamed ||
		    transaction->sd_request.total != transaction->body_expected)
			return false;
	} else if (was_streamed) {
		return false;
	}
	if (transaction->sd_request.kind == AIUS_SD_JOB_DOWNLOAD) {
		if (reply->body != NULL || reply->body_length != 0u ||
		    transaction->sd_request.content_type == NULL ||
		    strcmp(transaction->content_type,
		           transaction->sd_request.content_type) != 0)
			return false;
	}
	if (transaction->cleanup_requested) {
		discard_request_slices(transaction);
		return true;
	}
	if (transaction->sd_request.kind == AIUS_SD_JOB_DOWNLOAD) {
		transaction->response_is_download = true;
		transaction->last_status = reply->status;
	} else if (!format_response(transaction, reply->status,
	                            transaction->content_type, reply->body,
	                            reply->body_length)) {
		response_overflow(transaction);
	}
	if (transaction->cleanup_requested) {
		discard_request_slices(transaction);
		return true;
	}
	transaction->state = AIUS_HTTP_TX_SD_START;
	transaction->sd_phase = AIUS_HTTP_SD_PHASE_WAIT_START;
	discard_request_slices(transaction);
	return true;
}

static uint32_t sd_interval(const AiusHttpTransaction *transaction)
{
	return transaction->sd_request.kind == AIUS_SD_JOB_DOWNLOAD ?
		DOWNLOAD_TIMEOUT_MS : SD_TIMEOUT_MS;
}

static AiusHttpFault sd_timeout_fault(const AiusHttpTransaction *transaction)
{
	if (transaction->sd_request.kind == AIUS_SD_JOB_UPLOAD)
		return AIUS_HTTP_FAULT_UPLOAD_TIMEOUT;
	if (transaction->sd_request.kind == AIUS_SD_JOB_DOWNLOAD)
		return AIUS_HTTP_FAULT_DOWNLOAD_TIMEOUT;
	return AIUS_HTTP_FAULT_SD_TIMEOUT;
}

static bool sd_kind_allowed(const AiusHttpTransaction *transaction,
	                        AiusHttpSdEventKind kind)
{
	if (transaction->outstanding_action == AIUS_HTTP_ACTION_SD_START)
		return kind == AIUS_HTTP_SD_STARTED || kind == AIUS_HTTP_SD_FAILED;
	if (transaction->outstanding_action == AIUS_HTTP_ACTION_SD_CANCEL)
		return kind == AIUS_HTTP_SD_PROGRESS ||
		       kind == AIUS_HTTP_SD_CANCELLED;
	if (transaction->outstanding_action != AIUS_HTTP_ACTION_SD_STEP)
		return false;
	switch (transaction->sd_phase) {
	case AIUS_HTTP_SD_PHASE_UPLOAD_SUPPLY:
		return kind == AIUS_HTTP_SD_PROGRESS;
	case AIUS_HTTP_SD_PHASE_DOWNLOAD_ACK:
		if (transaction->cleanup_requested &&
		    transaction->download_data != NULL &&
		    transaction->download_offset <
			    transaction->download_length)
			return kind == AIUS_HTTP_SD_PROGRESS ||
			       kind == AIUS_HTTP_SD_CANCELLED;
		return kind == AIUS_HTTP_SD_PROGRESS ||
		       kind == AIUS_HTTP_SD_DOWNLOAD_DATA ||
		       kind == AIUS_HTTP_SD_COMPLETE ||
		       kind == AIUS_HTTP_SD_FAILED;
	case AIUS_HTTP_SD_PHASE_CANCEL:
		return kind == AIUS_HTTP_SD_PROGRESS ||
		       kind == AIUS_HTTP_SD_CANCELLED;
	default:
		if (transaction->sd_request.kind == AIUS_SD_JOB_UPLOAD)
			return kind == AIUS_HTTP_SD_PROGRESS ||
			       kind == AIUS_HTTP_SD_NEED_UPLOAD ||
			       kind == AIUS_HTTP_SD_COMPLETE ||
			       kind == AIUS_HTTP_SD_FAILED;
		if (transaction->sd_request.kind == AIUS_SD_JOB_DOWNLOAD)
			return kind == AIUS_HTTP_SD_PROGRESS ||
			       kind == AIUS_HTTP_SD_DOWNLOAD_DATA ||
			       kind == AIUS_HTTP_SD_COMPLETE ||
			       kind == AIUS_HTTP_SD_FAILED;
		return kind == AIUS_HTTP_SD_PROGRESS ||
		       kind == AIUS_HTTP_SD_COMPLETE ||
		       kind == AIUS_HTTP_SD_FAILED;
	}
}

static void select_sd_error(AiusHttpTransaction *transaction,
	                        AiusSdFault fault, uint32_t driver_fault)
{
	unsigned status = 500u;
	const uint8_t *body = sd_failure_body;
	size_t size = sizeof(sd_failure_body);
	record_fault(transaction, AIUS_HTTP_FAULT_SD);
	transaction->last_sd_fault = fault;
	transaction->last_driver_fault = driver_fault;
	if (fault == AIUS_SD_FAULT_NOT_FOUND ||
	    (fault == AIUS_SD_FAULT_OPEN &&
	     transaction->sd_request.kind == AIUS_SD_JOB_DOWNLOAD)) {
		status = 404u;
		body = sd_not_found_body;
		size = sizeof(sd_not_found_body);
	} else if (fault == AIUS_SD_FAULT_RANGE) {
		status = 416u;
		body = sd_range_body;
		size = sizeof(sd_range_body);
	} else if (fault == AIUS_SD_FAULT_MEDIA) {
		status = 503u;
		body = sd_unavailable_body;
		size = sizeof(sd_unavailable_body);
	} else if (fault == AIUS_SD_FAULT_CAPACITY ||
	           fault == AIUS_SD_FAULT_OVERFLOW) {
		status = 507u;
		body = sd_capacity_body;
		size = sizeof(sd_capacity_body);
	}
	if (!transaction->response_started &&
	    transaction->last_transport_fault == 0u) {
		if (!format_response(transaction, status, "application/json", body,
		                     size - 1u)) {
			transaction->last_fault = AIUS_HTTP_FAULT_RESPONSE_CAPACITY;
			transaction->last_status = 0u;
			transaction->response_length = 0u;
		}
	}
	request_cleanup(transaction);
}

static bool consume_sd_result(AiusHttpTransaction *transaction,
	                          const AiusHttpEvent *event, uint32_t now_ms)
{
	size_t supplied = transaction->outstanding_length;
	AiusHttpSdPhase prior_phase = transaction->sd_phase;
	bool releases_download =
		transaction->outstanding_action == AIUS_HTTP_ACTION_SD_STEP &&
		(prior_phase == AIUS_HTTP_SD_PHASE_DOWNLOAD_ACK ||
		 (prior_phase == AIUS_HTTP_SD_PHASE_CANCEL &&
		  transaction->download_data != NULL));
	if (!sd_kind_allowed(transaction, event->sd_kind))
		return false;
	if (event->sd_kind == AIUS_HTTP_SD_STARTED &&
	    transaction->sd_started)
		return false;
	if (event->sd_kind == AIUS_HTTP_SD_NEED_UPLOAD &&
	    (event->sd_length > transaction->sd_capacity ||
	     transaction->upload_supplied >= transaction->body_expected))
		return false;
	if (event->sd_kind == AIUS_HTTP_SD_DOWNLOAD_DATA) {
		if (event->sd_length > transaction->sd_capacity ||
		    transaction->sd_request.kind != AIUS_SD_JOB_DOWNLOAD ||
		    (transaction->download_data != NULL && !releases_download) ||
		    event->sd_size == 0u ||
		    (transaction->download_size != 0u &&
		     event->sd_size != transaction->download_size) ||
		    transaction->download_sent > event->sd_size ||
		    (uint64_t)event->sd_length >
			    event->sd_size - transaction->download_sent)
			return false;
	}
	if (event->sd_kind == AIUS_HTTP_SD_COMPLETE) {
		if (transaction->sd_request.kind == AIUS_SD_JOB_UPLOAD &&
		    transaction->upload_supplied != transaction->body_expected)
			return false;
		if (transaction->sd_request.kind == AIUS_SD_JOB_DOWNLOAD) {
			if (transaction->download_size == 0u) {
				if (event->sd_size != 0u)
					return false;
			} else if (event->sd_size != 0u ||
			           transaction->download_sent !=
				           transaction->download_size ||
			           (transaction->download_data != NULL &&
			            !releases_download)) {
				return false;
			}
		} else if (event->sd_size != 0u) {
			return false;
		}
	}

	if (releases_download) {
		transaction->download_data = NULL;
		transaction->download_length = 0u;
		transaction->download_offset = 0u;
	}
	clear_outstanding(transaction);
	switch (event->sd_kind) {
	case AIUS_HTTP_SD_STARTED:
		transaction->sd_started = true;
		transaction->sd_capacity = event->sd_capacity;
		transaction->state = AIUS_HTTP_TX_SD_TRANSFER;
		if (transaction->sd_request.kind == AIUS_SD_JOB_UPLOAD)
			transaction->sd_phase =
				AIUS_HTTP_SD_PHASE_UPLOAD_WAIT_INPUT;
		else if (transaction->sd_request.kind == AIUS_SD_JOB_DOWNLOAD)
			transaction->sd_phase =
				AIUS_HTTP_SD_PHASE_DOWNLOAD_ADVANCE;
		else
			transaction->sd_phase =
				AIUS_HTTP_SD_PHASE_WAIT_TERMINAL;
		arm_deadline(transaction, now_ms, sd_interval(transaction));
		break;
	case AIUS_HTTP_SD_PROGRESS:
		if (prior_phase == AIUS_HTTP_SD_PHASE_UPLOAD_SUPPLY) {
			transaction->upload_supplied += supplied;
			if (transaction->upload_prefix_supplied <
			    transaction->upload_prefix_length)
				transaction->upload_prefix_supplied += supplied;
			transaction->request_length = 0u;
			transaction->sd_phase =
				AIUS_HTTP_SD_PHASE_UPLOAD_WAIT_INPUT;
		} else if (prior_phase == AIUS_HTTP_SD_PHASE_DOWNLOAD_ACK) {
			transaction->sd_phase =
				AIUS_HTTP_SD_PHASE_DOWNLOAD_ADVANCE;
		}
		arm_deadline(transaction, now_ms, sd_interval(transaction));
		break;
	case AIUS_HTTP_SD_NEED_UPLOAD:
		transaction->outstanding_length = event->sd_length;
		transaction->sd_phase = AIUS_HTTP_SD_PHASE_UPLOAD_WAIT_INPUT;
		arm_deadline(transaction, now_ms, SD_TIMEOUT_MS);
		break;
	case AIUS_HTTP_SD_DOWNLOAD_DATA:
		transaction->download_data = event->sd_data;
		transaction->download_length = event->sd_length;
		transaction->download_offset = 0u;
		transaction->download_size = event->sd_size;
		if (transaction->cleanup_requested)
			break;
		if (transaction->response_length == 0u) {
			transaction->response_length = aius_http_write_headers(
				transaction->response,
				transaction->response_capacity,
				transaction->last_status,
				transaction->content_type,
				event->sd_size);
			if (transaction->response_length == 0u) {
				response_overflow(transaction);
				request_cleanup(transaction);
				return true;
			}
		}
		transaction->state = AIUS_HTTP_TX_SEND;
		transaction->sd_phase = AIUS_HTTP_SD_PHASE_DOWNLOAD_WRITE;
		arm_deadline(transaction, now_ms, RESPONSE_TIMEOUT_MS);
		break;
	case AIUS_HTTP_SD_COMPLETE:
		transaction->sd_terminal = true;
		transaction->deadline_active = false;
		if (transaction->cleanup_requested) {
			break;
		} else if (transaction->sd_request.kind == AIUS_SD_JOB_DOWNLOAD &&
		    transaction->download_size == 0u) {
			transaction->response_length = aius_http_write_headers(
				transaction->response,
				transaction->response_capacity,
				transaction->last_status,
				transaction->content_type, 0u);
			if (transaction->response_length == 0u)
				response_overflow(transaction);
			else {
				transaction->state = AIUS_HTTP_TX_SEND;
				arm_deadline(transaction, now_ms,
				             RESPONSE_TIMEOUT_MS);
			}
		} else if (!transaction->cleanup_requested &&
		           !transaction->response_is_download) {
			transaction->state = AIUS_HTTP_TX_SEND;
			arm_deadline(transaction, now_ms, RESPONSE_TIMEOUT_MS);
		} else {
			request_cleanup(transaction);
		}
		break;
	case AIUS_HTTP_SD_FAILED:
		transaction->sd_terminal = true;
		if (transaction->cleanup_requested) {
			transaction->last_sd_fault = event->sd_fault;
			transaction->last_driver_fault =
				event->sd_driver_fault;
		} else {
			select_sd_error(transaction, event->sd_fault,
			                event->sd_driver_fault);
		}
		break;
	case AIUS_HTTP_SD_CANCELLED:
		transaction->sd_terminal = true;
		transaction->deadline_active = false;
		if (event->sd_fault != AIUS_SD_FAULT_NONE) {
			transaction->last_sd_fault = event->sd_fault;
			transaction->last_driver_fault = event->sd_driver_fault;
		}
		break;
	default:
		return false;
	}
	return true;
}

static void timeout_transaction(AiusHttpTransaction *transaction)
{
	if (is_transport_action(transaction->outstanding_action) &&
	    transaction->outstanding_action != AIUS_HTTP_ACTION_CLOSE)
		clear_outstanding(transaction);
	if (transaction->state == AIUS_HTTP_TX_RECEIVE) {
		select_fixed(transaction, 408u, request_timeout_body,
		             sizeof(request_timeout_body),
		             AIUS_HTTP_FAULT_REQUEST_TIMEOUT);
		return;
	}
	if (transaction->state == AIUS_HTTP_TX_SEND) {
		record_fault(transaction, AIUS_HTTP_FAULT_RESPONSE_TIMEOUT);
		transaction->response_length = 0u;
		request_cleanup(transaction);
		return;
	}
	record_fault(transaction, sd_timeout_fault(transaction));
	transaction->response_length = 0u;
	request_cleanup(transaction);
}

static void consume_io(AiusHttpTransaction *transaction,
	                   const AiusHttpEvent *event, uint32_t now_ms)
{
	AiusHttpActionKind completed = transaction->outstanding_action;
	size_t completed_length = transaction->outstanding_length;
	size_t old_request_length = transaction->request_length;
	clear_outstanding(transaction);
	if (completed == AIUS_HTTP_ACTION_ACCEPT) {
		if (event->io.result == AIUS_IO_PROGRESS)
			reset_client(transaction, now_ms);
		else if (event->io.result == AIUS_IO_WOULD_BLOCK)
			restart_idle(transaction);
		else {
			transaction->last_fault = AIUS_HTTP_FAULT_TRANSPORT;
			record_transport_driver_fault(transaction,
			                              event->io.fault);
			transaction->last_status = 0u;
			restart_idle(transaction);
		}
		return;
	}
	if (completed == AIUS_HTTP_ACTION_CLOSE) {
		transaction->transport_terminal = true;
		if (event->io.result == AIUS_IO_FAULT) {
			record_fault(transaction, AIUS_HTTP_FAULT_TRANSPORT);
			record_transport_driver_fault(transaction,
			                              event->io.fault);
		} else if (event->io.result == AIUS_IO_WOULD_BLOCK) {
			record_fault(transaction, AIUS_HTTP_FAULT_PROTOCOL);
		}
		return;
	}
	if (event->io.result == AIUS_IO_CLOSED) {
		record_fault(transaction, AIUS_HTTP_FAULT_TRANSPORT);
		transaction->response_length = 0u;
		request_cleanup(transaction);
		return;
	}
	if (event->io.result == AIUS_IO_FAULT) {
		transport_fault(transaction, event->io.fault);
		return;
	}
	if (event->io.result == AIUS_IO_WOULD_BLOCK) {
		if (completed == AIUS_HTTP_ACTION_READ &&
		    transaction->state == AIUS_HTTP_TX_SD_TRANSFER)
			transaction->outstanding_length = completed_length;
		return;
	}
	if (completed == AIUS_HTTP_ACTION_READ) {
		if (transaction->state == AIUS_HTTP_TX_RECEIVE) {
			transaction->request_length += event->io.bytes;
			arm_deadline(transaction, now_ms, REQUEST_TIMEOUT_MS);
			if (transaction->header_length == 0u)
				transaction->header_length = find_header_end(
					transaction->request,
					transaction->request_length,
					old_request_length);
			if (transaction->header_length != 0u)
				parse_ready_request(transaction);
		} else {
			transaction->request_length = event->io.bytes;
			transaction->sd_phase =
				AIUS_HTTP_SD_PHASE_UPLOAD_SUPPLY;
			arm_deadline(transaction, now_ms, SD_TIMEOUT_MS);
		}
		return;
	}
	if (completed == AIUS_HTTP_ACTION_WRITE) {
		bool completed_download_body = false;
		transaction->response_started = true;
		if (transaction->response_offset <
		    transaction->response_length) {
			transaction->response_offset += event->io.bytes;
		} else {
			transaction->download_offset += event->io.bytes;
			transaction->download_sent += event->io.bytes;
			completed_download_body =
				transaction->download_data != NULL &&
				transaction->download_offset ==
					transaction->download_length;
		}
		arm_deadline(transaction, now_ms,
		             completed_download_body ?
			             DOWNLOAD_TIMEOUT_MS :
			             RESPONSE_TIMEOUT_MS);
	}
}

static void handle_malformed(AiusHttpTransaction *transaction)
{
	if (!transaction->client_active) {
		transaction->last_fault = AIUS_HTTP_FAULT_PROTOCOL;
		transaction->last_status = 0u;
		restart_idle(transaction);
		return;
	}
	if (transaction->outstanding_action == AIUS_HTTP_ACTION_CLOSE)
		transaction->transport_terminal = true;
	if (!is_sd_action(transaction->outstanding_action))
		clear_outstanding(transaction);
	protocol_fault(transaction);
}

static bool validate_and_consume_event(AiusHttpTransaction *transaction,
	                                   uint32_t now_ms,
	                                   const AiusHttpEvent *event,
	                                   bool *progress)
{
	bool peer_aborts_active;
	*progress = false;
	switch (event->kind) {
	case AIUS_HTTP_EVENT_NONE:
		if (!event_common_zero(event)) {
			handle_malformed(transaction);
			return false;
		}
		if (transaction->outstanding_action == AIUS_HTTP_ACTION_DISPATCH ||
		    transaction->outstanding_action == AIUS_HTTP_ACTION_SD_START) {
			handle_malformed(transaction);
			return false;
		}
		return true;
	case AIUS_HTTP_EVENT_IO_RESULT:
		if (!io_payload_valid(transaction, event)) {
			handle_malformed(transaction);
			return false;
		}
		*progress = event->io.result == AIUS_IO_PROGRESS;
		consume_io(transaction, event, now_ms);
		return true;
	case AIUS_HTTP_EVENT_APPLICATION_RESPONSE:
		if (!app_event_valid(transaction, event)) {
			handle_malformed(transaction);
			return false;
		}
		clear_outstanding(transaction);
		if (!accept_application(transaction, event->application_response,
		                        now_ms)) {
			memset(&transaction->sd_request, 0,
			       sizeof(transaction->sd_request));
			discard_request_slices(transaction);
			protocol_fault(transaction);
		}
		return true;
	case AIUS_HTTP_EVENT_SD_RESULT:
		if (!sd_event_correlated(transaction, event) ||
		    !sd_kind_allowed(transaction, event->sd_kind)) {
			handle_malformed(transaction);
			return false;
		}
		if (!consume_sd_result(transaction, event, now_ms)) {
			handle_malformed(transaction);
			return false;
		}
		*progress = event->sd_kind == AIUS_HTTP_SD_PROGRESS ||
		            event->sd_kind == AIUS_HTTP_SD_NEED_UPLOAD ||
		            event->sd_kind == AIUS_HTTP_SD_DOWNLOAD_DATA;
		return true;
	case AIUS_HTTP_EVENT_PEER_CLOSED:
	case AIUS_HTTP_EVENT_PEER_FAULT:
		if (!transaction->client_active || !peer_event_valid(event)) {
			handle_malformed(transaction);
			return false;
		}
		peer_aborts_active =
			!transaction->transport_terminal;
		if (transaction->outstanding_action ==
		    AIUS_HTTP_ACTION_CLOSE)
			transaction->transport_terminal = true;
		if (is_transport_action(transaction->outstanding_action))
			clear_outstanding(transaction);
		if (event->kind == AIUS_HTTP_EVENT_PEER_FAULT &&
		    peer_aborts_active)
			transport_fault(transaction, event->peer_fault);
		else {
			if (event->kind == AIUS_HTTP_EVENT_PEER_CLOSED &&
			    peer_aborts_active)
				record_fault(
					transaction,
					AIUS_HTTP_FAULT_TRANSPORT);
			transaction->response_length = 0u;
			request_cleanup(transaction);
		}
		return true;
	default:
		handle_malformed(transaction);
		return false;
	}
}

static void emit_read(AiusHttpTransaction *transaction,
	                  AiusHttpAction *action, uint8_t *buffer, size_t length)
{
	if (!emit_action(transaction, action, AIUS_HTTP_ACTION_READ))
		return;
	action->read_buffer = buffer;
	action->length = length;
	transaction->outstanding_length = length;
}

static void emit_write(AiusHttpTransaction *transaction,
	                   AiusHttpAction *action, const uint8_t *data,
	                   size_t length)
{
	if (!emit_action(transaction, action, AIUS_HTTP_ACTION_WRITE))
		return;
	action->data = data;
	action->length = length;
	transaction->outstanding_length = length;
}

static void emit_sd_step(AiusHttpTransaction *transaction,
	                     AiusHttpAction *action, AiusHttpSdOperation operation,
	                     const uint8_t *data, size_t length)
{
	if (!emit_action(transaction, action, AIUS_HTTP_ACTION_SD_STEP))
		return;
	action->sd_operation = operation;
	action->data = data;
	action->length = length;
	action->fault = operation == AIUS_HTTP_SD_OP_FAIL_DOWNLOAD ?
		AIUS_HTTP_SD_OUTPUT_CANCEL_FAULT : 0u;
	transaction->outstanding_length = length;
}

static void drive_receive(AiusHttpTransaction *transaction,
	                      AiusHttpAction *action)
{
	size_t available;
	size_t offer;
	if (transaction->header_length != 0u)
		parse_ready_request(transaction);
	if (transaction->state != AIUS_HTTP_TX_RECEIVE ||
	    transaction->cleanup_requested)
		return;
	if (transaction->request_length >= transaction->request_capacity ||
	    transaction->request_length >= AIUS_HTTP_MAX_REQUEST) {
		select_fixed(transaction, 413u, request_large_body,
		             sizeof(request_large_body),
		             AIUS_HTTP_FAULT_REQUEST_CAPACITY);
		return;
	}
	available = transaction->request_capacity - transaction->request_length;
	if (available > AIUS_HTTP_MAX_REQUEST - transaction->request_length)
		available = AIUS_HTTP_MAX_REQUEST - transaction->request_length;
	offer = available < AIUS_HTTP_RX_QUOTA ? available : AIUS_HTTP_RX_QUOTA;
	emit_read(transaction, action,
	          transaction->request + transaction->request_length, offer);
}

static void drive_send(AiusHttpTransaction *transaction,
	                   AiusHttpAction *action)
{
	size_t remaining;
	size_t offer;
	if (transaction->response_offset < transaction->response_length) {
		remaining =
			transaction->response_length - transaction->response_offset;
		offer = remaining < AIUS_HTTP_TX_QUOTA ?
			remaining : AIUS_HTTP_TX_QUOTA;
		emit_write(transaction, action,
		           transaction->response + transaction->response_offset,
		           offer);
		return;
	}
	if (transaction->response_is_download &&
	    transaction->download_data != NULL &&
	    transaction->download_offset < transaction->download_length) {
		remaining =
			transaction->download_length - transaction->download_offset;
		offer = remaining < AIUS_HTTP_TX_QUOTA ?
			remaining : AIUS_HTTP_TX_QUOTA;
		emit_write(transaction, action,
		           transaction->download_data +
			           transaction->download_offset,
		           offer);
		return;
	}
	if (transaction->response_is_download &&
	    transaction->download_data != NULL) {
		transaction->state = AIUS_HTTP_TX_SD_TRANSFER;
		transaction->sd_phase = AIUS_HTTP_SD_PHASE_DOWNLOAD_ACK;
		emit_sd_step(transaction, action, AIUS_HTTP_SD_OP_ACK_DOWNLOAD,
		             NULL, 0u);
		return;
	}
	request_cleanup(transaction);
}

static void drive_sd(AiusHttpTransaction *transaction,
	                 AiusHttpAction *action)
{
	size_t remaining;
	size_t offered;
	uint64_t body_remaining;
	if (transaction->state == AIUS_HTTP_TX_SD_START) {
		if (!emit_action(transaction, action, AIUS_HTTP_ACTION_SD_START))
			return;
		action->sd_request = &transaction->sd_request;
		return;
	}
	if (transaction->sd_phase == AIUS_HTTP_SD_PHASE_UPLOAD_WAIT_INPUT &&
	    transaction->outstanding_length != 0u) {
		offered = transaction->outstanding_length;
		transaction->outstanding_length = 0u;
		if (transaction->upload_prefix_supplied <
		    transaction->upload_prefix_length) {
			remaining = transaction->upload_prefix_length -
			            transaction->upload_prefix_supplied;
			if (offered > remaining)
				offered = remaining;
			transaction->sd_phase =
				AIUS_HTTP_SD_PHASE_UPLOAD_SUPPLY;
			emit_sd_step(
				transaction, action,
				AIUS_HTTP_SD_OP_SUPPLY_UPLOAD,
				transaction->request +
					transaction->upload_prefix_offset +
					transaction->upload_prefix_supplied,
				offered);
			return;
		}
		body_remaining = transaction->body_expected -
		                 transaction->upload_supplied;
		if (offered > AIUS_HTTP_RX_QUOTA)
			offered = AIUS_HTTP_RX_QUOTA;
		if (offered > transaction->request_capacity)
			offered = transaction->request_capacity;
		if ((uint64_t)offered > body_remaining)
			offered = (size_t)body_remaining;
		emit_read(transaction, action, transaction->request, offered);
		return;
	}
	if (transaction->sd_phase == AIUS_HTTP_SD_PHASE_UPLOAD_SUPPLY) {
		emit_sd_step(transaction, action, AIUS_HTTP_SD_OP_SUPPLY_UPLOAD,
		             transaction->request, transaction->request_length);
		return;
	}
	emit_sd_step(transaction, action, AIUS_HTTP_SD_OP_ADVANCE, NULL, 0u);
}

static void drive_cleanup(AiusHttpTransaction *transaction,
	                      uint32_t now_ms, AiusHttpAction *action)
{
	if (transaction->download_data != NULL) {
		transaction->state = AIUS_HTTP_TX_SD_TRANSFER;
		transaction->sd_phase = AIUS_HTTP_SD_PHASE_CANCEL;
		emit_sd_step(transaction, action, AIUS_HTTP_SD_OP_FAIL_DOWNLOAD,
		             NULL, 0u);
		return;
	}
	if (!transaction->response_started &&
	    transaction->response_length != 0u &&
	    transaction->last_transport_fault == 0u &&
	    !transaction->transport_terminal) {
		transaction->state = AIUS_HTTP_TX_SEND;
		transaction->cleanup_requested = false;
		arm_deadline(transaction, now_ms, RESPONSE_TIMEOUT_MS);
		drive_send(transaction, action);
		return;
	}
	if (!transaction->transport_terminal && !transaction->close_emitted) {
		transaction->state = AIUS_HTTP_TX_CLOSE;
		if (emit_action(transaction, action, AIUS_HTTP_ACTION_CLOSE))
			transaction->close_emitted = true;
		return;
	}
	if (!transaction->transport_terminal)
		return;
	if (transaction->sd_started && !transaction->sd_terminal) {
		transaction->state = AIUS_HTTP_TX_SD_TRANSFER;
		transaction->sd_phase = AIUS_HTTP_SD_PHASE_CANCEL;
		if (!transaction->sd_cancel_emitted) {
			if (emit_action(transaction, action,
			                AIUS_HTTP_ACTION_SD_CANCEL))
				transaction->sd_cancel_emitted = true;
		} else {
			emit_sd_step(transaction, action,
			             AIUS_HTTP_SD_OP_ADVANCE, NULL, 0u);
		}
		return;
	}
	restart_idle(transaction);
}

void aius_http_transaction_init(AiusHttpTransaction *transaction,
	                            uint8_t *request, size_t request_capacity,
	                            uint8_t *response, size_t response_capacity,
	                            uint32_t now_ms)
{
	uintptr_t request_begin;
	uintptr_t response_begin;
	bool overlap;
	(void)now_ms;
	if (transaction == NULL)
		return;
	memset(transaction, 0, sizeof(*transaction));
	transaction->request = request;
	transaction->request_capacity = request_capacity;
	transaction->response = response;
	transaction->response_capacity = response_capacity;
	transaction->next_action_id = UINT64_C(1);
	transaction->state = AIUS_HTTP_TX_IDLE;
	transaction->sd_phase = AIUS_HTTP_SD_PHASE_NONE;
	transaction->last_fault = AIUS_HTTP_FAULT_INVALID_INIT;
	if (request == NULL || response == NULL || request_capacity == 0u ||
	    response_capacity == 0u ||
	    request_capacity > AIUS_HTTP_MAX_REQUEST ||
	    response_capacity > AIUS_HTTP_MAX_RESPONSE)
		return;
	request_begin = (uintptr_t)request;
	response_begin = (uintptr_t)response;
	overlap = request_begin <= response_begin ?
		response_begin - request_begin < request_capacity :
		request_begin - response_begin < response_capacity;
	if (overlap)
		return;
	transaction->valid = true;
	transaction->last_fault = AIUS_HTTP_FAULT_NONE;
}

void aius_http_transaction_step(AiusHttpTransaction *transaction,
	                            uint32_t now_ms, AiusHttpEvent event,
	                            AiusHttpAction *action)
{
	bool progress = false;
	bool consumed;
	if (action == NULL)
		return;
	clear_action(action);
	if (transaction == NULL || !transaction->valid)
		return;

	consumed = validate_and_consume_event(transaction, now_ms, &event,
	                                     &progress);
	if (!transaction->valid)
		return;
	if (!consumed && !transaction->client_active &&
	    transaction->state == AIUS_HTTP_TX_IDLE)
		return;
	if (consumed && event.kind == AIUS_HTTP_EVENT_IO_RESULT &&
	    event.completed_action == AIUS_HTTP_ACTION_ACCEPT &&
	    transaction->state == AIUS_HTTP_TX_IDLE)
		return;

	if (consumed && transaction->deadline_active &&
	    deadline_expired(now_ms, transaction->deadline_ms) &&
	    !progress &&
	    (event.kind == AIUS_HTTP_EVENT_NONE ||
	     (event.kind == AIUS_HTTP_EVENT_IO_RESULT &&
	      event.io.result == AIUS_IO_WOULD_BLOCK)))
		timeout_transaction(transaction);

	if (transaction->outstanding_action != AIUS_HTTP_ACTION_NONE)
		return;
	if (!consumed && transaction->cleanup_requested) {
		drive_cleanup(transaction, now_ms, action);
		return;
	}
	if (transaction->cleanup_requested) {
		drive_cleanup(transaction, now_ms, action);
		return;
	}
	switch (transaction->state) {
	case AIUS_HTTP_TX_IDLE:
		emit_action(transaction, action, AIUS_HTTP_ACTION_ACCEPT);
		break;
	case AIUS_HTTP_TX_RECEIVE:
		drive_receive(transaction, action);
		if (transaction->cleanup_requested &&
		    action->kind == AIUS_HTTP_ACTION_NONE)
			drive_cleanup(transaction, now_ms, action);
		break;
	case AIUS_HTTP_TX_DISPATCH:
		if (!transaction->dispatch_emitted &&
		    emit_action(transaction, action,
		                AIUS_HTTP_ACTION_DISPATCH)) {
			transaction->dispatch_emitted = true;
			action->request = &transaction->parsed_request;
		}
		break;
	case AIUS_HTTP_TX_SD_START:
	case AIUS_HTTP_TX_SD_TRANSFER:
		drive_sd(transaction, action);
		break;
	case AIUS_HTTP_TX_SEND:
		drive_send(transaction, action);
		if (transaction->cleanup_requested &&
		    action->kind == AIUS_HTTP_ACTION_NONE)
			drive_cleanup(transaction, now_ms, action);
		break;
	case AIUS_HTTP_TX_CLOSE:
		drive_cleanup(transaction, now_ms, action);
		break;
	default:
		protocol_fault(transaction);
		drive_cleanup(transaction, now_ms, action);
		break;
	}
}
