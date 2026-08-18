#!/bin/zsh
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
APP="$ROOT/desktop/mac/尘埃X.app"
if [[ ! -d "$APP" ]]; then
  APP="$ROOT/dist/尘埃X.app"
fi
if [[ -d "$APP" ]]; then
  exec open "$APP"
fi
echo "还没有打好桌面程序。开发机打包一次即可（用户之后只需双击 尘埃X.app）："
echo "  python3 -m remote pack"
exit 1
