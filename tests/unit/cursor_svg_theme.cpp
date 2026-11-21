#include "check.h"
#include "input/cursor_svg.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace {

  void writeFile(const std::filesystem::path& path, std::string_view contents) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << contents;
  }

  std::filesystem::path makeThemeDir(const std::string& name) {
    const auto dir = std::filesystem::temp_directory_path() / ("umbriel-cursor-svg-test-" + name);
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
  }

  constexpr std::string_view kFrameSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24"><rect width="24" height="24" fill="#000"/></svg>)svg";

} // namespace

UMBRIEL_TEST(cursorSvgThemeStaticCursor) {
  const auto dir = makeThemeDir("static");
  writeFile(dir / "left_ptr.svg", kFrameSvg);
  writeFile(dir / "theme.toml", "[cursor.left_ptr]\nhotspot_x = 0.5\nhotspot_y = 0.5\n");

  umbriel::SvgCursorTheme theme(dir.string());
  CHECK(theme.valid());
  const auto* image = theme.image("left_ptr", 24);
  CHECK(image != nullptr);
  CHECK_EQ(image->frameCount, 1);
  CHECK_EQ(image->frameDelayMs, 0);
  CHECK_EQ(image->pixelHeight, 96); // 24 logical * 4x supersample
  CHECK(theme.image("missing", 24) == nullptr);

  std::filesystem::remove_all(dir);
}

UMBRIEL_TEST(cursorSvgThemeFrameSequence) {
  const auto dir = makeThemeDir("frames");
  writeFile(dir / "spin" / "f0.svg", kFrameSvg);
  writeFile(dir / "spin" / "f1.svg", kFrameSvg);
  writeFile(
      dir / "theme.toml",
      "[cursor.spin]\n"
      "hotspot_x = 0.25\nhotspot_y = 0.75\n"
      "frames = [\"spin/f0.svg\", \"spin/f1.svg\"]\n"
      "delays = [50, 150]\n"
  );

  umbriel::SvgCursorTheme theme(dir.string());
  CHECK(theme.valid());

  const auto* first = theme.image("spin", 24);
  CHECK(first != nullptr);
  CHECK_EQ(first->frameCount, 2);
  CHECK_EQ(first->frame, 0);
  CHECK_EQ(first->frameDelayMs, 50);

  const auto* second = theme.imageFrame("spin", 24, 1);
  CHECK(second != nullptr);
  CHECK_EQ(second->frame, 1);
  CHECK_EQ(second->frameDelayMs, 150);
  CHECK(second->buffer != first->buffer); // distinct frame renders

  // Out-of-range frames clamp; hotspot is shared across frames.
  const auto* clamped = theme.imageFrame("spin", 24, 99);
  CHECK(clamped != nullptr);
  CHECK_EQ(clamped->frame, 1);
  CHECK_EQ(clamped->hotspotX, first->hotspotX);

  std::filesystem::remove_all(dir);
}

UMBRIEL_TEST(cursorSvgThemeRejectsBadFrames) {
  const auto dir = makeThemeDir("badframes");
  writeFile(dir / "f0.svg", kFrameSvg);
  // Traversal and missing files must not load.
  writeFile(
      dir / "theme.toml",
      "[cursor.evil]\n"
      "hotspot_x = 0\nhotspot_y = 0\n"
      "frames = [\"f0.svg\", \"../outside.svg\"]\n"
  );

  umbriel::SvgCursorTheme theme(dir.string());
  CHECK(theme.valid());
  CHECK(theme.image("evil", 24) == nullptr);

  std::filesystem::remove_all(dir);
}

UMBRIEL_TEST(cursorSvgThemeFrameDefaultDelay) {
  const auto dir = makeThemeDir("defaultdelay");
  writeFile(dir / "f0.svg", kFrameSvg);
  writeFile(dir / "f1.svg", kFrameSvg);
  writeFile(
      dir / "theme.toml",
      "[theme]\nhotspot_x = 0\nhotspot_y = 0\n"
      "[cursor.spin]\n"
      "frames = [\"f0.svg\", \"f1.svg\"]\n"
  );

  umbriel::SvgCursorTheme theme(dir.string());
  const auto* image = theme.image("spin", 24);
  CHECK(image != nullptr);
  CHECK_EQ(image->frameCount, 2);
  CHECK_EQ(image->frameDelayMs, 100); // kDefaultFrameDelayMs

  std::filesystem::remove_all(dir);
}

UMBRIEL_TEST(cursorSvgThemeScalableLayout) {
  const auto dir = makeThemeDir("scalable");
  // KDE/GNOME cursors_scalable layout: <theme>/cursors_scalable/<name>/ with
  // SVG frames and a metadata.json hotspot in nominal-size pixels.
  writeFile(dir / "cursors_scalable" / "left_ptr" / "left_ptr.svg", kFrameSvg);
  writeFile(
      dir / "cursors_scalable" / "left_ptr" / "metadata.json",
      R"json([{"filename": "left_ptr.svg", "hotspot_x": 2, "hotspot_y": 2, "nominal_size": 24}])json"
  );
  // Alias via symlink, as KDE themes provide for alternate names.
  std::filesystem::create_directory_symlink(
      dir / "cursors_scalable" / "left_ptr", dir / "cursors_scalable" / "default"
  );
  writeFile(dir / "cursors_scalable" / "wait" / "w0.svg", kFrameSvg);
  writeFile(dir / "cursors_scalable" / "wait" / "w1.svg", kFrameSvg);
  writeFile(
      dir / "cursors_scalable" / "wait" / "metadata.json",
      R"json([{"filename": "w0.svg", "hotspot_x": 12, "hotspot_y": 12, "nominal_size": 24},
              {"filename": "w1.svg", "hotspot_x": 12, "hotspot_y": 12, "nominal_size": 24}])json"
  );

  umbriel::SvgCursorTheme theme(dir.string());
  CHECK(theme.valid());

  const auto* arrow = theme.image("left_ptr", 24);
  CHECK(arrow != nullptr);
  CHECK_EQ(arrow->frameCount, 1);
  CHECK_EQ(arrow->pixelHeight, 96);
  // hotspot 2/24 normalized -> 8 of 96 texture px scaled back to logical 24.
  CHECK_EQ(arrow->hotspotX, 2);

  const auto* alias = theme.image("default", 24);
  CHECK(alias != nullptr);

  const auto* animated = theme.image("wait", 24);
  CHECK(animated != nullptr);
  CHECK_EQ(animated->frameCount, 2);
  CHECK_EQ(animated->frameDelayMs, 100); // no delay fields -> default

  std::filesystem::remove_all(dir);
}

int main() { return RUN_TESTS(); }
