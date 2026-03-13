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

# Optional: verify `mmap` shell command prints a summary line.
# Enable with: TEST_MMAP=1 make test
TEST_MMAP=${TEST_MMAP:-0}

# Optional: verify `pmm` shell command works and allocation/free affects counters.
# Enable with: TEST_PMM=1 make test
TEST_PMM=${TEST_PMM:-0}

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
require_fixed "[init] gdt ok" "gdt init"
require_fixed "[init] idt ok" "idt init"

# Multiboot2 magic should be correct when launched from GRUB multiboot2
require_fixed "[mb2] magic=36d76289" "multiboot2 magic"

# MB2 tag dump markers (address varies)
require_regex "\\[mb2\\] info @ 0x[0-9a-fA-F]{8}, total_size=[0-9]+" "mb2 info header"
require_fixed "[mb2] end tag" "mb2 end tag"

if [[ "${TEST_MMAP}" == "1" ]]; then
  echo "[test] cmd: mmap (feed input via serial stdio)"
  LOG_MMAP_RAW="${LOG_DIR}/serial-mmap-${TS}.raw.log"
  LOG_MMAP="${LOG_DIR}/serial-mmap-${TS}.log"

  # Feed `mmap` into the shell via QEMU stdio.
  # Note: QEMU's stdio can be line-buffered; keep the timeout a bit longer.
  (printf 'mmap\n' | timeout "${QEMU_TIMEOUT_SEC}s" "${QEMU_BIN}" \
    -cdrom "${ISO_IMAGE}" \
    -serial stdio \
    -display none \
    -no-reboot \
    -no-shutdown \
    >"${LOG_MMAP_RAW}" 2>&1) || true

  tr -d '\r' <"${LOG_MMAP_RAW}" >"${LOG_MMAP}" || true

  if ! grep -Eq -- "Available RAM: 0x[0-9a-fA-F]+ - 0x[0-9a-fA-F]+ \([0-9]+MB\)" "${LOG_MMAP}"; then
    echo "[FAIL] mmap output: missing 'Available RAM' line"
    echo "[test] mmap log (last 80 lines):"
    tail -n 80 "${LOG_MMAP}" || true
    exit 1
  else
    echo "[ OK ] mmap output"
  fi
fi

if [[ "${TEST_PMM}" == "1" ]]; then
  echo "[test] cmd: pmm (feed scripted input via serial stdio)"
  LOG_PMM_RAW="${LOG_DIR}/serial-pmm-${TS}.raw.log"
  LOG_PMM="${LOG_DIR}/serial-pmm-${TS}.log"

  # Shell is interactive; give it a bit more time than the default smoke run.
  QEMU_TIMEOUT_PMM_SEC=${QEMU_TIMEOUT_PMM_SEC:-8}

  # Script:
  #  1) print initial state
  #  2) alloc 3 pages
  #  3) print state (free should drop by 3)
  #  4) freeall
  #  5) print state (free should return)
  # NOTE: Piping input immediately at QEMU start is racy; early bytes can be
  # consumed before the shell read loop is ready. Add a short delay and space
  # commands out a bit so the interactive console can process them.
  ({
    sleep 1
    printf 'pmm state\n'
    sleep 0.1
    printf 'pmm alloc 3\n'
    sleep 0.1
    printf 'pmm state\n'
    sleep 0.1
    printf 'pmm freeall\n'
    sleep 0.1
    printf 'pmm state\n'
  } ) | (timeout "${QEMU_TIMEOUT_PMM_SEC}s" "${QEMU_BIN}" \
    -cdrom "${ISO_IMAGE}" \
    -serial stdio \
    -display none \
    -no-reboot \
    -no-shutdown \
    >"${LOG_PMM_RAW}" 2>&1) || true

  tr -d '\r' <"${LOG_PMM_RAW}" >"${LOG_PMM}" || true

  # Basic command presence checks.
  if ! grep -Fq -- "PMM: base=" "${LOG_PMM}"; then
    echo "[FAIL] pmm output: missing 'PMM: base=' line"
    echo "[test] pmm log (last 120 lines):"
    tail -n 120 "${LOG_PMM}" || true
    exit 1
  fi

  # Ensure alloc printed three addresses.
  alloc_lines=$(grep -c -F -- "pmm alloc:" "${LOG_PMM}" || true)
  if [[ "${alloc_lines}" -lt 3 ]]; then
    echo "[FAIL] pmm alloc: expected >=3 lines, got ${alloc_lines}"
    echo "[test] pmm log (last 120 lines):"
    tail -n 120 "${LOG_PMM}" || true
    exit 1
  fi

  if ! grep -Fq -- "pmm freeall: freed 3 pages" "${LOG_PMM}"; then
    echo "[FAIL] pmm freeall: missing 'freed 3 pages' confirmation"
    echo "[test] pmm log (last 120 lines):"
    tail -n 120 "${LOG_PMM}" || true
    exit 1
  fi

  # Extract the free page counts from the three `pmm state` outputs.
  mapfile -t frees < <(awk '/^PMM: free [0-9]+ \/ [0-9]+ pages/ {print $3}' "${LOG_PMM}")
  if [[ "${#frees[@]}" -lt 3 ]]; then
    echo "[FAIL] pmm state: expected >=3 'PMM: free' lines, got ${#frees[@]}"
    echo "[test] pmm log (last 160 lines):"
    tail -n 160 "${LOG_PMM}" || true
    exit 1
  fi

  free0=${frees[0]}
  free1=${frees[1]}
  free2=${frees[2]}

  if [[ $((free0 - free1)) -ne 3 ]]; then
    echo "[FAIL] pmm alloc: expected free to drop by 3 (before=${free0}, after=${free1})"
    echo "[test] pmm log (last 160 lines):"
    tail -n 160 "${LOG_PMM}" || true
    exit 1
  fi

  if [[ "${free2}" -ne "${free0}" ]]; then
    echo "[FAIL] pmm freeall: expected free to return (before=${free0}, after_freeall=${free2})"
    echo "[test] pmm log (last 160 lines):"
    tail -n 160 "${LOG_PMM}" || true
    exit 1
  fi

  echo "[ OK ] pmm command"
fi

# Optional: verify `heap test` (vmm_alloc_pages / vmm_free_pages selftest).
# Enable with: TEST_HEAP=1 make test
TEST_HEAP=${TEST_HEAP:-0}

if [[ "${TEST_HEAP}" == "1" ]]; then
  echo "[test] cmd: heap test (vmm_alloc_pages selftest)"
  LOG_HEAP_RAW="${LOG_DIR}/serial-heap-${TS}.raw.log"
  LOG_HEAP="${LOG_DIR}/serial-heap-${TS}.log"

  QEMU_TIMEOUT_HEAP_SEC=${QEMU_TIMEOUT_HEAP_SEC:-8}

  ({
    sleep 1
    printf 'heap test\n'
  } ) | (timeout "${QEMU_TIMEOUT_HEAP_SEC}s" "${QEMU_BIN}" \
    -cdrom "${ISO_IMAGE}" \
    -serial stdio \
    -display none \
    -no-reboot \
    -no-shutdown \
    >"${LOG_HEAP_RAW}" 2>&1) || true

  tr -d '\r' <"${LOG_HEAP_RAW}" >"${LOG_HEAP}" || true

  if ! grep -Fq -- "[heap-test] === ALL PASS ===" "${LOG_HEAP}"; then
    echo "[FAIL] heap test: missing 'ALL PASS' marker"
    echo "[test] heap log (last 80 lines):"
    tail -n 80 "${LOG_HEAP}" || true
    exit 1
  fi

  echo "[ OK ] heap test (vmm_alloc_pages selftest)"
fi

if [[ "${fail}" -ne 0 ]]; then
  echo "[test] RESULT: FAIL"
  echo "[test] Tip: open the log file to see what printed."
  exit 1
fi

echo "[test] RESULT: OK"
