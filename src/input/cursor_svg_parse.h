#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace umbriel::cursor_svg {

  // Path commands are absolute and flattened to Move/Line/Cubic/Close: quadratic
  // and arc segments are converted to cubics during parsing so the renderer only
  // needs three Cairo calls.
  enum class PathOp : uint8_t { Move, Line, Cubic, Close };

  struct PathCommand {
    PathOp op = PathOp::Move;
    // Move/Line endpoint, Cubic endpoint.
    double x = 0;
    double y = 0;
    // Cubic control points.
    double cx1 = 0;
    double cy1 = 0;
    double cx2 = 0;
    double cy2 = 0;
  };

  struct Paint {
    float r = 0;
    float g = 0;
    float b = 0;
    float a = 1;
  };

  // One shape inside a <clipPath>: geometry plus the transform accumulated
  // within the clipPath subtree. Coordinates live in the user space of the
  // element that referenced the clip.
  struct ClipShape {
    double m[6]{1, 0, 0, 1, 0, 0};
    std::vector<PathCommand> path;
    bool evenOdd = false;
  };

  // A clip-path="url(#id)" reference resolved at parse time. `anchor` is the
  // referencing element's matrix (its user space -> viewBox), captured after
  // that element's own transform; `clip` indexes SvgCursorImage::clips.
  struct ClipRef {
    int clip = -1;
    double anchor[6]{1, 0, 0, 1, 0, 0};
  };

  struct DrawItem {
    std::vector<PathCommand> path;
    // Clip paths that apply to this item, outermost first; each is intersected
    // with the previous, matching nested clip-path semantics.
    std::vector<ClipRef> clipRefs;
    Paint fill;
    bool hasFill = false;
    Paint stroke;
    bool hasStroke = false;
    double strokeWidth = 1;
    int lineCap = 0;  // 0 butt, 1 round, 2 square
    int lineJoin = 0; // 0 miter, 1 round, 2 bevel
    bool evenOdd = false;
    // SVG transform mapping item-local coordinates into viewBox units, in
    // cairo_matrix_t order {xx, yx, xy, yy, x0, y0}.
    double m[6]{1, 0, 0, 1, 0, 0};
  };

  struct SvgCursorImage {
    // viewBox rectangle in SVG user units.
    double viewX = 0;
    double viewY = 0;
    double width = 0;
    double height = 0;
    std::vector<DrawItem> items;
    // Geometry of every <clipPath> in the document, in document order;
    // ClipRef::clip indexes into this.
    std::vector<std::vector<ClipShape>> clips;
  };

  // Parses the SVG subset cursor themes use. Returns nullopt on malformed XML or
  // any construct outside the subset; callers fall back to another backend.
  std::optional<SvgCursorImage> parseSvgCursor(std::string_view xml);

} // namespace umbriel::cursor_svg
