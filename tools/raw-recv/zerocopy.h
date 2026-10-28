/* zero-copy VAAPI -> OpenGL (Linux, EGL).
 *
 * The GPU decoder's frame stays on the GPU: its NV12 surface is exported as
 * two DMA-BUFs (Y as R8, UV as GR88), imported as EGLImages, bound to two GL
 * textures and turned into RGB by upscale.h's colour shader - no copy back to
 * memory in the decoder thread and no upload in the display thread.
 *
 * Measured on an i3-8130U (UHD 620, iHD), 1080p60 H.264 (tools/raw-recv/
 * decbench.c, paced at 60 fps): the decoder thread's time per frame goes from
 * ~7 ms (decode + av_hwframe_transfer_data, p99 13-19 ms) to ~2 ms (decode +
 * vaSyncSurface, p99 ~5 ms).
 *
 * Used when: the decoder is VAAPI, SDL's renderer is "opengl" on an EGL
 * context (Wayland; X11 with GLX is not) with EGL_EXT_image_dma_buf_import,
 * and OpenGL 4.3 (the upscalers' shaders). Picture settings other than the
 * defaults (P) need the pixels in memory: those frames are copied back as
 * before, and so are dynamic-resolution frames (a crop). Anything that
 * fails turns it off for the session, said once in the log. --no-zero-copy
 * turns it off.
 */
#pragma once
#if defined(SFT_ZEROCOPY)
#include <unistd.h>
#include <va/va.h>
#include <va/va_drmcommon.h>
#include <libavutil/hwcontext_vaapi.h>

static int g_zc = 1;                       /* --no-zero-copy: 0 */
static volatile int g_zc_ready = 0;        /* the display can draw VAAPI frames: the decoder hands them over as they are */

#define ZC_EGL_EXTENSIONS 0x3055
#define ZC_EGL_WIDTH 0x3057
#define ZC_EGL_HEIGHT 0x3056
#define ZC_EGL_NONE 0x3038
#define ZC_EGL_LINUX_DMA_BUF_EXT 0x3270
#define ZC_EGL_LINUX_DRM_FOURCC_EXT 0x3271
#define ZC_EGL_DMA_BUF_PLANE0_FD_EXT 0x3272
#define ZC_EGL_DMA_BUF_PLANE0_OFFSET_EXT 0x3273
#define ZC_EGL_DMA_BUF_PLANE0_PITCH_EXT 0x3274
#define ZC_EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT 0x3443
#define ZC_EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT 0x3444
#define ZC_DRM_FORMAT_MOD_INVALID 0x00ffffffffffffffULL

typedef void *(*zc_getdisplay_t)(void);
typedef void *(*zc_getcontext_t)(void);
typedef const char *(*zc_querystring_t)(void *, int);
typedef void *(*zc_createimage_t)(void *, void *, unsigned, void *, const int32_t *);
typedef unsigned (*zc_destroyimage_t)(void *, void *);
typedef void (*zc_imagetarget_t)(GLenum, void *);

static struct {
    int tried, ok, modifiers;
    int last;                              /* the frame on screen was drawn here: SDL's texture does not have it */
    void *dpy;
    zc_createimage_t create;
    zc_destroyimage_t destroy;
    zc_imagetarget_t target;
    GLuint tex[2];
    char why[128];
} zc;

/* the display thread, once its renderer exists: can it draw VAAPI frames? */
static int zc_probe(SDL_Renderer *ren)
{
    if (zc.tried) return zc.ok;
    zc.tried = 1;
    if (!g_zc) { snprintf(zc.why, sizeof(zc.why), "off (--no-zero-copy)"); return 0; }
    if (!up_init(ren)) { snprintf(zc.why, sizeof(zc.why), "needs the OpenGL 4.3 renderer (%s)", g_up_note); return 0; }
    zc_getcontext_t getctx = (zc_getcontext_t)SDL_GL_GetProcAddress("eglGetCurrentContext");
    zc_getdisplay_t getdpy = (zc_getdisplay_t)SDL_GL_GetProcAddress("eglGetCurrentDisplay");
    zc_querystring_t query = (zc_querystring_t)SDL_GL_GetProcAddress("eglQueryString");
    zc.create = (zc_createimage_t)SDL_GL_GetProcAddress("eglCreateImageKHR");
    zc.destroy = (zc_destroyimage_t)SDL_GL_GetProcAddress("eglDestroyImageKHR");
    zc.target = (zc_imagetarget_t)SDL_GL_GetProcAddress("glEGLImageTargetTexture2DOES");
    if (!getctx || !getdpy || !query || !zc.create || !zc.destroy || !zc.target || !getctx()) {
        snprintf(zc.why, sizeof(zc.why), "the OpenGL context is not EGL (X11/GLX)");
        return 0;
    }
    zc.dpy = getdpy();
    const char *ext = zc.dpy ? query(zc.dpy, ZC_EGL_EXTENSIONS) : NULL;
    if (!ext || !strstr(ext, "EGL_EXT_image_dma_buf_import")) {
        snprintf(zc.why, sizeof(zc.why), "EGL cannot import DMA-BUFs");
        return 0;
    }
    zc.modifiers = strstr(ext, "EGL_EXT_image_dma_buf_import_modifiers") != NULL;
    up_glGenTextures(2, zc.tex);
    zc.ok = 1;
    return 1;
}

/* the frame's VA display and surface */
static VADisplay zc_va(const AVFrame *f, VASurfaceID *surf)
{
    const AVHWFramesContext *fc = (const AVHWFramesContext *)f->hw_frames_ctx->data;
    const AVVAAPIDeviceContext *vc = fc->device_ctx->hwctx;
    *surf = (VASurfaceID)(uintptr_t)f->data[3];
    return vc->display;
}

/* the decoder thread: wait for the GPU here, not in the display thread */
static void zc_sync(const AVFrame *f)
{
    VASurfaceID s;
    VADisplay d = zc_va(f, &s);
    vaSyncSurface(d, s);
}

/* one layer of the exported surface as an EGLImage bound to tex */
static void *zc_import(const VADRMPRIMESurfaceDescriptor *d, int layer, int w, int h, GLuint tex)
{
    if (d->layers[layer].num_planes != 1) return NULL;
    const int obj = (int)d->layers[layer].object_index[0];
    const uint64_t mod = d->objects[obj].drm_format_modifier;
    int32_t a[20];
    int n = 0;
    a[n++] = ZC_EGL_WIDTH; a[n++] = w;
    a[n++] = ZC_EGL_HEIGHT; a[n++] = h;
    a[n++] = ZC_EGL_LINUX_DRM_FOURCC_EXT; a[n++] = (int32_t)d->layers[layer].drm_format;
    a[n++] = ZC_EGL_DMA_BUF_PLANE0_FD_EXT; a[n++] = d->objects[obj].fd;
    a[n++] = ZC_EGL_DMA_BUF_PLANE0_OFFSET_EXT; a[n++] = (int32_t)d->layers[layer].offset[0];
    a[n++] = ZC_EGL_DMA_BUF_PLANE0_PITCH_EXT; a[n++] = (int32_t)d->layers[layer].pitch[0];
    if (zc.modifiers && mod != ZC_DRM_FORMAT_MOD_INVALID) {
        a[n++] = ZC_EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT; a[n++] = (int32_t)(mod & 0xffffffffu);
        a[n++] = ZC_EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT; a[n++] = (int32_t)(mod >> 32);
    }
    a[n++] = ZC_EGL_NONE;
    void *img = zc.create(zc.dpy, NULL, ZC_EGL_LINUX_DMA_BUF_EXT, NULL, a);
    if (!img) return NULL;
    up_glBindTexture(GL_TEXTURE_2D, tex);
    zc.target(GL_TEXTURE_2D, img);
    up_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    up_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    up_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    up_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return img;
}

/* the display thread: draw a VAAPI frame (1), or 0 - then it is copied back as before */
static int zc_draw(SDL_Renderer *ren, const AVFrame *f, int W, int H, int mode)
{
    if (!zc.ok) return 0;
    VASurfaceID s;
    VADisplay vd = zc_va(f, &s);
    VADRMPRIMESurfaceDescriptor d;
    memset(&d, 0, sizeof(d));
    if (vaExportSurfaceHandle(vd, s, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                              VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS, &d) != VA_STATUS_SUCCESS) {
        snprintf(zc.why, sizeof(zc.why), "the driver does not export its surfaces");
        zc.ok = 0;
        return 0;
    }
    int ok = 0;
    void *img[2] = { NULL, NULL };
    if (d.num_layers == 2) {
        SDL_RenderFlush(ren);
        up_saved_t sv;
        up_save(&sv);
        up_glActiveTexture(GL_TEXTURE0);
        img[0] = zc_import(&d, 0, W, H, zc.tex[0]);
        img[1] = img[0] ? zc_import(&d, 1, W / 2, H / 2, zc.tex[1]) : NULL;
        up_restore(&sv);
        if (img[0] && img[1]) ok = up_draw_ex(ren, NULL, NULL, W, H, mode, 1, zc.tex);
    }
    for (int i = 0; i < 2; i++) if (img[i]) zc.destroy(zc.dpy, img[i]);
    for (uint32_t i = 0; i < d.num_objects; i++) close(d.objects[i].fd);
    if (!ok) {
        snprintf(zc.why, sizeof(zc.why), "EGL could not import the decoder's surface");
        zc.ok = 0;
    }
    return ok;
}

/* a repaint (window event, settings) while the last frame was drawn here:
 * its RGB picture is still in upscale.h's t_rgb - shown as SDL would show the
 * texture (not upscaled). 0: draw SDL's texture as usual */
static int zc_repaint(SDL_Renderer *ren)
{
    if (!zc.ok || !zc.last || !g_up.ok || g_up.in_w <= 0 || g_up.in_h <= 0) return 0;
    const int W = g_up.in_w, H = g_up.in_h;
    int ow = 0, oh = 0;
    SDL_GetRendererOutputSize(ren, &ow, &oh);
    if (ow <= 0 || oh <= 0) return 0;
    int vw, vh;
    if ((long)ow * H > (long)oh * W) { vh = oh; vw = (int)((long)oh * W / H); } else { vw = ow; vh = (int)((long)ow * H / W); }
    const int vx = (ow - vw) / 2, vy = (oh - vh) / 2;
    SDL_RenderFlush(ren);
    up_saved_t sv;
    up_save(&sv);
    up_glDisable(GL_SCISSOR_TEST);
    up_glDisable(GL_BLEND);
    up_glBindVertexArray(g_up.vao);
    up_glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)sv.dfbo);
    up_glViewport(vx, vy, vw, vh);
    up_glUseProgram(g_up.p_show);
    up_glUniform4i(up_glGetUniformLocation(g_up.p_show, "vp"), vx, vy, vw, vh);
    up_bind(0, g_up.t_rgb);
    up_glDrawArrays(GL_TRIANGLES, 0, 3);
    up_restore(&sv);
    return 1;
}
#else
static int g_zc = 0;
static const int g_zc_ready = 0;
static int zc_repaint(SDL_Renderer *ren) { (void)ren; return 0; }
#endif
