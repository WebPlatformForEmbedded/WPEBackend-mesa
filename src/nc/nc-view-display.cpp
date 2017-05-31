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

#include "nc-view-display.h"

#include "nc-renderer-host.h"
#include <cstring>
#include <cstdio>
#include <wayland-server.h>
#include "wpe-mesa-server-protocol.h"

namespace NC {

template<class P, class M>
P* container_of(M* ptr, const M P::*member) {
    return reinterpret_cast<P*>(reinterpret_cast<char*>(ptr) -
            (size_t)&(reinterpret_cast<P*>(0)->*member));
}

template<class C>
static C const* getResource(C const* r)
{
    if (r && r->resource()) {
        return r;
    }
    return nullptr;
}

static void unsupportedOperation(struct wl_resource* resource)
{
    fprintf(stderr, "Unsupported Wayland operation\n");
    wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_METHOD, "Unsupported method");
}

ViewDisplay::ViewDisplay(Client* client)
{
    setClient(client);
}

void ViewDisplay::initialize(struct wl_display* display)
{
    static const struct wpe_mesa_compositor_interface compositorInterface[] = {
        // destroy
        [](struct wl_client* client, struct wl_resource* resource)
        {
        },
        // create_surface
        [](struct wl_client* client, struct wl_resource* resource, uint32_t id)
        {
            auto& view = *static_cast<ViewDisplay*>(wl_resource_get_user_data(resource));
            view.createSurface(client, resource, id, Surface::OnScreen);
        },
        // create_offscreen_surface
        [](struct wl_client* client, struct wl_resource* resource, uint32_t id)
        {
            auto& view = *static_cast<ViewDisplay*>(wl_resource_get_user_data(resource));
            view.createSurface(client, resource, id, Surface::OffScreen);
        },
        // render_callback
        [](struct wl_client* client, struct wl_resource* resource, uint32_t id, struct wl_resource* surface_resource)
        {
            auto& view = *static_cast<ViewDisplay*>(wl_resource_get_user_data(resource));
            auto& surface = *static_cast<Surface*>(wl_resource_get_user_data(surface_resource));
            surface.createFrameCallback(client, id, true);
        },
    };

    m_compositor_global = wl_global_create(display, &wpe_mesa_compositor_interface, 1, this,
            [](struct wl_client* client, void* data, uint32_t version, uint32_t id)
            {
                auto& view = *static_cast<ViewDisplay*>(data);

                view.m_compositor = wl_resource_create(client, &wpe_mesa_compositor_interface, version, id);

                if (!view.m_compositor) {
                    wl_client_post_no_memory(client);
                    return;
                }

                wl_resource_set_implementation(view.m_compositor, &compositorInterface, &view,
                    nullptr);
            });
}

ViewDisplay::~ViewDisplay()
{
    if (m_compositor_global)
        wl_global_destroy(m_compositor_global);
}

void ViewDisplay::setClient(ViewDisplay::Client* client)
{
    m_client = client;
}

ViewDisplay::Surface* ViewDisplay::createSurface(struct wl_client* client, struct wl_resource* resource, uint32_t id, Surface::Type type)
{
    struct wl_resource* surface_resource = wl_resource_create(client, &wl_surface_interface, 1, id);

    if (!surface_resource) {
        wl_client_post_no_memory(client);
        return nullptr;
    }

    Surface* surface;
    if (m_client)
        surface = m_client->createSurface(surface_resource, *this, type);
    else
        surface = new Surface(surface_resource, *this, type);

    wl_resource_set_implementation(surface_resource, &Surface::surfaceInterface, surface,
            [](struct wl_resource* resource)
            {
                auto* surface = static_cast<Surface*>(wl_resource_get_user_data(resource));
                delete surface;
            });

    return surface;
}

ViewDisplay::Resource::Resource(struct wl_resource* resource, ViewDisplay& view)
    : m_resource(resource)
    , m_view(view)
{
    memset(&m_listener, 0, sizeof(m_listener));

    m_listener.notify = destroyListener;

    if (resource)
        wl_resource_add_destroy_listener(resource, &m_listener);
}

ViewDisplay::Resource::~Resource()
{
    if (m_resource)
        wl_list_remove(&m_listener.link);
}

void ViewDisplay::Resource::addDestroyListener(ViewDisplay::Resource::DestroyListener* l)
{
    m_destroyListeners.push_back(l);
}

void ViewDisplay::Resource::removeDestroyListener(ViewDisplay::Resource::DestroyListener* l)
{
    m_destroyListeners.remove(l);
}

void ViewDisplay::Resource::destroyListener(struct wl_listener* listener, void*)
{
    auto& r = *container_of(listener, &ViewDisplay::Resource::m_listener);

    for(auto i : r.m_destroyListeners)
        i->onResourceDestroyed(r);

    r.m_resource = nullptr;
    wl_list_remove(&r.m_listener.link);
}


void ViewDisplay::Damage::expand(int32_t x, int32_t y, int32_t w, int32_t h)
{
    if (x < m_x) {
        m_width += m_x - x;
        m_x = x;
    }

    if (y < m_y) {
        m_height += m_y - y;
        m_y = y;
    }

    if (w > m_width)
        m_width = w;

    if (h > m_height)
        m_height = h;
}

ViewDisplay::Surface::~Surface()
{
    clearPending();

    while (!m_current.frameCallbacks.empty()) {
        delete m_pending.frameCallbacks.front();
        m_pending.frameCallbacks.pop_front();
    }
}

void ViewDisplay::Surface::clearPending()
{
    if (m_pending.state.buffer) {
        delete m_pending.state.buffer;
        m_pending.state.buffer = nullptr;
    }
    m_pending.state.bufferAttached = false;

    if (m_pending.state.damage) {
        delete m_pending.state.damage;
        m_pending.state.damage = nullptr;
    }

    while (!m_pending.renderCallbacks.empty()) {
        delete m_pending.renderCallbacks.front();
        m_pending.renderCallbacks.pop_front();
    }

    while (!m_pending.frameCallbacks.empty()) {
        delete m_pending.frameCallbacks.front();
        m_pending.frameCallbacks.pop_front();
    }
}

void ViewDisplay::Surface::createFrameCallback(struct wl_client* client, uint32_t id, bool render)
{
    struct wl_resource* callback_resource = wl_resource_create(client, &wl_callback_interface,
            1, id);

    if (!callback_resource) {
        wl_resource_post_no_memory(resource());
        return;
    }

    if (render)
        m_pending.renderCallbacks.push_back(callback_resource);
    else
        m_pending.frameCallbacks.push_back(callback_resource);

    wl_resource_set_implementation(callback_resource, nullptr, this, frameCallbackDestroy);
}

void ViewDisplay::Surface::frameCallbackDestroy(struct wl_resource* resource)
{
    auto& surface = *static_cast<Surface*>(wl_resource_get_user_data(resource));
    surface.m_pending.frameCallbacks.remove(resource);
    surface.m_pending.renderCallbacks.remove(resource);
    surface.m_current.frameCallbacks.remove(resource);
}

void ViewDisplay::Surface::frameComplete(uint32_t callback_data)
{
    auto& l = m_current.frameCallbacks;

    while (!l.empty()) {
        struct wl_resource* c = l.front();

        wl_callback_send_done(c, callback_data);

        // Note: Destroying the resource will remove it from the list
        wl_resource_destroy(c);
    }
}

const struct wl_surface_interface ViewDisplay::Surface::surfaceInterface = {
    // destroy
    [](struct wl_client*, struct wl_resource* resource)
    {
    },
    // attach
    [](struct wl_client*, struct wl_resource* resource, struct wl_resource* buffer_resource, int32_t x,
            int32_t y)
    {
        auto& surface = *static_cast<Surface*>(wl_resource_get_user_data(resource));

        if (surface.m_pending.state.buffer) {
            delete surface.m_pending.state.buffer;
            surface.m_pending.state.buffer = nullptr;
        }

        if (buffer_resource)
            surface.m_pending.state.buffer = new ViewDisplay::Buffer(buffer_resource, surface.view(), x, y);

        surface.m_pending.state.bufferAttached = true;

        surface.onSurfaceAttach(surface.m_pending.state.buffer);
    },
    // damage
    [](struct wl_client*, struct wl_resource* resource, int32_t x, int32_t y, int32_t width, int32_t height)
    {
        auto& surface = *static_cast<Surface*>(wl_resource_get_user_data(resource));

        if (surface.m_pending.state.damage)
            surface.m_pending.state.damage->expand(x, y, width, height);
        else
            surface.m_pending.state.damage = new Damage(x, y, width, height);
    },
    // frame
    [](struct wl_client* client, struct wl_resource* resource, uint32_t id)
    {
        auto& surface = *static_cast<Surface*>(wl_resource_get_user_data(resource));
        surface.createFrameCallback(client, id);
    },
    // set_opaque_region
    [](struct wl_client*, struct wl_resource* resource, struct wl_resource*)
    {
        unsupportedOperation(resource);
    },
    // set_input_region
    [](struct wl_client*, struct wl_resource* resource, struct wl_resource*)
    {
        unsupportedOperation(resource);
    },
    // commit
    [](struct wl_client*, struct wl_resource* resource)
    {
        auto& surface = *static_cast<Surface*>(wl_resource_get_user_data(resource));
        auto* damage = surface.m_pending.state.damage;

        surface.m_current.frameCallbacks.splice(surface.m_current.frameCallbacks.end(),
                surface.m_pending.frameCallbacks);

        surface.m_current.frameCallbacks.splice(surface.m_current.frameCallbacks.end(),
                surface.m_pending.renderCallbacks);

        surface.onSurfaceCommit(surface.m_pending.state);

        surface.clearPending();
    },
    // set_buffer_transform
    [](struct wl_client*, struct wl_resource* resource, int32_t)
    {
        unsupportedOperation(resource);
    },
    // set_buffer_scale
    [](struct wl_client*, struct wl_resource* resource, int32_t)
    {
        unsupportedOperation(resource);
    }
};



}; // namespace NC
