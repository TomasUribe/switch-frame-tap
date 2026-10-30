/* v0.7.11: GPU upscalers for the live picture - AMD FSR 1 and NVIDIA Image
 * Scaling - drawn with OpenGL inside SDL's own "opengl" renderer.
 *
 * Off by default (U cycles off / FSR 1 / NIS, remembered). The video frame is
 * uploaded as three planes, turned into RGB (BT.709, limited range), then:
 *   FSR 1: EASU (edge-adaptive upscale) into a texture at the picture's size
 *          on screen, then RCAS (sharpening) onto the screen;
 *   NIS:   NVScaler (a compute shader, up to 2x - its limit) into a texture,
 *          then a linear scale onto the screen for whatever is left.
 * SDL then draws the overlay and menu on top as before. Every bit of GL state
 * SDL relies on is saved and put back. Needs OpenGL 4.3 (any desktop driver
 * from the last decade); otherwise the option says so and stays off.
 * Recordings are not affected (they keep the console's own picture).
 */
#pragma once
#include <SDL2/SDL_opengl.h>
#include "upscale_shaders.h"
#include "clarity.h"

#ifdef _WIN32   /* + NVIDIA RTX Video Super Resolution, a Windows driver feature (Direct3D 11) */
enum { UP_OFF = 0, UP_FSR, UP_NIS, UP_RTX, UP_COUNT };
static const char *const up_name[UP_COUNT] = { "off", "FSR 1", "NIS", "RTX VSR" };
static const char *const up_key[UP_COUNT] = { "off", "fsr", "nis", "rtx" };
#elif defined(__APPLE__)
enum { UP_OFF = 0, UP_FSR, UP_COUNT, UP_NIS = UP_COUNT };
static const char *const up_name[UP_COUNT] = { "off", "FSR 1" };
static const char *const up_key[UP_COUNT] = { "off", "fsr" };
#else
enum { UP_OFF = 0, UP_FSR, UP_NIS, UP_COUNT };
static const char *const up_name[UP_COUNT] = { "off", "FSR 1", "NIS" };
static const char *const up_key[UP_COUNT] = { "off", "fsr", "nis" };
#endif
static int g_upscaler = UP_OFF;          /* the choice (viewer.ini: upscaler=) */
/* v0.7.16: dynamic resolution - the part of the frame (from its top left) the
 * game drew this time; the rest is older picture. 0: the whole frame. The
 * upscalers stretch only that part over the picture's place on screen. */
static int g_up_crop_w = 0, g_up_crop_h = 0;
static int g_up_crop_x = 0, g_up_crop_y = 0;   /* v0.7.16: its top left (Kingdom Hearts: in the middle) */
#define UP_CW(W) (g_up_crop_w > 0 && g_up_crop_w <= (W) ? g_up_crop_w : (W))
#define UP_CH(H) (g_up_crop_h > 0 && g_up_crop_h <= (H) ? g_up_crop_h : (H))
static int g_fsr_sharpness = 70;         /* macOS RCAS strength, percent */
static char g_up_note[72] = "";         /* why it is not available, if not */
static char g_up_why[40] = "";          /* why the last frame was not upscaled (the overlay) */

/* the last frame was drawn the plain way: say why, on the overlay and once in the log */
static int up_skip(const char *why, long hr)
{
    if (strcmp(why, g_up_why) != 0) {
        snprintf(g_up_why, sizeof(g_up_why), "%s", why);
        if (hr) fprintf(stderr, "upscaler: %s (0x%08lx)\n", why, (unsigned long)hr);
        else fprintf(stderr, "upscaler: %s\n", why);
    }
    return 0;
}

static void up_set_by_key(const char *k)
{
    for (int i = 0; i < UP_COUNT; i++) if (!strcmp(k, up_key[i])) g_upscaler = i;
}

/* before the renderer is made: the upscalers draw with OpenGL, which Linux's
 * SDL uses anyway; Windows' default is Direct3D 11, so it follows the choice
 * (and U switches between the two on the fly - raw-view.c ren_swap) */
static const char *up_driver_for(int mode)
{
    return (mode == UP_FSR || mode == UP_NIS) ? "opengl" : "direct3d11";
}

static void up_pick_renderer(void)
{
#ifdef _WIN32
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, up_driver_for(g_upscaler));
#endif
}

/* Windows: does the choice need the other renderer? (the viewer then makes a
 * new one between frames - raw-view.c ren_swap; elsewhere it never does) */
static int up_needs_swap(SDL_Renderer *ren)
{
#ifdef _WIN32
    SDL_RendererInfo ri;
    if (g_upscaler == UP_OFF || SDL_GetRendererInfo(ren, &ri) != 0) return 0;
    return strcmp(ri.name, up_driver_for(g_upscaler)) != 0;
#else
    (void)ren;
    return 0;
#endif
}

#ifdef __APPLE__
static struct { int active, out_w, out_h; } g_up;
#else
#ifndef GL_COMPUTE_SHADER
#define GL_COMPUTE_SHADER 0x91B9
#endif
#ifndef GL_SHADER_IMAGE_ACCESS_BARRIER_BIT
#define GL_SHADER_IMAGE_ACCESS_BARRIER_BIT 0x00000020
#endif
#ifndef GL_TEXTURE_FETCH_BARRIER_BIT
#define GL_TEXTURE_FETCH_BARRIER_BIT 0x00000008
#endif
#ifndef GL_UNIFORM_BUFFER
#define GL_UNIFORM_BUFFER 0x8A11
#endif
#ifndef GL_UNIFORM_BUFFER_BINDING
#define GL_UNIFORM_BUFFER_BINDING 0x8A28
#endif
#ifndef GL_VERTEX_ARRAY_BINDING
#define GL_VERTEX_ARRAY_BINDING 0x85B5
#endif
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif
#ifndef GL_MAJOR_VERSION
#define GL_MAJOR_VERSION 0x821B
#define GL_MINOR_VERSION 0x821C
#endif
#ifndef GL_RGBA32F
#define GL_RGBA32F 0x8814
#endif
#ifndef GL_R8
#define GL_R8 0x8229
#endif
#ifndef GL_RED
#define GL_RED 0x1903
#endif
#ifndef GL_RG
#define GL_RG 0x8227
#define GL_RG8 0x822B
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_UNPACK_ROW_LENGTH
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#endif
#ifndef GL_COMPILE_STATUS
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#endif
#ifndef GL_STREAM_DRAW
#define GL_STREAM_DRAW 0x88E0
#endif
#ifndef GL_CURRENT_PROGRAM
#define GL_CURRENT_PROGRAM 0x8B8D
#endif
#ifndef GL_ACTIVE_TEXTURE
#define GL_ACTIVE_TEXTURE 0x84E0
#endif
#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#endif

typedef char up_GLchar;
typedef ptrdiff_t up_GLsizeiptr;
#define UP_GL_FUNCS(X) \
    X(GLuint, glCreateShader, (GLenum)) \
    X(void, glShaderSource, (GLuint, GLsizei, const up_GLchar *const *, const GLint *)) \
    X(void, glCompileShader, (GLuint)) \
    X(void, glGetShaderiv, (GLuint, GLenum, GLint *)) \
    X(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, up_GLchar *)) \
    X(void, glDeleteShader, (GLuint)) \
    X(GLuint, glCreateProgram, (void)) \
    X(void, glAttachShader, (GLuint, GLuint)) \
    X(void, glLinkProgram, (GLuint)) \
    X(void, glGetProgramiv, (GLuint, GLenum, GLint *)) \
    X(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei *, up_GLchar *)) \
    X(void, glUseProgram, (GLuint)) \
    X(GLint, glGetUniformLocation, (GLuint, const up_GLchar *)) \
    X(void, glUniform1i, (GLint, GLint)) \
    X(void, glUniform2f, (GLint, GLfloat, GLfloat)) \
    X(void, glUniform4i, (GLint, GLint, GLint, GLint, GLint)) \
    X(void, glUniform4uiv, (GLint, GLsizei, const GLuint *)) \
    X(void, glGenFramebuffers, (GLsizei, GLuint *)) \
    X(void, glBindFramebuffer, (GLenum, GLuint)) \
    X(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) \
    X(GLenum, glCheckFramebufferStatus, (GLenum)) \
    X(void, glGenVertexArrays, (GLsizei, GLuint *)) \
    X(void, glBindVertexArray, (GLuint)) \
    X(void, glActiveTexture, (GLenum)) \
    X(void, glGenBuffers, (GLsizei, GLuint *)) \
    X(void, glBindBuffer, (GLenum, GLuint)) \
    X(void, glBufferData, (GLenum, up_GLsizeiptr, const void *, GLenum)) \
    X(void, glBindBufferBase, (GLenum, GLuint, GLuint)) \
    X(void, glDispatchCompute, (GLuint, GLuint, GLuint)) \
    X(void, glMemoryBarrier, (GLbitfield)) \
    X(void, glBindImageTexture, (GLuint, GLuint, GLint, GLboolean, GLint, GLenum, GLenum)) \
    X(void, glGenTextures, (GLsizei, GLuint *)) \
    X(void, glBindTexture, (GLenum, GLuint)) \
    X(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
    X(void, glTexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *)) \
    X(void, glTexParameteri, (GLenum, GLenum, GLint)) \
    X(void, glPixelStorei, (GLenum, GLint)) \
    X(void, glViewport, (GLint, GLint, GLsizei, GLsizei)) \
    X(void, glDrawArrays, (GLenum, GLint, GLsizei)) \
    X(void, glGetIntegerv, (GLenum, GLint *)) \
    X(GLboolean, glIsEnabled, (GLenum)) \
    X(void, glEnable, (GLenum)) \
    X(void, glDisable, (GLenum))

#define UP_DECL(ret, name, args) typedef ret (APIENTRY *up_##name##_t) args; static up_##name##_t up_##name;
UP_GL_FUNCS(UP_DECL)
#undef UP_DECL

typedef struct {
    int tried, ok, nis_ok;
    GLuint vao, fbo, ubo;
    GLuint p_yuv, p_easu, p_rcas, p_nis, p_show;
    GLuint t_y, t_u, t_v, t_uv, t_rgb, t_mid, t_coef_scale, t_coef_usm;
    int in_w, in_h, mid_w, mid_h;
    int active, out_w, out_h;            /* the last frame: upscaled, and to what size */
} up_state_t;
static up_state_t g_up;

static const char *up_vs =
    "#version 430 compatibility\n"
    "void main() { vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2); gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }\n";

static const char *up_fs_yuv =
    "#version 430 compatibility\n"
    "layout(binding = 0) uniform sampler2D ty;\n"
    "layout(binding = 1) uniform sampler2D tu;\n"
    "layout(binding = 2) uniform sampler2D tv;\n"
    "uniform vec2 size;\n"
    "uniform int nv12;\n"                     /* U and V together in tu (the GPU decoder's NV12) */
    "out vec4 o;\n"
    "void main() {\n"
    "  vec2 uv = gl_FragCoord.xy / size;\n"   /* texture row 0 = the picture's top, and so is the FBO's */
    "  float y = (texture(ty, uv).r - 16.0 / 255.0) * (255.0 / 219.0);\n"
    "  vec2 c = nv12 != 0 ? texture(tu, uv).rg : vec2(texture(tu, uv).r, texture(tv, uv).r);\n"
    "  float u = (c.x - 128.0 / 255.0) * (255.0 / 224.0);\n"
    "  float v = (c.y - 128.0 / 255.0) * (255.0 / 224.0);\n"
    "  o = vec4(clamp(vec3(y + 1.5748 * v, y - 0.1873 * u - 0.4681 * v, y + 1.8556 * u), 0.0, 1.0), 1.0);\n"
    "}\n";

static const char *up_fs_easu_head =
    "#version 430 compatibility\n#define A_GPU 1\n#define A_GLSL 1\n";
static const char *up_fs_easu_mid =
    "#define FSR_EASU_F 1\n"
    "layout(binding = 0) uniform sampler2D InputTexture;\n"
    "uniform uvec4 con[4];\n"
    "AF4 FsrEasuRF(AF2 p) { return textureGather(InputTexture, p, 0); }\n"
    "AF4 FsrEasuGF(AF2 p) { return textureGather(InputTexture, p, 1); }\n"
    "AF4 FsrEasuBF(AF2 p) { return textureGather(InputTexture, p, 2); }\n";
static const char *up_fs_easu_tail =
    "out vec4 o;\n"
    "void main() { AF3 c; FsrEasuF(c, AU2(gl_FragCoord.xy), con[0], con[1], con[2], con[3]); o = vec4(c, 1.0); }\n";

static const char *up_fs_rcas_mid =
    "#define FSR_RCAS_F 1\n"
    "layout(binding = 0) uniform sampler2D InputTexture;\n"
    "uniform uvec4 con[1];\n"
    "uniform ivec4 vp;\n"   /* the picture on screen: x, y (from the bottom), w, h */
    "AF4 FsrRcasLoadF(ASU2 p) { return texelFetch(InputTexture, clamp(ivec2(p), ivec2(0), vp.zw - 1), 0); }\n"
    "void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}\n";
static const char *up_fs_rcas_tail =
    "out vec4 o;\n"
    "void main() {\n"
    "  ivec2 q = ivec2(gl_FragCoord.xy) - vp.xy; q.y = vp.w - 1 - q.y;\n"   /* the screen counts from the bottom */
    "  AF1 r, g, b; FsrRcasF(r, g, b, AU2(q), con[0]); o = vec4(r, g, b, 1.0);\n"
    "}\n";

static const char *up_fs_show =
    "#version 430 compatibility\n"
    "layout(binding = 0) uniform sampler2D t;\n"
    "uniform ivec4 vp;\n"
    "out vec4 o;\n"
    "void main() { vec2 q = (gl_FragCoord.xy - vec2(vp.xy)) / vec2(vp.zw); q.y = 1.0 - q.y; o = texture(t, q); }\n";

static const char *up_cs_nis_head =
    "#version 430 compatibility\n"
    "#define NIS_GLSL 1\n#define NIS_SCALER 1\n#define NIS_USE_HALF_PRECISION 0\n#define NIS_VIEWPORT_SUPPORT 1\n"
    "#define NIS_BLOCK_WIDTH 32\n#define NIS_BLOCK_HEIGHT 24\n#define NIS_THREAD_GROUP_SIZE 128\n"
    "layout(local_size_x = NIS_THREAD_GROUP_SIZE) in;\n"
    "layout(std140, binding = 0) uniform const_buffer {\n"
    "  float kDetectRatio; float kDetectThres; float kMinContrastRatio; float kRatioNorm;\n"
    "  float kContrastBoost; float kEps; float kSharpStartY; float kSharpScaleY;\n"
    "  float kSharpStrengthMin; float kSharpStrengthScale; float kSharpLimitMin; float kSharpLimitScale;\n"
    "  float kScaleX; float kScaleY; float kDstNormX; float kDstNormY;\n"
    "  float kSrcNormX; float kSrcNormY;\n"
    "  uint kInputViewportOriginX; uint kInputViewportOriginY; uint kInputViewportWidth; uint kInputViewportHeight;\n"
    "  uint kOutputViewportOriginX; uint kOutputViewportOriginY; uint kOutputViewportWidth; uint kOutputViewportHeight;\n"
    "  float reserved0; float reserved1;\n"
    "};\n"
    "layout(binding = 0) uniform sampler2D in_texture;\n"
    "layout(binding = 1) uniform sampler2D coef_scaler;\n"
    "layout(binding = 2) uniform sampler2D coef_usm;\n"
    "layout(rgba8, binding = 0) uniform writeonly image2D out_texture;\n";
static const char *up_cs_nis_tail =
    "void main() { NVScaler(gl_WorkGroupID.xy, gl_LocalInvocationID.x); }\n";

/* NIS's constants (NIS_Config.h NVScalerUpdateConfig, SDR, sharpness 0.5) */
typedef struct {
    float kDetectRatio, kDetectThres, kMinContrastRatio, kRatioNorm;
    float kContrastBoost, kEps, kSharpStartY, kSharpScaleY;
    float kSharpStrengthMin, kSharpStrengthScale, kSharpLimitMin, kSharpLimitScale;
    float kScaleX, kScaleY, kDstNormX, kDstNormY;
    float kSrcNormX, kSrcNormY;
    uint32_t kInputViewportOriginX, kInputViewportOriginY, kInputViewportWidth, kInputViewportHeight;
    uint32_t kOutputViewportOriginX, kOutputViewportOriginY, kOutputViewportWidth, kOutputViewportHeight;
    float reserved0, reserved1;
} up_nis_config_t;

/* in_w x in_h: the part of the tex_w x tex_h texture to scale */
static void up_nis_config(up_nis_config_t *c, float sharpness, int in_x, int in_y, int in_w, int in_h, int tex_w, int tex_h, int out_w, int out_h)
{
    memset(c, 0, sizeof(*c));
    if (sharpness < 0.f) sharpness = 0.f;
    if (sharpness > 1.f) sharpness = 1.f;
    const float s = sharpness - 0.5f;
    const float MaxScale = s >= 0.f ? 1.25f : 1.75f, MinScale = s >= 0.f ? 1.25f : 1.0f, LimitScale = s >= 0.f ? 1.25f : 1.0f;
    const float kMaxContrastRatio = 10.0f, kSharpEndY = 0.9f;
    float smin = 0.4f + s * MinScale * 1.2f; if (smin < 0.f) smin = 0.f;
    const float smax = 1.6f + s * MaxScale * 1.8f;
    float lmin = 0.14f + s * LimitScale * 0.32f; if (lmin < 0.1f) lmin = 0.1f;
    const float lmax = 0.5f + s * LimitScale * 0.6f;
    c->kDetectRatio = 2 * 1127.f / 1024.f;
    c->kDetectThres = 64.0f / 1024.0f;
    c->kMinContrastRatio = 2.0f;
    c->kRatioNorm = 1.0f / (kMaxContrastRatio - c->kMinContrastRatio);
    c->kContrastBoost = 1.0f;
    c->kEps = 1.0f / 255.0f;
    c->kSharpStartY = 0.45f;
    c->kSharpScaleY = 1.0f / (kSharpEndY - c->kSharpStartY);
    c->kSharpStrengthMin = smin;
    c->kSharpStrengthScale = smax - smin;
    c->kSharpLimitMin = lmin;
    c->kSharpLimitScale = lmax - lmin;
    c->kInputViewportOriginX = (uint32_t)in_x; c->kInputViewportOriginY = (uint32_t)in_y;
    c->kInputViewportWidth = (uint32_t)in_w; c->kInputViewportHeight = (uint32_t)in_h;
    c->kOutputViewportWidth = (uint32_t)out_w; c->kOutputViewportHeight = (uint32_t)out_h;
    c->kSrcNormX = 1.f / tex_w; c->kSrcNormY = 1.f / tex_h;
    c->kDstNormX = 1.f / out_w; c->kDstNormY = 1.f / out_h;
    c->kScaleX = (float)in_w / out_w; c->kScaleY = (float)in_h / out_h;
}

static GLuint up_shader(GLenum type, const char *const *parts, int n, const char *what)
{
    GLuint s = up_glCreateShader(type);
    up_glShaderSource(s, n, parts, NULL);
    up_glCompileShader(s);
    GLint ok = 0;
    up_glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048] = "";
        up_glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "upscaler: the %s shader does not compile:\n%s\n", what, log);
        snprintf(g_up_note, sizeof(g_up_note), "the %s shader does not compile on this GPU", what);
        up_glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint up_program(GLuint a, GLuint b)
{
    if (!a || (b == (GLuint)-1 ? 0 : !b)) return 0;
    GLuint p = up_glCreateProgram();
    up_glAttachShader(p, a);
    if (b != (GLuint)-1) up_glAttachShader(p, b);
    up_glLinkProgram(p);
    GLint ok = 0;
    up_glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048] = "";
        up_glGetProgramInfoLog(p, sizeof(log), NULL, log);
        fprintf(stderr, "upscaler: a program does not link:\n%s\n", log);
        snprintf(g_up_note, sizeof(g_up_note), "a shader program does not link on this GPU");
        return 0;
    }
    return p;
}

static GLuint up_tex(GLint ifmt, int w, int h, GLenum fmt, GLenum type, const void *data, GLint filter)
{
    GLuint t = 0;
    up_glGenTextures(1, &t);
    up_glBindTexture(GL_TEXTURE_2D, t);
    up_glTexImage2D(GL_TEXTURE_2D, 0, ifmt, w, h, 0, fmt, type, data);
    up_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    up_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    up_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    up_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

/* the GL state SDL's renderer relies on, saved around our passes */
typedef struct { GLint prog, active, tex[3], dfbo, rfbo, vp[4], vao, ubo, unpack_row, unpack_align; GLboolean scissor, blend, tex2d; } up_saved_t;

static void up_save(up_saved_t *s)
{
    up_glGetIntegerv(GL_CURRENT_PROGRAM, &s->prog);
    up_glGetIntegerv(GL_ACTIVE_TEXTURE, &s->active);
    for (int i = 0; i < 3; i++) { up_glActiveTexture(GL_TEXTURE0 + i); up_glGetIntegerv(GL_TEXTURE_BINDING_2D, &s->tex[i]); }
    up_glActiveTexture(GL_TEXTURE0);
    s->tex2d = up_glIsEnabled(GL_TEXTURE_2D);
    up_glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &s->dfbo);
    up_glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &s->rfbo);
    up_glGetIntegerv(GL_VIEWPORT, s->vp);
    up_glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &s->vao);
    up_glGetIntegerv(GL_UNIFORM_BUFFER_BINDING, &s->ubo);
    up_glGetIntegerv(GL_UNPACK_ROW_LENGTH, &s->unpack_row);
    up_glGetIntegerv(GL_UNPACK_ALIGNMENT, &s->unpack_align);
    s->scissor = up_glIsEnabled(GL_SCISSOR_TEST);
    s->blend = up_glIsEnabled(GL_BLEND);
}

static void up_restore(const up_saved_t *s)
{
    up_glUseProgram((GLuint)s->prog);
    for (int i = 2; i >= 0; i--) { up_glActiveTexture(GL_TEXTURE0 + i); up_glBindTexture(GL_TEXTURE_2D, (GLuint)s->tex[i]); }
    up_glActiveTexture((GLenum)s->active);
    if (s->tex2d) up_glEnable(GL_TEXTURE_2D); else up_glDisable(GL_TEXTURE_2D);
    up_glBindFramebuffer(0x8CA9 /* GL_DRAW_FRAMEBUFFER */, (GLuint)s->dfbo);
    up_glBindFramebuffer(0x8CA8 /* GL_READ_FRAMEBUFFER */, (GLuint)s->rfbo);
    up_glViewport(s->vp[0], s->vp[1], s->vp[2], s->vp[3]);
    up_glBindVertexArray((GLuint)s->vao);
    up_glBindBuffer(GL_UNIFORM_BUFFER, (GLuint)s->ubo);
    up_glPixelStorei(GL_UNPACK_ROW_LENGTH, s->unpack_row);
    up_glPixelStorei(GL_UNPACK_ALIGNMENT, s->unpack_align);
    if (s->scissor) up_glEnable(GL_SCISSOR_TEST); else up_glDisable(GL_SCISSOR_TEST);
    if (s->blend) up_glEnable(GL_BLEND); else up_glDisable(GL_BLEND);
}

/* once: the GL functions, the version, the shaders */
static int up_init(SDL_Renderer *ren)
{
    if (g_up.tried) return g_up.ok;
    g_up.tried = 1;
    SDL_RendererInfo ri;
    if (SDL_GetRendererInfo(ren, &ri) != 0 || strcmp(ri.name, "opengl") != 0) {
        snprintf(g_up_note, sizeof(g_up_note), "needs the OpenGL renderer (this one is %s)", ri.name ? ri.name : "?");
        return 0;
    }
#define UP_LOAD(ret, name, args) up_##name = (up_##name##_t)SDL_GL_GetProcAddress(#name); if (!up_##name) { snprintf(g_up_note, sizeof(g_up_note), "OpenGL lacks %s", #name); return 0; }
    UP_GL_FUNCS(UP_LOAD)
#undef UP_LOAD
    GLint maj = 0, min = 0;
    up_glGetIntegerv(GL_MAJOR_VERSION, &maj);
    up_glGetIntegerv(GL_MINOR_VERSION, &min);
    if (maj * 10 + min < 43) { snprintf(g_up_note, sizeof(g_up_note), "needs OpenGL 4.3 (this driver has %d.%d)", maj, min); return 0; }

    up_saved_t sv;
    up_save(&sv);
    GLuint vs = up_shader(GL_VERTEX_SHADER, &up_vs, 1, "vertex");
    GLuint fy = up_shader(GL_FRAGMENT_SHADER, &up_fs_yuv, 1, "colour");
    const char *easu[] = { up_fs_easu_head, up_ffx_a, up_fs_easu_mid, up_ffx_fsr1, up_fs_easu_tail };
    const char *rcas[] = { up_fs_easu_head, up_ffx_a, up_fs_rcas_mid, up_ffx_fsr1, up_fs_rcas_tail };
    GLuint fe = up_shader(GL_FRAGMENT_SHADER, easu, 5, "FSR EASU");
    GLuint fr = up_shader(GL_FRAGMENT_SHADER, rcas, 5, "FSR RCAS");
    GLuint fs = up_shader(GL_FRAGMENT_SHADER, &up_fs_show, 1, "present");
    g_up.p_yuv = up_program(vs, fy);
    g_up.p_easu = up_program(vs, fe);
    g_up.p_rcas = up_program(vs, fr);
    g_up.p_show = up_program(vs, fs);
    const char *nis[] = { up_cs_nis_head, up_nis_scaler, up_cs_nis_tail };
    GLuint cn = up_shader(GL_COMPUTE_SHADER, nis, 3, "NIS");
    g_up.p_nis = cn ? up_program(cn, (GLuint)-1) : 0;
    g_up.nis_ok = g_up.p_nis != 0;
    g_up.ok = g_up.p_yuv && g_up.p_easu && g_up.p_rcas && g_up.p_show;
    if (g_up.ok) {
        up_glGenVertexArrays(1, &g_up.vao);
        up_glGenFramebuffers(1, &g_up.fbo);
        up_glGenBuffers(1, &g_up.ubo);
        /* NIS's filter tables: 64 phases x 8 taps = 64 rows of 2 RGBA texels */
        up_glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);   /* SDL leaves its own there */
        up_glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        g_up.t_coef_scale = up_tex(GL_RGBA32F, 2, 64, GL_RGBA, GL_FLOAT, nis_coef_scale, GL_NEAREST);
        g_up.t_coef_usm = up_tex(GL_RGBA32F, 2, 64, GL_RGBA, GL_FLOAT, nis_coef_usm, GL_NEAREST);
        fprintf(stderr, "upscaler: ready (OpenGL %d.%d, FSR 1%s)\n", maj, min, g_up.nis_ok ? ", NIS" : "; NIS unavailable");
    }
    up_restore(&sv);
    return g_up.ok;
}


#ifdef _WIN32
/* ---- NVIDIA RTX Video Super Resolution (Windows, Direct3D 11) ---------------
 * The driver's own AI upscaler, switched on per video-processor stream with
 * NVIDIA's extension (as Chrome, VLC and mpv do). The frame goes up as NV12,
 * the D3D11 video processor scales it straight onto SDL's back buffer, and SDL
 * draws the overlay on top. Needs an RTX card, a recent driver and "RTX Video
 * Enhancement - Super resolution" on in the NVIDIA Control Panel / NVIDIA app;
 * elsewhere the video processor still scales (plainly). */
#define COBJMACROS
#include <d3d11.h>

static const GUID up_IID_ID3D11VideoDevice = { 0x10ec4d5b, 0x975a, 0x4689, { 0xb9, 0xe4, 0xd0, 0xaa, 0xc3, 0x0f, 0xe3, 0x33 } };
static const GUID up_IID_ID3D11VideoContext = { 0x61f21c45, 0x3c0e, 0x4a74, { 0x9c, 0xea, 0x67, 0x10, 0x0d, 0x9a, 0xd5, 0xe4 } };
static const GUID up_IID_ID3D11Texture2D = { 0x6f15aaf2, 0xd208, 0x4e89, { 0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c } };
static const GUID up_nv_ppe = { 0xd43ce1b3, 0x1f4b, 0x48ac, { 0xba, 0xee, 0xc3, 0xc2, 0x53, 0x75, 0xe6, 0xf7 } };

static struct {
    int tried, ok, vsr;                    /* vsr: the driver took the Super Resolution switch */
    char note[72];
    char gpu[128];                         /* the adapter's name */
    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    ID3D11VideoDevice *vdev;
    ID3D11VideoContext *vctx;
    ID3D11VideoProcessorEnumerator *en;
    ID3D11VideoProcessor *vp;
    ID3D11Texture2D *stage, *in;
    ID3D11VideoProcessorInputView *iv;
    int in_w, in_h, vp_w, vp_h;
    int bind, bind_ok;                     /* which input bind flags this driver takes */
} g_vsr;

static int vsr_init(SDL_Renderer *ren)
{
    if (g_vsr.tried) return g_vsr.ok;
    g_vsr.tried = 1;
    g_vsr.dev = SDL_RenderGetD3D11Device(ren);      /* NULL unless SDL draws with Direct3D 11 */
    if (!g_vsr.dev) { snprintf(g_vsr.note, sizeof(g_vsr.note), "needs the Direct3D 11 renderer"); return 0; }
    ID3D11Device_GetImmediateContext(g_vsr.dev, &g_vsr.ctx);
    if (FAILED(ID3D11Device_QueryInterface(g_vsr.dev, &up_IID_ID3D11VideoDevice, (void **)&g_vsr.vdev)) ||
        FAILED(ID3D11DeviceContext_QueryInterface(g_vsr.ctx, &up_IID_ID3D11VideoContext, (void **)&g_vsr.vctx))) {
        snprintf(g_vsr.note, sizeof(g_vsr.note), "this GPU has no Direct3D 11 video processor");
        return 0;
    }
    g_vsr.ok = 1;
    {   /* which GPU SDL drew with: on a laptop, Windows may pick the integrated one */
        static const GUID iid_dxgi_device = { 0x54ec77fa, 0x1377, 0x44e6, { 0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c } };
        IDXGIDevice *dd = NULL;
        IDXGIAdapter *ad = NULL;
        DXGI_ADAPTER_DESC desc;
        char name[128] = "?";
        if (SUCCEEDED(ID3D11Device_QueryInterface(g_vsr.dev, &iid_dxgi_device, (void **)&dd))) {
            if (SUCCEEDED(IDXGIDevice_GetAdapter(dd, &ad))) {
                if (SUCCEEDED(IDXGIAdapter_GetDesc(ad, &desc))) WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), NULL, NULL);
                IDXGIAdapter_Release(ad);
            }
            IDXGIDevice_Release(dd);
        }
        snprintf(g_vsr.gpu, sizeof(g_vsr.gpu), "%s", name);
    }
    fprintf(stderr, "upscaler: Direct3D 11 video processor ready (%s)\n", g_vsr.gpu);
    return 1;
}

static void vsr_release(IUnknown **p) { if (*p) { (*p)->lpVtbl->Release(*p); *p = NULL; } }
#define VSR_RELEASE(p) vsr_release((IUnknown **)&(p))

/* the processor for these sizes (made again when the picture or window changes) */
static int vsr_setup(int W, int H, int vw, int vh)
{
    if (g_vsr.vp && W == g_vsr.in_w && H == g_vsr.in_h && vw == g_vsr.vp_w && vh == g_vsr.vp_h) return 1;
    VSR_RELEASE(g_vsr.iv); VSR_RELEASE(g_vsr.vp); VSR_RELEASE(g_vsr.en);
    if (W != g_vsr.in_w || H != g_vsr.in_h) {
        VSR_RELEASE(g_vsr.stage); VSR_RELEASE(g_vsr.in);
        D3D11_TEXTURE2D_DESC td = {0};
        td.Width = (UINT)W; td.Height = (UINT)H; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_NV12; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        HRESULT hr = ID3D11Device_CreateTexture2D(g_vsr.dev, &td, NULL, &g_vsr.stage);
        if (FAILED(hr)) return up_skip("failed: upload texture", hr);
        g_vsr.in_w = W; g_vsr.in_h = H;
    }
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd = {0};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputFrameRate.Numerator = 60; cd.InputFrameRate.Denominator = 1;
    cd.InputWidth = (UINT)W; cd.InputHeight = (UINT)H;
    cd.OutputFrameRate.Numerator = 60; cd.OutputFrameRate.Denominator = 1;
    cd.OutputWidth = (UINT)vw; cd.OutputHeight = (UINT)vh;
    cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    HRESULT hr = ID3D11VideoDevice_CreateVideoProcessorEnumerator(g_vsr.vdev, &cd, &g_vsr.en);
    if (FAILED(hr)) return up_skip("failed: processor sizes", hr);
    hr = ID3D11VideoDevice_CreateVideoProcessor(g_vsr.vdev, g_vsr.en, 0, &g_vsr.vp);
    if (FAILED(hr)) return up_skip("failed: video processor", hr);
    UINT fmt = 0;
    ID3D11VideoProcessorEnumerator_CheckVideoProcessorFormat(g_vsr.en, DXGI_FORMAT_NV12, &fmt);
    if (!(fmt & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT)) return up_skip("failed: no NV12 input", 0);
    /* the NV12 texture the processor reads: drivers differ in the bind flags
     * they accept for it, so try the usual ones (the first that works is kept) */
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd = {0};
    ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    static const UINT binds[] = { D3D11_BIND_DECODER, D3D11_BIND_RENDER_TARGET, 0,
                                  D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, D3D11_BIND_SHADER_RESOURCE };
    for (int i = g_vsr.bind_ok ? g_vsr.bind : 0; i < (int)(sizeof(binds) / sizeof(binds[0])) && !g_vsr.iv; i++) {
        VSR_RELEASE(g_vsr.in);
        D3D11_TEXTURE2D_DESC td = {0};
        td.Width = (UINT)W; td.Height = (UINT)H; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_NV12; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = binds[i];
        hr = ID3D11Device_CreateTexture2D(g_vsr.dev, &td, NULL, &g_vsr.in);
        if (SUCCEEDED(hr))
            hr = ID3D11VideoDevice_CreateVideoProcessorInputView(g_vsr.vdev, (ID3D11Resource *)g_vsr.in, g_vsr.en, &ivd, &g_vsr.iv);
        if (!g_vsr.bind_ok)
            fprintf(stderr, "upscaler: NV12 input, bind flags 0x%x: %s (0x%08lx)\n", binds[i], SUCCEEDED(hr) ? "ok" : "refused", (unsigned long)hr);
        if (SUCCEEDED(hr)) { g_vsr.bind = i; g_vsr.bind_ok = 1; }
    }
    if (!g_vsr.iv) return up_skip("failed: input view", hr);
    /* the stream: BT.709 limited range in, full-range RGB out */
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE in_cs = {0}, out_cs = {0};
    in_cs.YCbCr_Matrix = 1;
    in_cs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    ID3D11VideoContext_VideoProcessorSetStreamColorSpace(g_vsr.vctx, g_vsr.vp, 0, &in_cs);
    out_cs.RGB_Range = 0;
    ID3D11VideoContext_VideoProcessorSetOutputColorSpace(g_vsr.vctx, g_vsr.vp, &out_cs);
    ID3D11VideoContext_VideoProcessorSetStreamFrameFormat(g_vsr.vctx, g_vsr.vp, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    ID3D11VideoContext_VideoProcessorSetStreamAutoProcessingMode(g_vsr.vctx, g_vsr.vp, 0, FALSE);
    struct { UINT version, method, enable; } ext = { 0x1, 0x2, 1 };    /* NVIDIA: Super Resolution on */
    hr = ID3D11VideoContext_VideoProcessorSetStreamExtension(g_vsr.vctx, g_vsr.vp, 0, &up_nv_ppe, sizeof(ext), &ext);
    if (g_vsr.vsr != (SUCCEEDED(hr) ? 1 : -1))
        fprintf(stderr, "upscaler: RTX Video Super Resolution %s (0x%08lx)\n", SUCCEEDED(hr) ? "on" : "refused by the driver", (unsigned long)hr);
    g_vsr.vsr = SUCCEEDED(hr) ? 1 : -1;
    g_vsr.vp_w = vw; g_vsr.vp_h = vh;
    return 1;
}

static int vsr_draw(SDL_Renderer *ren, uint8_t *const data[3], const int linesize[3], int W, int H, int nv12)
{
    if (!vsr_init(ren)) return up_skip("unavailable, press U", 0);
    int ow = 0, oh = 0;
    SDL_GetRendererOutputSize(ren, &ow, &oh);
    if (W <= 0 || H <= 0 || ow <= 0 || oh <= 0 || (W & 1) || (H & 1)) return up_skip("odd picture size", 0);
    int vw, vh;
    if ((long)ow * H > (long)oh * W) { vh = oh; vw = (int)((long)oh * W / H); } else { vw = ow; vh = (int)((long)ow * H / W); }
    const int cw = UP_CW(W), ch = UP_CH(H);     /* v0.7.16: the part drawn this frame */
    if (vw <= cw && vh <= ch) return up_skip("window too small", 0);
    const int vx = (ow - vw) / 2, vy = (oh - vh) / 2;
    if (!vsr_setup(W, H, vw, vh)) return 0;
    /* the planes into NV12 (Y, then U and V interleaved) */
    D3D11_MAPPED_SUBRESOURCE m;
    HRESULT hr = ID3D11DeviceContext_Map(g_vsr.ctx, (ID3D11Resource *)g_vsr.stage, 0, D3D11_MAP_WRITE, 0, &m);
    if (FAILED(hr)) return up_skip("failed: upload", hr);
    uint8_t *dst = (uint8_t *)m.pData;
    for (int y = 0; y < H; y++) memcpy(dst + (size_t)y * m.RowPitch, data[0] + (size_t)y * linesize[0], (size_t)W);
    uint8_t *uv = dst + (size_t)m.RowPitch * H;
    for (int y = 0; y < H / 2; y++) {
        uint8_t *o = uv + (size_t)y * m.RowPitch;
        if (nv12) { memcpy(o, data[1] + (size_t)y * linesize[1], (size_t)W); continue; }   /* already NV12 (GPU decoder) */
        const uint8_t *u = data[1] + (size_t)y * linesize[1], *v = data[2] + (size_t)y * linesize[2];
        for (int x = 0; x < W / 2; x++) { o[2 * x] = u[x]; o[2 * x + 1] = v[x]; }
    }
    ID3D11DeviceContext_Unmap(g_vsr.ctx, (ID3D11Resource *)g_vsr.stage, 0);
    ID3D11DeviceContext_CopyResource(g_vsr.ctx, (ID3D11Resource *)g_vsr.in, (ID3D11Resource *)g_vsr.stage);

    /* SDL's back buffer: draw one black point so SDL binds it, then take it */
    SDL_RenderDrawPoint(ren, 0, 0);
    SDL_RenderFlush(ren);
    ID3D11RenderTargetView *rtv = NULL;
    ID3D11DeviceContext_OMGetRenderTargets(g_vsr.ctx, 1, &rtv, NULL);
    if (!rtv) return up_skip("failed: no back buffer", 0);
    ID3D11Resource *bb = NULL;
    ID3D11View_GetResource((ID3D11View *)rtv, &bb);
    ID3D11RenderTargetView_Release(rtv);
    if (!bb) return up_skip("failed: no back buffer", 0);
    /* the output view is made and let go every frame: holding the back buffer
     * would stop SDL from resizing it */
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd = {0};
    ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    ID3D11VideoProcessorOutputView *ov = NULL;
    hr = ID3D11VideoDevice_CreateVideoProcessorOutputView(g_vsr.vdev, bb, g_vsr.en, &ovd, &ov);
    ID3D11Resource_Release(bb);
    if (FAILED(hr) || !ov) return up_skip("failed: output view", hr);
    const int cx = cw < W ? g_up_crop_x : 0, cy = ch < H ? g_up_crop_y : 0;
    RECT src = { cx, cy, cx + cw, cy + ch }, dstr = { vx, vy, vx + vw, vy + vh };
    ID3D11VideoContext_VideoProcessorSetStreamSourceRect(g_vsr.vctx, g_vsr.vp, 0, TRUE, &src);
    ID3D11VideoContext_VideoProcessorSetStreamDestRect(g_vsr.vctx, g_vsr.vp, 0, TRUE, &dstr);
    ID3D11VideoContext_VideoProcessorSetOutputTargetRect(g_vsr.vctx, g_vsr.vp, TRUE, &dstr);
    D3D11_VIDEO_PROCESSOR_STREAM st = {0};
    st.Enable = TRUE;
    st.pInputSurface = g_vsr.iv;
    hr = ID3D11VideoContext_VideoProcessorBlt(g_vsr.vctx, g_vsr.vp, ov, 0, 1, &st);
    ID3D11VideoProcessorOutputView_Release(ov);
    if (FAILED(hr)) return up_skip("failed: video processor blit", hr);
    g_up_why[0] = 0;
    g_up.active = 1; g_up.out_w = vw; g_up.out_h = vh;
    return 1;
}
#endif

static void up_fbo_target(GLuint tex, int w, int h)
{
    up_glBindFramebuffer(GL_FRAMEBUFFER, g_up.fbo);
    up_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    up_glViewport(0, 0, w, h);
}

static void up_bind(int unit, GLuint tex)
{
    up_glActiveTexture(GL_TEXTURE0 + unit);
    up_glBindTexture(GL_TEXTURE_2D, tex);
}

static GLuint fbits(float f) { GLuint u; memcpy(&u, &f, 4); return u; }
#endif /* non-Metal backends */

/* the frame, upscaled into the window: 1 drawn, 0 not (draw it the usual way).
 * ext = two GL textures already holding Y and UV (zerocopy.h, Linux): then
 * the frame is always drawn here, upscaled or not (never cropped: zerocopy.h
 * leaves dynamic-resolution frames to the copy) */
static int up_draw_ex(SDL_Renderer *ren, uint8_t *const data[3], const int linesize[3], int W, int H, int mode, int nv12, const GLuint *ext)
{
    g_up.active = 0;
    (void)ext;
#ifdef __APPLE__
    if (mode == UP_OFF) { g_up_why[0] = 0; return 0; }
    int ow = 0, oh = 0;
    SDL_GetRendererOutputSize(ren, &ow, &oh);
    if (W <= 0 || H <= 0 || ow <= 0 || oh <= 0) return 0;
    const double scale = SDL_min((double)ow / W, (double)oh / H);
    if (scale <= 1.001) return up_skip("window too small", 0);
    if (UP_CW(W) != W || UP_CH(H) != H) return up_skip("dynamic resolution", 0);   /* v0.7.16: drawn plainly (part of the frame) */
    if (!clarity_draw(ren, (const uint8_t *const *)data, linesize, W, H,
                      g_fsr_sharpness / 100.0f, nv12)) return up_skip("Metal FSR unavailable", 0);
    g_up.active = 1;
    g_up.out_w = (int)SDL_lround(W * scale); g_up.out_h = (int)SDL_lround(H * scale);
    g_up_why[0] = 0;
    return 1;
#else
#ifdef _WIN32
    if (mode == UP_RTX) return vsr_draw(ren, data, linesize, W, H, nv12);
#endif
    if (mode == UP_OFF && !ext) { g_up_why[0] = 0; return 0; }
    if (!up_init(ren)) return up_skip("unavailable, press U", 0);
    if (mode == UP_NIS && !g_up.nis_ok) { up_skip("NIS unavailable", 0); if (!ext) return 0; mode = UP_OFF; }
    int ow = 0, oh = 0;
    SDL_GetRendererOutputSize(ren, &ow, &oh);
    if (W <= 0 || H <= 0 || ow <= 0 || oh <= 0) return 0;
    int vw, vh;                                   /* the picture on screen, letterboxed */
    if ((long)ow * H > (long)oh * W) { vh = oh; vw = (int)((long)oh * W / H); } else { vw = ow; vh = (int)((long)ow * H / W); }
    const int cw = UP_CW(W), ch = UP_CH(H);     /* v0.7.16: the part drawn this frame */
    const int cx = (cw < W && g_up_crop_x + cw <= W) ? g_up_crop_x : 0, cy = (ch < H && g_up_crop_y + ch <= H) ? g_up_crop_y : 0;
    if (vw <= cw && vh <= ch && mode != UP_OFF) { up_skip("window too small", 0); if (!ext) return 0; mode = UP_OFF; }
    const int vx = (ow - vw) / 2, vy = (oh - vh) / 2;

    SDL_RenderFlush(ren);
    up_saved_t sv;
    up_save(&sv);
    up_glDisable(GL_SCISSOR_TEST);
    up_glDisable(GL_BLEND);
    up_glDisable(GL_TEXTURE_2D);
    up_glBindVertexArray(g_up.vao);

    if (W != g_up.in_w || H != g_up.in_h) {        /* (re)allocate for this picture size */
        g_up.t_y = up_tex(GL_R8, W, H, GL_RED, GL_UNSIGNED_BYTE, NULL, GL_LINEAR);
        g_up.t_u = up_tex(GL_R8, W / 2, H / 2, GL_RED, GL_UNSIGNED_BYTE, NULL, GL_LINEAR);
        g_up.t_v = up_tex(GL_R8, W / 2, H / 2, GL_RED, GL_UNSIGNED_BYTE, NULL, GL_LINEAR);
        g_up.t_uv = up_tex(GL_RG8, W / 2, H / 2, GL_RG, GL_UNSIGNED_BYTE, NULL, GL_LINEAR);
        g_up.t_rgb = up_tex(GL_RGBA8, W, H, GL_RGBA, GL_UNSIGNED_BYTE, NULL, GL_LINEAR);
        g_up.in_w = W; g_up.in_h = H;
        g_up.mid_w = g_up.mid_h = 0;
    }
    int mw = vw, mh = vh;                         /* NIS goes up to 2x; the rest is a linear scale */
    if (mode == UP_NIS) { if (mw > 2 * cw) mw = 2 * cw; if (mh > 2 * ch) mh = 2 * ch; }
    if (mw != g_up.mid_w || mh != g_up.mid_h) {
        g_up.t_mid = up_tex(GL_RGBA8, mw, mh, GL_RGBA, GL_UNSIGNED_BYTE, NULL, GL_LINEAR);
        g_up.mid_w = mw; g_up.mid_h = mh;
    }

    /* the planes in, the RGB picture out */
    if (ext) {
        up_bind(0, ext[0]);
        up_bind(1, ext[1]);
        nv12 = 1;
    } else {
    up_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    up_bind(0, g_up.t_y); up_glPixelStorei(GL_UNPACK_ROW_LENGTH, linesize[0]);
    up_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RED, GL_UNSIGNED_BYTE, data[0]);
    if (nv12) {
        up_bind(1, g_up.t_uv); up_glPixelStorei(GL_UNPACK_ROW_LENGTH, linesize[1] / 2);
        up_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W / 2, H / 2, GL_RG, GL_UNSIGNED_BYTE, data[1]);
    } else {
        up_bind(1, g_up.t_u); up_glPixelStorei(GL_UNPACK_ROW_LENGTH, linesize[1]);
        up_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W / 2, H / 2, GL_RED, GL_UNSIGNED_BYTE, data[1]);
        up_bind(2, g_up.t_v); up_glPixelStorei(GL_UNPACK_ROW_LENGTH, linesize[2]);
        up_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W / 2, H / 2, GL_RED, GL_UNSIGNED_BYTE, data[2]);
    }
    up_glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }
    up_fbo_target(g_up.t_rgb, W, H);
    up_glUseProgram(g_up.p_yuv);
    up_glUniform2f(up_glGetUniformLocation(g_up.p_yuv, "size"), (float)W, (float)H);
    up_glUniform1i(up_glGetUniformLocation(g_up.p_yuv, "nv12"), nv12);
    up_glDrawArrays(GL_TRIANGLES, 0, 3);

    if (mode == UP_OFF) {
        /* ext, not upscaled: the whole RGB picture scaled onto the screen as SDL would */
        up_glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)sv.dfbo);
        up_glViewport(vx, vy, vw, vh);
        up_glUseProgram(g_up.p_show);
        up_glUniform4i(up_glGetUniformLocation(g_up.p_show, "vp"), vx, vy, vw, vh);
        up_bind(0, g_up.t_rgb);
        up_glDrawArrays(GL_TRIANGLES, 0, 3);
        up_restore(&sv);
        if (g_upscaler == UP_OFF) g_up_why[0] = 0;
        return 1;
    } else if (mode == UP_FSR) {
        /* EASU into the picture's on-screen size */
        GLuint con[16];
        const float rx = (float)cw / vw, ry = (float)ch / vh;   /* the drawn part over the screen; texel steps below: the whole texture */
        con[0] = fbits(rx); con[1] = fbits(ry); con[2] = fbits(0.5f * rx - 0.5f + cx); con[3] = fbits(0.5f * ry - 0.5f + cy);   /* + the part's offset (FsrEasuConOffset) */
        con[4] = fbits(1.f / W); con[5] = fbits(1.f / H); con[6] = fbits(1.f / W); con[7] = fbits(-1.f / H);
        con[8] = fbits(-1.f / W); con[9] = fbits(2.f / H); con[10] = fbits(1.f / W); con[11] = fbits(2.f / H);
        con[12] = fbits(0.f); con[13] = fbits(4.f / H); con[14] = 0; con[15] = 0;
        up_fbo_target(g_up.t_mid, vw, vh);
        up_glUseProgram(g_up.p_easu);
        up_glUniform4uiv(up_glGetUniformLocation(g_up.p_easu, "con"), 4, con);
        up_bind(0, g_up.t_rgb);
        up_glDrawArrays(GL_TRIANGLES, 0, 3);
        /* RCAS onto the screen (sharpness 0.25 stops: AMD's suggested default range is 0 to 2) */
        const float sharp = 0.25f;
        GLuint rc[4] = { fbits(SDL_powf(2.f, -sharp)), 0, 0, 0 };
        up_glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)sv.dfbo);
        up_glViewport(vx, vy, vw, vh);
        up_glUseProgram(g_up.p_rcas);
        up_glUniform4uiv(up_glGetUniformLocation(g_up.p_rcas, "con"), 1, rc);
        up_glUniform4i(up_glGetUniformLocation(g_up.p_rcas, "vp"), vx, vy, vw, vh);
        up_bind(0, g_up.t_mid);
        up_glDrawArrays(GL_TRIANGLES, 0, 3);
    } else {
        /* NIS: the compute pass, then whatever scaling is left */
        up_nis_config_t cfg;
        up_nis_config(&cfg, 0.5f, cx, cy, cw, ch, W, H, mw, mh);
        up_glBindBuffer(GL_UNIFORM_BUFFER, g_up.ubo);
        up_glBufferData(GL_UNIFORM_BUFFER, sizeof(cfg), &cfg, GL_STREAM_DRAW);
        up_glBindBufferBase(GL_UNIFORM_BUFFER, 0, g_up.ubo);
        up_glUseProgram(g_up.p_nis);
        up_bind(0, g_up.t_rgb);
        up_bind(1, g_up.t_coef_scale);
        up_bind(2, g_up.t_coef_usm);
        up_glBindImageTexture(0, g_up.t_mid, 0, GL_FALSE, 0, 0x88B9 /* GL_WRITE_ONLY */, GL_RGBA8);
        up_glDispatchCompute((GLuint)((mw + 31) / 32), (GLuint)((mh + 23) / 24), 1);
        up_glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
        up_glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)sv.dfbo);
        up_glViewport(vx, vy, vw, vh);
        up_glUseProgram(g_up.p_show);
        up_glUniform4i(up_glGetUniformLocation(g_up.p_show, "vp"), vx, vy, vw, vh);
        up_bind(0, g_up.t_mid);
        up_glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    up_restore(&sv);
    g_up_why[0] = 0;
    g_up.active = 1; g_up.out_w = vw; g_up.out_h = vh;
    return 1;
#endif
}

static int up_draw(SDL_Renderer *ren, uint8_t *const data[3], const int linesize[3], int W, int H, int mode, int nv12)
{
    return up_draw_ex(ren, data, linesize, W, H, mode, nv12, NULL);
}

/* the note shown when U is pressed */
static void up_describe(SDL_Renderer *ren, char *out, size_t cap)
{
    if (g_upscaler == UP_OFF) { snprintf(out, cap, "Upscaling off  (U)"); return; }
#ifdef __APPLE__
    snprintf(out, cap, clarity_init(ren) ? "Metal FSR 1, RCAS %d%% (F9)" : "Metal FSR unavailable",
             g_fsr_sharpness);
#else
#ifdef _WIN32
    if (g_upscaler == UP_RTX) {
        if (!vsr_init(ren)) {
            SDL_RendererInfo ri;
            if (SDL_GetRendererInfo(ren, &ri) == 0 && strcmp(ri.name, "direct3d11") != 0)
                snprintf(out, cap, "RTX VSR: restart the viewer to turn it on  (U)");
            else
                snprintf(out, cap, "RTX VSR unavailable: %s  (U)", g_vsr.note);
        } else {
            snprintf(out, cap, "Upscaling: RTX VSR on %.60s  (U)", g_vsr.gpu);
        }
        return;
    }
#endif
    if (!up_init(ren)) {
        SDL_RendererInfo ri;
        if (SDL_GetRendererInfo(ren, &ri) == 0 && strcmp(ri.name, "opengl") != 0)
            snprintf(out, cap, "%s: restart the viewer to turn it on  (U)", up_name[g_upscaler]);
        else
            snprintf(out, cap, "%s unavailable: %s  (U)", up_name[g_upscaler], g_up_note);
    } else if (g_upscaler == UP_NIS && !g_up.nis_ok) {
        snprintf(out, cap, "NIS unavailable: %s  (U)", g_up_note);
    } else {
        snprintf(out, cap, "Upscaling: %s  (U)%s", up_name[g_upscaler],
                 g_upscaler == UP_NIS ? " - NVIDIA Image Scaling" : " - AMD FidelityFX Super Resolution");
    }
#endif
}

/* the overlay's line */
static void up_hud_text(char *out, size_t cap)
{
    if (!g_up.active) { snprintf(out, cap, "%s", g_upscaler == UP_OFF ? "off (U)" : g_up_why[0] ? g_up_why : "-"); return; }
#ifdef _WIN32
    if (g_upscaler == UP_RTX && g_vsr.vsr < 0) { snprintf(out, cap, "plain, to %dx%d (no RTX VSR)", g_up.out_w, g_up.out_h); return; }
#endif
#ifdef __APPLE__
    int kernel = 0;
    clarity_status(&kernel, NULL);
    snprintf(out, cap, "%s/RCAS %d%%", kernel == 2 ? "EASU" : "Cubic", g_fsr_sharpness);
#else
    snprintf(out, cap, "%s to %dx%d", up_name[g_upscaler], g_up.out_w, g_up.out_h);
#endif
}

/* the renderer is about to go: forget everything made with it */
static void up_reset(void)
{
    clarity_close();
    memset(&g_up, 0, sizeof(g_up));           /* the GL objects go with SDL's context */
    g_up_why[0] = 0;
    g_up_note[0] = 0;
#ifdef _WIN32
    VSR_RELEASE(g_vsr.iv); VSR_RELEASE(g_vsr.vp); VSR_RELEASE(g_vsr.en);
    VSR_RELEASE(g_vsr.in); VSR_RELEASE(g_vsr.stage);
    VSR_RELEASE(g_vsr.vctx); VSR_RELEASE(g_vsr.vdev); VSR_RELEASE(g_vsr.ctx); VSR_RELEASE(g_vsr.dev);
    const int bind = g_vsr.bind, bind_ok = g_vsr.bind_ok;
    memset(&g_vsr, 0, sizeof(g_vsr));
    g_vsr.bind = bind; g_vsr.bind_ok = bind_ok;
#endif
}
