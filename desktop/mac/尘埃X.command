#!/bin/zsh
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
for APP in \
  "$ROOT/desktop/cpp/build/尘埃X.app" \
  "$ROOT/desktop/mac/尘埃X.app" \
  "$ROOT/dist/尘埃X.app"
do
  if [[ -d "$APP" ]]; then
    exec open "$APP"
  fi
done
echo "还没有打好桌面程序。在这台 Mac 上编译一次即可："
echo "  cd desktop/cpp && cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j"
exit 1
