/*
 * Copyright (C) 2016 Garmin Ltd.
 * All rights reserved.
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

#ifndef nc_view_display_h
#define nc_view_display_h

#include <list>
#include <glib.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

struct wl_display;
struct wl_global;
struct wl_client;
struct wl_resource;

namespace NC {

class ViewDisplay {
public:
    class Resource {
    public:
        class DestroyListener {
        public:
            virtual void onResourceDestroyed(const Resource&) = 0;
        };

        struct wl_resource* resource() const { return m_resource; }

        ViewDisplay& view() const {
            return m_view;
        }

        void addDestroyListener(DestroyListener*);
        void removeDestroyListener(DestroyListener*);

    protected:
        Resource(struct wl_resource* resource, ViewDisplay& view);
        virtual ~Resource();

        ViewDisplay& m_view;

    private:
        struct wl_resource* m_resource;
        struct wl_listener m_listener;
        std::list<DestroyListener*> m_destroyListeners;

        static void destroyListener(struct wl_listener*, void*);
    };

    class Buffer: public Resource {
    public:
        int32_t X() const { return m_x; }
        int32_t Y() const { return m_y; }

        Buffer(struct wl_resource* resource, ViewDisplay& view, int32_t x, int32_t y)
            : Resource(resource, view)
            , m_x(x)
            , m_y(y)
            {}

    private:
        int32_t m_x;
        int32_t m_y;
    };

    class FrameCallback;
    class Damage;

    class Surface: public Resource {
    public:
        enum Type {
            OnScreen,
            OffScreen,
        };

        Surface(struct wl_resource* resource, ViewDisplay& view, Type type)
            : Resource(resource, view)
            , m_type(type)
            {}

        virtual ~Surface();

        void createFrameCallback(struct wl_client* client, uint32_t id, bool render=false);
        void frameComplete(uint32_t callback_data);

        static const struct wl_surface_interface surfaceInterface;

    protected:
        struct CommitState {
            Buffer* buffer;
            bool bufferAttached;
            Damage* damage;
        };

        Type type() const {
            return m_type;
        }

        virtual void onSurfaceAttach(Buffer*) { };
        virtual void onSurfaceCommit(const CommitState&) { };

    private:
        Type m_type;

        struct {
            CommitState state { nullptr, false, nullptr };
            std::list<struct wl_resource*> frameCallbacks;
            std::list<struct wl_resource*> renderCallbacks;
        } m_pending;

        struct {
            std::list<struct wl_resource*> frameCallbacks;
        } m_current;

        void clearPending();

        static void frameCallbackDestroy(struct wl_resource* resource);
    };

    class FrameCallback: public Resource {
    public:
        FrameCallback(struct wl_resource* resource, ViewDisplay& view)
            : Resource(resource, view)
            {}
    };

    class Damage {
    public:
        int32_t X() const { return m_x; }
        int32_t Y() const { return m_y; }
        int32_t width() const { return m_width; }
        int32_t height() const { return m_height; }

        void expand(int32_t, int32_t, int32_t, int32_t);

        Damage(int32_t x, int32_t y, int32_t w, int32_t h)
            : m_x(x)
            , m_y(y)
            , m_width(w)
            , m_height(h)
            {}

    private:
        int32_t m_x;
        int32_t m_y;
        int32_t m_width;
        int32_t m_height;
    };

    class Client {
    public:
        virtual Surface* createSurface(struct wl_resource*, ViewDisplay&, Surface::Type) = 0;
    };

    ViewDisplay(Client*);
    ~ViewDisplay();

    void initialize(struct wl_display*);

    void setClient(Client*);

    Surface* mainSurface() { return m_mainSurface; }

private:
    Client* m_client {nullptr};

    struct wl_global* m_compositor_global {nullptr};
    struct wl_resource* m_compositor {nullptr};
    Surface* m_mainSurface {nullptr};

    Surface* createSurface(struct wl_client* client, struct wl_resource* resource, uint32_t id, Surface::Type type);
};

}; // namespace NC

#endif // nc_view_display


