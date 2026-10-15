#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>
#include <unistd.h>
#include <sys/wait.h>

#ifndef EGL_PLATFORM_DEVICE_EXT
#define EGL_PLATFORM_DEVICE_EXT 0x313F
#endif
#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif
#ifndef EGL_LINUX_DMA_BUF_EXT
#define EGL_LINUX_DMA_BUF_EXT 0x3270
#endif
#ifndef EGL_LINUX_DRM_FOURCC_EXT
#define EGL_LINUX_DRM_FOURCC_EXT 0x3271
#endif
#ifndef EGL_DMA_BUF_PLANE0_FD_EXT
#define EGL_DMA_BUF_PLANE0_FD_EXT 0x3272
#endif
#ifndef EGL_DMA_BUF_PLANE0_OFFSET_EXT
#define EGL_DMA_BUF_PLANE0_OFFSET_EXT 0x3273
#endif
#ifndef EGL_DMA_BUF_PLANE0_PITCH_EXT
#define EGL_DMA_BUF_PLANE0_PITCH_EXT 0x3274
#endif
#ifndef EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT
#define EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT 0x3443
#endif
#ifndef EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT
#define EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT 0x3444
#endif
#ifndef EGL_GL_TEXTURE_2D_KHR
#define EGL_GL_TEXTURE_2D_KHR 0x30B1
#endif
#ifndef EGL_GL_TEXTURE_LEVEL_KHR
#define EGL_GL_TEXTURE_LEVEL_KHR 0x30BC
#endif

typedef EGLImageKHR (*PCI)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint *);
typedef EGLBoolean (*EDQ)(EGLDisplay, EGLImageKHR, int *, int *, uint64_t *);
typedef EGLBoolean (*EDI)(EGLDisplay, EGLImageKHR, int *, EGLint *, EGLint *);
typedef void (*GEIT)(GLenum, EGLClientBuffer);
typedef void (*PGEN)(GLsizei, GLuint *);
typedef void (*PBIND)(GLenum, GLuint);
typedef void (*PTEX)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
typedef void (*PFBOBIND)(GLenum, GLuint);
typedef void (*PFBOTEX)(GLenum, GLuint, GLenum, GLuint, GLint);
typedef EGLBoolean (*PMC)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
typedef EGLDisplay (*GPD)(EGLenum, void *, const EGLint *);
typedef EGLBoolean (*QDE)(EGLint, void *, EGLint *);
typedef EGLBoolean (*PCFG)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
typedef EGLSurface (*PCPS)(EGLDisplay, EGLConfig, const EGLint *);
typedef const GLubyte *(*PSTR)(GLenum);

static PCI pCreateImage; static EDQ pQueryDmaBuf; static EDI pExportDmaBuf;
static GEIT pImageTargetTex;
static PGEN pGenTextures; static PBIND pBindTexture; static PTEX pTexImage2D;
static PGEN pGenFramebuffers; static PFBOBIND pBindFramebuffer; static PFBOTEX pFramebufferTexture2D;
static PMC pMakeCurrent;
static GPD g_gpd; static QDE g_qd;
static EGLDisplay g_display = EGL_NO_DISPLAY;
static EGLContext g_context = EGL_NO_CONTEXT;
static int g_deviceIndex = -1;
static bool g_usedDevice = false;
static const int W = 256, H = 256;

static void pattern (std::vector<uint8_t> &rgba, int w, int h) {
    rgba.resize (size_t (w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            size_t o = (size_t (y) * w + x) * 4;
            rgba[o + 0] = uint8_t ((x * 255) / (w - 1));
            rgba[o + 1] = uint8_t ((y * 255) / (h - 1));
            rgba[o + 2] = uint8_t (((x + y) % 32) * 8);
            rgba[o + 3] = 0xFF;
        }
}

static void fillImportAttrs (EGLint *attrs, int w, int h, int fourcc, int fd, int stride, int offset,
                             uint64_t modifier, bool withModifier) {
    int n = 0;
    attrs[n++] = EGL_WIDTH; attrs[n++] = w;
    attrs[n++] = EGL_HEIGHT; attrs[n++] = h;
    attrs[n++] = EGL_LINUX_DRM_FOURCC_EXT; attrs[n++] = fourcc;
    attrs[n++] = EGL_DMA_BUF_PLANE0_FD_EXT; attrs[n++] = fd;
    attrs[n++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT; attrs[n++] = offset;
    attrs[n++] = EGL_DMA_BUF_PLANE0_PITCH_EXT; attrs[n++] = stride;
    if (withModifier) {
        attrs[n++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
        attrs[n++] = EGLint (modifier & 0xFFFFFFFFu);
        attrs[n++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
        attrs[n++] = EGLint (modifier >> 32);
    }
    attrs[n] = EGL_NONE;
}

static bool init_egl () {
    EGLDisplay d = EGL_NO_DISPLAY;
    GPD gpd = g_gpd;
    QDE qd = g_qd;
    if (g_usedDevice && g_deviceIndex >= 0 && gpd && qd) {
        EGLint count = 0;
        if (qd (0, nullptr, &count) && count > g_deviceIndex) {
            std::vector<void *> devs (count);
            qd (count, devs.data (), &count);
            d = gpd (EGL_PLATFORM_DEVICE_EXT, devs[g_deviceIndex], nullptr);
        }
    }
    if (d == EGL_NO_DISPLAY && gpd)
        d = gpd (EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    if (d == EGL_NO_DISPLAY)
        d = eglGetDisplay (EGL_DEFAULT_DISPLAY);
    if (d == EGL_NO_DISPLAY || !eglInitialize (d, nullptr, nullptr)) {
        printf ("egl init fail\n");
        return false;
    }
    eglBindAPI (EGL_OPENGL_API);
    EGLContext c = eglCreateContext (d, nullptr, EGL_NO_CONTEXT, nullptr);
    if (c == EGL_NO_CONTEXT) { printf ("ctx fail\n"); return false; }
    if (pMakeCurrent (d, EGL_NO_SURFACE, EGL_NO_SURFACE, c) != EGL_TRUE) {
        EGLint cfgn = 0;
        EGLConfig cfg = nullptr;
        PCFG choose = (PCFG) eglGetProcAddress ("eglChooseConfig");
        PCPS mkpbuf = (PCPS) eglGetProcAddress ("eglCreatePbufferSurface");
        const EGLint pba[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE};
        const EGLint pbat[] = {EGL_WIDTH, 32, EGL_HEIGHT, 32, EGL_NONE};
        choose (d, pba, &cfg, 1, &cfgn);
        EGLSurface s = mkpbuf (d, cfg, pbat);
        if (s == EGL_NO_SURFACE || pMakeCurrent (d, s, s, c) != EGL_TRUE) {
            printf ("makeCurrent fail\n");
            return false;
        }
    }
    g_display = d;
    g_context = c;
    return true;
}

static int verifyImport (int fd, int w, int h, int stride, int offset, int fourcc, uint64_t modifier,
                         bool withModifier, const char *label) {
    std::vector<uint8_t> want;
    pattern (want, w, h);
    EGLint attrs[24];
    fillImportAttrs (attrs, w, h, fourcc, fd, stride, offset, modifier, withModifier);
    EGLImageKHR img = pCreateImage (g_display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attrs);
    if (img == EGL_NO_IMAGE_KHR) {
        printf ("%s: import create fail eglErr=%d\n", label, (int) eglGetError ());
        return 3;
    }
    GLuint tex = 0;
    pGenTextures (1, &tex);
    pBindTexture (GL_TEXTURE_2D, tex);
    pImageTargetTex (GL_TEXTURE_2D, img);
    GLuint fbo = 0;
    pGenFramebuffers (1, &fbo);
    pBindFramebuffer (0x8D40, fbo);
    pFramebufferTexture2D (0x8D40, 0x8CE0, GL_TEXTURE_2D, tex, 0);
    std::vector<uint8_t> got (size_t (w) * h * 4);
    glReadPixels (0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, got.data ());
    pBindFramebuffer (0x8D40, 0);
    int bad = 0;
    for (size_t k = 0; k < got.size (); ++k)
        if (got[k] != want[k]) ++bad;
    printf ("%s: %s (mismatch %d/%zu glErr=%d)\n", label, bad == 0 ? "PASS" : "FAIL", bad, got.size (),
            (int) glGetError ());
    return bad == 0 ? 0 : 1;
}

static int child_verify (int fd, int w, int h, int stride, int offset, int fourcc, uint64_t modifier,
                         bool withModifier) {
    if (!init_egl ()) return 2;
    return verifyImport (fd, w, h, stride, offset, fourcc, modifier, withModifier, "cross-process import");
}

int main () {
    g_gpd = (GPD) eglGetProcAddress ("eglGetPlatformDisplayEXT");
    g_qd = (QDE) eglGetProcAddress ("eglQueryDevicesEXT");
    pMakeCurrent = (PMC) eglGetProcAddress ("eglMakeCurrent");
    if (!init_egl ()) return 2;
    PSTR gs = (PSTR) eglGetProcAddress ("glGetString");
    printf ("renderer : %s\n", (const char *) gs (0x1F01));
    const char *ext = eglQueryString (g_display, EGL_EXTENSIONS);
    auto has = [&](const char *e) { return ext && strstr (ext, e) != nullptr; };
    bool canExport = has ("EGL_MESA_image_dma_buf_export");
    bool hasModImport = has ("EGL_EXT_image_dma_buf_import_modifiers");
    bool canImport = has ("EGL_EXT_image_dma_buf_import");
    printf ("export ext : %s | import ext : %s | modifiers ext : %s\n",
            canExport ? "YES" : "NO", canImport ? "YES" : "NO", hasModImport ? "YES" : "NO");
    if (!canExport) {
        printf ("L3 VERDICT: FALLBACK-SHM (no mesa dma-buf export; try consumer-allocates GBM direction)\n");
        return 0;
    }

    pCreateImage = (PCI) eglGetProcAddress ("eglCreateImageKHR");
    pQueryDmaBuf = (EDQ) eglGetProcAddress ("eglExportDMABUFImageQueryMESA");
    pExportDmaBuf = (EDI) eglGetProcAddress ("eglExportDMABUFImageMESA");
    pImageTargetTex = (GEIT) eglGetProcAddress ("glEGLImageTargetTexture2DOES");
    pGenTextures = (PGEN) eglGetProcAddress ("glGenTextures");
    pBindTexture = (PBIND) eglGetProcAddress ("glBindTexture");
    pTexImage2D = (PTEX) eglGetProcAddress ("glTexImage2D");
    pGenFramebuffers = (PGEN) eglGetProcAddress ("glGenFramebuffers");
    pBindFramebuffer = (PFBOBIND) eglGetProcAddress ("glBindFramebuffer");
    pFramebufferTexture2D = (PFBOTEX) eglGetProcAddress ("glFramebufferTexture2D");
    if (!pCreateImage || !pQueryDmaBuf || !pExportDmaBuf || !pImageTargetTex) {
        printf ("missing entry points\n");
        return 2;
    }

    std::vector<uint8_t> want;
    pattern (want, W, H);
    GLuint tex = 0;
    pGenTextures (1, &tex);
    pBindTexture (GL_TEXTURE_2D, tex);
    pTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, want.data ());
    glFinish ();

    const EGLint imgAttrs[] = {EGL_GL_TEXTURE_LEVEL_KHR, 0, EGL_NONE};
    EGLImageKHR img = pCreateImage (g_display, g_context, EGL_GL_TEXTURE_2D_KHR,
                                    (EGLClientBuffer) (uintptr_t) tex, imgAttrs);
    if (img == EGL_NO_IMAGE_KHR) { printf ("export: create image fail eglErr=%d\n", (int) eglGetError ()); return 3; }

    int fourcc = 0, planes = 0;
    uint64_t modifier = 0;
    if (!pQueryDmaBuf (g_display, img, &fourcc, &planes, &modifier)) {
        printf ("export: query fail\n");
        return 3;
    }
    int fd = -1, stride = 0, offset = 0;
    if (!pExportDmaBuf (g_display, img, &fd, &stride, &offset) || fd < 0) {
        printf ("export: dmabuf export fail\n");
        return 3;
    }
    char fcc[5] = {char (fourcc & 0xff), char ((fourcc >> 8) & 0xff), char ((fourcc >> 16) & 0xff),
                   char ((fourcc >> 24) & 0xff), 0};
    printf ("export: fourcc=%s planes=%d fd=%d stride=%d offset=%d modifier=%016llx\n",
            fcc, planes, fd, stride, offset, (unsigned long long) modifier);
    printf ("modifier decode: vendor=0x%02llx code=0x%llx (%s)\n",
            (unsigned long long) (modifier >> 56), (unsigned long long) (modifier & 0x00FFFFFFFFFFFFFF),
            modifier == 0 ? "linear" : (modifier == 0x00ffffffffffffffULL ? "invalid" : "tiled/other"));

    constexpr uint64_t drmFormatModInvalid = 0x00ffffffffffffffULL;
    const bool useModifier = hasModImport && modifier != 0 && modifier != drmFormatModInvalid;
    int ok = verifyImport (fd, W, H, stride, offset, fourcc, modifier, useModifier, "same-process import");

    pid_t pid = fork ();
    if (pid == 0) {
        int r = child_verify (fd, W, H, stride, offset, fourcc, modifier, useModifier);
        fflush (stdout);
        _exit (r);
    }
    int status = 0;
    waitpid (pid, &status, 0);
    int childOk = WIFEXITED (status) ? WEXITSTATUS (status) : 1;

    printf ("L3 VERDICT: %s\n", (ok == 0 && childOk == 0)
        ? "FEASIBLE (zero-copy dma-buf works)"
        : "FALLBACK-SHM (import failed)");
    return (ok == 0 && childOk == 0) ? 0 : 1;
}
