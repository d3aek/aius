#!/usr/bin/env bash
# Verify the KiCad wrapper argv and rendered Compose isolation contract.
# Usage: scripts/test_kicad_wrapper.sh
# Usage: CLAUDE_SESSION_SCRATCH=.scratch/manual scripts/test_kicad_wrapper.sh
# Environment: CLAUDE_SESSION_SCRATCH=.scratch/scripts (temporary test files).
# Exit status is 0 when every assertion passes, 2 for contract drift.
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
aius_dir="$(cd -- "$script_dir/.." && pwd)"
repo_root="$(cd -- "$aius_dir/../../.." && pwd)"
wrapper="$script_dir/kicad.sh"
compose_path="$aius_dir/compose.yaml"
scratch_root="${CLAUDE_SESSION_SCRATCH:-$repo_root/.scratch/scripts}"
test_dir=""

fail() {
  echo "KiCad wrapper test failed: $*" >&2
  exit 2
}

cleanup() {
  if [[ -n "$test_dir" && "$test_dir" == "$scratch_root"/aius-kicad-wrapper.* ]]; then
    rm -rf -- "$test_dir"
  fi
}

mkdir -p -- "$scratch_root"
test_dir="$(mktemp -d "$scratch_root/aius-kicad-wrapper.XXXXXX")"
trap cleanup EXIT

fake_docker="$test_dir/docker"
cat > "$fake_docker" <<'FAKE_DOCKER'
#!/usr/bin/env bash
set -euo pipefail

: "${AIUS_DOCKER_ARGV_FILE:?AIUS_DOCKER_ARGV_FILE is required}"
: "${AIUS_DOCKER_REPO_ROOT_FILE:?AIUS_DOCKER_REPO_ROOT_FILE is required}"
[[ "${COMPOSE_PROJECT_NAME:-}" == "hostile-project" ]] || exit 64
printf '%s\0' "$@" > "$AIUS_DOCKER_ARGV_FILE"
printf '%s' "${AIUS_REPO_ROOT:-}" > "$AIUS_DOCKER_REPO_ROOT_FILE"
FAKE_DOCKER
chmod +x "$fake_docker"

run_wrapper_case() {
  local case_name="$1"
  local cwd="$2"
  shift 2
  local argv_file="$test_dir/$case_name.argv"
  local expected_file="$test_dir/$case_name.expected"
  local repo_root_file="$test_dir/$case_name.repo-root"

  (
    cd -- "$cwd"
    COMPOSE_PROJECT_NAME=hostile-project \
      AIUS_DOCKER_BIN="$fake_docker" \
      AIUS_DOCKER_ARGV_FILE="$argv_file" \
      AIUS_DOCKER_REPO_ROOT_FILE="$repo_root_file" \
      "$wrapper" "$@"
  )

  printf '%s\0' \
    compose \
    --project-name aius-kicad \
    -f "$compose_path" \
    run --rm \
    kicad kicad-cli \
    "$@" > "$expected_file"

  cmp -s -- "$expected_file" "$argv_file" || fail "$case_name produced unexpected Docker argv"
  [[ "$(<"$repo_root_file")" == "$repo_root" ]] || fail "$case_name exported an unexpected repository bind path"
}

forwarded_args=(
  sch
  erc
  "electronics/reference/board with spaces.kicad_sch"
  --severity-all
  ""
  '*literal*'
)
run_wrapper_case repo-root "$repo_root" "${forwarded_args[@]}"
run_wrapper_case unrelated-cwd / "${forwarded_args[@]}"

config_json="$test_dir/compose-config.json"
expected_image="kicad/kicad:10.0.4@sha256:770e2fa006695077b5ba218742d5c3137896af91257b2ca971243fa56b01bb86"
AIUS_REPO_ROOT="$repo_root" docker compose \
  --project-name aius-kicad \
  -f "$compose_path" \
  config --format json > "$config_json"

if ! jq -e --arg image "$expected_image" --arg repo_root "$repo_root" '
  .name == "aius-kicad"
  and .services.kicad.image == $image
  and .services.kicad.network_mode == "none"
  and .services.kicad.user == "1000:1000"
  and .services.kicad.working_dir == "/workspace/src/hardware/aius"
  and ([.services.kicad.volumes[]
    | select(.type == "bind" and .source == $repo_root and .target == "/workspace")]
    | length == 1)
  and ([.services.kicad.volumes[]
    | select(.type == "volume" and .source == "aius-kicad-cache" and .target == "/home/kicad")]
    | length == 1)
  and .volumes["aius-kicad-cache"].name == "aius-kicad-cache"
' "$config_json" > /dev/null; then
  fail "rendered Compose configuration violated the pinned KiCad contract"
fi

echo "KiCad wrapper tests passed."
