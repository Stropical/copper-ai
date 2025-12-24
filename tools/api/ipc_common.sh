#!/usr/bin/env bash
# Shared helpers for KiCad IPC API test scripts.

set -euo pipefail

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
REPO_ROOT="$( cd "${SCRIPT_DIR}/../.." && pwd )"
PROTO_ROOT="${REPO_ROOT}/api/proto"
DEFAULT_SOCKET="ipc:///tmp/kicad/api.sock"

require_command()
{
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "Missing required command: $1" >&2
        exit 1
    fi
}

ensure_protoc()
{
    require_command protoc
}

get_transport_cmd()
{
    if command -v nngcat >/dev/null 2>&1; then
        echo "nngcat --req --dial"
    elif command -v nanocat >/dev/null 2>&1; then
        echo "nanocat --req --connect"
    else
        echo "Need either 'nngcat' or 'nanocat' available in PATH." >&2
        exit 1
    fi
}

mm_to_nm()
{
    python3 - <<'PY' "$@"
import sys
val = float(sys.argv[1])
print(int(round(val * 1_000_000)))
PY
}
