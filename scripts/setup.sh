#!/usr/bin/env bash
# Installs what the web build and the duel server need on a Debian VPS:
# build tools, Emscripten (emsdk) and Go. Same versions as the Dockerfile.
#   scripts/setup.sh
# Override with EMSDK_VERSION, GO_VERSION or EMSDK_DIR. Safe to re-run.
set -euo pipefail

EMSDK_VERSION="${EMSDK_VERSION:-6.0.12}"
GO_VERSION="${GO_VERSION:-1.27.2}"
EMSDK_DIR="${EMSDK_DIR:-$HOME/emsdk}"

SUDO=""
if [ "$(id -u)" -ne 0 ]; then SUDO="sudo"; fi

echo "==> Installing apt packages"
$SUDO apt-get update
$SUDO apt-get install -y --no-install-recommends \
  git cmake build-essential python3 xz-utils curl ca-certificates

echo "==> Installing emsdk ${EMSDK_VERSION} into ${EMSDK_DIR}"
if [ ! -d "$EMSDK_DIR" ]; then
  git clone --depth 1 https://github.com/emscripten-core/emsdk.git "$EMSDK_DIR"
else
  git -C "$EMSDK_DIR" pull --ff-only
fi
# The emsdk download server drops connections now and then, so retry the install.
tries=10
for i in $(seq "$tries"); do
  "$EMSDK_DIR/emsdk" install "$EMSDK_VERSION" && break
  if [ "$i" -eq "$tries" ]; then
    echo "emsdk install failed after $tries tries; re-run scripts/setup.sh to try again" >&2
    exit 1
  fi
  echo "emsdk install failed (try $i/$tries), retrying in $((i * 15))s" >&2
  sleep $((i * 15))
done
"$EMSDK_DIR/emsdk" activate "$EMSDK_VERSION"

echo "==> Installing Go ${GO_VERSION} into /usr/local/go"
if [ "$(/usr/local/go/bin/go env GOVERSION 2>/dev/null || true)" != "go${GO_VERSION}" ]; then
  arch="$(dpkg --print-architecture)"  # amd64, arm64, ...
  curl -fsSL "https://go.dev/dl/go${GO_VERSION}.linux-${arch}.tar.gz" -o /tmp/go.tar.gz
  $SUDO rm -rf /usr/local/go
  $SUDO tar -C /usr/local -xzf /tmp/go.tar.gz
  rm -f /tmp/go.tar.gz
fi
/usr/local/go/bin/go version

echo "==> Done. Next: scripts/build.sh, then scripts/run.sh"
