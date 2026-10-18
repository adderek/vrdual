// vrdual-gltest — small OpenGL 2.1-era app (fixed function + GLSL 1.20 +
// texture uploads), run through Zink to exercise the layer the way old games
// do. Static scene: calibration posts at 0.5/1/2/5/20 m like `vrdual-cube
// --scene calib` (same geometry, so measure_posts.py applies).
//
// Plain Xlib + GLX (like SDL 1.2 games, and it builds as 32-bit too).
//
//   MESA_LOADER_DRIVER_OVERRIDE=zink vrdual-gltest [--frames N] [--title T]
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

static const char* kVs = "#version 120\n"
                         "varying vec3 c;\n"
                         "void main() { c = gl_Color.rgb; gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex; }\n";
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
    std::string title = "vrdual-gltest";
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atol(argv[++i]);
        else if (!strcmp(argv[i], "--title") && i + 1 < argc) title = argv[++i];
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

    glEnable(GL_DEPTH_TEST);
    for (long f = 0; frames < 0 || f < frames; ++f) {
        while (XPending(dpy)) { XEvent e; XNextEvent(dpy, &e); }
        glViewport(0, 0, 960, 720);
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
        glDrawArrays(GL_TRIANGLES, 0, GLsizei(v.size()));
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
        glDisable(GL_TEXTURE_2D);
        glXSwapBuffers(dpy, win);
    }
    glXMakeCurrent(dpy, None, nullptr);
    glXDestroyContext(dpy, ctx);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return 0;
}
