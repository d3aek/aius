#!/usr/bin/env bash
set -euo pipefail

usage() {
    printf '%s\n' "Usage: $0 --device URL --www DIRECTORY --token TOKEN"
    printf '%s\n' "Uploads a web tree to the Aius device over HTTP PUT."
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
    curl --fail-with-body --silent --show-error \
        --request PUT \
        --header "X-Aius-SD-Auth: $auth_token" \
        --header 'Content-Type: application/octet-stream' \
        --upload-file "$file" \
        "$device_url/sd/www/$resource"
    printf '\n'
}

while IFS= read -r -d '' file; do
    upload_file "$file"
done < <(find "$www_dir" -type f -print0)
