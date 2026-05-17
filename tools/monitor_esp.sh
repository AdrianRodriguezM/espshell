#!/usr/bin/env bash
set -euo pipefail

# ESP serial monitor with sane defaults for this project.
# Usage:
#   tools/monitor_esp.sh                 # auto-detect port, 115200
#   tools/monitor_esp.sh /dev/ttyUSB0    # explicit port
#   tools/monitor_esp.sh /dev/ttyUSB0 9600

PORT="${1:-}"
BAUD="${2:-115200}"

pick_port() {
  local p
  for p in /dev/ttyUSB0 /dev/ttyACM0 /dev/ttyUSB1 /dev/ttyACM1; do
    if [[ -c "$p" ]]; then
      echo "$p"
      return 0
    fi
  done
  return 1
}

if [[ -z "$PORT" ]]; then
  if ! PORT="$(pick_port)"; then
    echo "No se encontró puerto serie (probado: /dev/ttyUSB0,/dev/ttyACM0,/dev/ttyUSB1,/dev/ttyACM1)." >&2
    exit 1
  fi
fi

if [[ ! -c "$PORT" ]]; then
  echo "Puerto inválido: $PORT" >&2
  exit 1
fi

if [[ ! "$BAUD" =~ ^[0-9]+$ ]]; then
  echo "Baud inválido: $BAUD" >&2
  exit 1
fi

echo "Abriendo monitor en $PORT @ ${BAUD} (8N1, flow-control OFF). Ctrl+C para salir."

stty -F "$PORT" "$BAUD" cs8 -cstopb -parenb -ixon -ixoff -crtscts -echo -icrnl
exec cat "$PORT"
