// peony-alpha —— UKUI 桌面透明注入器(lwe-forge 持有的一等源码)。
//
// 部署为 libpeony-alpha.so,经 LD_PRELOAD 进入 peony-qt-desktop,把桌面壳
// 的背景变为真透明,让下层的引擎桌面层壁纸透出来。它是 interposer(符号
// 拦截器),不改宿主内存:LD_PRELOAD 使它排在 libQt5Widgets/libxcb/libX11
// 之前,被钩符号优先绑定到这里,再经 dlsym(RTLD_NEXT) 链到真实现。
// 未设置 PEONY_ALPHA_WALLPAPER 时整个库惰性(加载但不做任何事)。
//
// 拦截点与其原理在各站点注释详述(两个 QPixmap mangled 构造器、
// xcb_change_property / XChangeProperty 属性改写、constructor 的
// LD_PRELOAD 丢弃)。符号表对照麒麟 V10 SP1 的 peony 3.20.4.14 / Qt 5.12,
// 全部行为经真机截图 + xprop 验证。
//
// 窗口堆叠:引擎(DESKTOP 层)< peony(NORMAL+BELOW 半透明)< 普通窗口。

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
// Qt 头必须在 X11 之前 —— X11 的 Bool/Status/None 宏会污染 Qt 声明。
#include <QPixmap>
#include <QString>
#include <QSize>
#include <QFileInfo>
#include <Qt>

#include <dlfcn.h>
#include <xcb/xcb.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include <cstdarg>
#include <cstdio>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <pwd.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

// ---- 路径 --------------------------------------------------------------------
//
// home_dir/data_home 按惯例回退 (HOME / XDG_DATA_HOME)。shim_log_path 是与
// pkg/peony (tail.go::shimLogPath) 共享的契约 —— 改一处必改另一处; 日志
// 目录由 pkg/peony 在注入前创建, 这里只 append, 不做任何文件系统管理。
inline std::string home_dir() {
    const char* home = getenv("HOME");
    if (home != nullptr && *home != '\0')
        return home;
    if (const passwd* pw = getpwuid(getuid()); pw != nullptr && pw->pw_dir != nullptr)
        return pw->pw_dir;
    return {};
}

inline std::string data_home() {
    const char* dataHome = getenv("XDG_DATA_HOME");
    if (dataHome != nullptr && *dataHome != '\0')
        return dataHome;
    return home_dir() + "/.local/share";
}

inline std::string shim_log_path() {
    return data_home() + "/lwe-forge/peony-alpha.log";
}

// ---- 日志 -------------------------------------------------------------------
//
// printf 风格 + 栈上缓冲 + 单次 write:并发进程不会交错半行,常规路径
// 零堆分配,格式串错误最坏截断一行,永不波及宿主。行首带墙钟与 pid:
// peony 崩溃重生在日志里呈现为不同代际,可与进程事件对时。
namespace {

int log_fd() {
    static const int fd = []() {
        const std::string path = shim_log_path();
        if (path.empty())
            return -1;
        return open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    }();
    return fd;
}

void shim_log(const char* format, ...) {
    const int fd = log_fd();
    if (fd < 0)
        return;

    timespec now {};
    clock_gettime(CLOCK_REALTIME, &now);
    char buffer[1024];
    int length = snprintf(buffer, sizeof(buffer), "%lld.%03ld [pid %ld] ", static_cast<long long>(now.tv_sec),
                          now.tv_nsec / 1000000L, static_cast<long>(getpid()));
    if (length < 0 || static_cast<size_t>(length) >= sizeof(buffer))
        return;

    va_list arguments;
    va_start(arguments, format);
    const int appended = vsnprintf(buffer + length, sizeof(buffer) - static_cast<size_t>(length), format, arguments);
    va_end(arguments);
    if (appended < 0)
        return;
    length += appended > static_cast<int>(sizeof(buffer) - static_cast<size_t>(length) - 1)
                  ? static_cast<int>(sizeof(buffer) - static_cast<size_t>(length) - 1)
                  : appended;
    if (length <= 0 || buffer[length - 1] != '\n') {
        if (length >= static_cast<int>(sizeof(buffer)) - 1)
            length = static_cast<int>(sizeof(buffer)) - 2;
        buffer[length++] = '\n';
    }

    size_t written = 0;
    while (written < static_cast<size_t>(length)) {
        const ssize_t n = write(fd, buffer + written, static_cast<size_t>(length) - written);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        written += static_cast<size_t>(n);
    }
}

} // namespace

bool shim_enabled() {
    const char* p = getenv("PEONY_ALPHA_WALLPAPER");
    return p && *p;
}

// ---- 0. 不让注入传染给子进程 -------------------------------------------------
//
// LD_PRELOAD 会被 peony 的每个子进程继承,但本库只能在已加载 Qt 的进程里
// 解析符号:非 Qt 子进程(GIO 经 /bin/sh 起文件等)会死于
//    symbol lookup error: libpeony-alpha.so: undefined symbol: qt_version_tag
// 曾见的症状:桌面上只有 peony 自建的图标(计算机/回收站/主目录)点得开,
// 真实文件全部无响应。构造器运行时库已映射,丢掉变量对桌面零成本。
__attribute__((constructor)) static void shim_drop_preload_for_children() {
    shim_log("[shim] interposer mapped, enabled=%d\n", shim_enabled() ? 1 : 0);
    unsetenv("LD_PRELOAD");
    shim_log("[shim] dropped LD_PRELOAD so children load clean\n");
}

// ---- 1. QPixmap 构造器钩子 ----------------------------------------------------
//
// peony 的 setBackground()/switchBackground() 经 QPixmap(路径) 加载壁纸,
// 命中匹配列表即换为同尺寸全透明图:paintEvent 画出透明,ARGB 桌面窗口
// (WA_TranslucentBackground)于是透出下层内容 —— 零色偏、零毛边。
//
// peony 二进制引用的符号:
//   _ZN7QPixmapC1ERK7QStringPKc6QFlagsIN2Qt19ImageConversionFlagEE
// (nm -D /usr/bin/peony-qt-desktop)。inline 的 QPixmap(fileName) 最终落到
// 这个三参构造。不能在注入库里直接定义该构造器:编译器会合成成员构造/
// 析构代码,而 QExplicitlySharedDataPointer<QPlatformPixmap> 成员的完整
// 类型在私有头文件里。因此以"携带精确 mangled 名的普通函数"顶替
// (ABI:this=RDI,其余参数依次入寄存器),真构造器经 dlsym(RTLD_NEXT)
// 取得后在 this 上调用。

using pixmap_ctor3_t = void (*)(QPixmap*, const QString&, const char*, Qt::ImageConversionFlags);

// PEONY_ALPHA_WALLPAPER 是冒号分隔的路径列表;命中任一(精确路径或文件名)
// 即替换。/var/lib/AccountsService/backgrounds/ 下的一切也匹配:
// accountsservice 会把用户壁纸归一化拷贝进该目录,peony 启动加载的是归一化
// 路径,列表条目未必能原样穿过这一趟。
static constexpr const char* k_accounts_background_dir = "/var/lib/AccountsService/backgrounds/";

bool is_wallpaper_path(const QString& file_name) {
    const char* list = getenv("PEONY_ALPHA_WALLPAPER");
    if (!list || !*list)
        return false;
    if (file_name.isEmpty())
        return false;
    if (file_name.startsWith(k_accounts_background_dir))
        return true;
    QString base = QFileInfo(file_name).fileName();
    const char* start = list;
    for (const char* p = list;; p++) {
        if (*p == ':' || *p == '\0') {
            if (p > start) {
                QString candidate = QString::fromLocal8Bit(start, static_cast<int>(p - start));
                if (file_name == candidate || base == candidate)
                    return true;
            }
            if (*p == '\0')
                break;
            start = p + 1;
        }
    }
    return false;
}

void nullify_if_wallpaper(QPixmap* pm, const QString& file_name) {
    if (!pm->isNull() && is_wallpaper_path(file_name)) {
        // 同尺寸全透明替换;fill(transparent) 得到合法的预乘 alpha=0 像素
        QPixmap transparent(pm->size());
        transparent.fill(Qt::transparent);
        *pm = transparent;
        const QByteArray name = file_name.toUtf8();
        shim_log("[shim] nullified wallpaper pixmap: %s (%dx%d)\n", name.constData(), pm->size().width(),
                 pm->size().height());
    }
}

// 以下导出名是 ABI 而非 API:存在的意义是让动态链接器把 peony 的引用绑到
// 这里,拼写即 mangled 名/C 库符号,不可改动,故豁免常规命名约定。

extern "C" __attribute__((visibility("default"))) void
_ZN7QPixmapC1ERK7QStringPKc6QFlagsIN2Qt19ImageConversionFlagEE(QPixmap* pm, const QString& file_name,
                                                               const char* format, Qt::ImageConversionFlags flags) {
    static pixmap_ctor3_t real = nullptr;
    if (!real)
        real = reinterpret_cast<pixmap_ctor3_t>(
            dlsym(RTLD_NEXT, "_ZN7QPixmapC1ERK7QStringPKc6QFlagsIN2Qt19ImageConversionFlagEE"));
    real(pm, file_name, format, flags);
    nullify_if_wallpaper(pm, file_name);
}

extern "C" __attribute__((visibility("default"))) void
_ZN7QPixmapC2ERK7QStringPKc6QFlagsIN2Qt19ImageConversionFlagEE(QPixmap* pm, const QString& file_name,
                                                               const char* format, Qt::ImageConversionFlags flags) {
    static pixmap_ctor3_t real = nullptr;
    if (!real)
        real = reinterpret_cast<pixmap_ctor3_t>(
            dlsym(RTLD_NEXT, "_ZN7QPixmapC2ERK7QStringPKc6QFlagsIN2Qt19ImageConversionFlagEE"));
    real(pm, file_name, format, flags);
    nullify_if_wallpaper(pm, file_name);
}

// ---- 2. 属性改写(xcb/Xlib)---------------------------------------------------
//
// peony 给桌面窗口设置 _NET_WM_WINDOW_TYPE_DESKTOP,但 ukui-kwin 把
// DESKTOP 型窗口按不透明合成("It's a desktop after all, there is no
// window below")。在属性上传前把 DESKTOP 改写为 NORMAL,并向
// _NET_WM_STATE 追加 BELOW:发生在 map 之前,kwin 从一开始就把它当普通
// 半透明窗口管理,无需重启 WM。SkipTaskbar/SkipPager/SkipSwitcher 由
// peony 自己设置,与窗口类型无关,任务栏与 alt-tab 不受影响。

static xcb_atom_t a_wm_type = 0, a_type_desktop = 0, a_type_normal = 0;
static xcb_atom_t a_wm_state = 0, a_state_below = 0;

void ensure_atoms(xcb_connection_t* c) {
    // C++11 线程安全静态初始化:即使多线程同时冲进属性钩子,intern 也只跑一次
    static const bool atoms_ready = [c] {
        struct {
            const char* name;
            xcb_atom_t* out;
        } list[] = {
            {"_NET_WM_WINDOW_TYPE", &a_wm_type},
            {"_NET_WM_WINDOW_TYPE_DESKTOP", &a_type_desktop},
            {"_NET_WM_WINDOW_TYPE_NORMAL", &a_type_normal},
            {"_NET_WM_STATE", &a_wm_state},
            {"_NET_WM_STATE_BELOW", &a_state_below},
        };
        for (auto& it : list) {
            xcb_intern_atom_cookie_t ck = xcb_intern_atom(c, 0, strlen(it.name), it.name);
            xcb_intern_atom_reply_t* r = xcb_intern_atom_reply(c, ck, nullptr);
            if (r) {
                *it.out = r->atom;
                free(r);
            }
        }
        shim_log("[shim] xcb atoms ready (type=%u desktop=%u normal=%u state=%u below=%u)\n",
                 static_cast<unsigned>(a_wm_type), static_cast<unsigned>(a_type_desktop),
                 static_cast<unsigned>(a_type_normal), static_cast<unsigned>(a_wm_state),
                 static_cast<unsigned>(a_state_below));
        return true;
    }();
    (void)atoms_ready;
}

using xcb_ccp_t = xcb_void_cookie_t (*)(xcb_connection_t*, uint8_t, xcb_window_t, xcb_atom_t, xcb_atom_t, uint8_t,
                                        uint32_t, const void*);
static xcb_ccp_t real_xcb_ccp = nullptr;

__attribute__((visibility("default"))) xcb_void_cookie_t
xcb_change_property(xcb_connection_t* c, uint8_t mode, xcb_window_t window, xcb_atom_t property, xcb_atom_t type,
                    uint8_t format, uint32_t data_len, const void* data) {
    if (!real_xcb_ccp)
        real_xcb_ccp = reinterpret_cast<xcb_ccp_t>(dlsym(RTLD_NEXT, "xcb_change_property"));

    if (shim_enabled() && type == XCB_ATOM_ATOM && format == 32 && data_len > 0 && data_len <= 32) {
        ensure_atoms(c);
        const auto* atoms = static_cast<const xcb_atom_t*>(data);

        if (property == a_wm_type) {
            xcb_atom_t buf[32];
            memcpy(buf, data, data_len * 4);
            bool changed = false;
            for (uint32_t i = 0; i < data_len; i++) {
                if (buf[i] == a_type_desktop) {
                    buf[i] = a_type_normal;
                    changed = true;
                }
            }
            if (changed) {
                shim_log("[shim] win 0x%x: WINDOW_TYPE DESKTOP -> NORMAL\n", static_cast<unsigned>(window));
                return real_xcb_ccp(c, mode, window, property, type, format, data_len, buf);
            }
        } else if (property == a_wm_state) {
            bool has_below = false;
            for (uint32_t i = 0; i < data_len; i++)
                has_below |= (atoms[i] == a_state_below);
            if (!has_below && data_len < 32) {
                xcb_atom_t buf[33];
                memcpy(buf, data, data_len * 4);
                buf[data_len++] = a_state_below;
                shim_log("[shim] win 0x%x: appended STATE BELOW (%u atoms)\n", static_cast<unsigned>(window),
                         static_cast<unsigned>(data_len));
                return real_xcb_ccp(c, mode, window, property, type, format, data_len, buf);
            }
        }
    }
    return real_xcb_ccp(c, mode, window, property, type, format, data_len, data);
}

// Xlib 回退(KWindowSystem 等路径走 Xlib;注意 32 位属性数据是 long 数组)
using xlib_ccp_t = int (*)(Display*, Window, Atom, Atom, int, int, const unsigned char*, int);
static xlib_ccp_t real_xlib_ccp = nullptr;

static Atom xlib_atom(Display* d, const char* name) {
    return XInternAtom(d, name, False);
}

__attribute__((visibility("default"))) int
XChangeProperty(Display* display, Window w, Atom property, Atom type, int format, int mode,
                const unsigned char* data, int nelements) {
    if (!real_xlib_ccp)
        real_xlib_ccp = reinterpret_cast<xlib_ccp_t>(dlsym(RTLD_NEXT, "XChangeProperty"));

    if (shim_enabled() && format == 32 && nelements > 0 && nelements <= 32) {
        static Atom x_wm_type = 0, x_type_desktop = 0, x_type_normal = 0, x_wm_state = 0, x_state_below = 0;
        // C++11 线程安全静态初始化(同 xcb 路径):由第一个进入的调用者的
        // display 完成 atom intern, 且仅此一次
        static const bool xlib_atoms_ready = [&] {
            x_wm_type = xlib_atom(display, "_NET_WM_WINDOW_TYPE");
            x_type_desktop = xlib_atom(display, "_NET_WM_WINDOW_TYPE_DESKTOP");
            x_type_normal = xlib_atom(display, "_NET_WM_WINDOW_TYPE_NORMAL");
            x_wm_state = xlib_atom(display, "_NET_WM_STATE");
            x_state_below = xlib_atom(display, "_NET_WM_STATE_BELOW");
            shim_log("[shim] xlib atoms ready\n");
            return true;
        }();
        (void)xlib_atoms_ready;

        if (type == XA_ATOM && property == x_wm_type) {
            const auto* in = reinterpret_cast<const unsigned long*>(data);
            unsigned long buf[32];
            bool changed = false;
            for (int i = 0; i < nelements; i++) {
                buf[i] = in[i];
                if (buf[i] == (unsigned long)x_type_desktop) {
                    buf[i] = (unsigned long)x_type_normal;
                    changed = true;
                }
            }
            if (changed) {
                shim_log("[shim] xlib win 0x%lx: DESKTOP -> NORMAL\n", static_cast<unsigned long>(w));
                return real_xlib_ccp(display, w, property, type, format, mode, (unsigned char*)buf, nelements);
            }
        } else if (type == XA_ATOM && property == x_wm_state) {
            const auto* in = reinterpret_cast<const unsigned long*>(data);
            bool has_below = false;
            for (int i = 0; i < nelements; i++)
                has_below |= (in[i] == (unsigned long)x_state_below);
            if (!has_below) {
                unsigned long buf[33];
                for (int i = 0; i < nelements; i++)
                    buf[i] = in[i];
                buf[nelements++] = (unsigned long)x_state_below;
                shim_log("[shim] xlib win 0x%lx: appended BELOW\n", static_cast<unsigned long>(w));
                return real_xlib_ccp(display, w, property, type, format, mode, (unsigned char*)buf, nelements);
            }
        }
    }
    return real_xlib_ccp(display, w, property, type, format, mode, data, nelements);
}
