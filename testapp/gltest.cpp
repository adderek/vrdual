// vrdual-gltest — small OpenGL 2.1-era app (fixed function + GLSL 1.20 +
// texture uploads), run through Zink to exercise the layer the way old games
// do. Static scene: calibration posts at 0.5/1/2/5/20 m like `vrdual-cube
// --scene calib` (same geometry, so measure_posts.py applies).
//
// Plain Xlib + GLX (like SDL 1.2 games, and it builds as 32-bit too).
//
//   MESA_LOADER_DRIVER_OVERRIDE=zink vrdual-gltest [--frames N] [--title T] [--shadow]
//
// --shadow: each frame first draws the 0.5 m post into a 512x512 FBO with
// the scene's own shader (like a game's shadow map), then shows that texture
// as a w == 1 quad in the top left corner (left 256x256 of the window). The
// quad is never shifted, so it shows each eye's FBO as it was rendered: with
// auto_noshift both eyes match, without it the post sits ~17 px apart.
// --view W H: the scene is drawn into a WxH FBO, then blitted to the window
// (a 3D view at another size than the swapchain, like Civ V's 1368x768);
// with --shadow the "shadow map" FBO is WxH too: same size, another target.
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define GLF(ret, name, args) static ret (*name) args
GLF(GLuint, glCreateShader_, (GLenum));
GLF(void, glShaderSource_, (GLuint, GLsizei, const GLchar* const*, const GLint*));
GLF(void, glCompileShader_, (GLuint));
GLF(GLuint, glCreateProgram_, ());
GLF(void, glAttachShader_, (GLuint, GLuint));
GLF(void, glLinkProgram_, (GLuint));
GLF(void, glUseProgram_, (GLuint));
GLF(void, glGenBuffers_, (GLsizei, GLuint*));
GLF(void, glBindBuffer_, (GLenum, GLuint));
GLF(void, glBufferData_, (GLenum, GLsizeiptr, const void*, GLenum));
GLF(void, glBufferSubData_, (GLenum, GLintptr, GLsizeiptr, const void*));
GLF(GLint, glGetUniformLocation_, (GLuint, const GLchar*));
GLF(void, glUniform3f_, (GLint, GLfloat, GLfloat, GLfloat));
GLF(void, glGenFramebuffers_, (GLsizei, GLuint*));
GLF(void, glBindFramebuffer_, (GLenum, GLuint));
GLF(void, glFramebufferTexture2D_, (GLenum, GLenum, GLenum, GLuint, GLint));
GLF(void, glGenRenderbuffers_, (GLsizei, GLuint*));
GLF(void, glBindRenderbuffer_, (GLenum, GLuint));
GLF(void, glRenderbufferStorage_, (GLenum, GLenum, GLsizei, GLsizei));
GLF(void, glFramebufferRenderbuffer_, (GLenum, GLenum, GLenum, GLuint));
GLF(GLenum, glCheckFramebufferStatus_, (GLenum));
GLF(void, glBlitFramebuffer_, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum));
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#define GL_RENDERBUFFER 0x8D41
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif
#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#endif
#ifndef GL_DEPTH_COMPONENT24
#define GL_DEPTH_COMPONENT24 0x81A6
#endif

static const char* kVs = "#version 120\n"
                         "varying vec3 c;\n"
                         "void main() { c = gl_Color.rgb; gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex; }\n";
static const char* kVs2 = "#version 120\n"  // --view: the far posts, a second key in the 3D view
                          "varying vec3 c;\n"
                          "void main() { c = gl_Color.rgb * 0.98; gl_Position = ftransform(); }\n";
static const char* kFs = "#version 120\n"
                         "varying vec3 c; uniform vec3 tint;\n"
                         "void main() { gl_FragColor = vec4(c * tint, 1.0); }\n";

struct V { float x, y, z, r, g, b; };

// Same posts as testapp calib: x = 0 offset per distance, 0.04 m wide, floor bar.
static void post(std::vector<V>& v, float x, float d, float r, float g, float b) {
    float w = 0.02f * d, y0 = -0.25f * d, y1 = 0.05f * d;  // scaled so each spans similar screen height
    float z = -d;
    V q[6] = {{x - w, y0, z, r, g, b}, {x + w, y0, z, r, g, b}, {x + w, y1, z, r, g, b},
              {x - w, y0, z, r, g, b}, {x + w, y1, z, r, g, b}, {x - w, y1, z, r, g, b}};
    v.insert(v.end(), q, q + 6);
}

int main(int argc, char** argv) {
    long frames = -1;
    bool shadow = false;
    int vw = 0, vh = 0;  // --view
    std::string title = "vrdual-gltest";
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atol(argv[++i]);
        else if (!strcmp(argv[i], "--title") && i + 1 < argc) title = argv[++i];
        else if (!strcmp(argv[i], "--shadow")) shadow = true;
        else if (!strcmp(argv[i], "--view") && i + 2 < argc) vw = atoi(argv[++i]), vh = atoi(argv[++i]);
    }
    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) { fprintf(stderr, "no X display\n"); return 1; }
    int attr[] = {GLX_RGBA, GLX_DOUBLEBUFFER, GLX_DEPTH_SIZE, 24, None};
    XVisualInfo* vi = glXChooseVisual(dpy, DefaultScreen(dpy), attr);
    if (!vi) { fprintf(stderr, "no GLX visual\n"); return 1; }
    XSetWindowAttributes swa{};
    swa.colormap = XCreateColormap(dpy, RootWindow(dpy, vi->screen), vi->visual, AllocNone);
    swa.event_mask = StructureNotifyMask;
    Window win = XCreateWindow(dpy, RootWindow(dpy, vi->screen), 0, 0, 960, 720, 0, vi->depth, InputOutput, vi->visual,
                               CWColormap | CWEventMask, &swa);
    XStoreName(dpy, win, title.c_str());
    XMapWindow(dpy, win);
    GLXContext ctx = glXCreateContext(dpy, vi, nullptr, True);
    if (!ctx || !glXMakeCurrent(dpy, win, ctx)) { fprintf(stderr, "GL context failed\n"); return 1; }
#define L(n) n##_ = (decltype(n##_))glXGetProcAddress((const GLubyte*)#n)
    L(glCreateShader); L(glShaderSource); L(glCompileShader); L(glCreateProgram); L(glAttachShader); L(glLinkProgram);
    L(glUseProgram); L(glGenBuffers); L(glBindBuffer); L(glBufferData); L(glBufferSubData); L(glGetUniformLocation); L(glUniform3f);
    L(glGenFramebuffers); L(glBindFramebuffer); L(glFramebufferTexture2D); L(glGenRenderbuffers); L(glBindRenderbuffer);
    L(glRenderbufferStorage); L(glFramebufferRenderbuffer); L(glCheckFramebufferStatus); L(glBlitFramebuffer);
#undef L
    fprintf(stderr, "vrdual-gltest: GL_RENDERER %s\n", (const char*)glGetString(GL_RENDERER));

    GLuint vs = glCreateShader_(GL_VERTEX_SHADER), fs = glCreateShader_(GL_FRAGMENT_SHADER), prog = glCreateProgram_();
    glShaderSource_(vs, 1, &kVs, nullptr);
    glShaderSource_(fs, 1, &kFs, nullptr);
    glCompileShader_(vs);
    glCompileShader_(fs);
    glAttachShader_(prog, vs);
    glAttachShader_(prog, fs);
    glLinkProgram_(prog);
    GLint tint = glGetUniformLocation_(prog, "tint");
    GLuint vs2 = glCreateShader_(GL_VERTEX_SHADER), prog2 = glCreateProgram_();
    glShaderSource_(vs2, 1, &kVs2, nullptr);
    glCompileShader_(vs2);
    glAttachShader_(prog2, vs2);
    glAttachShader_(prog2, fs);
    glLinkProgram_(prog2);
    GLint tint2 = glGetUniformLocation_(prog2, "tint");

    std::vector<V> v;
    const float d[5] = {0.5f, 1, 2, 5, 20};
    const float col[5][3] = {{0.84f, 0.5f, 0.45f}, {0.85f, 0.73f, 0.43f}, {0.53f, 0.78f, 0.55f}, {0.47f, 0.66f, 0.84f}, {0.76f, 0.6f, 0.85f}};
    for (int i = 0; i < 5; ++i) post(v, (i - 2) * 0.25f * d[i], d[i], col[i][0], col[i][1], col[i][2]);
    GLuint vbo;
    glGenBuffers_(1, &vbo);
    glBindBuffer_(GL_ARRAY_BUFFER, vbo);
    glBufferData_(GL_ARRAY_BUFFER, v.size() * sizeof(V), nullptr, GL_STREAM_DRAW);

    // Small texture re-uploaded every frame (streaming path, mapped memory).
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    std::vector<uint32_t> px(64 * 64);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());

    // colour texture + depth renderbuffer FBO of w x h
    auto make_fbo = [&](int w, int h, GLuint& ctex) {
        glGenTextures(1, &ctex);
        glBindTexture(GL_TEXTURE_2D, ctex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        GLuint rb, f;
        glGenRenderbuffers_(1, &rb);
        glBindRenderbuffer_(GL_RENDERBUFFER, rb);
        glRenderbufferStorage_(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
        glGenFramebuffers_(1, &f);
        glBindFramebuffer_(GL_FRAMEBUFFER, f);
        glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ctex, 0);
        glFramebufferRenderbuffer_(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rb);
        if (glCheckFramebufferStatus_(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { fprintf(stderr, "vrdual-gltest: FBO incomplete\n"); exit(1); }
        glBindFramebuffer_(GL_FRAMEBUFFER, 0);
        glBindTexture(GL_TEXTURE_2D, tex);
        return f;
    };
    const int sw = vw ? vw : 512, sh = vw ? vh : 512;  // shadow FBO size
    GLuint fbo = 0, stex = 0, vfbo = 0, vtex = 0;
    if (shadow) fbo = make_fbo(sw, sh, stex);
    if (vw) vfbo = make_fbo(vw, vh, vtex);
    const int W = vw ? vw : 960, H = vw ? vh : 720;  // the 3D view's size

    glEnable(GL_DEPTH_TEST);
    for (long f = 0; frames < 0 || f < frames; ++f) {
        while (XPending(dpy)) { XEvent e; XNextEvent(dpy, &e); }
        glBindFramebuffer_(GL_FRAMEBUFFER, vfbo);
        glViewport(0, 0, W, H);
        glClearColor(0.25f, 0.27f, 0.33f, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        double n = 0.1, fa = 100, t = n;  // 90 deg horizontal FOV, 4:3
        glFrustum(-t, t, -t * 0.75, t * 0.75, n, fa);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();

        // geometry streamed every frame through glBufferSubData
        glBufferSubData_(GL_ARRAY_BUFFER, 0, v.size() * sizeof(V), v.data());
        glUseProgram_(prog);
        glUniform3f_(tint, 1, 1, 1);
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_COLOR_ARRAY);
        glVertexPointer(3, GL_FLOAT, sizeof(V), (void*)0);
        glColorPointer(3, GL_FLOAT, sizeof(V), (void*)12);
        if (shadow) {  // the near post (first 6 vertices) into the FBO, same shader and matrices
            glBindFramebuffer_(GL_FRAMEBUFFER, fbo);
            glViewport(0, 0, sw, sh);
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            glDrawArrays(GL_TRIANGLES, 0, 6);
            glBindFramebuffer_(GL_FRAMEBUFFER, vfbo);
            glViewport(0, 0, W, H);
        }
        if (vw) {  // near post with the shadow pass's shader, the rest with another
            glDrawArrays(GL_TRIANGLES, 0, 6);
            glUseProgram_(prog2);
            glUniform3f_(tint2, 1, 1, 1);
            glDrawArrays(GL_TRIANGLES, 6, GLsizei(v.size() - 6));
        } else {
            glDrawArrays(GL_TRIANGLES, 0, GLsizei(v.size()));
        }
        glDisableClientState(GL_COLOR_ARRAY);
        glDisableClientState(GL_VERTEX_ARRAY);

        // fixed function: textured floor bar in immediate mode, 2 m away
        glUseProgram_(0);
        for (int i = 0; i < 64 * 64; ++i) px[i] = 0xff999999u ^ (((i + f) & 8) ? 0x00080808u : 0);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        glEnable(GL_TEXTURE_2D);
        glColor3f(1, 1, 1);
        glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex3f(-1.6f, -0.62f, -2);
        glTexCoord2f(1, 0); glVertex3f(1.6f, -0.62f, -2);
        glTexCoord2f(1, 1); glVertex3f(1.6f, -0.5f, -2);
        glTexCoord2f(0, 1); glVertex3f(-1.6f, -0.5f, -2);
        glEnd();
        if (vfbo) {  // the 3D view to the window
            glBindFramebuffer_(GL_READ_FRAMEBUFFER, vfbo);
            glBindFramebuffer_(GL_DRAW_FRAMEBUFFER, 0);
            glBlitFramebuffer_(0, 0, W, H, 0, 0, 960, 720, GL_COLOR_BUFFER_BIT, GL_LINEAR);
            glBindFramebuffer_(GL_FRAMEBUFFER, 0);
            glViewport(0, 0, 960, 720);
        }
        if (shadow) {  // the FBO as a w == 1 quad, top left 256x256 px, over everything
            glBindTexture(GL_TEXTURE_2D, stex);
            glMatrixMode(GL_PROJECTION);
            glLoadIdentity();
            glMatrixMode(GL_MODELVIEW);
            glLoadIdentity();
            glDisable(GL_DEPTH_TEST);
            float x1 = -1 + 512.0f / 960, y0 = 1 - 512.0f / 720;
            glBegin(GL_QUADS);
            glTexCoord2f(0, 0); glVertex2f(-1, y0);
            glTexCoord2f(1, 0); glVertex2f(x1, y0);
            glTexCoord2f(1, 1); glVertex2f(x1, 1);
            glTexCoord2f(0, 1); glVertex2f(-1, 1);
            glEnd();
            glEnable(GL_DEPTH_TEST);
            glBindTexture(GL_TEXTURE_2D, tex);
        }
        glDisable(GL_TEXTURE_2D);
        glXSwapBuffers(dpy, win);
    }
    glXMakeCurrent(dpy, None, nullptr);
    glXDestroyContext(dpy, ctx);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return 0;
}
