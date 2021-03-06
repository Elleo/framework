/* * This file is part of Maliit framework *
 *
 * Copyright (C) 2013 Openismus GmbH
 *
 * Contact: maliit-discuss@lists.maliit.org
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1 as published by the Free Software Foundation
 * and appearing in the file LICENSE.LGPL included in the packaging
 * of this file.
 */

#include <wayland-client.h>
#include "qwayland-input-method.h"

#include <QDebug>
#include <QGuiApplication>
#include <QRegion>
#include <QVector>
#include <QWindow>
#include <qpa/qplatformnativeinterface.h>

#include "waylandplatform.h"
#include "windowdata.h"

namespace
{
QtWayland::wl_input_panel_surface::position maliitToWestonPosition(Maliit::Position position)
{
    switch (position) {
    case Maliit::PositionCenterBottom:
        return QtWayland::wl_input_panel_surface::position_center_bottom;
    default:
        qWarning() << "Weston only supports center bottom position for top-level surfaces.";

        return QtWayland::wl_input_panel_surface::position_center_bottom;
    }
}
} // unnamed namespace

namespace Maliit
{

class WaylandPlatformPrivate
{
public:
    WaylandPlatformPrivate();
    ~WaylandPlatformPrivate();

    void handleRegistryGlobal(uint32_t name,
                              const char *interface,
                              uint32_t version);
    void handleRegistryGlobalRemove(uint32_t name);
    void setupInputSurface(QWindow *window,
                           Maliit::Position position,
                           bool avoid_crash = false);

    struct wl_registry *m_registry;
    QScopedPointer<QtWayland::wl_input_panel> m_panel;
    uint32_t m_panel_name;
    QVector<WindowData> m_scheduled_windows;
};

namespace {

void registryGlobal(void *data,
                    wl_registry *registry,
                    uint32_t name,
                    const char *interface,
                    uint32_t version)
{
    qDebug() << __PRETTY_FUNCTION__;
    WaylandPlatformPrivate *d = static_cast<WaylandPlatformPrivate *>(data);

    Q_UNUSED(registry);
    d->handleRegistryGlobal(name, interface, version);
}

void registryGlobalRemove(void *data,
                          wl_registry *registry,
                          uint32_t name)
{
    qDebug() << __PRETTY_FUNCTION__;
    WaylandPlatformPrivate *d = static_cast<WaylandPlatformPrivate *>(data);

    Q_UNUSED(registry);
    d->handleRegistryGlobalRemove(name);
}

const wl_registry_listener maliit_registry_listener = {
    registryGlobal,
    registryGlobalRemove
};

} // unnamed namespace

WaylandPlatformPrivate::WaylandPlatformPrivate()
    : m_registry(0),
      m_panel(0),
      m_panel_name(0),
      m_scheduled_windows()
{
    wl_display *display = static_cast<wl_display *>(QGuiApplication::platformNativeInterface()->nativeResourceForIntegration("display"));
    if (!display) {
        qCritical() << __PRETTY_FUNCTION__ << "Failed to get a display.";
        return;
    }
    m_registry = wl_display_get_registry(display);
    wl_registry_add_listener(m_registry, &maliit_registry_listener, this);
    // QtWayland will do dispatching for us.
}

WaylandPlatformPrivate::~WaylandPlatformPrivate()
{
    if (m_registry) {
        wl_registry_destroy (m_registry);
    }
}

void WaylandPlatformPrivate::handleRegistryGlobal(uint32_t name,
                                                  const char *interface,
                                                  uint32_t version)
{
    Q_UNUSED(version);

    qDebug() << __PRETTY_FUNCTION__ << "Name:" << name << "Interface:" << interface;
    if (!strcmp(interface, "wl_input_panel")) {
        m_panel.reset(new QtWayland::wl_input_panel(m_registry, name));
        m_panel_name = name;

        Q_FOREACH (const WindowData& data, m_scheduled_windows) {
            setupInputSurface(data.m_window, data.m_position, true);
        }
        m_scheduled_windows.clear();
    }
}

void WaylandPlatformPrivate::handleRegistryGlobalRemove(uint32_t name)
{
    qDebug() << __PRETTY_FUNCTION__ << "Name:" << name;
    if (m_panel and m_panel_name == name) {
        m_panel.reset();
    }
}

void WaylandPlatformPrivate::setupInputSurface(QWindow *window,
                                               Maliit::Position position,
                                               bool avoid_crash)
{
    struct wl_surface *surface = avoid_crash ? 0 : static_cast<struct wl_surface *>(QGuiApplication::platformNativeInterface()->nativeResourceForWindow("surface", window));

    struct wl_output *output = static_cast<struct wl_output*>(QGuiApplication::platformNativeInterface()->nativeResourceForScreen("output", QGuiApplication::primaryScreen()));

    if (!surface) {
        if (avoid_crash) {
            qDebug() << "Creating surface to avoid crash in QtWayland";
        } else {
            qDebug() << "No wl_surface, calling create.";
        }
        qDebug() << __PRETTY_FUNCTION__ << "WId:" << window->winId();
        window->create();
    }

    surface = static_cast<struct wl_surface *>(QGuiApplication::platformNativeInterface()->nativeResourceForWindow("surface", window));
    qDebug() << __PRETTY_FUNCTION__ << "WId:" << window->winId();
    if (!surface) {
        qDebug() << __PRETTY_FUNCTION__ << "Still no surface, giving up.";
        return;
    }

    struct wl_input_panel_surface *ip_surface = m_panel->get_input_panel_surface(surface);
    QtWayland::wl_input_panel_surface::position weston_position = maliitToWestonPosition(position);

    wl_input_panel_surface_set_toplevel(ip_surface, output, weston_position);
}

WaylandPlatform::WaylandPlatform()
    : d_ptr(new WaylandPlatformPrivate)
{}

void WaylandPlatform::setupInputPanel(QWindow* window,
                                      Maliit::Position position)
{
    // we ignore non toplevel surfaces
    if (not window or window->parent()) {
        return;
    }

    Q_D(WaylandPlatform);

    // If we have an input panel, we set up the surface now. Otherwise we
    // schedule it for later.
    if (d->m_panel) {
        d->setupInputSurface(window, position);
    } else {
        d->m_scheduled_windows.append(WindowData(window, position));
    }
}

void WaylandPlatform::setInputRegion(QWindow* window,
                                     const QRegion& region)
{
    if (not window) {
        return;
    }

    QPlatformNativeInterface *wliface = QGuiApplication::platformNativeInterface();
    wl_compositor *wlcompositor = static_cast<wl_compositor *>(wliface->nativeResourceForIntegration("compositor"));
    wl_region *wlregion = wl_compositor_create_region(wlcompositor);

    Q_FOREACH (const QRect &rect, region.rects()) {
        wl_region_add(wlregion, rect.x(), rect.y(),
                      rect.width(), rect.height());
    }

    wl_surface *wlsurface = static_cast<wl_surface *>(wliface->nativeResourceForWindow("surface", window));
    wl_surface_set_input_region(wlsurface, wlregion);
    wl_region_destroy(wlregion);
}

} // namespace Maliit
