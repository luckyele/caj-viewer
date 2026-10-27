cmake_minimum_required(VERSION 3.16)
project(caj-viewer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# 无 root 环境下先用 ./bootstrap-deps.sh 拉取 GTK3 开发包到 .sysroot/，
# 然后： cmake -S . -B build -DCAJ_SYSROOT=$PWD/.sysroot/root && cmake --build build
set(CAJ_SYSROOT "" CACHE PATH "本地依赖 sysroot（无 root 时从 .sysroot/root 取 GTK 头文件与链接目录）")
if(CAJ_SYSROOT)
  set(ENV{PKG_CONFIG_SYSROOT_DIR} "${CAJ_SYSROOT}")
  set(ENV{PKG_CONFIG_PATH}
      "${CAJ_SYSROOT}/usr/lib/x86_64-linux-gnu/pkgconfig:${CAJ_SYSROOT}/usr/share/pkgconfig")
  message(STATUS "使用本地 sysroot: ${CAJ_SYSROOT}")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)

# 部分发行版的 .pc 名为 pangocairo.pc（无版本后缀），两种写法都试
pkg_check_modules(PANGOCAIRO IMPORTED_TARGET pangocairo)
if(NOT PANGOCAIRO_FOUND)
  pkg_check_modules(PANGOCAIRO REQUIRED IMPORTED_TARGET pangocairo-1.0)
endif()

find_package(ZLIB REQUIRED)

add_executable(caj-viewer
  src/caj_viewer.cpp
  src/caj_core.cpp
  src/sel_brush.cpp
)
target_include_directories(caj-viewer PRIVATE src)
target_compile_options(caj-viewer PRIVATE -Wall -Wextra)
target_link_libraries(caj-viewer PRIVATE
  PkgConfig::GTK3
  PkgConfig::PANGOCAIRO
  ZLIB::ZLIB
  ${CMAKE_DL_LIBS}
)

# 纯几何单元测试（无 GTK 依赖）
enable_testing()
add_executable(test_sel_brush tests/test_sel_brush.cpp src/sel_brush.cpp)
target_include_directories(test_sel_brush PRIVATE src)
target_compile_options(test_sel_brush PRIVATE -Wall -Wextra)
add_test(NAME sel_brush COMMAND test_sel_brush)

install(TARGETS caj-viewer RUNTIME DESTINATION bin)
