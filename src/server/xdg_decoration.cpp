#include "server/xdg_decoration.h"

#include <xdg-decoration-unstable-v1-server-protocol.h>

#include <cassert>
#include <cstdlib>
#include <new>

namespace umbriel {
  namespace {
    constexpr uint32_t kDecorationManagerVersion = 2;

    // Defined below once the request handlers they point at exist.
    extern const struct zxdg_toplevel_decoration_v1_interface decorationImpl;
    extern const struct zxdg_decoration_manager_v1_interface managerImpl;

    XdgToplevelDecoration* decorationFromResource(wl_resource* resource) {
      assert(wl_resource_instance_of(resource, &zxdg_toplevel_decoration_v1_interface, &decorationImpl));
      return static_cast<XdgToplevelDecoration*>(wl_resource_get_user_data(resource));
    }

    XdgDecorationManager* managerFromResource(wl_resource* resource) {
      assert(wl_resource_instance_of(resource, &zxdg_decoration_manager_v1_interface, &managerImpl));
      return static_cast<XdgDecorationManager*>(wl_resource_get_user_data(resource));
    }

    void decorationHandleDestroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }

    void decorationHandleSetMode(wl_client*, wl_resource* resource, uint32_t mode) {
      if (mode != ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE
          && mode != ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE) {
        wl_resource_post_error(
            resource, ZXDG_TOPLEVEL_DECORATION_V1_ERROR_INVALID_MODE, "invalid decoration mode"
        );
        return;
      }
      XdgToplevelDecoration* decoration = decorationFromResource(resource);
      decoration->requestedMode = static_cast<XdgDecorationMode>(mode);
      wl_signal_emit_mutable(&decoration->events.requestMode, decoration);
    }

    void decorationHandleUnsetMode(wl_client*, wl_resource* resource) {
      XdgToplevelDecoration* decoration = decorationFromResource(resource);
      decoration->requestedMode = XdgDecorationModeNone;
      wl_signal_emit_mutable(&decoration->events.requestMode, decoration);
    }

    const struct zxdg_toplevel_decoration_v1_interface decorationImpl = {
        .destroy = decorationHandleDestroy,
        .set_mode = decorationHandleSetMode,
        .unset_mode = decorationHandleUnsetMode,
    };

    void decorationHandleResourceDestroy(wl_resource* resource) {
      XdgToplevelDecoration* decoration = decorationFromResource(resource);
      wl_signal_emit_mutable(&decoration->events.destroy, decoration);

      assert(wl_list_empty(&decoration->events.destroy.listener_list));
      assert(wl_list_empty(&decoration->events.requestMode.listener_list));

      wlr_surface_synced_finish(&decoration->synced);
      wl_list_remove(&decoration->toplevelDestroy.link);
      wl_list_remove(&decoration->surfaceConfigure.link);
      wl_list_remove(&decoration->surfaceAckConfigure.link);
      XdgDecorationConfigure* configure;
      XdgDecorationConfigure* tmp;
      wl_list_for_each_safe(configure, tmp, &decoration->configureList, link) {
        free(configure);
      }
      wl_list_remove(&decoration->link);
      delete decoration;
    }

    void decorationHandleToplevelDestroy(wl_listener* listener, void*) {
      XdgToplevelDecoration* decoration = wl_container_of(listener, decoration, toplevelDestroy);
      wl_resource_post_error(
          decoration->resource, ZXDG_TOPLEVEL_DECORATION_V1_ERROR_ORPHANED,
          "xdg_toplevel destroyed before xdg_toplevel_decoration"
      );
      wl_resource_destroy(decoration->resource);
    }

    void decorationHandleSurfaceConfigure(wl_listener* listener, void* data) {
      XdgToplevelDecoration* decoration = wl_container_of(listener, decoration, surfaceConfigure);
      auto* surfaceConfigure = static_cast<wlr_xdg_surface_configure*>(data);

      if (decoration->pending.mode == decoration->scheduledMode) {
        return;
      }

      auto* configure = static_cast<XdgDecorationConfigure*>(calloc(1, sizeof(XdgDecorationConfigure)));
      if (configure == nullptr) {
        return;
      }
      configure->surfaceConfigure = surfaceConfigure;
      configure->mode = decoration->scheduledMode;
      wl_list_insert(decoration->configureList.prev, &configure->link);

      zxdg_toplevel_decoration_v1_send_configure(decoration->resource, configure->mode);
    }

    void decorationHandleSurfaceAckConfigure(wl_listener* listener, void* data) {
      XdgToplevelDecoration* decoration = wl_container_of(listener, decoration, surfaceAckConfigure);
      auto* surfaceConfigure = static_cast<wlr_xdg_surface_configure*>(data);

      bool found = false;
      XdgDecorationConfigure* configure;
      XdgDecorationConfigure* tmp;
      wl_list_for_each(configure, &decoration->configureList, link) {
        if (configure->surfaceConfigure == surfaceConfigure) {
          found = true;
          break;
        }
      }
      if (!found) {
        return;
      }
      wl_list_for_each_safe(configure, tmp, &decoration->configureList, link) {
        if (configure->surfaceConfigure == surfaceConfigure) {
          break;
        }
        wl_list_remove(&configure->link);
        free(configure);
      }

      decoration->pending.mode = configure->mode;

      wl_list_remove(&configure->link);
      free(configure);
    }

    const wlr_surface_synced_impl surfaceSyncedImpl = {
        .state_size = sizeof(XdgDecorationState),
        .init_state = nullptr,
        .finish_state = nullptr,
        .move_state = nullptr,
        .commit = nullptr,
    };

    void managerHandleDestroy(wl_client*, wl_resource* managerResource) { wl_resource_destroy(managerResource); }

    void managerHandleGetToplevelDecoration(
        wl_client* client, wl_resource* managerResource, uint32_t id, wl_resource* toplevelResource
    ) {
      XdgDecorationManager* manager = managerFromResource(managerResource);
      wlr_xdg_toplevel* toplevel = wlr_xdg_toplevel_from_resource(toplevelResource);
      const uint32_t version = wl_resource_get_version(managerResource);

      // Version 1 forbids creating a decoration once the toplevel has a
      // buffer; version 2 allows it and assumes a client-side mode until the
      // first configure.
      if (version == 1 && wlr_surface_has_buffer(toplevel->base->surface)) {
        wl_resource_post_error(
            managerResource, ZXDG_TOPLEVEL_DECORATION_V1_ERROR_UNCONFIGURED_BUFFER,
            "xdg_toplevel_decoration must not have a buffer at creation"
        );
        return;
      }

      XdgToplevelDecoration* existing;
      wl_list_for_each(existing, &manager->decorations, link) {
        if (existing->toplevel == toplevel) {
          wl_resource_post_error(
              managerResource, ZXDG_TOPLEVEL_DECORATION_V1_ERROR_ALREADY_CONSTRUCTED,
              "xdg_toplevel already has a decoration object"
          );
          return;
        }
      }

      auto* decoration = new (std::nothrow) XdgToplevelDecoration{};
      if (decoration == nullptr) {
        wl_client_post_no_memory(client);
        return;
      }
      decoration->manager = manager;
      decoration->toplevel = toplevel;

      if (!wlr_surface_synced_init(
              &decoration->synced, toplevel->base->surface, &surfaceSyncedImpl, &decoration->pending,
              &decoration->current
          )) {
        delete decoration;
        wl_client_post_no_memory(client);
        return;
      }

      decoration->resource = wl_resource_create(client, &zxdg_toplevel_decoration_v1_interface, version, id);
      if (decoration->resource == nullptr) {
        wlr_surface_synced_finish(&decoration->synced);
        delete decoration;
        wl_client_post_no_memory(client);
        return;
      }
      wl_resource_set_implementation(
          decoration->resource, &decorationImpl, decoration, decorationHandleResourceDestroy
      );

      wl_list_init(&decoration->configureList);

      wl_signal_init(&decoration->events.destroy);
      wl_signal_init(&decoration->events.requestMode);

      decoration->toplevelDestroy.notify = decorationHandleToplevelDestroy;
      wl_signal_add(&toplevel->events.destroy, &decoration->toplevelDestroy);
      decoration->surfaceConfigure.notify = decorationHandleSurfaceConfigure;
      wl_signal_add(&toplevel->base->events.configure, &decoration->surfaceConfigure);
      decoration->surfaceAckConfigure.notify = decorationHandleSurfaceAckConfigure;
      wl_signal_add(&toplevel->base->events.ack_configure, &decoration->surfaceAckConfigure);

      wl_list_insert(&manager->decorations, &decoration->link);

      wl_signal_emit_mutable(&manager->events.newToplevelDecoration, decoration);
    }

    const struct zxdg_decoration_manager_v1_interface managerImpl = {
        .destroy = managerHandleDestroy,
        .get_toplevel_decoration = managerHandleGetToplevelDecoration,
    };
  } // namespace

  XdgDecorationManager* xdgDecorationManagerCreate(wl_display* display) {
    auto* manager = new (std::nothrow) XdgDecorationManager{};
    if (manager == nullptr) {
      return nullptr;
    }
    manager->global = wl_global_create(
        display, &zxdg_decoration_manager_v1_interface, kDecorationManagerVersion, manager,
        [](wl_client* client, void* data, uint32_t version, uint32_t id) {
          auto* bound = static_cast<XdgDecorationManager*>(data);
          wl_resource* resource =
              wl_resource_create(client, &zxdg_decoration_manager_v1_interface, version, id);
          if (resource == nullptr) {
            wl_client_post_no_memory(client);
            return;
          }
          wl_resource_set_implementation(resource, &managerImpl, bound, nullptr);
        }
    );
    if (manager->global == nullptr) {
      delete manager;
      return nullptr;
    }
    wl_list_init(&manager->decorations);
    wl_signal_init(&manager->events.newToplevelDecoration);
    wl_signal_init(&manager->events.destroy);
    return manager;
  }

  void xdgDecorationManagerDestroy(XdgDecorationManager* manager) {
    wl_global_destroy(manager->global);
    delete manager;
  }

  uint32_t xdgToplevelDecorationSetMode(XdgToplevelDecoration* decoration, XdgDecorationMode mode) {
    assert(mode != XdgDecorationModeNone);
    decoration->scheduledMode = mode;
    return wlr_xdg_surface_schedule_configure(decoration->toplevel->base);
  }

} // namespace umbriel
