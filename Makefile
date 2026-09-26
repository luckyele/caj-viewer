# CAJ 阅读器 —— 构建入口（C++ / GTK3 主线）
#
#   make            等价于 make build：配置 + 编译，产出 build/caj-viewer
#   make build      检测到 .sysroot/root 时自动带上 -DCAJ_SYSROOT
#   make verify     格式回归：对基准样本跑 --info（SAMPLE=... 可换样本）
#   make smoke      GUI 冒烟：8 秒后自动结束，退出码 124 视为通过
#   make run        运行（FILE=... 指定文献；不给 FILE 则在界面里弹选择框）
#   make clean      清理 CMake 产物与 .deb 产物
#
# 以下目标打包的是 Python 参考实现 caj_viewer.py，与 C++ 主线无关；它们依赖的
# packaging/ 目录未纳入版本控制，从干净克隆执行会失败。保留仅为兼容旧流程：
#   make deb / install-local / install / uninstall
# 注意 install / uninstall 需要 sudo，而本机环境没有 root（见 AGENTS.md），别用。

CXX_BUILD := build
DIST      := dist

# 基准样本（仓库不含测试样本，只有本机有；文件名含全角冒号，故整体加引号）
SAMPLE ?= $(HOME)/下载/数智时代休闲文明的构建：从大众观光到个性化体验的产业演进_钱建农.caj
FILE   ?=

# 无 root 环境靠本地 sysroot 提供 GTK3 头文件与链接目录
SYSROOT := $(if $(wildcard .sysroot/root),-DCAJ_SYSROOT=$(CURDIR)/.sysroot/root,)

.PHONY: all build verify smoke run check-sample clean deb install-local install uninstall

all: build

build:
	cmake -S . -B $(CXX_BUILD) $(SYSROOT)
	cmake --build $(CXX_BUILD) -j

# 样本不存在时给出可操作提示，而不是让后续命令报一堆无关错误
check-sample:
	@test -f "$(SAMPLE)" || { \
	    echo "缺少样本: $(SAMPLE)"; \
	    echo "用 make <目标> SAMPLE=/path/to/file.caj 覆盖"; \
	    exit 1; }

verify: build check-sample
	./$(CXX_BUILD)/caj-viewer --info "$(SAMPLE)"

smoke: build check-sample
	@GDK_BACKEND=x11 timeout 8 ./$(CXX_BUILD)/caj-viewer "$(SAMPLE)"; \
	    code=$$?; \
	    if [ $$code -eq 124 ]; then \
	        echo "✓ GUI 冒烟通过（8 秒后自动结束）"; \
	    else \
	        echo "✗ GUI 退出码 $$code（只有 124 才是正常的超时终止）"; exit $$code; \
	    fi

run: build
	./$(CXX_BUILD)/caj-viewer $(FILE)

# ---------------------------------------------------------------------------
# 以下为 Python 参考实现的 .deb 打包（遗留）

PKG      := caj-viewer
VERSION  := 1.0.1
ARCH     := all
DEB_FILE := $(PKG)_$(VERSION)_$(ARCH).deb
PKG_DIR  := packaging

deb: $(DIST)/$(DEB_FILE)

# 零 sudo 安装到 ~/.local（适合开发测试或本机试用）
install-local: deb
	@echo "==> 解包到 ~/.local"
	mkdir -p $(HOME)/.local/share/applications \
	         $(HOME)/.local/bin \
	         $(HOME)/.local/share/icons/hicolor
	dpkg-deb -x $(DIST)/$(DEB_FILE) $(HOME)/.local
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

$(DIST)/$(DEB_FILE): caj_viewer.py $(PKG_DIR)/DEBIAN/control \
                     $(PKG_DIR)/DEBIAN/postinst $(PKG_DIR)/DEBIAN/postrm
	@mkdir -p $(DIST) $(PKG_DIR)/usr/share/caj-viewer \
	           $(PKG_DIR)/usr/share/doc/caj-viewer
	# 同步源码（防止 caj_viewer.py 修改后未同步到打包目录）
	cp caj_viewer.py $(PKG_DIR)/usr/share/caj-viewer/caj_viewer.py
	cp README.md   $(PKG_DIR)/usr/share/doc/caj-viewer/README
	cp LICENSE     $(PKG_DIR)/usr/share/doc/caj-viewer/LICENSE
	# 窗口图标副本（iconphoto 从脚本同目录加载）
	cp $(PKG_DIR)/usr/share/icons/hicolor/256x256/apps/caj-viewer.png \
	   $(PKG_DIR)/usr/share/caj-viewer/appicon.png
	# 打包（dpkg-deb 要求所有者为 root:root）
	fakeroot dpkg-deb --build --root-owner-group $(PKG_DIR) $(DIST)/$(DEB_FILE)
	@echo
	@echo "✓ 打包完成: $(DIST)/$(DEB_FILE)"
	@echo "  安装: sudo apt install ./$(DIST)/$(DEB_FILE)"
	@echo "  或:  sudo dpkg -i $(DIST)/$(DEB_FILE) && sudo apt-get install -f"

install: deb
	sudo apt install -y ./$(DIST)/$(DEB_FILE)
	@echo
	@echo "✓ 安装完成。在应用菜单查找 'CAJ 阅读器'，或："
	@echo "  caj-viewer /path/to/file.caj"

uninstall:
	sudo apt remove -y $(PKG) || true

# ---------------------------------------------------------------------------

clean:
	rm -rf $(CXX_BUILD) $(DIST)
	rm -rf $(PKG_DIR)/usr/share/caj-viewer/__pycache__
	rm -f $(PKG_DIR)/usr/share/caj-viewer/caj_viewer.py
	rm -f $(PKG_DIR)/usr/share/caj-viewer/appicon.png
	rm -f $(PKG_DIR)/usr/share/doc/caj-viewer/README
	rm -f $(PKG_DIR)/usr/share/doc/caj-viewer/LICENSE
