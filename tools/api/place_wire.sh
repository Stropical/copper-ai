#!/usr/bin/env bash
# Draw schematic wires/buses through KiCad's IPC API.
#
# Usage:
#   ./place_wire.sh --points "0,0 25.4,0 25.4,12.7" [--layer wire|bus|graphic]

set -euo pipefail

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
source "${SCRIPT_DIR}/ipc_common.sh"

IPC_URL="${DEFAULT_SOCKET}"
CLIENT_NAME="schematic-place-wire"
BOARD_FILE=""
LAYER="wire"
POINTS_STR=""

usage() {
    cat <<EOF
Usage: $0 --points "x1,y1 x2,y2 ..." [options]

Options:
  --socket <url>      IPC URL (default ${DEFAULT_SOCKET})
  --client <name>     Client identifier
  --board <file>      Optional board filename override
  --points "<list>"   Space-separated list of mm coordinates (required)
  --layer <wire|bus|graphic>   Net layer (default wire)
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --socket) IPC_URL="$2"; shift 2 ;;
        --client) CLIENT_NAME="$2"; shift 2 ;;
        --board) BOARD_FILE="$2"; shift 2 ;;
        --points) POINTS_STR="$2"; shift 2 ;;
        --layer) LAYER="$2"; shift 2 ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            usage
            exit 1
            ;;
    esac
done

if [[ -z "${POINTS_STR}" ]]; then
    usage
    exit 1
fi

case "${LAYER,,}" in
    wire) LAYER_ENUM="SL_WIRE" ;;
    bus) LAYER_ENUM="SL_BUS" ;;
    graphic) LAYER_ENUM="SL_GRAPHIC" ;;
    *)
        echo "Unknown layer '${LAYER}' (choose wire|bus|graphic)" >&2
        exit 1
        ;;
esac

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
    echo "  [type.googleapis.com/kiapi.schematic.commands.PlaceWire] {"
    echo "    document {"
    echo "      type: DOCTYPE_SCHEMATIC"
    if [[ -n "${BOARD_FILE}" ]]; then
        echo "      board_filename: \"${BOARD_FILE}\""
    fi
    echo "    }"
    echo "    layer: ${LAYER_ENUM}"

    for pair in ${POINTS_STR}; do
        if [[ "$pair" != *,* ]]; then
            echo "Invalid point '${pair}', expected format x,y" >&2
            exit 1
        fi

        x_mm="${pair%%,*}"
        y_mm="${pair#*,}"
        x_nm=$(mm_to_nm "$x_mm")
        y_nm=$(mm_to_nm "$y_mm")

        echo "    points { x_nm: ${x_nm} y_nm: ${y_nm} }"
    done

    echo "  }"
    echo "}"
} >"$tmp_proto"

protoc -I"${PROTO_ROOT}" --encode=kiapi.common.ApiRequest \
    "${PROTO_ROOT}/common/envelope.proto" <"$tmp_proto" >"$tmp_req"

echo "Submitting wire request..."
if "${TRANSPORT_CMD[@]}" "${IPC_URL}" --data @"${tmp_req}" >"$tmp_resp"; then
    protoc -I"${PROTO_ROOT}" --decode=kiapi.common.ApiResponse \
        "${PROTO_ROOT}/common/envelope.proto" <"$tmp_resp"
else
    echo "Wire placement failed." >&2
    exit 1
fi
