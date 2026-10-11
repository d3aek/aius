#ifndef AIUS_SD_OVERLAY_REQUEST_H
#define AIUS_SD_OVERLAY_REQUEST_H

/* The overlay job's card request (aius-sample-rate-following.md Phase 4h,
 * FR-S7): maps an AiusSdOverlayJob onto lib/aius_sd's AiusSdJobRequest. A
 * separate header, inline, so aius_sd_overlay.c itself stays free of
 * lib/aius_sd; the platform, which links both, includes it. */

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "aius_sd.h"
#include "aius_sd_overlay.h"

#ifdef __cplusplus
static_assert(AIUS_SD_OVERLAY_PATH_STORAGE == AIUS_SD_PATH_STORAGE,
              "overlay job paths must fit lib/aius_sd's path storage");
extern "C" {
#else
_Static_assert(AIUS_SD_OVERLAY_PATH_STORAGE == AIUS_SD_PATH_STORAGE,
               "overlay job paths must fit lib/aius_sd's path storage");
#endif

/* Fills `request` for `job`. The request points into `job`'s path arrays, so
 * `job` must outlive aius_sd_job_start (which copies the paths).
 *
 * COPY_BEFORE_WRITE: UPLOAD in AIUS_SD_INSTALL_COPY_BEFORE_WRITE, total
 * `length`, target_first = (order == TARGET_FIRST), skip_backup as the job
 * says, expected_crc32 = file_crc (the store's zlib CRC-32 of the accepted
 * bytes), the fixed backup_path, temp_path NULL, no publish.
 * REPLACE: today's UPLOAD in AIUS_SD_INSTALL_REPLACE with the platform's
 * `replace_temp_path` and `replace_backup_path` (the job carries neither).
 * DELETE: the job's target_path (already the backup's path for a backup
 * step), require_existing false (S2).
 *
 * False, with `request` zeroed, for JOB_NONE, a COPY_BEFORE_WRITE job with
 * no backup path or no length, or a REPLACE without both platform paths. */
static inline bool aius_sd_overlay_job_request(const AiusSdOverlayJob *job,
                                               const char *replace_temp_path,
                                               const char *replace_backup_path,
                                               AiusSdJobRequest *request)
{
	if (request == NULL)
		return false;
	memset(request, 0, sizeof(*request));
	if (job == NULL)
		return false;
	switch (job->kind) {
	case AIUS_SD_OVERLAY_JOB_COPY_BEFORE_WRITE:
		if (job->backup_path[0] == '\0' || job->length == 0u)
			return false;
		request->kind = AIUS_SD_JOB_UPLOAD;
		request->install_mode = AIUS_SD_INSTALL_COPY_BEFORE_WRITE;
		request->target_path = job->target_path;
		request->backup_path = job->backup_path;
		request->total = job->length;
		request->target_first =
			job->order == AIUS_SD_OVERLAY_ORDER_TARGET_FIRST;
		request->skip_backup = job->skip_backup;
		request->expected_crc32 = job->file_crc;
		return true;
	case AIUS_SD_OVERLAY_JOB_REPLACE:
		if (replace_temp_path == NULL || replace_backup_path == NULL)
			return false;
		request->kind = AIUS_SD_JOB_UPLOAD;
		request->install_mode = AIUS_SD_INSTALL_REPLACE;
		request->target_path = job->target_path;
		request->temp_path = replace_temp_path;
		request->backup_path = replace_backup_path;
		request->total = job->length;
		return true;
	case AIUS_SD_OVERLAY_JOB_DELETE:
		request->kind = AIUS_SD_JOB_DELETE;
		request->target_path = job->target_path;
		request->require_existing = false;
		return true;
	case AIUS_SD_OVERLAY_JOB_NONE:
	default:
		return false;
	}
}

#ifdef __cplusplus
}
#endif

#endif
