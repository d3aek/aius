#!/usr/bin/env bash
# Uploads a web tree to the Aius device over HTTP PUT, one file at a time.
#
# ADDITIVE ONLY -- this must never delete, prune, or mirror. User data lives
# inside the served tree (/sd/www/presets/<name>.json, the DSP editor's named
# presets), so any future provisioning tool that removes files not present in
# the source tree would destroy it. Keep every card-provisioning path additive,
# or exclude presets/ explicitly before adding a destructive mode.
set -euo pipefail

usage() {
    printf '%s\n' "Usage: $0 --device URL --www DIRECTORY (--token TOKEN | --token-file PATH)"
    printf '%s\n' "Uploads a web tree to the Aius device over HTTP PUT."
    printf '%s\n' "The token may also come from AIUS_SD_AUTH. --token-file is the form an"
    printf '%s\n' "agent session uses: the token never appears on a command line."
}

device_url=''
www_dir=''
auth_token="${AIUS_SD_AUTH:-}"

while (($# > 0)); do
    case "$1" in
        --device)
            (($# >= 2)) || { usage >&2; exit 2; }
            device_url=${2%/}
            shift 2
            ;;
        --www)
            (($# >= 2)) || { usage >&2; exit 2; }
            www_dir=$2
            shift 2
            ;;
        --token)
            (($# >= 2)) || { usage >&2; exit 2; }
            auth_token=$2
            shift 2
            ;;
        --token-file)
            (($# >= 2)) || { usage >&2; exit 2; }
            [[ -r "$2" ]] || { printf 'token file not readable: %s\n' "$2" >&2; exit 2; }
            # First line only, trailing whitespace stripped: one secret per file.
            IFS= read -r auth_token < "$2" || true
            auth_token=${auth_token%%[[:space:]]*}
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            usage >&2
            exit 2
            ;;
    esac
done

[[ -n "$device_url" && -n "$www_dir" && -n "$auth_token" ]] || {
    usage >&2
    exit 2
}
command -v curl >/dev/null || { printf '%s\n' 'curl is required' >&2; exit 127; }
[[ -d "$www_dir" ]] || { printf 'web directory not found: %s\n' "$www_dir" >&2; exit 1; }

if [[ ${#auth_token} -ne 16 ]]; then
    printf '%s\n' 'token must contain exactly 16 hexadecimal characters' >&2
    exit 2
fi
case "$auth_token" in
    (*[!0123456789abcdefABCDEF]*)
        printf '%s\n' 'token must contain exactly 16 hexadecimal characters' >&2
        exit 2
        ;;
esac

upload_file() {
    local file=$1
    local relative=${file#"$www_dir"/}
    local resource
    resource=$(printf '%s' "$relative" | sed 's|/|%2F|g')
    [[ "${relative##*/}" != 'config.json' ]] || {
        printf '%s\n' 'refusing to upload config.json as a web asset' >&2
        exit 2
    }
    case "$relative" in
        (*\?*|*\#*|*%*|*\\*|*/../*|../*|*/./*|./*)
            printf 'unsupported web path: %s\n' "$relative" >&2
            exit 2
            ;;
    esac
    printf 'PUT /sd/www/%s\n' "$relative"
    # The device serves one HTTP client at a time and drops a connection that
    # arrives before the previous one is fully released, so a sequential upload
    # run hits that window intermittently -- observed as one failure in ten,
    # closing in 0.04s rather than timing out. Retry rather than widening the
    # firmware's socket budget, which is bounded on purpose.
    local attempt
    for attempt in 1 2 3 4; do
        if curl --fail-with-body --silent --show-error \
            --request PUT \
            --header "X-Aius-SD-Auth: $auth_token" \
            --header 'Content-Type: application/octet-stream' \
            --upload-file "$file" \
            "$device_url/sd/www/$resource"
        then
            printf '\n'
            return 0
        fi
        printf '  retry %d/3 for %s\n' "$attempt" "$relative" >&2
        sleep 1
    done
    printf 'upload failed after 4 attempts: %s\n' "$relative" >&2
    exit 1
}

while IFS= read -r -d '' file; do
    upload_file "$file"
done < <(find "$www_dir" -type f -print0)
