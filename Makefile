# CAJ 阅读器 —— .deb 一键打包
# 用法：
#   make            等价于 make deb
#   make deb        生成 caj-viewer_1.0.0_all.deb
#   make clean      清理构建产物
#   make install    编译后直接用 dpkg 安装（需要 sudo）

PKG      := caj-viewer
VERSION  := 1.0.0
ARCH     := all
DEB_FILE := $(PKG)_$(VERSION)_$(ARCH).deb
PKG_DIR  := packaging
BUILD    := build

.PHONY: all deb clean install install-local uninstall

all: deb

deb: $(BUILD)/$(DEB_FILE)

# 零 sudo 安装到 ~/.local（适合开发测试或本机试用）
install-local: deb
	@echo "==> 解包到 ~/.local"
	mkdir -p $(HOME)/.local/share/applications \
	         $(HOME)/.local/bin \
	         $(HOME)/.local/share/icons/hicolor
	dpkg-deb -x $(BUILD)/$(DEB_FILE) $(HOME)/.local
	# 把可执行文件链接到 ~/.local/bin
	ln -sf $(HOME)/.local/usr/bin/caj-viewer $(HOME)/.local/bin/caj-viewer
	# 把 .desktop 链接到 XDG 应用目录
	ln -sf $(HOME)/.local/usr/share/applications/caj-viewer.desktop \
	       $(HOME)/.local/share/applications/caj-viewer.desktop
	# 把图标链接到 XDG 图标目录
	@set -e; \
	for size in 16 32 48 64 128 256; do \
	    mkdir -p $(HOME)/.local/share/icons/hicolor/$${size}x$${size}/apps; \
	    ln -sf $(HOME)/.local/usr/share/icons/hicolor/$${size}x$${size}/apps/caj-viewer.png \
	           $(HOME)/.local/share/icons/hicolor/$${size}x$${size}/apps/caj-viewer.png; \
	done
	mkdir -p $(HOME)/.local/share/icons/hicolor/scalable/apps
	ln -sf $(HOME)/.local/usr/share/icons/hicolor/scalable/apps/caj-viewer.svg \
	       $(HOME)/.local/share/icons/hicolor/scalable/apps/caj-viewer.svg
	# 刷新 XDG 缓存
	-command -v update-desktop-database >/dev/null && \
	    update-desktop-database $(HOME)/.local/share/applications || true
	-command -v gtk-update-icon-cache >/dev/null && \
	    gtk-update-icon-cache -q -t -f $(HOME)/.local/share/icons/hicolor || true
	@echo
	@echo "✓ 已安装到 ~/.local（无需 sudo）"
	@echo "  PATH: 确保 \$$HOME/.local/bin 在 PATH 中"
	@echo "  运行: caj-viewer /path/to/file.caj"
	@echo "  或:   在应用菜单查找 'CAJ 阅读器'"

$(BUILD)/$(DEB_FILE): caj_viewer.py $(PKG_DIR)/DEBIAN/control \
                     $(PKG_DIR)/DEBIAN/postinst $(PKG_DIR)/DEBIAN/postrm
	@mkdir -p $(BUILD) $(PKG_DIR)/usr/share/caj-viewer \
	           $(PKG_DIR)/usr/share/doc/caj-viewer
	# 同步源码（防止 caj_viewer.py 修改后未同步到打包目录）
	cp caj_viewer.py $(PKG_DIR)/usr/share/caj-viewer/caj_viewer.py
	cp README.md   $(PKG_DIR)/usr/share/doc/caj-viewer/README
	cp LICENSE     $(PKG_DIR)/usr/share/doc/caj-viewer/LICENSE
	# 窗口图标副本（iconphoto 从脚本同目录加载）
	cp $(PKG_DIR)/usr/share/icons/hicolor/256x256/apps/caj-viewer.png \
	   $(PKG_DIR)/usr/share/caj-viewer/appicon.png
	# 打包（dpkg-deb 要求所有者为 root:root）
	fakeroot dpkg-deb --build --root-owner-group $(PKG_DIR) $(BUILD)/$(DEB_FILE)
	@echo
	@echo "✓ 打包完成: $(BUILD)/$(DEB_FILE)"
	@echo "  安装: sudo apt install ./$(BUILD)/$(DEB_FILE)"
	@echo "  或:  sudo dpkg -i $(BUILD)/$(DEB_FILE) && sudo apt-get install -f"

install: deb
	sudo apt install -y ./$(BUILD)/$(DEB_FILE)
	@echo
	@echo "✓ 安装完成。在应用菜单查找 'CAJ 阅读器'，或："
	@echo "  caj-viewer /path/to/file.caj"

uninstall:
	sudo apt remove -y $(PKG) || true

clean:
	rm -rf $(BUILD)
	rm -f $(PKG_DIR)/usr/share/caj-viewer/caj_viewer.py
	rm -f $(PKG_DIR)/usr/share/caj-viewer/appicon.png
	rm -f $(PKG_DIR)/usr/share/doc/caj-viewer/README
	rm -f $(PKG_DIR)/usr/share/doc/caj-viewer/LICENSE
