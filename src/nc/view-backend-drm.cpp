/*
 * Copyright (C) 2016 Garmin Ltd.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "nested-compositor.h"

#include "nc-renderer-host.h"
#include "nc-view-display.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <gbm.h>
#include <glib.h>
#include <unordered_map>
#include <unistd.h>
#include <utility>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

namespace NC {
namespace DRM {

template<class P, class M>
P* container_of(M* ptr, const M P::*member) {
    return reinterpret_cast<P*>(reinterpret_cast<char*>(ptr) -
            (size_t)&(reinterpret_cast<P*>(0)->*member));
}

template<typename T>
struct Cleanup {
    ~Cleanup()
    {
        if (valid)
            cleanup();
    }

    T cleanup;
    bool valid;
};

template<typename T>
auto defer(T&& cleanup) -> Cleanup<T>
{
    return Cleanup<T>{ std::forward<T>(cleanup), true };
}

class EventSource {
public:
    static GSourceFuncs sourceFuncs;

    GSource source;
    GPollFD pfd;

    int drmFD;
    drmEventContext eventContext;
};

GSourceFuncs EventSource::sourceFuncs = {
    nullptr, // prepare
    // check
    [](GSource* base) -> gboolean
    {
        auto* source = reinterpret_cast<EventSource*>(base);
        return !!source->pfd.revents;
    },
    // dispatch
    [](GSource* base, GSourceFunc, gpointer) -> gboolean
    {
        auto* source = reinterpret_cast<EventSource*>(base);

        if (source->pfd.revents & G_IO_IN)
            drmHandleEvent(source->drmFD, &source->eventContext);

        if (source->pfd.revents & (G_IO_ERR | G_IO_HUP))
            return FALSE;

        source->pfd.revents = 0;
        return TRUE;
    },
    nullptr, // finalize
    nullptr, // closure_callback
    nullptr, // closure_marshall
};

class ViewBackend;

struct PageFlipHandlerData {
    ViewBackend* backend;
};

class FBInfo: public NC::ViewDisplay::Resource::DestroyListener {
public:
    FBInfo(struct gbm_bo*);
    ~FBInfo();

    struct gbm_bo* getBo() const {return m_bo;}

    uint32_t getFBID() const {return m_fb_id;}

    void setSurface(NC::ViewDisplay::Surface*);
    void setBuffer(NC::ViewDisplay::Buffer*);

    void releaseBuffer();
    void sendFrameCallback(uint32_t);

    virtual void onResourceDestroyed(const NC::ViewDisplay::Resource&);

private:
    struct gbm_bo* m_bo {nullptr};
    uint32_t m_fb_id {0};
    NC::ViewDisplay::Buffer* m_buffer { nullptr };
    NC::ViewDisplay::Surface* m_surface { nullptr };
};

class ViewBackend: public NC::ViewDisplay::Client {
public:
    class Surface: public NC::ViewDisplay::Surface {
    public:
        Surface(ViewBackend& backend, struct wl_resource* resource,
                NC::ViewDisplay& view, NC::ViewDisplay::Surface::Type type)
            : NC::ViewDisplay::Surface(resource, view, type)
            , m_backend(backend)
            { }

        virtual ~Surface()
        { }

    protected:
        virtual void onSurfaceCommit(const NC::ViewDisplay::Surface::CommitState&) override;

    private:
        ViewBackend& m_backend;
    };

    ViewBackend(struct wpe_view_backend*);
    virtual ~ViewBackend();

    virtual NC::ViewDisplay::Surface* createSurface(struct wl_resource*, NC::ViewDisplay&, NC::ViewDisplay::Surface::Type) override;

    void flipBuffer(NC::ViewDisplay::Buffer*, Surface*);

    struct wpe_view_backend* backend;

    struct {
        int fd { -1 };
        drmModeModeInfo* mode { nullptr };
        std::pair<uint16_t, uint16_t> size;
        uint32_t crtcId { 0 };
        uint32_t connectorId { 0 };
    } m_drm;

    struct {
        int fd { -1 };
        struct gbm_device* device;
        struct gbm_surface* surface;
    } m_gbm;

    struct {
        GSource* source;
        struct gbm_bo* current_bo {nullptr};
        struct gbm_bo* next_bo {nullptr};
    } m_display;

    struct {
        PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES;
        PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR;
        PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR;
        PFNEGLBINDWAYLANDDISPLAYWL eglBindWaylandDisplayWL;
        PFNEGLUNBINDWAYLANDDISPLAYWL eglUnbindWaylandDisplayWL;
        PFNEGLQUERYWAYLANDBUFFERWL eglQueryWaylandBufferWL;

        EGLDisplay display;
        EGLContext context;
        EGLSurface surface;
        EGLImageKHR egl_image {EGL_NO_IMAGE_KHR};

        GLuint tex_id;
    } m_egl;

    NC::ViewDisplay m_viewDisplay;

    static void pageFlipHandler(int, unsigned, unsigned, unsigned, void*);

    static struct FBInfo* getFBInfo(struct gbm_bo*);
    static void boDestroyed(struct gbm_bo*, void*);
};

void ViewBackend::pageFlipHandler(int, unsigned, unsigned sec, unsigned usec, void* data)
{
    auto& backend = *static_cast<ViewBackend*>(data);

    if (backend.m_display.current_bo) {
        auto* fb = getFBInfo(backend.m_display.current_bo);
        fb->releaseBuffer();
        gbm_surface_release_buffer(backend.m_gbm.surface, backend.m_display.current_bo);
    }

    backend.m_display.current_bo = backend.m_display.next_bo;
    backend.m_display.next_bo = nullptr;

    if (backend.m_display.current_bo) {
        auto* fb = getFBInfo(backend.m_display.current_bo);
        fb->sendFrameCallback((sec * 1000) + (usec / 1000));
    }
}


static GLuint LoadShader(const char *shaderSrc, GLenum type)
{
    GLuint shader;
    GLint compiled;

    shader = glCreateShader(type);
    if(shader == 0)
        return 0;

    glShaderSource(shader, 1, &shaderSrc, nullptr);

    glCompileShader(shader);
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if(!compiled) {
        GLint infoLen = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &infoLen);

        if(infoLen > 1) {
            char infoLog[infoLen];
            glGetShaderInfoLog(shader, infoLen, nullptr, infoLog);
            fprintf(stderr, "Error compiling shader:\n%s\n", infoLog);
        }
        glDeleteShader(shader);
        fprintf(stderr, "Cannot create shader\n");
        abort();
    }
    return shader;
}

template<class T>
static void bindEGLproc(T& p, char const* name)
{
    p = reinterpret_cast<T>(eglGetProcAddress(name));
    if (!p) {
        fprintf(stderr, "Cannot bind %s\n", name);
        abort();
    }
}

ViewBackend::ViewBackend(struct wpe_view_backend* backend)
    : backend(backend)
    , m_viewDisplay(this)
{
    decltype(m_drm) drm;
    auto drmCleanup = defer(
        [&drm] {
            if (drm.mode)
                drmModeFreeModeInfo(drm.mode);
            if (drm.fd >= 0)
                close(drm.fd);
        });

    // FIXME: This path should be retrieved via udev.
    const char* renderCard = getenv("WPE_RENDER_CARD");
    if (!renderCard)
        renderCard = "/dev/dri/card0";
    drm.fd = open(renderCard, O_RDWR | O_CLOEXEC);
    if (drm.fd < 0) {
        fprintf(stderr, "ViewBackend: couldn't connect DRM to card %s\n", renderCard);
        return;
    }

    drm_magic_t magic;
    if (drmGetMagic(drm.fd, &magic) || drmAuthMagic(drm.fd, magic)) {
        fprintf(stderr, "Cannot authenticate DRM device\n");
        return;
    }

    decltype(m_gbm) gbm;
    auto gbmCleanup = defer(
        [&gbm] {
            if (gbm.device)
                gbm_device_destroy(gbm.device);
            if (gbm.fd >= 0)
                close(gbm.fd);
        });

    gbm.device = gbm_create_device(drm.fd);
    if (!gbm.device) {
        fprintf(stderr, "Cannot create GBM device\n");
        return;
    }

    drmModeRes* resources = drmModeGetResources(drm.fd);
    if (!resources) {
        fprintf(stderr, "Cannot get DRM resource\n");
        return;
    }
    auto drmResourcesCleanup = defer([&resources] { drmModeFreeResources(resources); });

    drmModeConnector* connector = nullptr;
    for (int i = 0; i < resources->count_connectors; ++i) {
        connector = drmModeGetConnector(drm.fd, resources->connectors[i]);
        if (connector->connection == DRM_MODE_CONNECTED)
            break;

        drmModeFreeConnector(connector);
    }

    if (!connector) {
        fprintf(stderr, "No DRM connector\n");
        return;
    }
    auto drmConnectorCleanup = defer([&connector] { drmModeFreeConnector(connector); });

    int area = 0;
    for (int i = 0; i < connector->count_modes; ++i) {
        drmModeModeInfo* currentMode = &connector->modes[i];
        int modeArea = currentMode->hdisplay * currentMode->vdisplay;
        if (modeArea > area) {
            drm.mode = currentMode;
            drm.size = { currentMode->hdisplay, currentMode->vdisplay };
            area = modeArea;
        }
    }

    if (!drm.mode) {
        fprintf(stderr, "No DRM mode\n");
        return;
    }

    drmModeEncoder* encoder = nullptr;
    for (int i = 0; i < resources->count_encoders; ++i) {
        encoder = drmModeGetEncoder(drm.fd, resources->encoders[i]);
        if (encoder->encoder_id == connector->encoder_id)
            break;

        drmModeFreeEncoder(encoder);
    }

    if (!encoder) {
        fprintf(stderr, "No DRM encoder\n");
        return;
    }
    auto drmEncoderCleanup = defer([&encoder] { drmModeFreeEncoder(encoder); });

    drm.crtcId = encoder->crtc_id;
    drm.connectorId = connector->connector_id;

    gbm.surface = gbm_surface_create(gbm.device, drm.size.first, drm.size.second,
            GBM_FORMAT_ARGB8888, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);

    m_drm = drm;
    drmCleanup.valid = false;
    m_gbm = gbm;
    gbmCleanup.valid = false;
    fprintf(stderr, "ViewBackend: successfully initialized DRM.\n");

    m_display.source = g_source_new(&DRM::EventSource::sourceFuncs, sizeof(DRM::EventSource));
    auto* displaySource = reinterpret_cast<DRM::EventSource*>(m_display.source);
    displaySource->drmFD = m_drm.fd;
    displaySource->eventContext = {
        DRM_EVENT_CONTEXT_VERSION,
        nullptr,
        &pageFlipHandler
    };

    displaySource->pfd.fd = m_drm.fd;
    displaySource->pfd.events = G_IO_IN | G_IO_ERR | G_IO_HUP;
    displaySource->pfd.revents = 0;
    g_source_add_poll(m_display.source, &displaySource->pfd);

    g_source_set_name(m_display.source, "[WPE] DRM");
    g_source_set_priority(m_display.source, G_PRIORITY_HIGH + 30);
    g_source_set_can_recurse(m_display.source, TRUE);
    g_source_attach(m_display.source, g_main_context_get_thread_default());

    auto& server = ::NC::RendererHost::singleton();
    server.initialize();

    m_viewDisplay.initialize(server.display());

    m_egl.display = eglGetDisplay(reinterpret_cast<NativeDisplayType>(m_gbm.device));

    if (m_egl.display == EGL_NO_DISPLAY) {
        fprintf(stderr, "Cannot create EGL display\n");
        abort();
    }

    EGLint major, minor;
    EGLBoolean ret;
    ret = eglInitialize(m_egl.display, &major, &minor);
    if (!ret) {
        fprintf(stderr, "Cannot initialize EGL\n");
        abort();
    }

    eglBindAPI(EGL_OPENGL_ES_API);

    EGLConfig config;
    EGLint numConfigs = 0;

    static const EGLint g_configAttr[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_NONE
    };

    static const EGLint g_attributes[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };

    ret = eglChooseConfig(m_egl.display, g_configAttr, &config, 1, &numConfigs);

    if (!ret || numConfigs == 0) {
        fprintf(stderr, "Cannot find a suitable EGL config\n");
        abort();
    }

    m_egl.context = eglCreateContext(m_egl.display, config, EGL_NO_CONTEXT, g_attributes);
    if (m_egl.context == EGL_NO_CONTEXT) {
        fprintf(stderr, "Cannot create EGL context: %i\n", eglGetError());
        abort();
    }

    m_egl.surface = eglCreateWindowSurface(m_egl.display, config, (EGLNativeWindowType)gbm.surface, nullptr);

    if (m_egl.surface == EGL_NO_SURFACE) {
        fprintf(stderr, "Cannot create EGL surface\n");
        abort();
    }

    eglMakeCurrent(m_egl.display, m_egl.surface, m_egl.surface, m_egl.context);

    static const char vShaderStr[] =
        "attribute vec4 vPosition;\n"
        "attribute vec2 aTexCoord;\n"
        "varying vec2 vTexCoord;\n"
        "void main() {\n"
        "   gl_Position = vPosition;\n"
        "   vTexCoord = aTexCoord;\n"
        "}\n";

    static char const fShaderStr[] =
        "#version 100\n"
        "precision mediump float;\n"
        "uniform sampler2D u_texture;\n"
        "varying vec2 vTexCoord;\n"
        "void main() {\n"
        "   gl_FragColor = texture2D(u_texture, vTexCoord);\n"
        "}\n";

    GLuint vertexShader = LoadShader(vShaderStr, GL_VERTEX_SHADER);
    GLuint fragShader = LoadShader(fShaderStr, GL_FRAGMENT_SHADER);

    GLuint program = glCreateProgram();
    glAttachShader(program, vertexShader);
    glAttachShader(program, fragShader);

    glBindAttribLocation(program, 0, "vPosition");
    glBindAttribLocation(program, 1, "aTexCoord");
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);

    glLinkProgram(program);

    GLint linked;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if(!linked) {
        GLint infoLen = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &infoLen);

        if(infoLen > 1) {
            char infoLog[infoLen];
            glGetProgramInfoLog(program, infoLen, nullptr, infoLog);
            fprintf(stderr, "Error linking program:\n%s\n", infoLog);
        }
        glDeleteProgram(program);
        fprintf(stderr, "Cannot link program\n");
        abort();
    }

    glUseProgram(program);

    glGenTextures(1, &m_egl.tex_id);
    glBindTexture(GL_TEXTURE_2D, m_egl.tex_id);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

    glViewport(0, 0, drm.size.first, drm.size.second);

    bindEGLproc(m_egl.glEGLImageTargetTexture2DOES, "glEGLImageTargetTexture2DOES");
    bindEGLproc(m_egl.eglCreateImageKHR, "eglCreateImageKHR");
    bindEGLproc(m_egl.eglDestroyImageKHR, "eglDestroyImageKHR");
    bindEGLproc(m_egl.eglBindWaylandDisplayWL, "eglBindWaylandDisplayWL");
    bindEGLproc(m_egl.eglUnbindWaylandDisplayWL, "eglUnbindWaylandDisplayWL");
    bindEGLproc(m_egl.eglQueryWaylandBufferWL, "eglQueryWaylandBufferWL");

    ret = m_egl.eglBindWaylandDisplayWL(m_egl.display, server.display());

    if (!ret) {
        fprintf(stderr, "Cannot bind Wayland display to EGL\n");
        abort();
    }

    glClear(GL_COLOR_BUFFER_BIT);
    eglSwapBuffers(m_egl.display, m_egl.surface);

    m_display.current_bo = gbm_surface_lock_front_buffer(m_gbm.surface);
    auto* fb = getFBInfo(m_display.current_bo);

    drmModeSetCrtc(m_drm.fd, m_drm.crtcId, fb->getFBID(), 0, 0, &m_drm.connectorId, 1, m_drm.mode);
}

ViewBackend::~ViewBackend()
{
    if (m_display.source)
        g_source_unref(m_display.source);
    m_display.source = nullptr;

    if (m_gbm.device)
        gbm_device_destroy(m_gbm.device);
    m_gbm = { };

    if (m_drm.mode)
        drmModeFreeModeInfo(m_drm.mode);
    if (m_drm.fd >= 0)
        close(m_drm.fd);
    m_drm = { };

    eglMakeCurrent(m_egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(m_egl.display, m_egl.surface);
    eglDestroyContext(m_egl.display, m_egl.context);
}

NC::ViewDisplay::Surface* ViewBackend::createSurface(struct wl_resource* resource, NC::ViewDisplay& view, NC::ViewDisplay::Surface::Type type)
{
    return new Surface(*this, resource, view, type);
}

void ViewBackend::flipBuffer(NC::ViewDisplay::Buffer* buffer, ViewBackend::Surface* surface)
{
    if (m_display.next_bo) {
        fprintf(stderr, "Page flip already pending\n");
        abort();
    }

    if (m_egl.egl_image != EGL_NO_IMAGE_KHR) {
        m_egl.eglDestroyImageKHR(m_egl.display, m_egl.egl_image);
    }

    m_egl.egl_image = m_egl.eglCreateImageKHR(m_egl.display,
            nullptr, EGL_WAYLAND_BUFFER_WL, buffer->resource(), nullptr);

    if (m_egl.egl_image == EGL_NO_IMAGE_KHR) {
        fprintf(stderr, "Cannot create EGL image: %i\n", eglGetError());
        return;
    }

    m_egl.glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, m_egl.egl_image);

    glClear(GL_COLOR_BUFFER_BIT);

    static const GLfloat vertices[] = {
        -1.0f, 1.0f, 0.0f,
        1.0f, 1.0f, 0.0f,
        -1.0f, -1.0f, 0.0f,
        1.0f, -1.0f, 0.0f,
    };

    static const GLfloat texCoord[] = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        0.0f, 1.0f,
        1.0f, 1.0f
    };

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, vertices);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, texCoord);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    eglSwapBuffers(m_egl.display, m_egl.surface);

    m_display.next_bo = gbm_surface_lock_front_buffer(m_gbm.surface);

    auto* fb = getFBInfo(m_display.next_bo);
    fb->setBuffer(buffer);
    fb->setSurface(surface);

    drmModePageFlip(m_drm.fd, m_drm.crtcId, fb->getFBID(),
            DRM_MODE_PAGE_FLIP_EVENT, this);
}

void ViewBackend::Surface::onSurfaceCommit(const NC::ViewDisplay::Surface::CommitState& state)
{
    if (state.bufferAttached && state.buffer && type() == NC::ViewDisplay::Surface::OnScreen)
        m_backend.flipBuffer(state.buffer, this);
}

void ViewBackend::boDestroyed(struct gbm_bo* bo, void* data)
{
    auto* fb = static_cast<FBInfo*>(data);
    delete fb;
}

struct FBInfo* ViewBackend::getFBInfo(struct gbm_bo* bo)
{
    auto* info = static_cast<FBInfo*>(gbm_bo_get_user_data(bo));

    if (info)
        return info;

    auto* fb = new FBInfo(bo);

    gbm_bo_set_user_data(bo, fb, boDestroyed);

    return fb;
}

FBInfo::FBInfo(struct gbm_bo* bo)
    : m_bo(bo)
{
    struct gbm_device* device = gbm_bo_get_device(bo);

    uint32_t width = gbm_bo_get_width(bo);
    uint32_t height = gbm_bo_get_height(bo);
    uint32_t stride = gbm_bo_get_stride(bo);
    uint32_t handle = gbm_bo_get_handle(bo).u32;

    int ret = drmModeAddFB(gbm_device_get_fd(device), width, height, 24, 32,
            stride, handle, &m_fb_id);

    if (ret) {
        fprintf(stderr, "Failed to create buffer: %s\n", strerror(errno));
    }
}

FBInfo::~FBInfo()
{
    struct gbm_device* device = gbm_bo_get_device(m_bo);

    setBuffer(nullptr);
    setSurface(nullptr);

    if (m_fb_id)
        drmModeRmFB(gbm_device_get_fd(device), m_fb_id);
}

void FBInfo::setSurface(NC::ViewDisplay::Surface* surface)
{
    if (m_surface) {
        m_surface->removeDestroyListener(this);
        m_surface = nullptr;
    }

    if (surface) {
        m_surface = surface;
        m_surface->addDestroyListener(this);
    }
}

void FBInfo::setBuffer(NC::ViewDisplay::Buffer* buffer)
{
    if (m_buffer) {
        m_buffer->removeDestroyListener(this);
        m_buffer = nullptr;
    }

    if (buffer) {
        m_buffer = buffer;
        m_buffer->addDestroyListener(this);
    }
}

void FBInfo::releaseBuffer()
{
    if (m_buffer) {
        wl_buffer_send_release(m_buffer->resource());
        setBuffer(nullptr);
    }
}

void FBInfo::sendFrameCallback(uint32_t t)
{
    if (m_surface) {
        m_surface->frameComplete(t);
        setSurface(nullptr);
    }
}

void FBInfo::onResourceDestroyed(const NC::ViewDisplay::Resource& resource)
{
    if (&resource == static_cast<NC::ViewDisplay::Resource*>(m_surface))
        setSurface(nullptr);

    if (&resource == static_cast<NC::ViewDisplay::Resource*>(m_buffer))
        setBuffer(nullptr);
}

} // namespace DRM
} // namespace NC

extern "C" {

struct wpe_view_backend_interface nc_view_backend_drm_interface = {
    // create
    [](void*, struct wpe_view_backend* backend) -> void*
    {
        return new NC::DRM::ViewBackend(backend);
    },
    // destroy
    [](void* data)
    {
        auto* backend = static_cast<NC::DRM::ViewBackend*>(data);
        delete backend;
    },
    // initialize
    [](void* data)
    {
        auto* backend = static_cast<NC::DRM::ViewBackend*>(data);
        wpe_view_backend_dispatch_set_size(backend->backend,
            backend->m_drm.size.first, backend->m_drm.size.second);
    },
    // get_renderer_host_fd
    [](void* data) -> int
    {
        return -1;
    },
};

}

