#pragma once

#include <stddef.h>

// Emits the firmware-owned fallback home document. Kept host-buildable so the
// browser contract exercises the exact generator used on-device.
size_t aius_http_home_page(char *body, size_t capacity, const char *git_revision, const char *build_utc);
