#!/usr/bin/env bash
# Stops the duel server started from this checkout, rebuilds, and starts it again.
#   scripts/restart.sh                      # listens on 0.0.0.0:8080
#   scripts/restart.sh -addr 0.0.0.0:80     # extra flags go to the server
# Run scripts/setup.sh once first.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SERVER="$ROOT/build-server/tetris-server"

# Only servers running this checkout's binary, not other copies on the machine.
pids=()
for pid in $(pgrep -x tetris-server || true); do
  if [ "$(readlink "/proc/$pid/exe" 2>/dev/null)" = "$SERVER" ]; then
    pids+=("$pid")
  fi
done

if [ "${#pids[@]}" -gt 0 ]; then
  echo "==> Stopping the server (pid ${pids[*]})"
  kill "${pids[@]}" 2>/dev/null || true
  for _ in $(seq 50); do
    alive=0
    for pid in "${pids[@]}"; do kill -0 "$pid" 2>/dev/null && alive=1; done
    [ "$alive" -eq 0 ] && break
    sleep 0.1
  done
  if [ "$alive" -ne 0 ]; then
    kill -9 "${pids[@]}" 2>/dev/null || true
  fi
else
  echo "==> No server running"
fi

"$ROOT/scripts/build.sh"
exec "$ROOT/scripts/run.sh" "$@"
