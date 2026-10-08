# peony-alpha —— UKUI 桌面透明注入器(lwe-forge)
#
# 单翻译单元的 LD_PRELOAD 注入库。构建要求刻意压到最低:
#   - cmake >= 3.10(麒麟系统 3.16 直接够用,无需用户态工具链)
#   - Qt5 仅取头文件(Qt 符号在宿主 peony 进程内解析,不链接)
#   - 无任何 FetchContent,构建完全离线
cmake_minimum_required(VERSION 3.10)
project(peony-alpha LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt5 REQUIRED COMPONENTS Core Gui)
find_package(X11 REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(XCB REQUIRED xcb)

add_library(peony-alpha SHARED peony-alpha.cpp)

# -fvisibility=hidden:除源码里显式 visibility("default") 的 interposition
# 符号(两个 QPixmap mangled 构造器、xcb_change_property、XChangeProperty)
# 外,其余符号一律不导出 —— 日志与内部辅助函数不得进入拦截 ABI。
target_compile_options(peony-alpha PRIVATE -O2 -Wall -fvisibility=hidden)

target_include_directories(peony-alpha PRIVATE
	${Qt5Core_INCLUDE_DIRS}
	${Qt5Gui_INCLUDE_DIRS}
	${X11_INCLUDE_DIR}
	${XCB_INCLUDE_DIRS}
)

# dl:dlsym(RTLD_NEXT) 链到真实现
target_link_libraries(peony-alpha PRIVATE
	${X11_LIBRARIES}
	${XCB_LIBRARIES}
	${CMAKE_DL_LIBS}
)

# 扁平安装:前缀即 payload 根(与引擎产物同风格),产物 libpeony-alpha.so
install(TARGETS peony-alpha
	LIBRARY DESTINATION .
)
