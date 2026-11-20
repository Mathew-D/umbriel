#pragma once

#include "wlr.h"

#include <wayland-server-core.h>

namespace umbriel {

  // zxdg_decoration_manager_v1 / zxdg_toplevel_decoration_v1, protocol version 2.
  // wlroots-0.20 implements the same wire format but only advertises version 1
  // and rejects decoration objects created after the initial commit, which
  // version 2 clients such as Wine's Wayland driver rely on.

  enum XdgDecorationMode : uint32_t {
    XdgDecorationModeNone = 0,
    XdgDecorationModeClientSide = 1,
    XdgDecorationModeServerSide = 2,
  };

  struct XdgDecorationState {
    XdgDecorationMode mode = XdgDecorationModeNone;
  };

  struct XdgDecorationConfigure {
    wlr_xdg_surface_configure* surfaceConfigure = nullptr;
    XdgDecorationMode mode = XdgDecorationModeNone;
    wl_list link{};
  };

  struct XdgDecorationManager {
    wl_global* global = nullptr;
    wl_list decorations{}; // XdgToplevelDecoration.link
    struct {
      wl_signal newToplevelDecoration; // XdgToplevelDecoration*
      wl_signal destroy;
    } events{};
  };

  struct XdgToplevelDecoration {
    wl_resource* resource = nullptr;
    wlr_xdg_toplevel* toplevel = nullptr;
    XdgDecorationManager* manager = nullptr;
    wl_list link{}; // XdgDecorationManager.decorations

    XdgDecorationState current, pending;

    XdgDecorationMode scheduledMode = XdgDecorationModeNone;
    XdgDecorationMode requestedMode = XdgDecorationModeNone;

    wl_list configureList{}; // XdgDecorationConfigure.link

    struct {
      wl_signal destroy;
      wl_signal requestMode; // XdgToplevelDecoration*
    } events{};

    void* data = nullptr;

    wl_listener toplevelDestroy{};
    wl_listener surfaceConfigure{};
    wl_listener surfaceAckConfigure{};

    wlr_surface_synced synced{};
  };

  // Creates the zxdg_decoration_manager_v1 global at version 2. Returns nullptr on failure.
  XdgDecorationManager* xdgDecorationManagerCreate(wl_display* display);
  void xdgDecorationManagerDestroy(XdgDecorationManager* manager);

  // Schedules an xdg_surface configure carrying `mode` to the client. Returns
  // the configure serial, or 0 when the surface is not ready for configure.
  uint32_t xdgToplevelDecorationSetMode(XdgToplevelDecoration* decoration, XdgDecorationMode mode);

} // namespace umbriel
