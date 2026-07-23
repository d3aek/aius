#!/usr/bin/env bash
set -euo pipefail

usage() {
	printf '%s\n' "Usage: $0 --device URL --token TOKEN --id ID FILE.iec"
}

device=''
token="${AIUS_SD_AUTH:-}"
id=''

while (($# > 0)); do
	case "$1" in
		--device) device=${2%/}; shift 2 ;;
		--token) token=$2; shift 2 ;;
		--id) id=$2; shift 2 ;;
		-h|--help) usage; exit 0 ;;
		*) file=$1; shift; (($# == 0)) || { usage >&2; exit 2; } ;;
	esac
done

[[ -n "$device" && -n "$token" && -n "$id" && -n "${file:-}" ]] || {
	usage >&2
	exit 2
}
[[ ${#token} -eq 16 ]] || { printf '%s\n' 'token must contain 16 hex characters' >&2; exit 2; }
[[ -f "$file" ]] || { printf 'file not found: %s\n' "$file" >&2; exit 1; }

size=$(wc -c < "$file")
offset=0
if listing=$(curl --fail --silent --show-error --max-time 5 "$device/api/replays"); then
	prior=$(printf '%s' "$listing" | sed -n "s/.*\"id\":\"$id\"[^}]*\"upload_next_offset\":\([0-9]*\).*/\1/p")
	[[ -n "$prior" ]] && offset=$prior
fi
((offset <= size)) || { printf 'device offset %s exceeds file size %s\n' "$offset" "$size" >&2; exit 1; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
while ((offset < size)); do
	remaining=$((size - offset))
	count=$((remaining < 4096 ? remaining : 4096))
	dd if="$file" of="$tmp/chunk" bs=1 skip="$offset" count="$count" status=none
	end=$((offset + count - 1))
	response=$(curl --fail-with-body --silent --show-error --max-time 15 \
		-X PUT "$device/sd/replays/$id" \
		-H "X-Aius-SD-Auth: $token" \
		-H "Content-Range: bytes $offset-$end/$size" \
		-H 'Content-Type: application/octet-stream' \
		--data-binary "@$tmp/chunk")
	next=$(printf '%s' "$response" | sed -n 's/.*"next_offset":\([0-9]*\).*/\1/p')
	if [[ -z "$next" ]]; then
		next=$size
	fi
	((next > offset)) || { printf 'upload made no progress: %s\n' "$response" >&2; exit 1; }
	offset=$next
	printf '\r%s: %s/%s bytes' "$id" "$offset" "$size" >&2
done
printf '\n' >&2
