#include "check.h"
#include "input/cursor_svg_parse.h"

#include <cmath>
#include <string>

using umbriel::cursor_svg::DrawItem;
using umbriel::cursor_svg::parseSvgCursor;
using umbriel::cursor_svg::PathCommand;
using umbriel::cursor_svg::PathOp;

namespace {

  constexpr std::string_view kSvgPrefix = R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24">)svg";
  constexpr std::string_view kSvgSuffix = "</svg>";

  std::string wrap(std::string_view body) {
    return std::string{kSvgPrefix} + std::string{body} + std::string{kSvgSuffix};
  }

  const DrawItem& onlyItem(const umbriel::cursor_svg::SvgCursorImage& image) {
    CHECK_EQ(image.items.size(), 1u);
    return image.items.front();
  }

} // namespace

UMBRIEL_TEST(cursorSvgParsesBasicShape) {
  const auto image = parseSvgCursor(wrap(R"svg(<rect x="2" y="3" width="10" height="4" fill="#ff0000"/>)svg"));
  CHECK(image.has_value());
  CHECK_EQ(image->width, 24.0);
  CHECK_EQ(image->height, 24.0);
  const DrawItem& item = onlyItem(*image);
  CHECK(item.hasFill);
  CHECK_NEAR(item.fill.r, 1.0, 0.001);
  CHECK_NEAR(item.fill.g, 0.0, 0.001);
  CHECK_NEAR(item.fill.b, 0.0, 0.001);
  CHECK_EQ(item.path.size(), 5u); // M + 3 lines + close
  CHECK_EQ(item.path.back().op, PathOp::Close);
}

UMBRIEL_TEST(cursorSvgParsesPathCommands) {
  const auto image =
      parseSvgCursor(wrap(R"svg(<path d="M1 1 L4 1 H6 V3 C1 2 3 4 5 6 S7 8 9 9 Q1 1 2 2 T3 3 A1 1 0 0 1 5 5 Z"/>)svg"));
  CHECK(image.has_value());
  const DrawItem& item = onlyItem(*image);
  // Curves may emit several cubics; just verify endpoints stay finite and a
  // close was recorded.
  CHECK(item.path.size() > 4);
  CHECK_EQ(item.path.back().op, PathOp::Close);
  for (const PathCommand& command : item.path) {
    CHECK(std::isfinite(command.x));
    CHECK(std::isfinite(command.y));
  }
}

UMBRIEL_TEST(cursorSvgRelativeAndImplicitCommands) {
  const auto image = parseSvgCursor(wrap(R"svg(<path d="m1 1 2 0 v2 h-2 z" fill="black"/>)svg"));
  CHECK(image.has_value());
  const DrawItem& item = onlyItem(*image);
  CHECK_EQ(item.path.front().op, PathOp::Move);
  CHECK_NEAR(item.path.front().x, 1.0, 0.001);
  CHECK_EQ(item.path.back().op, PathOp::Close);
}

UMBRIEL_TEST(cursorSvgAppliesGroupTransform) {
  const auto image = parseSvgCursor(
      wrap(R"svg(<g transform="translate(10 5) scale(2)"><rect x="0" y="0" width="4" height="4"/></g>)svg")
  );
  CHECK(image.has_value());
  const DrawItem& item = onlyItem(*image);
  // translate then scale: point (0,0) -> (10,5); matrix {xx,yx,xy,yy,x0,y0}
  CHECK_NEAR(item.m[0], 2.0, 0.001);
  CHECK_NEAR(item.m[4], 10.0, 0.001);
  CHECK_NEAR(item.m[5], 5.0, 0.001);
}

UMBRIEL_TEST(cursorSvgHonorsDisplayNone) {
  const auto image =
      parseSvgCursor(wrap(R"svg(<g display="none"><rect width="4" height="4"/></g><circle cx="4" cy="4" r="2"/>)svg"));
  CHECK(image.has_value());
  CHECK_EQ(image->items.size(), 1u);
}

UMBRIEL_TEST(cursorSvgMultipliesOpacity) {
  const auto image = parseSvgCursor(
      wrap(R"svg(<g opacity="0.5"><rect width="4" height="4" fill="#00ff00" fill-opacity="0.5"/></g>)svg")
  );
  CHECK(image.has_value());
  const DrawItem& item = onlyItem(*image);
  CHECK_NEAR(static_cast<double>(item.fill.a), 0.25, 0.001);
}

UMBRIEL_TEST(cursorSvgParsesStrokeAndNamedColor) {
  const auto image = parseSvgCursor(wrap(
      R"svg(<line x1="0" y1="0" x2="4" y2="4" fill="none" stroke="red" stroke-width="2" stroke-linecap="round"/>)svg"
  ));
  CHECK(image.has_value());
  const DrawItem& item = onlyItem(*image);
  CHECK(!item.hasFill);
  CHECK(item.hasStroke);
  CHECK_NEAR(static_cast<double>(item.stroke.r), 1.0, 0.001);
  CHECK_NEAR(item.strokeWidth, 2.0, 0.001);
  CHECK_EQ(item.lineCap, 1);
}

UMBRIEL_TEST(cursorSvgRejectsUnsupportedElements) {
  CHECK(!parseSvgCursor(wrap(R"svg(<use href="#x"/>)svg")));
  CHECK(!parseSvgCursor(wrap(R"svg(<text x="0" y="0">hi</text>)svg")));
  CHECK(!parseSvgCursor(wrap(R"svg(<g><animate attributeName="x"/></g><rect width="4" height="4"/>)svg")));
}

UMBRIEL_TEST(cursorSvgRejectsMalformed) {
  CHECK(!parseSvgCursor(""));
  CHECK(!parseSvgCursor("<svg>"));
  CHECK(!parseSvgCursor("<svg viewBox='0 0 10 10'><rect width='4'/></svg>"));
  CHECK(!parseSvgCursor(wrap("<rect width='4' height='4'"))); // unclosed
  CHECK(!parseSvgCursor("<div></div>"));
}

UMBRIEL_TEST(cursorSvgWidthHeightFallback) {
  const auto image = parseSvgCursor(
      std::string{"<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"32\" height=\"16\">"
                  "<rect width=\"32\" height=\"16\"/>"
                  "</svg>"}
  );
  CHECK(image.has_value());
  CHECK_EQ(image->width, 32.0);
  CHECK_EQ(image->height, 16.0);
}

UMBRIEL_TEST(cursorSvgClipPath) {
  const auto image = parseSvgCursor(std::string{kSvgPrefix} + R"svg(
        <defs><clipPath id="c"><rect x="0" y="0" width="10" height="10"/></clipPath></defs>
        <g clip-path="url(#c)"><rect width="20" height="20" fill="#fff"/></g>
      )svg" + std::string{kSvgSuffix});
  CHECK(image.has_value());
  CHECK_EQ(image->clips.size(), 1u);
  CHECK_EQ(image->clips[0].size(), 1u);
  CHECK_EQ(image->items.size(), 1u);
  CHECK_EQ(image->items[0].clipRefs.size(), 1u);
  CHECK_EQ(image->items[0].clipRefs[0].clip, 0);
}

UMBRIEL_TEST(cursorSvgClipPathRejects) {
  // objectBoundingBox units and unresolved references are outside the subset.
  CHECK(!parseSvgCursor(std::string{kSvgPrefix} + R"svg(
        <defs><clipPath id="c" clipPathUnits="objectBoundingBox"><rect width="1" height="1"/></clipPath></defs>
        <rect width="4" height="4" clip-path="url(#c)"/>
      )svg" + std::string{kSvgSuffix}));
  CHECK(!parseSvgCursor(wrap(R"svg(<rect width="4" height="4" clip-path="url(#missing)"/>)svg")));
  CHECK(!parseSvgCursor(wrap(R"svg(<rect width="4" height="4" clip-path="path('M0 0')"/>)svg")));
  // clip-path="none" is valid and produces no clip reference.
  const auto image = parseSvgCursor(wrap(R"svg(<rect width="4" height="4" clip-path="none"/>)svg"));
  CHECK(image.has_value());
  CHECK(image->items[0].clipRefs.empty());
}

int main() { return RUN_TESTS(); }
