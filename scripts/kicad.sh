#!/usr/bin/env bash
# Run the digest-pinned KiCad CLI from any current working directory.
# Usage: scripts/kicad.sh version
# Usage: scripts/kicad.sh sch erc electronics/aius-carrier.kicad_sch
# Usage: scripts/kicad.sh --python electronics/scripts/scaffold_pcb.py
# Environment: AIUS_DOCKER_BIN=docker (test seam for the Docker executable).
# Exit status is the Docker Compose/KiCad CLI exit status.
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
aius_dir="$(cd -- "$script_dir/.." && pwd)"
compose_path="$aius_dir/compose.yaml"
docker_bin="${AIUS_DOCKER_BIN:-docker}"

export AIUS_PROJECT_ROOT="$aius_dir"
AIUS_UID="$(id -u)"
AIUS_GID="$(id -g)"
export AIUS_UID AIUS_GID
if [[ "${1:-}" == "--python" ]]; then
  shift
  set -- python3 "$@"
else
  set -- kicad-cli "$@"
fi
exec "$docker_bin" compose --project-name aius-kicad -f "$compose_path" run --rm kicad "$@"
