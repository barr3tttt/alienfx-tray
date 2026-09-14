#!/usr/bin/env bash
# Runs every end-to-end script. Each one skips itself when the hardware or the graphical
# session it needs is absent, so this is safe to run anywhere - it just does less.
#
#   tests/e2e/run-all.sh
#   ALIENFX_TRAY_BIN=/usr/bin/alienfx-tray tests/e2e/run-all.sh

set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

failed=0
for script in ./*.sh; do
  [[ "$script" == "./run-all.sh" || "$script" == "./lib.sh" ]] && continue
  "$script" || failed=$((failed + 1))
done

if [[ "$failed" -ne 0 ]]; then
  printf '\n%d end-to-end script(s) failed\n' "$failed"
  exit 1
fi
printf '\nall end-to-end scripts passed\n'
