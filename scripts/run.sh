#!/usr/bin/env bash
# Runs the duel server, serving the web build from build-web/.
#   scripts/run.sh                      # listens on 0.0.0.0:8080
#   scripts/run.sh -addr 0.0.0.0:80     # extra flags go to the server
# Run scripts/build.sh first.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SERVER="$ROOT/build-server/tetris-server"

if [ ! -x "$SERVER" ] || [ ! -f "$ROOT/build-web/index.html" ]; then
  echo "Nothing built yet; run scripts/build.sh first." >&2
  exit 1
fi

exec "$SERVER" -dir "$ROOT/build-web" "$@"
