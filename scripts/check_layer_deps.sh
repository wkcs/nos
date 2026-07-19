#!/usr/bin/env bash
# scripts/check_layer_deps.sh — gate: portable layers must not pull board headers
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
fail=0

# Prefer ripgrep; fall back to grep -E for hosts without rg in PATH.
search() {
    local pattern="$1"
    shift
    if command -v rg >/dev/null 2>&1; then
        rg -n "$pattern" "$@"
    else
        local path="$1"
        shift
        # Consume --glob args and translate to grep --include
        local includes=()
        while [[ $# -gt 0 ]]; do
            if [[ "$1" == "--glob" ]]; then
                includes+=(--include "$2")
                shift 2
            else
                shift
            fi
        done
        grep -REn "$pattern" "${includes[@]}" "$path" || return 1
        return 0
    fi
}

if search '#include\s*[<"]board/board\.h[>"]|#include\s*[<"]stm32f1|#include\s*[<"]stm32f4' \
  "$ROOT/kernel" --glob '*.c' --glob '*.h' ; then
  echo "ERROR: kernel/ must not include board/STM32 headers"
  fail=1
fi

if search '#include\s*[<"]board/board\.h[>"]' \
  "$ROOT/arch/arm/cpu" --glob '*.c' --glob '*.h' ; then
  echo "ERROR: arch CPU code must not include board/board.h"
  fail=1
fi

# Golden-board layer rule: portable subsystems may use soc/*, never board/board.h.
for dir in drivers/serial drivers/block kernel fs; do
  if [[ ! -d "$ROOT/$dir" ]]; then
    continue
  fi
  if search '#include\s*[<"]board/board\.h[>"]' \
    "$ROOT/$dir" --glob '*.c' --glob '*.h' ; then
    echo "ERROR: $dir/ must not include board/board.h (use soc/ APIs instead)"
    fail=1
  fi
done

exit "$fail"
