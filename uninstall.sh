#!/bin/sh
# caj-viewer 全量卸载脚本
# 用法： sh uninstall.sh   （系统级步骤需要 sudo，会提示输入密码）

set -e
echo "============================================"
echo " CAJ 阅读器 全量卸载"
echo "============================================"

echo
echo "[1/4] 系统级卸载 (需要 sudo)"
if command -v sudo >/dev/null && command -v apt >/dev/null; then
    sudo apt purge -y caj-viewer || true
    sudo apt autoremove -y || true
else
    echo "  跳过：未找到 sudo/apt"
fi

echo
echo "[2/4] 清理 /usr 下可能残留 (需要 sudo)"
if command -v sudo >/dev/null; then
    sudo rm -rf /usr/share/caj-viewer \
                /usr/share/doc/caj-viewer \
                /usr/share/applications/caj-viewer.desktop 2>/dev/null || true
    for s in 16 32 48 64 128 256; do
        sudo rm -f /usr/share/icons/hicolor/${s}x${s}/apps/caj-viewer.png 2>/dev/null || true
    done
    sudo rm -f /usr/share/icons/hicolor/scalable/apps/caj-viewer.svg 2>/dev/null || true
    sudo update-desktop-database /usr/share/applications 2>/dev/null || true
    sudo gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor 2>/dev/null || true
else
    echo "  跳过"
fi

echo
echo "[3/4] 清理 ~/.local 残留 (无需 sudo)"
rm -f  ~/.local/bin/caj-viewer
rm -f  ~/.local/share/applications/caj-viewer.desktop
for s in 16 32 48 64 128 256; do
    rm -f ~/.local/share/icons/hicolor/${s}x${s}/apps/caj-viewer.png 2>/dev/null
done
rm -f  ~/.local/share/icons/hicolor/scalable/apps/caj-viewer.svg 2>/dev/null
rm -rf ~/.local/usr/share/caj-viewer
rm -rf ~/.local/usr/share/doc/caj-viewer
rm -rf ~/.local/usr/bin/caj-viewer ~/.local/usr/share/applications/caj-viewer.desktop
rm -rf ~/.local/usr/share/icons/hicolor
update-desktop-database ~/.local/share/applications 2>/dev/null || true

echo
echo "[4/4] 清理沙盒绕路目录 + sidecar 缓存 (无需 sudo)"
rm -rf ~/.caj-viewer-app ~/.caj-viewer-app.old.* 2>/dev/null
# 删除曾打开过的 .caj 文件的 sidecar 缓存
find ~ -maxdepth 4 -name "*.cajviewer.pdf" -delete 2>/dev/null
# ~/.cache/caj-viewer 下的全局缓存
rm -rf ~/.cache/caj-viewer 2>/dev/null

echo
echo "============================================"
echo " 验证"
echo "============================================"
echo "which caj-viewer:"
which caj-viewer 2>&1 || echo "  ✓ 已卸载"
echo
echo "dpkg -l caj-viewer:"
dpkg -l caj-viewer 2>&1 | tail -n 3
echo
echo "残留文件 (应为空):"
find ~/.local ~/.caj-viewer-app /usr/share/caj-viewer /usr/share/doc/caj-viewer \
     /usr/share/applications/caj-viewer.desktop 2>/dev/null
[ $? -ne 0 ] || true
echo
echo "完成。"
