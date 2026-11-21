#include "input/cursor_svg.h"

// clang-format off
#include <cairo.h>
// clang-format on

#include "core/log.h"
#include "scene/cairo_buffer.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <string>
#include <toml++/toml.hpp>

extern "C" {
#include <wlr/types/wlr_buffer.h>
}

namespace umbriel {
  namespace {
    constexpr Logger kLog("cursor-svg");

    // Rendered at 4x the logical size, capped, so one buffer stays crisp on
    // fractional and 2x-4x outputs; wlroots scales it down per output.
    constexpr double kSupersample = 4.0;
    constexpr int kMaxPixels = 256;
    // Delay applied to frames without an explicit `delays` entry in theme.toml.
    constexpr int kDefaultFrameDelayMs = 100;

    bool parseHotspotPair(const toml::table& table, double& x, double& y) {
      const toml::node* xn = table.get("hotspot_x");
      const toml::node* yn = table.get("hotspot_y");
      const auto xv = xn != nullptr ? xn->value<double>() : std::nullopt;
      const auto yv = yn != nullptr ? yn->value<double>() : std::nullopt;
      if (!xv || !yv || *xv < 0.0 || *xv > 1.0 || *yv < 0.0 || *yv > 1.0) {
        return false;
      }
      x = *xv;
      y = *yv;
      return true;
    }

    // Loads the `frames`/`delays` arrays of a [cursor.<name>] table. Frame
    // paths are relative to the theme directory and must stay inside it.
    bool loadFrames(
        const toml::table& entry, const std::filesystem::path& themeDir,
        std::vector<cursor_svg::SvgCursorImage>& frames, std::vector<int>& delays
    ) {
      const toml::node* framesNode = entry.get("frames");
      const auto* framesArray = framesNode != nullptr ? framesNode->as_array() : nullptr;
      if (framesArray == nullptr || framesArray->size() < 2) {
        return false;
      }
      frames.clear();
      delays.clear();
      frames.reserve(framesArray->size());
      for (const toml::node& node : *framesArray) {
        const auto framePath = node.value<std::string>();
        if (!framePath
            || framePath->empty()
            || framePath->front() == '/'
            || framePath->find("..") != std::string::npos) {
          kLog.warn("ignoring unsafe cursor frame path '{}'", framePath ? *framePath : std::string());
          return false;
        }
        const std::filesystem::path file = themeDir / *framePath;
        std::error_code ec;
        if (!std::filesystem::is_regular_file(file, ec)) {
          kLog.warn("ignoring missing cursor frame '{}'", file.string());
          return false;
        }
        std::ifstream in(file, std::ios::binary);
        if (!in) {
          return false;
        }
        std::ostringstream contents;
        contents << in.rdbuf();
        auto parsed = cursor_svg::parseSvgCursor(contents.str());
        if (!parsed) {
          kLog.warn("ignoring unparseable cursor frame '{}'", file.string());
          return false;
        }
        frames.push_back(std::move(*parsed));
      }
      if (const toml::node* delaysNode = entry.get("delays"); delaysNode != nullptr) {
        if (const auto* delaysArray = delaysNode->as_array()) {
          for (const toml::node& node : *delaysArray) {
            if (const auto delay = node.value<int>()) {
              delays.push_back(std::max(0, *delay));
            }
          }
        }
      }
      return true;
    }

    wlr_buffer* renderSvg(const cursor_svg::SvgCursorImage& svg, int pixelWidth, int pixelHeight) {
      cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pixelWidth, pixelHeight);
      if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        return nullptr;
      }
      cairo_t* cr = cairo_create(surface);

      // Map the viewBox onto the pixel buffer, centered horizontally when
      // rounding makes the widths differ by a pixel.
      const double scale = pixelHeight / svg.height;
      const double offsetX = (pixelWidth - svg.width * scale) / 2.0 - svg.viewX * scale;
      const double offsetY = -svg.viewY * scale;
      cairo_matrix_t base;
      cairo_matrix_init(&base, scale, 0, 0, scale, offsetX, offsetY);
      cairo_set_matrix(cr, &base);

      for (const cursor_svg::DrawItem& item : svg.items) {
        cairo_set_matrix(cr, &base);
        cairo_matrix_t itemMatrix;
        cairo_matrix_init(&itemMatrix, item.m[0], item.m[1], item.m[2], item.m[3], item.m[4], item.m[5]);
        cairo_transform(cr, &itemMatrix);

        // Clip paths are evaluated in the referencing element's user space:
        // clip-shape transform composed with the reference anchor, then the
        // same base->item matrix convention as the item itself.
        if (!item.clipRefs.empty()) {
          cairo_save(cr);
          cairo_set_matrix(cr, &base);
          for (const cursor_svg::ClipRef& ref : item.clipRefs) {
            const auto& shapes = svg.clips[ref.clip];
            for (const cursor_svg::ClipShape& shape : shapes) {
              // combined = anchor * shape.m (column-vector convention).
              const double* a = ref.anchor;
              const double* b = shape.m;
              cairo_matrix_t clipMatrix;
              cairo_matrix_init(
                  &clipMatrix, a[0] * b[0] + a[2] * b[1], a[1] * b[0] + a[3] * b[1], a[0] * b[2] + a[2] * b[3],
                  a[1] * b[2] + a[3] * b[3], a[0] * b[4] + a[2] * b[5] + a[4], a[1] * b[4] + a[3] * b[5] + a[5]
              );
              cairo_transform(cr, &clipMatrix);
              for (const cursor_svg::PathCommand& command : shape.path) {
                switch (command.op) {
                case cursor_svg::PathOp::Move:
                  cairo_move_to(cr, command.x, command.y);
                  break;
                case cursor_svg::PathOp::Line:
                  cairo_line_to(cr, command.x, command.y);
                  break;
                case cursor_svg::PathOp::Cubic:
                  cairo_curve_to(cr, command.cx1, command.cy1, command.cx2, command.cy2, command.x, command.y);
                  break;
                case cursor_svg::PathOp::Close:
                  cairo_close_path(cr);
                  break;
                }
              }
              cairo_set_fill_rule(cr, shape.evenOdd ? CAIRO_FILL_RULE_EVEN_ODD : CAIRO_FILL_RULE_WINDING);
              cairo_clip(cr);
            }
          }
          // Restore the item's own transform for the paint pass.
          cairo_set_matrix(cr, &base);
          cairo_transform(cr, &itemMatrix);
        }

        for (const cursor_svg::PathCommand& command : item.path) {
          switch (command.op) {
          case cursor_svg::PathOp::Move:
            cairo_move_to(cr, command.x, command.y);
            break;
          case cursor_svg::PathOp::Line:
            cairo_line_to(cr, command.x, command.y);
            break;
          case cursor_svg::PathOp::Cubic:
            cairo_curve_to(cr, command.cx1, command.cy1, command.cx2, command.cy2, command.x, command.y);
            break;
          case cursor_svg::PathOp::Close:
            cairo_close_path(cr);
            break;
          }
        }

        cairo_set_fill_rule(cr, item.evenOdd ? CAIRO_FILL_RULE_EVEN_ODD : CAIRO_FILL_RULE_WINDING);
        if (item.hasFill) {
          cairo_set_source_rgba(cr, item.fill.r, item.fill.g, item.fill.b, item.fill.a);
          if (item.hasStroke) {
            cairo_fill_preserve(cr);
          } else {
            cairo_fill(cr);
          }
        }
        if (item.hasStroke) {
          cairo_set_source_rgba(cr, item.stroke.r, item.stroke.g, item.stroke.b, item.stroke.a);
          cairo_set_line_width(cr, item.strokeWidth);
          cairo_set_line_cap(
              cr,
              item.lineCap == 1       ? CAIRO_LINE_CAP_ROUND
                  : item.lineCap == 2 ? CAIRO_LINE_CAP_SQUARE
                                      : CAIRO_LINE_CAP_BUTT
          );
          cairo_set_line_join(
              cr,
              item.lineJoin == 1       ? CAIRO_LINE_JOIN_ROUND
                  : item.lineJoin == 2 ? CAIRO_LINE_JOIN_BEVEL
                                       : CAIRO_LINE_JOIN_MITER
          );
          cairo_stroke(cr);
        }
        if (!item.clipRefs.empty()) {
          cairo_restore(cr);
        }
      }

      cairo_destroy(cr);
      return createCairoBuffer(surface);
    }

  } // namespace

  SvgCursorTheme::SvgCursorTheme(std::string_view directory) {
    const std::filesystem::path dir{directory};
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
      kLog.warn("cursor theme '{}' is not a directory", directory);
      return;
    }
    m_loaded = true;

    // KDE/GNOME cursors_scalable layout: <theme>/cursors_scalable/<name>/
    // with SVG frames plus a metadata.json. When present it is the theme's
    // cursor source; the flat <name>.svg scan below covers Umbriel-native
    // themes. theme.toml still applies on top for hotspot overrides.
    const std::filesystem::path scalableDir = dir / "cursors_scalable";
    if (std::filesystem::is_directory(scalableDir, ec)) {
      loadScalableTheme(scalableDir);
    } else {
      for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) {
          kLog.warn("cannot list cursor theme '{}': {}", directory, ec.message());
          return;
        }
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".svg") {
          continue;
        }
        std::ifstream in(entry.path(), std::ios::binary);
        if (!in) {
          continue;
        }
        std::ostringstream contents;
        contents << in.rdbuf();
        auto parsed = cursor_svg::parseSvgCursor(contents.str());
        if (!parsed) {
          kLog.warn("ignoring unparseable cursor SVG '{}'", entry.path().string());
          continue;
        }
        Definition definition;
        definition.svg = std::move(*parsed);
        m_definitions[entry.path().stem().string()] = std::move(definition);
      }
    }

    loadThemeToml(dir / "theme.toml");

    // Real-world themes frequently omit `default`; left_ptr is the arrow every
    // theme must have, so alias it rather than falling back to Xcursor.
    if (m_definitions.find("default") == m_definitions.end() && m_definitions.find("left_ptr") != m_definitions.end()) {
      m_definitions["default"] = m_definitions["left_ptr"];
    }

    if (m_definitions.empty()) {
      kLog.warn("cursor theme '{}' contains no usable SVG cursors", directory);
    }
  }

  SvgCursorTheme::~SvgCursorTheme() {
    for (auto& [name, sizes] : m_cache) {
      for (auto& [key, image] : sizes) {
        if (image.buffer != nullptr) {
          wlr_buffer_drop(image.buffer);
        }
      }
    }
  }

  void SvgCursorTheme::loadScalableTheme(const std::filesystem::path& dir) {
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
      if (ec) {
        kLog.warn("cannot list scalable cursor theme '{}': {}", dir.string(), ec.message());
        return;
      }
      // is_directory() follows symlinks, so aliased cursor dirs resolve here.
      if (!entry.is_directory(ec)) {
        continue;
      }
      const std::string name = entry.path().filename().string();
      std::ifstream metaIn(entry.path() / "metadata.json");
      if (!metaIn) {
        kLog.warn("scalable cursor '{}' has no metadata.json", name);
        continue;
      }
      const auto meta = nlohmann::json::parse(metaIn, nullptr, false);
      if (meta.is_discarded() || !meta.is_array() || meta.empty()) {
        kLog.warn("scalable cursor '{}' has invalid metadata.json", name);
        continue;
      }

      // Each array element is one animation frame; the first element's
      // hotspot applies to the whole cursor.
      std::vector<cursor_svg::SvgCursorImage> frames;
      std::vector<int> delays;
      bool allDelays = true;
      double hotspotX = 0, hotspotY = 0;
      bool ok = true;
      bool first = true;
      for (const auto& element : meta) {
        if (!element.is_object()) {
          ok = false;
          break;
        }
        const auto filenameIt = element.find("filename");
        const std::string filename =
            filenameIt != element.end() && filenameIt->is_string() ? filenameIt->get<std::string>() : std::string{};
        const auto file = entry.path() / filename;
        std::ifstream in(file, std::ios::binary);
        if (filename.empty() || filename.front() == '/' || filename.find("..") != std::string::npos || !in) {
          kLog.warn("scalable cursor '{}' frame '{}' is missing", name, filename);
          ok = false;
          break;
        }
        std::ostringstream contents;
        contents << in.rdbuf();
        auto parsed = cursor_svg::parseSvgCursor(contents.str());
        if (!parsed) {
          kLog.warn("ignoring unparseable scalable cursor frame '{}'", file.string());
          ok = false;
          break;
        }
        if (first) {
          first = false;
          // Hotspot arrives in nominal-size pixels; normalize by nominal_size,
          // falling back to the SVG's own viewBox units.
          const auto nominalIt = element.find("nominal_size");
          const double nominal = nominalIt != element.end() && nominalIt->is_number() ? nominalIt->get<double>() : 0;
          const double unit = nominal > 0 ? nominal : parsed->width;
          const auto hxIt = element.find("hotspot_x");
          const auto hyIt = element.find("hotspot_y");
          hotspotX = unit > 0 && hxIt != element.end() && hxIt->is_number() ? hxIt->get<double>() / unit : 0;
          hotspotY = unit > 0 && hyIt != element.end() && hyIt->is_number() ? hyIt->get<double>() / unit : 0;
        }
        const auto delay = element.find("delay");
        if (delay != element.end() && delay->is_number()) {
          delays.push_back(std::max(0, delay->get<int>()));
        } else {
          allDelays = false;
        }
        frames.push_back(std::move(*parsed));
      }
      if (!ok) {
        continue;
      }
      if (!allDelays) {
        delays.clear();
      }

      Definition definition;
      if (frames.size() == 1) {
        definition.svg = std::move(frames.front());
      } else {
        definition.svg = frames.front();
        definition.frames = std::move(frames);
        definition.frameDelaysMs = std::move(delays);
      }
      definition.hotspotX = hotspotX;
      definition.hotspotY = hotspotY;
      definition.hasHotspot = true;
      m_definitions[name] = std::move(definition);
    }
  }

  void SvgCursorTheme::loadThemeToml(const std::filesystem::path& path) {
    toml::table parsed;
    try {
      parsed = toml::parse_file(path.string());
    } catch (const toml::parse_error& error) {
      kLog.warn("ignoring invalid {}: {}", path.string(), error.description());
      return;
    }
    if (const auto* themeTable = parsed.get_as<toml::table>("theme")) {
      double x = 0, y = 0;
      if (parseHotspotPair(*themeTable, x, y)) {
        m_defaultHotspotX = x;
        m_defaultHotspotY = y;
        m_hasDefaultHotspot = true;
      }
    }
    const auto* cursors = parsed.get_as<toml::table>("cursor");
    if (cursors == nullptr) {
      return;
    }
    const std::filesystem::path themeDir = path.parent_path();
    for (const auto& [key, node] : *cursors) {
      const auto* entry = node.as_table();
      if (entry == nullptr) {
        continue;
      }
      const std::string name{key.str()};
      auto it = m_definitions.find(name);

      double x = 0, y = 0;
      const bool hasHotspotKeys = entry->get("hotspot_x") != nullptr || entry->get("hotspot_y") != nullptr;
      if (hasHotspotKeys) {
        if (!parseHotspotPair(*entry, x, y)) {
          kLog.warn("ignoring invalid hotspot for cursor '{}' in {}", name, path.string());
          continue;
        }
      }

      if (entry->get("frames") != nullptr) {
        // Animated cursors may exist only in theme.toml (no <name>.svg).
        std::vector<cursor_svg::SvgCursorImage> frames;
        std::vector<int> delays;
        if (!loadFrames(*entry, themeDir, frames, delays)) {
          kLog.warn("ignoring invalid frames for cursor '{}' in {}", name, path.string());
          continue;
        }
        Definition definition;
        // The static `svg` member is unused for animated cursors; seed it with
        // the first frame so the struct stays valid.
        definition.svg = frames.front();
        definition.frames = std::move(frames);
        definition.frameDelaysMs = std::move(delays);
        if (hasHotspotKeys) {
          definition.hotspotX = x;
          definition.hotspotY = y;
          definition.hasHotspot = true;
        }
        m_definitions[name] = std::move(definition);
        continue;
      }

      if (it == m_definitions.end()) {
        continue;
      }
      if (hasHotspotKeys) {
        it->second.hotspotX = x;
        it->second.hotspotY = y;
        it->second.hasHotspot = true;
      }
    }
  }

  const SvgCursorTheme::Definition*
  SvgCursorTheme::definitionFor(std::string_view name, double& hotspotX, double& hotspotY) const {
    auto it = m_definitions.find(std::string(name));
    if (it == m_definitions.end()) {
      return nullptr;
    }
    const Definition& definition = it->second;
    if (definition.hasHotspot) {
      hotspotX = definition.hotspotX;
      hotspotY = definition.hotspotY;
    } else if (m_hasDefaultHotspot) {
      hotspotX = m_defaultHotspotX;
      hotspotY = m_defaultHotspotY;
    } else {
      return nullptr;
    }
    return &definition;
  }

  SvgCursorTheme::Image* SvgCursorTheme::cachedRender(
      const cursor_svg::SvgCursorImage& svg, const std::string& name, int frame, int pixelWidth, int pixelHeight
  ) {
    auto& sizes = m_cache[name];
    const RenderKey key{frame, pixelWidth, pixelHeight};
    auto it = sizes.find(key);
    if (it == sizes.end()) {
      wlr_buffer* buffer = renderSvg(svg, pixelWidth, pixelHeight);
      if (buffer == nullptr) {
        return nullptr;
      }
      Image rendered;
      rendered.buffer = buffer;
      rendered.pixelWidth = pixelWidth;
      rendered.pixelHeight = pixelHeight;
      it = sizes.emplace(key, rendered).first;
    }
    return &it->second;
  }

  const cursor_svg::SvgCursorImage* SvgCursorTheme::frameSvg(const Definition& definition, int frame) {
    if (definition.frames.empty()) {
      return &definition.svg;
    }
    const int count = static_cast<int>(definition.frames.size());
    return &definition.frames[std::clamp(frame, 0, count - 1)];
  }

  const SvgCursorTheme::Image* SvgCursorTheme::image(std::string_view name, int logicalSize) {
    return imageFrame(name, logicalSize, 0);
  }

  const SvgCursorTheme::Image* SvgCursorTheme::imageFrame(std::string_view name, int logicalSize, int frame) {
    if (logicalSize <= 0) {
      return nullptr;
    }
    double hotspotX = 0, hotspotY = 0;
    const Definition* definition = definitionFor(name, hotspotX, hotspotY);
    if (definition == nullptr) {
      return nullptr;
    }
    const cursor_svg::SvgCursorImage* svg = frameSvg(*definition, frame);
    if (svg == nullptr) {
      return nullptr;
    }

    // Buffer pixels: supersampled, capped, aspect-preserving.
    double scale = kSupersample;
    auto dims = [&]() {
      return std::pair{
          std::max(1, static_cast<int>(std::lround(svg->width * scale))),
          std::max(1, static_cast<int>(std::lround(svg->height * scale)))
      };
    };
    auto [pixelWidth, pixelHeight] = dims();
    const int largest = std::max(pixelWidth, pixelHeight);
    if (largest > kMaxPixels) {
      scale *= static_cast<double>(kMaxPixels) / largest;
      std::tie(pixelWidth, pixelHeight) = dims();
    }
    Image* image = cachedRender(*svg, std::string(name), frame, pixelWidth, pixelHeight);
    if (image == nullptr) {
      return nullptr;
    }

    // Recompute per-call fields that depend on logicalSize; buffer and pixel
    // dims are cached, hotspot/scale are cheap.
    image->scale = static_cast<float>(pixelHeight) / static_cast<float>(logicalSize);
    const double logicalWidth = pixelWidth / static_cast<double>(image->scale);
    const double logicalHeight = pixelHeight / static_cast<double>(image->scale);
    image->hotspotX = static_cast<int32_t>(std::lround(hotspotX * logicalWidth));
    image->hotspotY = static_cast<int32_t>(std::lround(hotspotY * logicalHeight));
    image->hotspotX = std::clamp<int32_t>(image->hotspotX, 0, std::max(0, static_cast<int32_t>(logicalWidth) - 1));
    image->hotspotY = std::clamp<int32_t>(image->hotspotY, 0, std::max(0, static_cast<int32_t>(logicalHeight) - 1));

    const int frameCount = definition->frames.empty() ? 1 : static_cast<int>(definition->frames.size());
    image->frame = std::clamp(frame, 0, frameCount - 1);
    image->frameCount = frameCount;
    image->frameDelayMs = 0;
    if (frameCount > 1) {
      image->frameDelayMs = image->frame < static_cast<int>(definition->frameDelaysMs.size())
          ? std::max(0, definition->frameDelaysMs[image->frame])
          : kDefaultFrameDelayMs;
    }
    return image;
  }

  std::optional<SvgCursorTheme::Image> SvgCursorTheme::xwaylandImage(std::string_view name, int size) {
    if (size <= 0) {
      return std::nullopt;
    }
    double hotspotX = 0, hotspotY = 0;
    const Definition* definition = definitionFor(name, hotspotX, hotspotY);
    const cursor_svg::SvgCursorImage* svg = definition != nullptr ? frameSvg(*definition, 0) : nullptr;
    if (svg == nullptr) {
      return std::nullopt;
    }
    // One buffer pixel per logical pixel: Xwayland applies no scale of its own.
    const double unitScale = static_cast<double>(size) / svg->height;
    const int pixelHeight = std::max(1, static_cast<int>(std::lround(svg->height * unitScale)));
    const int pixelWidth = std::max(1, static_cast<int>(std::lround(svg->width * unitScale)));
    const Image* cached = cachedRender(*svg, std::string(name), 0, pixelWidth, pixelHeight);
    if (cached == nullptr) {
      return std::nullopt;
    }
    Image image = *cached;
    image.scale = 1.0f;
    image.hotspotX =
        std::clamp<int32_t>(static_cast<int32_t>(std::lround(hotspotX * pixelWidth)), 0, std::max(0, pixelWidth - 1));
    image.hotspotY =
        std::clamp<int32_t>(static_cast<int32_t>(std::lround(hotspotY * pixelHeight)), 0, std::max(0, pixelHeight - 1));
    return image;
  }

} // namespace umbriel
