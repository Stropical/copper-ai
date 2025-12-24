#!/usr/bin/env bash
# Create a schematic symbol via KiCad's IPC API.
#
# Usage:
#   ./place_symbol.sh --symbol Library:Entry --x 25.4 --y 12.7 [--reference R1] [--value 10k]

set -euo pipefail

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
source "${SCRIPT_DIR}/ipc_common.sh"

IPC_URL="${DEFAULT_SOCKET}"
CLIENT_NAME="schematic-place-symbol"
BOARD_FILE=""
REFERENCE=""
VALUE=""
UNIT=1
ROTATION=0
MIRROR_X=false
MIRROR_Y=false
SYMBOL=""
X_MM=""
Y_MM=""

usage() {
    cat <<EOF
Usage: $0 --symbol Lib:Name --x <mm> --y <mm> [options]

Options:
  --socket <url>       IPC URL (default ${DEFAULT_SOCKET})
  --client <name>      Client identifier
  --board <file>       Optional board filename for DocumentSpecifier
  --symbol <lib:sym>   Library nickname and entry (required)
  --x <mm>             X position in millimeters (required)
  --y <mm>             Y position in millimeters (required)
  --reference <ref>    Optional reference designator
  --value <text>       Optional value field text
  --unit <n>           Symbol unit number (default 1)
  --rotation <deg>     Rotation (0/90/180/270)
  --mirror-x           Mirror across X axis
  --mirror-y           Mirror across Y axis
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --socket) IPC_URL="$2"; shift 2 ;;
        --client) CLIENT_NAME="$2"; shift 2 ;;
        --board) BOARD_FILE="$2"; shift 2 ;;
        --symbol) SYMBOL="$2"; shift 2 ;;
        --x) X_MM="$2"; shift 2 ;;
        --y) Y_MM="$2"; shift 2 ;;
        --reference) REFERENCE="$2"; shift 2 ;;
        --value) VALUE="$2"; shift 2 ;;
        --unit) UNIT="$2"; shift 2 ;;
        --rotation) ROTATION="$2"; shift 2 ;;
        --mirror-x) MIRROR_X=true; shift 1 ;;
        --mirror-y) MIRROR_Y=true; shift 1 ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage
            exit 1
            ;;
    esac
done

if [[ -z "$SYMBOL" || -z "$X_MM" || -z "$Y_MM" ]]; then
    usage
    exit 1
fi

if [[ "$SYMBOL" != *:* ]]; then
    echo "Symbol must be in nickname:entry format" >&2
    exit 1
fi

LIB_NAME="${SYMBOL%%:*}"
ENTRY_NAME="${SYMBOL#*:}"
X_NM=$(mm_to_nm "$X_MM")
Y_NM=$(mm_to_nm "$Y_MM")

ensure_protoc
read -r -a TRANSPORT_CMD <<<"$(get_transport_cmd)"

tmp_proto=$(mktemp)
tmp_req=$(mktemp)
tmp_resp=$(mktemp)

cleanup() {
    rm -f "$tmp_proto" "$tmp_req" "$tmp_resp"
}

trap cleanup EXIT

{
    echo "header { client_name: \"${CLIENT_NAME}\" }"
    echo "message {"
    echo "  [type.googleapis.com/kiapi.schematic.commands.PlaceSymbol] {"
    echo "    document {"
    echo "      type: DOCTYPE_SCHEMATIC"
    if [[ -n "${BOARD_FILE}" ]]; then
        echo "      board_filename: \"${BOARD_FILE}\""
    fi
    echo "    }"
    echo "    lib_id {"
    echo "      library_nickname: \"${LIB_NAME}\""
    echo "      entry_name: \"${ENTRY_NAME}\""
    echo "    }"
    echo "    position {"
    echo "      x_nm: ${X_NM}"
    echo "      y_nm: ${Y_NM}"
    echo "    }"
    echo "    unit: ${UNIT}"
    echo "    rotation: ${ROTATION}"
    echo "    mirror_x: ${MIRROR_X}"
    echo "    mirror_y: ${MIRROR_Y}"
    if [[ -n "${REFERENCE}" ]]; then
        echo "    reference: \"${REFERENCE}\""
    fi
    if [[ -n "${VALUE}" ]]; then
        echo "    value: \"${VALUE}\""
    fi
    echo "  }"
    echo "}"
} >"$tmp_proto"

protoc -I"${PROTO_ROOT}" --encode=kiapi.common.ApiRequest \
    "${PROTO_ROOT}/common/envelope.proto" <"$tmp_proto" >"$tmp_req"

echo "Placing symbol via ${IPC_URL}..."
if "${TRANSPORT_CMD[@]}" "${IPC_URL}" --data @"${tmp_req}" >"$tmp_resp"; then
    protoc -I"${PROTO_ROOT}" --decode=kiapi.common.ApiResponse \
        "${PROTO_ROOT}/common/envelope.proto" <"$tmp_resp"
else
    echo "Symbol placement failed." >&2
    exit 1
fi
