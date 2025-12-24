#!/usr/bin/env bash
# Query the active schematic via KiCad's IPC API.
#
# Usage: ./get_schematic_data.sh [--socket ipc:///tmp/kicad/api.sock] [--client test] [--board foo.kicad_sch]

set -euo pipefail

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
source "${SCRIPT_DIR}/ipc_common.sh"

IPC_URL="${DEFAULT_SOCKET}"
CLIENT_NAME="schematic-data-script"
BOARD_FILE=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --socket)
            IPC_URL="$2"
            shift 2
            ;;
        --client)
            CLIENT_NAME="$2"
            shift 2
            ;;
        --board)
            BOARD_FILE="$2"
            shift 2
            ;;
        *)
            echo "Unknown argument: $1" >&2
            exit 1
            ;;
    esac
done

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
    echo "  [type.googleapis.com/kiapi.schematic.commands.GetSchematicData] {"
    echo "    document {"
    echo "      type: DOCTYPE_SCHEMATIC"
    if [[ -n "${BOARD_FILE}" ]]; then
        echo "      board_filename: \"${BOARD_FILE}\""
    fi
    echo "    }"
    echo "  }"
    echo "}"
} > "$tmp_proto"

protoc -I"${PROTO_ROOT}" --encode=kiapi.common.ApiRequest \
    "${PROTO_ROOT}/common/envelope.proto" <"$tmp_proto" >"$tmp_req"

echo "Sending request over ${IPC_URL}..."
if "${TRANSPORT_CMD[@]}" "${IPC_URL}" --data @"${tmp_req}" >"$tmp_resp"; then
    echo "Response:"
    protoc -I"${PROTO_ROOT}" --decode=kiapi.common.ApiResponse \
        "${PROTO_ROOT}/common/envelope.proto" <"$tmp_resp"
else
    echo "IPC request failed." >&2
    exit 1
fi
