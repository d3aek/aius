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
    printf '%s\n' "Usage: $0 --device URL --www DIRECTORY (--token TOKEN | --token-file PATH) [--file RELATIVE]..."
    printf '%s\n' "Uploads a web tree to the Aius device over HTTP PUT."
    printf '%s\n' "--file uploads only the named file, given relative to --www (repeatable);"
    printf '%s\n' "without it every file under --www is uploaded."
    printf '%s\n' "Each page also uploads its <page>.html.gz bundle (www_bundle.py), beside the"
    printf '%s\n' "unmodified sources. A 503 is retried per Retry-After for up to 15 s per file."
    printf '%s\n' "The token may also come from AIUS_SD_AUTH. --token-file is the form an"
    printf '%s\n' "agent session uses: the token never appears on a command line."
}

device_url=''
www_dir=''
auth_token="${AIUS_SD_AUTH:-}"
selected_files=()

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
        --file)
            (($# >= 2)) || { usage >&2; exit 2; }
            selected_files+=("$2")
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

# Phase 4g: a 503 carries Retry-After; honour it for up to retry_budget seconds
# per file, then give up. Other failures (a dropped connection) keep the short
# fixed retry below.
retry_budget=15
tool_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
work_dir=$(mktemp -d)
trap 'rm -rf "$work_dir"' EXIT

upload_file() {
    local file=$1
    local relative=$2
    # The relative path is sent literally: the firmware rejects '%' in a
    # resource and creates missing parent directories on the card.
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
    local attempt=0 waited=0 code retry_after
    local headers="$work_dir/headers" body="$work_dir/body"
    while true; do
        : > "$headers"
        : > "$body"
        code=$(curl --silent --show-error \
            --request PUT \
            --header "X-Aius-SD-Auth: $auth_token" \
            --header 'Content-Type: application/octet-stream' \
            --upload-file "$file" \
            --dump-header "$headers" --output "$body" \
            --write-out '%{http_code}' \
            "$device_url/sd/www/$relative") || code=000
        if [[ "$code" == 2?? ]]; then
            cat "$body"
            printf '\n'
            return 0
        fi
        if [[ "$code" == 503 ]]; then
            retry_after=$(sed -n 's/^[Rr]etry-[Aa]fter:[[:space:]]*\([0-9][0-9]*\).*/\1/p' "$headers" | head -n 1)
            retry_after=${retry_after:-1}
            if ((waited + retry_after > retry_budget)); then
                cat "$body" >&2
                printf 'upload failed, device busy (503) for %ss: %s\n' "$waited" "$relative" >&2
                exit 1
            fi
            printf '  503, retry in %ss for %s\n' "$retry_after" "$relative" >&2
            sleep "$retry_after"
            waited=$((waited + retry_after))
            continue
        fi
        # 503s are bounded by retry_budget alone; only other failures count here.
        attempt=$((attempt + 1))
        if ((attempt >= 4)); then
            cat "$body" >&2
            printf 'upload failed after 4 attempts (http %s): %s\n' "$code" "$relative" >&2
            exit 1
        fi
        printf '  retry %d/3 for %s\n' "$attempt" "$relative" >&2
        sleep 1
    done
}

# Bundle step (Phase 4g G1): <page>.html.gz is built into a scratch directory
# and uploaded beside the unmodified sources.
bundle_pages() {
    command -v python3 >/dev/null || { printf '%s\n' 'python3 is required' >&2; exit 127; }
    python3 "$tool_dir/www_bundle.py" --www "$www_dir" --out "$work_dir/bundles" >/dev/null || {
        printf '%s\n' 'www bundle step failed' >&2
        exit 1
    }
}

if ((${#selected_files[@]} > 0)); then
    for relative in "${selected_files[@]}"; do
        [[ -f "$www_dir/$relative" ]] || {
            printf 'file not found under %s: %s\n' "$www_dir" "$relative" >&2
            exit 1
        }
    done
    # The bundle step runs only when a selected file is a page or an inlined
    # asset, and then before any upload, so a broken tree uploads nothing.
    command -v python3 >/dev/null || { printf '%s\n' 'python3 is required' >&2; exit 127; }
    references=$'\n'$(python3 "$tool_dir/www_bundle.py" --www "$www_dir" --referenced)$'\n'
    needs_bundle=0
    for relative in "${selected_files[@]}"; do
        relative=${relative#./}
        case "$relative" in
            (index.html|status.html|upgrade.html|classic.html) needs_bundle=1 ;;
            (*) [[ "$references" != *$'\n'"$relative"$'\n'* ]] || needs_bundle=1 ;;
        esac
    done
    needed=$'\n'
    if ((needs_bundle)); then
        pairs=$(python3 "$tool_dir/www_bundle.py" --www "$www_dir" --inlined) || {
            printf '%s\n' 'www bundle step failed' >&2
            exit 1
        }
        bundle_pages
        # A selected file inlined by a page stales that page's bundle: upload
        # every bundle that is a selected page or inlines a selected file.
        for relative in "${selected_files[@]}"; do
            relative=${relative#./}
            needed+="$relative"$'\n'
            while read -r page asset; do
                [[ "$asset" != "$relative" ]] || needed+="$page"$'\n'
            done <<< "$pairs"
        done
    fi
    # The relative name is checked by upload_file's path rules once joined, so a
    # --file cannot reach outside --www.
    for relative in "${selected_files[@]}"; do
        upload_file "$www_dir/$relative" "$relative"
    done
    for page in index.html status.html upgrade.html classic.html; do
        if [[ "$needed" == *$'\n'"$page"$'\n'* ]]; then
            upload_file "$work_dir/bundles/$page.gz" "$page.gz"
        fi
    done
    exit 0
fi

bundle_pages
while IFS= read -r -d '' file; do
    upload_file "$file" "${file#"$www_dir"/}"
done < <(find "$www_dir" -type f -print0)
for bundle in "$work_dir"/bundles/*.html.gz; do
    upload_file "$bundle" "${bundle##*/}"
done
