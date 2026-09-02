#!/usr/bin/env bash
# 尘埃信令 → /opt/remotedesk/src/remote ，不碰官网、不删整棵 /opt/remotedesk
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HOST="${LOESSX_VPS_HOST:-dustx-vps}"

rsync -az --delete \
  --exclude '__pycache__/' \
  --exclude '*.pyc' \
  --exclude '.DS_Store' \
  "$ROOT/src/remote/" "$HOST:/opt/remotedesk/src/remote/"

ssh -o BatchMode=yes "$HOST" 'systemctl restart remotedesk && systemctl is-active remotedesk'
echo "published remotedesk → $HOST:/opt/remotedesk/src/remote/"
