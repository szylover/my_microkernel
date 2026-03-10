#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "${ROOT_DIR}"

if ! command -v bear >/dev/null 2>&1; then
  echo "Missing tool: bear"
  echo "Install (Ubuntu/WSL): sudo apt update && sudo apt install -y bear"
  exit 2
fi

echo "[compdb] generating compile_commands.json via bear"
# Build the kernel ELF; this captures all compile commands.
# Use $(MAKE) so it works even if users override CC/NASM.
bear -- make -B build/isodir/boot/kernel.elf

echo "[compdb] done: ${ROOT_DIR}/compile_commands.json"
