#!/usr/bin/env bash
# Builds the web game into build-web/ and the duel server into build-server/.
#   scripts/build.sh
# Run scripts/setup.sh once first.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
EMSDK_DIR="${EMSDK_DIR:-$HOME/emsdk}"
export PATH="/usr/local/go/bin:$PATH"

# shellcheck disable=SC1091
source "$EMSDK_DIR/emsdk_env.sh" >/dev/null

echo "==> Building the web game"
emcmake cmake -S "$ROOT" -B "$ROOT/build-web" -DCMAKE_BUILD_TYPE=Release
cmake --build "$ROOT/build-web" -j"$(nproc)"

echo "==> Testing and building the server"
cd "$ROOT/server"
go test ./...
mkdir -p "$ROOT/build-server"
CGO_ENABLED=0 go build -o "$ROOT/build-server/tetris-server" .

echo "==> Done. Start it with scripts/run.sh"
