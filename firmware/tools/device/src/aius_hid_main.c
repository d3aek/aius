/* P6 host proof: exercise the unchanged capability runner over hidraw. */

#include "aius_hid_runner.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *const vectors[] = {
    "stereo_tones", "surround51_tones", "stereo_impulse", "surround51_impulse"
};

static uint8_t *read_file(const char *path, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long size = ftell(f);
    if (size <= 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    uint8_t *data = malloc((size_t)size);
    if (data == NULL || fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *len_out = (size_t)size;
    return data;
}

static size_t find_sync(const uint8_t *data, size_t len, size_t start)
{
    for (size_t i = start; i + 4 <= len; i++) {
        if (data[i] == 0x7f && data[i + 1] == 0xfe &&
            data[i + 2] == 0x80 && data[i + 3] == 0x01)
            return i;
    }
    return len;
}

static uint64_t elapsed_us(const struct timespec *a, const struct timespec *b)
{
    int64_t sec = (int64_t)b->tv_sec - a->tv_sec;
    int64_t nsec = (int64_t)b->tv_nsec - a->tv_nsec;
    return (uint64_t)(sec * 1000000 + nsec / 1000);
}

int main(int argc, char **argv)
{
    if (argc < 3 || argc > 4) {
        fprintf(stderr, "usage: %s /dev/hidrawN vectors-dir [audit-file]\n"
                        "       %s /dev/hidrawN --reboot\n", argv[0], argv[0]);
        return 2;
    }

    AiusHidTransport hid;
    if (aius_hid_transport_open(&hid, argv[1]) != 0) {
        fprintf(stderr, "open %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    AiusHostRunner runner;
    aius_host_runner_init(&runner, aius_hid_transport_interface(&hid));
    aius_host_runner_set_write_chunk(&runner, AIUS_HID_REPORT_BYTES);
    runner.max_polls = 10000;
	runner.fragment_large = 1;
    struct timespec sequence_clock;
    clock_gettime(CLOCK_MONOTONIC, &sequence_clock);
    runner.next_seq = (uint16_t)(sequence_clock.tv_nsec | 1u);
    if (strcmp(argv[2], "--reboot") == 0) {
		int rc = aius_host_reboot_bootloader(&runner);
		aius_hid_transport_close(&hid);
		return rc == 0 ? 0 : 1;
	}
    char default_audit[256];
    snprintf(default_audit, sizeof(default_audit), "%s.hid.audit", argv[1]);
    const char *audit_path = argc == 4 ? argv[3] : default_audit;
    FILE *audit = fopen(audit_path, "w");
    if (audit == NULL) {
        fprintf(stderr, "open %s: %s\n", audit_path, strerror(errno));
        aius_hid_transport_close(&hid);
        return 1;
    }
    aius_host_runner_set_audit(&runner, audit);

    AiusCapCapabilities caps;
    if (aius_host_identify(&runner, &caps) != 0) {
        fprintf(stderr, "identify failed: error=%u\n", runner.last_error);
        fclose(audit);
        aius_hid_transport_close(&hid);
        return 1;
    }
    fprintf(audit, "capabilities name=%s abi=%u proto=%u max_payload=%u flags=0x%x\n",
            caps.name, caps.abi_version, caps.proto_version, caps.max_payload,
            caps.capability_flags);

    unsigned submitted = 0;
    unsigned failed = 0;
    unsigned iterations = 1;
    const char *iterations_text = getenv("AIUS_HID_ITERATIONS");
    if (iterations_text != NULL && *iterations_text != '\0') {
        unsigned long parsed = strtoul(iterations_text, NULL, 10);
        if (parsed > 0 && parsed <= UINT_MAX)
            iterations = (unsigned)parsed;
    }
    for (unsigned iteration = 0; iteration < iterations; iteration++) {
        for (size_t v = 0; v < sizeof(vectors) / sizeof(vectors[0]); v++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s.dts", argv[2], vectors[v]);
        size_t len = 0;
        uint8_t *data = read_file(path, &len);
        if (data == NULL) {
            fprintf(stderr, "read %s: %s\n", path, strerror(errno));
            failed++;
            continue;
        }
        size_t start = find_sync(data, len, 0);
        while (start < len) {
            size_t end = find_sync(data, len, start + 4);
            AiusCapFrameResult result = {0};
            struct timespec before, after;
            clock_gettime(CLOCK_MONOTONIC, &before);
            int rc = aius_host_submit_frame(&runner, data + start,
                                            (uint32_t)(end - start), &result);
            clock_gettime(CLOCK_MONOTONIC, &after);
            fprintf(audit, "result vector=%s offset=%zu bytes=%zu rc=%d status=%u "
                           "channels=%u frames=%u rate=%u mask=0x%x high_water=%u "
                           "alloc=%u elapsed_us=%llu\n",
                    vectors[v], start, end - start, rc, result.status, result.channels,
                    result.frames, result.sample_rate_hz, result.channel_mask,
                    result.arena_high_water, result.alloc_count,
                    (unsigned long long)elapsed_us(&before, &after));
            submitted++;
            if (rc != 0 || result.status != AIUS_OK)
                failed++;
            if ((rc != 0 || result.status != AIUS_OK) && failed <= 3)
                fprintf(stderr, "first-failure vector=%s offset=%zu rc=%d "
                                "runner_error=%u result_status=%u hid_errno=%d\n",
                        vectors[v], start, rc, runner.last_error, result.status,
                        hid.last_errno);
            start = end;
        }
            free(data);
        }
        if (iterations > 1)
            fprintf(stderr, "soak iteration %u/%u submitted=%u failed=%u\n",
                    iteration + 1, iterations, submitted, failed);
    }

    fprintf(audit, "summary iterations=%u submitted=%u failed=%u\n",
            iterations, submitted, failed);
    fclose(audit);
    aius_hid_transport_close(&hid);
    printf("P6 hidraw proof: iterations=%u submitted=%u failed=%u audit=%s\n",
           iterations, submitted, failed, audit_path);
    return failed == 0 ? 0 : 1;
}
