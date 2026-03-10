#!/usr/bin/env bash
set -euo pipefail

# Simple kernel smoke-test via QEMU serial output.
#
# What it does:
#  1) make clean
#  2) make iso
#  3) run QEMU for a short time and capture `-serial stdio` output
#  4) assert expected boot markers exist in the log
#
# This is intentionally lightweight: for early kernels, the only stable
# integration surface is serial output.

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
LOG_DIR="${ROOT_DIR}/build/test-logs"

TS=$(date +%Y%m%d-%H%M%S)
LOG_RAW="${LOG_DIR}/qemu-${TS}.raw.log"
LOG_SERIAL_RAW="${LOG_DIR}/serial-${TS}.raw.log"
LOG="${LOG_DIR}/serial-${TS}.log"

# How long to let QEMU run (seconds). Kernel typically hlt-loops forever.
QEMU_TIMEOUT_SEC=${QEMU_TIMEOUT_SEC:-5}

cd "${ROOT_DIR}"

echo "[test] build: clean"
make clean

# `make clean` removes build/, so create the log directory after cleaning.
mkdir -p "${LOG_DIR}"

echo "[test] build: iso"
make iso

echo "[test] run: qemu (timeout=${QEMU_TIMEOUT_SEC}s)"
# Be defensive in case something removed build/ again.
mkdir -p "${LOG_DIR}"

QEMU_BIN=${QEMU_BIN:-${QEMU:-qemu-system-i386}}
ISO_IMAGE="${ROOT_DIR}/build/kernel.iso"

if ! command -v "${QEMU_BIN}" >/dev/null 2>&1; then
  echo "[test] ERROR: qemu not found: ${QEMU_BIN}"
  exit 2
fi

if [[ ! -f "${ISO_IMAGE}" ]]; then
  echo "[test] ERROR: ISO missing: ${ISO_IMAGE}"
  exit 2
fi

# Run QEMU headless and write serial directly to a file.
# This avoids stdio buffering issues when QEMU is terminated by timeout.
(timeout "${QEMU_TIMEOUT_SEC}s" "${QEMU_BIN}" \
  -cdrom "${ISO_IMAGE}" \
  -serial "file:${LOG_SERIAL_RAW}" \
  -display none \
  -no-reboot \
  -no-shutdown \
  >"${LOG_RAW}" 2>&1) || true

# Normalize CRLF and other artifacts so greps are stable.
if [[ -f "${LOG_SERIAL_RAW}" ]]; then
  tr -d '\r' <"${LOG_SERIAL_RAW}" >"${LOG}"
else
  : >"${LOG}"
fi

echo "[test] log: ${LOG}"

if [[ ! -s "${LOG}" ]]; then
  echo "[test] WARN: serial log is empty"
  if [[ -s "${LOG_RAW}" ]]; then
    echo "[test] qemu output (last 50 lines):"
    tail -n 50 "${LOG_RAW}" || true
  fi
fi

fail=0

require_fixed() {
  local needle="$1"
  local label="$2"
  if ! grep -Fq -- "${needle}" "${LOG}"; then
    echo "[FAIL] missing: ${label} (fixed: ${needle})"
    fail=1
  else
    echo "[ OK ] ${label}"
  fi
}

require_regex() {
  local re="$1"
  local label="$2"
  if ! grep -Eq -- "${re}" "${LOG}"; then
    echo "[FAIL] missing: ${label} (regex: ${re})"
    fail=1
  else
    echo "[ OK ] ${label}"
  fi
}

# --- Smoke checks ---
# Basic boot markers
require_fixed "kmain: hello from C" "kmain banner"
require_fixed "gdt: after init" "gdt init"
require_fixed "idt: after init" "idt init"

# Multiboot2 magic should be correct when launched from GRUB multiboot2
require_fixed "mb2_magic=36d76289" "multiboot2 magic"

# MB2 tag dump markers (address varies)
require_regex "\\[mb2\\] info @ 0x[0-9a-fA-F]{8}, total_size=[0-9]+" "mb2 info header"
require_fixed "[mb2] end tag" "mb2 end tag"

if [[ "${fail}" -ne 0 ]]; then
  echo "[test] RESULT: FAIL"
  echo "[test] Tip: open the log file to see what printed."
  exit 1
fi

echo "[test] RESULT: OK"
