/*
 * Copyright (C) 2015, 2016 Igalia S.L.
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

#ifndef nested_compositor_h
#define nested_compositor_h

#define WL_EGL_PLATFORM

#include <wpe/wpe-egl.h>
#include <wpe/wpe.h>

#ifdef __cplusplus
extern "C" {
#endif

extern struct wpe_renderer_backend_egl_interface nc_renderer_backend_egl_interface;
extern struct wpe_renderer_backend_egl_target_interface nc_renderer_backend_egl_target_interface;
extern struct wpe_renderer_backend_egl_offscreen_target_interface nc_renderer_backend_egl_offscreen_target_interface;

extern struct wpe_renderer_host_interface nc_renderer_host_interface;

extern struct wpe_view_backend_interface nc_view_backend_drm_interface;
extern struct wpe_view_backend_interface nc_view_backend_wayland_interface;

#ifdef __cplusplus
}
#endif

#endif // nested_compositor_h
