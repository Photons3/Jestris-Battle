#!/usr/bin/env bash
# Installs the duel server as a systemd service that starts at boot and restarts if it crashes.
#   sudo scripts/install-service.sh                          # serves on 0.0.0.0:80
#   sudo scripts/install-service.sh -authority enforce       # extra flags go to the server
#   sudo scripts/install-service.sh -addr 0.0.0.0:8080       # a later -addr wins over the default
#   scripts/install-service.sh --print [flags]               # show the unit file, change nothing
#   sudo scripts/install-service.sh --uninstall              # stop, disable and remove the service
# Run scripts/build.sh first. Re-run to change the flags; logs: journalctl -u jestris -f
# Override the service name with SERVICE_NAME and the account it runs as with SERVICE_USER
# (default: the owner of this checkout).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SERVER="$ROOT/build-server/tetris-server"
NAME="${SERVICE_NAME:-jestris}"
UNIT="/etc/systemd/system/$NAME.service"
RUN_AS="${SERVICE_USER:-$(stat -c %U "$ROOT")}"

mode=install
case "${1:-}" in
  --print) mode=print; shift ;;
  --uninstall) mode=uninstall; shift ;;
esac

# systemd splits ExecStart on whitespace and treats quotes and % specially, so keep flags plain.
for arg in "$@"; do
  if [[ "$arg" =~ [[:space:]\"\'\\%\$] ]]; then
    echo "Flag '$arg' contains a space, quote, backslash, % or \$; pass plain values only." >&2
    exit 1
  fi
done

unit() {
  # Port 80 is privileged: a non-root account gets just the capability to bind it.
  local caps=""
  if [ "$RUN_AS" != root ]; then
    caps=$'AmbientCapabilities=CAP_NET_BIND_SERVICE\nCapabilityBoundingSet=CAP_NET_BIND_SERVICE\n'
  fi
  cat <<EOF
[Unit]
Description=Jestris Battle duel server
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=$RUN_AS
WorkingDirectory=$ROOT
ExecStart=$SERVER -dir $ROOT/build-web -addr 0.0.0.0:80${*:+ $*}
Restart=on-failure
RestartSec=2
${caps}NoNewPrivileges=true

[Install]
WantedBy=multi-user.target
EOF
}

if [ "$mode" = print ]; then
  unit "$@"
  exit 0
fi

if [ "$(id -u)" -ne 0 ]; then
  echo "Run it with sudo: it writes $UNIT and manages the service." >&2
  exit 1
fi
if ! command -v systemctl >/dev/null || [ ! -d /run/systemd/system ]; then
  echo "systemd isn't running on this machine." >&2
  exit 1
fi

if [ "$mode" = uninstall ]; then
  systemctl disable --now "$NAME" 2>/dev/null || true
  rm -f "$UNIT"
  systemctl daemon-reload
  echo "==> Removed the $NAME service"
  exit 0
fi

if [ ! -x "$SERVER" ] || [ ! -f "$ROOT/build-web/index.html" ]; then
  echo "Nothing built yet; run scripts/build.sh first." >&2
  exit 1
fi
if [ "$RUN_AS" != root ] && ! sudo -u "$RUN_AS" test -x "$SERVER"; then
  echo "$RUN_AS can't run $SERVER (a checkout under /root, for example); set SERVICE_USER." >&2
  exit 1
fi

# A server started by hand with run.sh would hold the port; stop it (this checkout's binary only).
for pid in $(pgrep -x tetris-server || true); do
  if [ "$(readlink "/proc/$pid/exe" 2>/dev/null)" = "$SERVER" ] &&
     [ "$(systemctl show -p MainPID --value "$NAME" 2>/dev/null)" != "$pid" ]; then
    echo "==> Stopping the server started by hand (pid $pid)"
    kill "$pid" 2>/dev/null || true
  fi
done

echo "==> Writing $UNIT (runs as $RUN_AS)"
unit "$@" > "$UNIT"
systemctl daemon-reload
systemctl enable "$NAME" >/dev/null
systemctl restart "$NAME"
sleep 1
systemctl --no-pager --lines=5 status "$NAME" || true
echo "==> Done. Logs: journalctl -u $NAME -f    Rebuild and restart: scripts/restart.sh"
