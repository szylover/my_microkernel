#!/usr/bin/env bash
set -euo pipefail

# Run QEMU with gdbstub (-S) and print a READY marker once the TCP port is open.
# This is used by VS Code background tasks to know when it can attach.

QEMU_BIN="${QEMU:-qemu-system-i386}"
PORT="${QEMU_GDB_PORT:-1234}"
MEM_MB="${QEMU_MEM_MB:-256}"
ISO="build/kernel.iso"

if [[ ! -f "$ISO" ]]; then
  echo "[qemu] missing $ISO; build it first (e.g. make DEBUG=1 iso)" >&2
  exit 2
fi

"$QEMU_BIN" -m "$MEM_MB" -cdrom "$ISO" -serial stdio -no-reboot -S -gdb "tcp::${PORT}" &
qemu_pid=$!

# Wait (up to ~10s) for the gdbstub port to accept connections.
ready=0
for _ in {1..200}; do
  if (echo > "/dev/tcp/127.0.0.1/${PORT}") >/dev/null 2>&1; then
    ready=1
    break
  fi
  sleep 0.05
done

if [[ "$ready" -ne 1 ]]; then
  echo "[qemu] GDB port ${PORT} not ready (timeout)" >&2
  kill "$qemu_pid" >/dev/null 2>&1 || true
  wait "$qemu_pid" >/dev/null 2>&1 || true
  exit 3
fi

echo "GDBSTUB READY (tcp::${PORT})"

# Keep task alive as long as QEMU runs.
wait "$qemu_pid"
