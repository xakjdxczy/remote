#!/usr/bin/env bash
# 尘埃产品页交给 loessx-vps 上机
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OPS="${LOESSX_OPS:-$ROOT/../loessx-vps}"
[[ -x "$OPS/scripts/publish-chenai-page.sh" ]] || {
  echo "missing $OPS/scripts/publish-chenai-page.sh （同级要有 loessx-vps）" >&2
  exit 2
}
"$OPS/scripts/publish-chenai-page.sh" "$ROOT/platforms/web"
