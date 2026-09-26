#!/usr/bin/env bash
# 无 root 拉取 C++ 版所需的 GTK3 开发依赖到 .sysroot/。
#
# 原理：apt-get download 只下载 .deb（不需要 root），dpkg-deb -x 解包到
# .sysroot/root，再把其中悬空的 .so 符号链接重定向到系统的同名 .so.N，
# 于是编译/链接都不需要往系统目录写任何东西。
#
# 用法：
#   ./bootstrap-deps.sh
#   cmake -S . -B build -DCAJ_SYSROOT=$PWD/.sysroot/root
#   cmake --build build -j
#
# 系统已装好 libgtk-3-dev 等开发包时无需运行本脚本，直接 cmake 即可。
set -euo pipefail
cd "$(dirname "$0")"

ROOT="$PWD/.sysroot/root"
DEBS="$PWD/.sysroot/debs"
BASE_PKGS=(libgtk-3-dev zlib1g-dev)

mkdir -p "$ROOT" "$DEBS"

echo "==> 解析 -dev 依赖闭包"
pkgs=$(apt-cache depends --recurse --no-recommends --no-suggests --no-conflicts \
        --no-breaks --no-replaces --no-enhances "${BASE_PKGS[@]}" 2>/dev/null |
       grep -E '^\w' | sort -u | grep -- '-dev$' || true)
if [ -z "$pkgs" ]; then
  echo "错误：无法解析依赖（需要 apt-cache / apt-get）" >&2
  exit 1
fi
# shellcheck disable=SC2086
count=$(echo "$pkgs" | wc -l)
echo "    共 $count 个 -dev 包"

echo "==> 下载到 $DEBS"
# shellcheck disable=SC2086
(cd "$DEBS" && apt-get download $pkgs >/dev/null 2>&1) || \
  echo "    提示：个别包下载失败可忽略（多为已内置的依赖）"

echo "==> 解包到 $ROOT"
for d in "$DEBS"/*.deb; do
  [ -e "$d" ] || continue
  dpkg-deb -x "$d" "$ROOT"
done

echo "==> 重定向悬空 .so 链接到系统库"
fixed=0
while IFS= read -r l; do
  base=$(basename "$l")
  tgt=$(ls /usr/lib/x86_64-linux-gnu/"$base".* 2>/dev/null | sort | tail -1 || true)
  if [ -n "$tgt" ]; then
    ln -sfn "$tgt" "$l"
    fixed=$((fixed + 1))
  fi
done < <(find "$ROOT" -name '*.so' -xtype l 2>/dev/null)
echo "    已重定向 $fixed 个"

echo
echo "完成。构建："
echo "  cmake -S . -B build -DCAJ_SYSROOT=$ROOT"
echo "  cmake --build build -j"
