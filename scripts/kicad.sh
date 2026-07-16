#!/usr/bin/env bash
# Run the digest-pinned KiCad CLI from any current working directory.
# Usage: scripts/kicad.sh version
# Usage: scripts/kicad.sh sch erc electronics/reference/aius-reference.kicad_sch
# Environment: AIUS_DOCKER_BIN=docker (test seam for the Docker executable).
# Exit status is the Docker Compose/KiCad CLI exit status.
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
aius_dir="$(cd -- "$script_dir/.." && pwd)"
repo_root="$(cd -- "$aius_dir/../../.." && pwd)"
compose_path="$aius_dir/compose.yaml"
docker_bin="${AIUS_DOCKER_BIN:-docker}"

export AIUS_REPO_ROOT="$repo_root"
exec "$docker_bin" compose --project-name aius-kicad -f "$compose_path" run --rm kicad kicad-cli "$@"
