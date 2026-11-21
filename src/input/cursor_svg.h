#pragma once

#include "input/cursor_svg_parse.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct wlr_buffer;

namespace umbriel {

  // A directory of <name>.svg cursor files plus an optional theme.toml with
  // per-cursor and theme-default hotspots (normalized 0..1). Renders on demand
  // through Cairo and caches buffers; see docs/user/input.md for the layout.
  class SvgCursorTheme {
  public:
    explicit SvgCursorTheme(std::string_view directory);
    ~SvgCursorTheme();

    SvgCursorTheme(const SvgCursorTheme&) = delete;
    SvgCursorTheme& operator=(const SvgCursorTheme&) = delete;

    bool valid() const { return m_loaded; }

    struct Image {
      wlr_buffer* buffer = nullptr;
      // Hotspot in logical cursor units, the space wlr_cursor_set_buffer
      // expects before it applies each output's scale.
      int32_t hotspotX = 0;
      int32_t hotspotY = 0;
      // Buffer scale: texture pixels per logical pixel, passed to
      // wlr_cursor_set_buffer so one buffer serves every output scale.
      float scale = 1.0f;
      int pixelWidth = 0;
      int pixelHeight = 0;
      // Animation: frameCount is 1 for static cursors; frameDelayMs is how long
      // this frame stays visible before the next (0 when static).
      int frame = 0;
      int frameCount = 1;
      int frameDelayMs = 0;
    };

    // Returns a cached rendered image, or nullptr when the theme lacks the
    // cursor, its hotspot metadata, or its SVG fails to parse.
    const Image* image(std::string_view name, int logicalSize);

    // Same as image() but for a specific animation frame; out-of-range frames
    // clamp to the first. Static cursors ignore `frame`.
    const Image* imageFrame(std::string_view name, int logicalSize, int frame);

    // Renders at exactly `size` pixels high with aspect-preserved width, for
    // consumers that take a raw buffer with no scale parameter (Xwayland's
    // default cursor). hotspotX/hotspotY are in buffer pixels and scale is 1.
    std::optional<Image> xwaylandImage(std::string_view name, int size);

  private:
    struct Definition {
      cursor_svg::SvgCursorImage svg;
      // Empty for static cursors; otherwise one parsed SVG per frame.
      std::vector<cursor_svg::SvgCursorImage> frames;
      // Per-frame delay in milliseconds; empty means the default.
      std::vector<int> frameDelaysMs;
      double hotspotX = 0;
      double hotspotY = 0;
      bool hasHotspot = false;
    };

    struct RenderKey {
      int frame = 0;
      int width = 0;
      int height = 0;
      bool operator==(const RenderKey&) const = default;
    };

    struct RenderKeyHash {
      size_t operator()(const RenderKey& key) const {
        return (static_cast<size_t>(key.frame) * 2654435761u)
            ^ (static_cast<size_t>(key.width) * 73856093u)
            ^ static_cast<size_t>(key.height);
      }
    };

    static const cursor_svg::SvgCursorImage* frameSvg(const Definition& definition, int frame);
    void loadThemeToml(const std::filesystem::path& path);
    void loadScalableTheme(const std::filesystem::path& dir);
    const Definition* definitionFor(std::string_view name, double& hotspotX, double& hotspotY) const;
    Image* cachedRender(
        const cursor_svg::SvgCursorImage& svg, const std::string& name, int frame, int pixelWidth, int pixelHeight
    );

    bool m_loaded = false;
    std::unordered_map<std::string, Definition> m_definitions;
    std::unordered_map<std::string, std::unordered_map<RenderKey, Image, RenderKeyHash>> m_cache;
    double m_defaultHotspotX = 0;
    double m_defaultHotspotY = 0;
    bool m_hasDefaultHotspot = false;
  };

} // namespace umbriel
