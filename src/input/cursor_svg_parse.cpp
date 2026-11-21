#include "input/cursor_svg_parse.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace umbriel::cursor_svg {
  namespace {

    // SVG number: optional sign, digits with at most one '.', optional exponent.
    // std::from_chars rejects ".5" and "1." which path data uses freely.
    bool parseNumber(std::string_view& s, double& out) {
      size_t i = 0;
      auto isDigit = [](char c) { return c >= '0' && c <= '9'; };
      if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
        ++i;
      }
      const size_t intStart = i;
      while (i < s.size() && isDigit(s[i])) {
        ++i;
      }
      const bool hasInt = i > intStart;
      bool hasFrac = false;
      if (i < s.size() && s[i] == '.') {
        ++i;
        const size_t fracStart = i;
        while (i < s.size() && isDigit(s[i])) {
          ++i;
        }
        hasFrac = i > fracStart;
      }
      if (!hasInt && !hasFrac) {
        return false;
      }
      if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        const size_t expPos = i;
        ++i;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
          ++i;
        }
        const size_t expStart = i;
        while (i < s.size() && isDigit(s[i])) {
          ++i;
        }
        if (i == expStart) {
          i = expPos;
        }
      }
      const std::string_view token = s.substr(0, i);
      // Normalize forms from_chars rejects: leading '+', bare '.', '.5', '1.'.
      std::string normalized(token);
      if (!normalized.empty() && normalized.front() == '+') {
        normalized.erase(0, 1);
      }
      if (normalized.size() >= 2 && normalized[0] == '-' && normalized[1] == '.') {
        normalized.insert(1, "0");
      } else if (!normalized.empty() && normalized.front() == '.') {
        normalized.insert(0, "0");
      }
      if (!normalized.empty() && normalized.back() == '.') {
        normalized.push_back('0');
      }
      const auto parsed = std::from_chars(normalized.data(), normalized.data() + normalized.size(), out);
      if (parsed.ec != std::errc{} || parsed.ptr != normalized.data() + normalized.size()) {
        return false;
      }
      s.remove_prefix(i);
      return true;
    }

    void skipSeparators(std::string_view& s) {
      while (!s.empty()
             && (s.front() == ','
                 || s.front() == ' '
                 || s.front() == '\t'
                 || s.front() == '\n'
                 || s.front() == '\r'
                 || s.front() == '\f')) {
        s.remove_prefix(1);
      }
    }

    bool parseNumberList(std::string_view text, std::vector<double>& out) {
      std::string_view s = text;
      while (true) {
        skipSeparators(s);
        if (s.empty()) {
          return true;
        }
        double value = 0;
        if (!parseNumber(s, value)) {
          return false;
        }
        out.push_back(value);
      }
    }

    std::string decodeEntities(std::string_view text) {
      std::string out;
      out.reserve(text.size());
      for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '&') {
          out.push_back(text[i]);
          continue;
        }
        const size_t semi = text.find(';', i + 1);
        if (semi == std::string_view::npos) {
          out.push_back(text[i]);
          continue;
        }
        const std::string_view entity = text.substr(i + 1, semi - i - 1);
        if (entity == "amp") {
          out.push_back('&');
        } else if (entity == "lt") {
          out.push_back('<');
        } else if (entity == "gt") {
          out.push_back('>');
        } else if (entity == "quot") {
          out.push_back('"');
        } else if (entity == "apos") {
          out.push_back('\'');
        } else {
          out.append(text.substr(i, semi - i + 1));
        }
        i = semi;
      }
      return out;
    }

    struct XmlElement {
      std::string name;
      std::vector<std::pair<std::string, std::string>> attrs;
      std::vector<XmlElement> children;

      const std::string* attr(std::string_view name) const {
        for (const auto& [key, value] : attrs) {
          if (key == name) {
            return &value;
          }
        }
        return nullptr;
      }
    };

    struct XmlParser {
      std::string_view xml;
      size_t pos = 0;
      bool ok = true;

      void skipMisc() {
        while (pos < xml.size()) {
          if (xml.substr(pos, 4) == "<!--") {
            const size_t end = xml.find("-->", pos + 4);
            if (end == std::string_view::npos) {
              ok = false;
              return;
            }
            pos = end + 3;
          } else if (xml.substr(pos, 2) == "<?") {
            const size_t end = xml.find("?>", pos + 2);
            if (end == std::string_view::npos) {
              ok = false;
              return;
            }
            pos = end + 2;
          } else if (xml.substr(pos, 2) == "<!") {
            const size_t end = xml.find('>', pos + 2);
            if (end == std::string_view::npos) {
              ok = false;
              return;
            }
            pos = end + 1;
          } else {
            return;
          }
        }
      }

      static bool nameChar(char c) {
        return (c >= 'a' && c <= 'z')
            || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9')
            || c == '_'
            || c == '-'
            || c == '.'
            || c == ':';
      }

      std::string readName() {
        const size_t start = pos;
        while (pos < xml.size() && nameChar(xml[pos])) {
          ++pos;
        }
        std::string name{xml.substr(start, pos - start)};
        for (char& c : name) {
          if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
          }
        }
        return name;
      }

      static bool space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

      void skipSpace() {
        while (pos < xml.size() && space(xml[pos])) {
          ++pos;
        }
      }

      // Parses one element starting at '<'. Returns false on malformed input.
      bool element(XmlElement& out, int depth) {
        if (depth > 64 || pos >= xml.size() || xml[pos] != '<') {
          return false;
        }
        ++pos;
        out.name = readName();
        if (out.name.empty()) {
          return false;
        }
        bool selfClosing = false;
        while (pos < xml.size()) {
          skipSpace();
          if (pos >= xml.size()) {
            return false;
          }
          if (xml[pos] == '>') {
            ++pos;
            break;
          }
          if (xml[pos] == '/' && pos + 1 < xml.size() && xml[pos + 1] == '>') {
            selfClosing = true;
            pos += 2;
            break;
          }
          std::string attrName = readName();
          if (attrName.empty()) {
            return false;
          }
          skipSpace();
          if (pos >= xml.size() || xml[pos] != '=') {
            return false;
          }
          ++pos;
          skipSpace();
          if (pos >= xml.size() || (xml[pos] != '"' && xml[pos] != '\'')) {
            return false;
          }
          const char quote = xml[pos++];
          const size_t valueStart = pos;
          while (pos < xml.size() && xml[pos] != quote) {
            ++pos;
          }
          if (pos >= xml.size()) {
            return false;
          }
          out.attrs.emplace_back(std::move(attrName), decodeEntities(xml.substr(valueStart, pos - valueStart)));
          ++pos;
        }
        if (selfClosing) {
          return true;
        }
        while (true) {
          // Skip character data and misc markup between children.
          while (pos < xml.size()) {
            if (xml.substr(pos, 4) == "<!--") {
              const size_t end = xml.find("-->", pos + 4);
              if (end == std::string_view::npos) {
                return false;
              }
              pos = end + 3;
            } else if (xml.substr(pos, 2) == "<?") {
              const size_t end = xml.find("?>", pos + 2);
              if (end == std::string_view::npos) {
                return false;
              }
              pos = end + 2;
            } else if (xml[pos] == '<') {
              break;
            } else {
              ++pos;
            }
          }
          if (!ok || pos >= xml.size()) {
            return false;
          }
          if (pos + 1 < xml.size() && xml[pos + 1] == '/') {
            pos += 2;
            const std::string closeName = readName();
            while (pos < xml.size() && xml[pos] != '>') {
              ++pos;
            }
            if (pos >= xml.size()) {
              return false;
            }
            ++pos;
            return closeName == out.name;
          }
          XmlElement child;
          if (!element(child, depth + 1)) {
            return false;
          }
          out.children.push_back(std::move(child));
        }
      }

      std::vector<XmlElement> document() {
        std::vector<XmlElement> roots;
        while (pos < xml.size()) {
          skipMisc();
          if (!ok || pos >= xml.size()) {
            break;
          }
          if (xml[pos] != '<') {
            ++pos;
            continue;
          }
          if (pos + 1 < xml.size() && xml[pos + 1] == '/') {
            return {};
          }
          XmlElement root;
          if (!element(root, 0)) {
            return {};
          }
          roots.push_back(std::move(root));
        }
        return ok ? roots : std::vector<XmlElement>{};
      }
    };

    using Mat = std::array<double, 6>;
    constexpr Mat kIdentity{1, 0, 0, 1, 0, 0};

    Mat matMul(const Mat& a, const Mat& b) {
      return {
          a[0] * b[0] + a[2] * b[1], a[1] * b[0] + a[3] * b[1],        a[0] * b[2] + a[2] * b[3],
          a[1] * b[2] + a[3] * b[3], a[0] * b[4] + a[2] * b[5] + a[4], a[1] * b[4] + a[3] * b[5] + a[5],
      };
    }

    Mat translate(double x, double y) { return {1, 0, 0, 1, x, y}; }
    Mat scale(double x, double y) { return {x, 0, 0, y, 0, 0}; }
    Mat rotate(double deg) {
      const double r = deg * M_PI / 180.0;
      const double c = std::cos(r);
      const double s = std::sin(r);
      return {c, s, -s, c, 0, 0};
    }
    Mat skewX(double deg) { return {1, 0, std::tan(deg * M_PI / 180.0), 1, 0, 0}; }
    Mat skewY(double deg) { return {1, std::tan(deg * M_PI / 180.0), 0, 1, 0, 0}; }

    // Parses transform="fn(..) fn(..) ...". Functions post-multiply left to
    // right, matching SVG's "applied in order" semantics.
    bool parseTransform(std::string_view text, Mat& out) {
      out = kIdentity;
      std::string_view s = text;
      while (true) {
        skipSeparators(s);
        if (s.empty()) {
          return true;
        }
        size_t nameEnd = 0;
        while (nameEnd < s.size() && s[nameEnd] >= 'a' && s[nameEnd] <= 'z') {
          ++nameEnd;
        }
        if (nameEnd == 0 || nameEnd >= s.size() || s[nameEnd] != '(') {
          return false;
        }
        const std::string_view name = s.substr(0, nameEnd);
        s.remove_prefix(nameEnd + 1); // consume name and '('
        const size_t close = s.find(')');
        if (close == std::string_view::npos) {
          return false;
        }
        std::vector<double> args;
        if (!parseNumberList(s.substr(0, close), args)) {
          return false;
        }
        s.remove_prefix(close + 1);

        Mat fn = kIdentity;
        if (name == "matrix" && args.size() == 6) {
          fn = {args[0], args[1], args[2], args[3], args[4], args[5]};
        } else if (name == "translate" && (args.size() == 1 || args.size() == 2)) {
          fn = translate(args[0], args.size() == 2 ? args[1] : 0);
        } else if (name == "scale" && (args.size() == 1 || args.size() == 2)) {
          fn = scale(args[0], args.size() == 2 ? args[1] : args[0]);
        } else if (name == "rotate" && (args.size() == 1 || args.size() == 3)) {
          fn = rotate(args[0]);
          if (args.size() == 3) {
            fn = matMul(translate(args[1], args[2]), matMul(fn, translate(-args[1], -args[2])));
          }
        } else if (name == "skewx" && args.size() == 1) {
          fn = skewX(args[0]);
        } else if (name == "skewy" && args.size() == 1) {
          fn = skewY(args[0]);
        } else {
          return false;
        }
        out = matMul(out, fn);
      }
    }

    // ---- colors ----

    float clamp01(double v) { return static_cast<float>(std::clamp(v, 0.0, 1.0)); }

    bool hexDigit(char c, int& out) {
      if (c >= '0' && c <= '9') {
        out = c - '0';
        return true;
      }
      if (c >= 'a' && c <= 'f') {
        out = c - 'a' + 10;
        return true;
      }
      if (c >= 'A' && c <= 'F') {
        out = c - 'A' + 10;
        return true;
      }
      return false;
    }

    bool parseHexColor(std::string_view v, Paint& out) {
      // v excludes the leading '#': 3/4 digits are #rgb/#rgba, 6/8 are #rrggbb/#rrggbbaa.
      auto nib = [&](size_t i, int& d) { return hexDigit(v[i], d); };
      if (v.size() == 3 || v.size() == 4) {
        int r, g, b, a = 15;
        if (!nib(0, r) || !nib(1, g) || !nib(2, b) || (v.size() == 4 && !nib(3, a))) {
          return false;
        }
        out = Paint{
            .r = static_cast<float>((r * 17) / 255.0),
            .g = static_cast<float>((g * 17) / 255.0),
            .b = static_cast<float>((b * 17) / 255.0),
            .a = static_cast<float>(a / 15.0),
        };
        return true;
      }
      if (v.size() == 6 || v.size() == 8) {
        auto byte = [&](size_t i) {
          int hi, lo;
          if (!nib(i, hi) || !nib(i + 1, lo)) {
            return -1;
          }
          return hi * 16 + lo;
        };
        const int r = byte(0), g = byte(2), b = byte(4);
        const int a = v.size() == 8 ? byte(6) : 255;
        if (r < 0 || g < 0 || b < 0 || a < 0) {
          return false;
        }
        out = Paint{
            .r = static_cast<float>(r / 255.0),
            .g = static_cast<float>(g / 255.0),
            .b = static_cast<float>(b / 255.0),
            .a = static_cast<float>(a / 255.0),
        };
        return true;
      }
      return false;
    }

    // Splits "rgb(1, 2, 3)" style arguments on commas or whitespace.
    bool splitColorArgs(std::string_view inner, std::vector<std::string_view>& out) {
      size_t start = 0;
      while (start < inner.size()) {
        while (start < inner.size() && (inner[start] == ' ' || inner[start] == '\t' || inner[start] == ',')) {
          ++start;
        }
        if (start >= inner.size()) {
          break;
        }
        size_t end = start;
        while (end < inner.size() && inner[end] != ',' && inner[end] != ' ' && inner[end] != '\t') {
          ++end;
        }
        out.push_back(inner.substr(start, end - start));
        start = end;
      }
      return !out.empty();
    }

    bool parseColorFunction(std::string_view name, std::string_view inner, Paint& out) {
      std::vector<std::string_view> args;
      if (!splitColorArgs(inner, args)) {
        return false;
      }
      auto component = [](std::string_view token, double max, double& value) -> bool {
        if (!token.empty() && token.back() == '%') {
          std::string_view digits = token.substr(0, token.size() - 1);
          double v;
          if (!parseNumber(digits, v)) {
            return false;
          }
          value = v * max / 100.0;
          return true;
        }
        if (!parseNumber(token, value)) {
          return false;
        }
        value = std::clamp(value, 0.0, max);
        return true;
      };
      auto alphaComponent = [](std::string_view token, double& value) -> bool {
        if (!token.empty() && token.back() == '%') {
          std::string_view digits = token.substr(0, token.size() - 1);
          double v;
          if (!parseNumber(digits, v)) {
            return false;
          }
          value = v / 100.0;
          return true;
        }
        return parseNumber(token, value);
      };

      if (name == "rgb" && args.size() == 3) {
        double r, g, b;
        if (!component(args[0], 255, r) || !component(args[1], 255, g) || !component(args[2], 255, b)) {
          return false;
        }
        out = Paint{
            .r = static_cast<float>(r / 255.0),
            .g = static_cast<float>(g / 255.0),
            .b = static_cast<float>(b / 255.0),
            .a = 1,
        };
        return true;
      }
      if (name == "rgba" && (args.size() == 3 || args.size() == 4)) {
        double r, g, b, a = 1;
        if (!component(args[0], 255, r)
            || !component(args[1], 255, g)
            || !component(args[2], 255, b)
            || (args.size() == 4 && !alphaComponent(args[3], a))) {
          return false;
        }
        out = Paint{
            .r = static_cast<float>(r / 255.0),
            .g = static_cast<float>(g / 255.0),
            .b = static_cast<float>(b / 255.0),
            .a = clamp01(a),
        };
        return true;
      }
      if ((name == "hsl" || name == "hsla") && (args.size() == 3 || (name == "hsla" && args.size() == 4))) {
        double h, s, l, a = 1;
        std::string_view hToken = args[0];
        // Bare degrees only; unit suffixes (deg/grad/rad/turn) are rejected.
        if (hToken.find_first_not_of("0123456789+-.eE") != std::string_view::npos) {
          return false;
        }
        double hNorm;
        if (!parseNumber(hToken, hNorm)) {
          return false;
        }
        auto pct = [](std::string_view token, double& value) -> bool {
          if (token.empty() || token.back() != '%') {
            return false;
          }
          std::string_view digits = token.substr(0, token.size() - 1);
          return parseNumber(digits, value);
        };
        if (!pct(args[1], s) || !pct(args[2], l) || (args.size() == 4 && !alphaComponent(args[3], a))) {
          return false;
        }
        h = std::fmod(hNorm, 360.0);
        if (h < 0) {
          h += 360.0;
        }
        s = std::clamp(s, 0.0, 100.0) / 100.0;
        l = std::clamp(l, 0.0, 100.0) / 100.0;
        const double c = (1.0 - std::abs(2.0 * l - 1.0)) * s;
        const double hp = h / 60.0;
        const double x = c * (1.0 - std::abs(std::fmod(hp, 2.0) - 1.0));
        double r = 0, g = 0, b = 0;
        if (hp < 1) {
          r = c;
          g = x;
        } else if (hp < 2) {
          r = x;
          g = c;
        } else if (hp < 3) {
          g = c;
          b = x;
        } else if (hp < 4) {
          g = x;
          b = c;
        } else if (hp < 5) {
          r = x;
          b = c;
        } else {
          r = c;
          b = x;
        }
        const double m = l - c / 2.0;
        out = Paint{
            .r = static_cast<float>(r + m),
            .g = static_cast<float>(g + m),
            .b = static_cast<float>(b + m),
            .a = clamp01(a),
        };
        return true;
      }
      return false;
    }

    // CSS named colors cursor themes plausibly use. Anything else fails the
    // file, which falls back to the Xcursor backend rather than misrendering.
    const std::unordered_map<std::string_view, Paint>& namedColors() {
      static const std::unordered_map<std::string_view, Paint> colors{
          {"black", {0, 0, 0, 1}},
          {"silver", {0.7529f, 0.7529f, 0.7529f, 1}},
          {"gray", {0.5019f, 0.5019f, 0.5019f, 1}},
          {"grey", {0.5019f, 0.5019f, 0.5019f, 1}},
          {"white", {1, 1, 1, 1}},
          {"maroon", {0.5019f, 0, 0, 1}},
          {"red", {1, 0, 0, 1}},
          {"purple", {0.5019f, 0, 0.5019f, 1}},
          {"fuchsia", {1, 0, 1, 1}},
          {"magenta", {1, 0, 1, 1}},
          {"green", {0, 0.5019f, 0, 1}},
          {"lime", {0, 1, 0, 1}},
          {"olive", {0.5019f, 0.5019f, 0, 1}},
          {"yellow", {1, 1, 0, 1}},
          {"navy", {0, 0, 0.5019f, 1}},
          {"blue", {0, 0, 1, 1}},
          {"teal", {0, 0.5019f, 0.5019f, 1}},
          {"aqua", {0, 1, 1, 1}},
          {"cyan", {0, 1, 1, 1}},
          {"orange", {1, 0.6470f, 0, 1}},
          {"transparent", {0, 0, 0, 0}},
      };
      return colors;
    }

    bool parsePaint(std::string_view value, const Paint& currentColor, Paint& out) {
      const size_t first = value.find_first_not_of(" \t\n\r\f");
      if (first == std::string_view::npos) {
        return false;
      }
      value = value.substr(first);
      const size_t last = value.find_last_not_of(" \t\n\r\f");
      value = value.substr(0, last + 1);
      // CSS keywords and named colors are case-insensitive; lowercasing is safe
      // for hex digits, numbers, and function names too.
      std::string lowered(value);
      for (char& c : lowered) {
        if (c >= 'A' && c <= 'Z') {
          c = static_cast<char>(c - 'A' + 'a');
        }
      }
      value = lowered;
      if (value == "currentcolor") {
        out = currentColor;
        return true;
      }
      if (value == "transparent") {
        out = {0, 0, 0, 0};
        return true;
      }
      if (value.empty() || value.rfind("url", 0) == 0) {
        return false;
      }
      if (value.front() == '#') {
        return parseHexColor(value.substr(1), out);
      }
      const size_t paren = value.find('(');
      if (paren != std::string_view::npos && value.back() == ')') {
        const std::string_view name = value.substr(0, paren);
        return parseColorFunction(name, value.substr(paren + 1, value.size() - paren - 2), out);
      }
      const auto named = namedColors().find(value);
      if (named != namedColors().end()) {
        out = named->second;
        return true;
      }
      return false;
    }

    bool isNoneToken(std::string_view raw) {
      const size_t first = raw.find_first_not_of(" \t\n\r\f");
      if (first == std::string_view::npos) {
        return false;
      }
      raw = raw.substr(first, raw.find_last_not_of(" \t\n\r\f") - first + 1);
      if (raw.size() != 4) {
        return false;
      }
      char lowered[4];
      for (size_t i = 0; i < 4; ++i) {
        char c = raw[i];
        lowered[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
      }
      return std::string_view(lowered, 4) == "none";
    }

    // ---- path data ----

    struct PathBuilder {
      std::vector<PathCommand>* out;
      double cx = 0, cy = 0;         // current point
      double sx = 0, sy = 0;         // subpath start
      double lastCx = 0, lastCy = 0; // last cubic control (for S reflection)
      double lastQx = 0, lastQy = 0; // last quadratic control (for T reflection)
      char prevCmd = 0;
      bool ok = true;

      void moveTo(double x, double y) {
        out->push_back({.op = PathOp::Move, .x = x, .y = y});
        cx = sx = x;
        cy = sy = y;
      }
      void lineTo(double x, double y) {
        out->push_back({.op = PathOp::Line, .x = x, .y = y});
        cx = x;
        cy = y;
      }
      void cubicTo(double x1, double y1, double x2, double y2, double x, double y) {
        out->push_back({.op = PathOp::Cubic, .x = x, .y = y, .cx1 = x1, .cy1 = y1, .cx2 = x2, .cy2 = y2});
        lastCx = x2;
        lastCy = y2;
        cx = x;
        cy = y;
      }
      void quadToCubic(double qx, double qy, double x, double y) {
        cubicTo(
            cx + 2.0 / 3.0 * (qx - cx), cy + 2.0 / 3.0 * (qy - cy), x + 2.0 / 3.0 * (qx - x), y + 2.0 / 3.0 * (qy - y),
            x, y
        );
        lastQx = qx;
        lastQy = qy;
      }
      void close() {
        out->push_back({.op = PathOp::Close});
        cx = sx;
        cy = sy;
      }

      // SVG F.6.5 endpoint-to-center arc conversion, emitted as cubics.
      void arcTo(double rx, double ry, double phiDeg, bool large, bool sweep, double x, double y) {
        if (rx == 0 || ry == 0) {
          lineTo(x, y);
          return;
        }
        rx = std::abs(rx);
        ry = std::abs(ry);
        const double phi = phiDeg * M_PI / 180.0;
        const double cosP = std::cos(phi), sinP = std::sin(phi);
        const double dx = (cx - x) / 2.0, dy = (cy - y) / 2.0;
        double x1p = cosP * dx + sinP * dy;
        double y1p = -sinP * dx + cosP * dy;
        double lambda = x1p * x1p / (rx * rx) + y1p * y1p / (ry * ry);
        if (lambda > 1.0) {
          const double s = std::sqrt(lambda);
          rx *= s;
          ry *= s;
        }
        const double rx2 = rx * rx, ry2 = ry * ry;
        const double x1p2 = x1p * x1p, y1p2 = y1p * y1p;
        const double num = rx2 * ry2 - rx2 * y1p2 - ry2 * x1p2;
        const double den = rx2 * y1p2 + ry2 * x1p2;
        const double coef = (large != sweep ? 1.0 : -1.0) * std::sqrt(std::max(0.0, num / den));
        const double cxp = coef * rx * y1p / ry;
        const double cyp = -coef * ry * x1p / rx;
        const double cxr = cosP * cxp - sinP * cyp + (cx + x) / 2.0;
        const double cyr = sinP * cxp + cosP * cyp + (cy + y) / 2.0;

        auto angle = [](double ux, double uy, double vx, double vy) {
          const double dot = ux * vx + uy * vy;
          const double len = std::sqrt((ux * ux + uy * uy) * (vx * vx + vy * vy));
          double a = std::acos(std::clamp(dot / len, -1.0, 1.0));
          if (ux * vy - uy * vx < 0) {
            a = -a;
          }
          return a;
        };
        const double theta1 = angle(1.0, 0.0, (x1p - cxp) / rx, (y1p - cyp) / ry);
        double dTheta = angle((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry);
        if (!sweep && dTheta > 0) {
          dTheta -= 2.0 * M_PI;
        } else if (sweep && dTheta < 0) {
          dTheta += 2.0 * M_PI;
        }
        const int segments = std::max(1, static_cast<int>(std::ceil(std::abs(dTheta) / (M_PI / 2.0))));
        const double delta = dTheta / segments;
        const double alpha = 4.0 / 3.0 * std::tan(delta / 4.0);
        double theta = theta1;
        for (int i = 0; i < segments; ++i) {
          const double t2 = theta + delta;
          const double cosT1 = std::cos(theta), sinT1 = std::sin(theta);
          const double cosT2 = std::cos(t2), sinT2 = std::sin(t2);
          auto point = [&](double c, double s) {
            return std::pair{cxr + rx * c * cosP - ry * s * sinP, cyr + rx * c * sinP + ry * s * cosP};
          };
          auto deriv = [&](double c, double s) {
            return std::pair{-rx * s * cosP - ry * c * sinP, -rx * s * sinP + ry * c * cosP};
          };
          const auto p2 = point(cosT2, sinT2);
          const auto d2 = deriv(cosT2, sinT2);
          const double x2 = p2.first - alpha * d2.first;
          const double y2 = p2.second - alpha * d2.second;
          const auto p1 = point(cosT1, sinT1);
          const auto d1 = deriv(cosT1, sinT1);
          const double x1 = p1.first + alpha * d1.first;
          const double y1 = p1.second + alpha * d1.second;
          out->push_back(
              {.op = PathOp::Cubic, .x = p2.first, .y = p2.second, .cx1 = x1, .cy1 = y1, .cx2 = x2, .cy2 = y2}
          );
          lastCx = x2;
          lastCy = y2;
          cx = p2.first;
          cy = p2.second;
          theta = t2;
        }
        // Land exactly on the endpoint to avoid drift.
        if (!out->empty() && out->back().op == PathOp::Cubic) {
          out->back().x = x;
          out->back().y = y;
          cx = x;
          cy = y;
        }
      }
    };

    int arityFor(char cmd) {
      switch (cmd) {
      case 'M':
      case 'L':
      case 'T':
        return 2;
      case 'H':
      case 'V':
        return 1;
      case 'C':
        return 6;
      case 'S':
      case 'Q':
        return 4;
      case 'A':
        return 7;
      default:
        return -1;
      }
    }

    bool parsePathData(std::string_view d, std::vector<PathCommand>& out) {
      PathBuilder b{.out = &out};
      std::string_view s = d;
      char cmd = 0;
      while (true) {
        skipSeparators(s);
        if (s.empty()) {
          return b.ok && !out.empty();
        }
        if ((s.front() >= 'a' && s.front() <= 'z') || (s.front() >= 'A' && s.front() <= 'Z')) {
          cmd = s.front();
          s.remove_prefix(1);
          if (cmd == 'Z' || cmd == 'z') {
            if (out.empty() || b.cx != b.sx || b.cy != b.sy) {
              b.close();
            }
            b.prevCmd = 0;
            continue;
          }
        } else if (cmd == 0) {
          return false;
        }
        // Implicit lineto after an initial moveto pair.
        const char effective = (cmd == 'M' && b.prevCmd == 'M') ? 'L' : cmd;
        const bool relative = effective >= 'a' && effective <= 'z';
        const char rel = relative ? static_cast<char>(effective - 'a' + 'A') : effective;
        const int arity = arityFor(rel);
        if (arity < 0) {
          return false;
        }
        double v[7] = {};
        for (int i = 0; i < arity; ++i) {
          skipSeparators(s);
          if (s.empty() || !parseNumber(s, v[i])) {
            return false;
          }
        }
        const auto rx = [&](double x) { return relative ? x + b.cx : x; };
        const auto ry = [&](double y) { return relative ? y + b.cy : y; };
        switch (rel) {
        case 'M':
          b.moveTo(rx(v[0]), ry(v[1]));
          cmd = relative ? 'l' : 'L'; // subsequent pairs are linetos
          break;
        case 'L':
          b.lineTo(rx(v[0]), ry(v[1]));
          break;
        case 'H':
          b.lineTo(relative ? v[0] + b.cx : v[0], b.cy);
          break;
        case 'V':
          b.lineTo(b.cx, relative ? v[0] + b.cy : v[0]);
          break;
        case 'C':
          b.cubicTo(rx(v[0]), ry(v[1]), rx(v[2]), ry(v[3]), rx(v[4]), ry(v[5]));
          break;
        case 'S': {
          double x1 = b.cx, y1 = b.cy;
          if (b.prevCmd == 'C' || b.prevCmd == 'S' || b.prevCmd == 'c' || b.prevCmd == 's') {
            x1 = 2 * b.cx - b.lastCx;
            y1 = 2 * b.cy - b.lastCy;
          }
          b.cubicTo(x1, y1, rx(v[0]), ry(v[1]), rx(v[2]), ry(v[3]));
          break;
        }
        case 'Q':
          b.quadToCubic(rx(v[0]), ry(v[1]), rx(v[2]), ry(v[3]));
          break;
        case 'T': {
          double qx = b.cx, qy = b.cy;
          if (b.prevCmd == 'Q' || b.prevCmd == 'T' || b.prevCmd == 'q' || b.prevCmd == 't') {
            qx = 2 * b.cx - b.lastQx;
            qy = 2 * b.cy - b.lastQy;
          }
          b.quadToCubic(qx, qy, rx(v[0]), ry(v[1]));
          break;
        }
        case 'A':
          b.arcTo(v[0], v[1], v[2], v[3] != 0, v[4] != 0, rx(v[5]), ry(v[6]));
          break;
        default:
          return false;
        }
        b.prevCmd = rel;
      }
    }

    // ---- shapes ----

    // Geometry attributes accept an optional "px" suffix; other units reject.
    bool attrNumber(const XmlElement& el, std::string_view name, double fallback, double& out) {
      const std::string* v = el.attr(name);
      if (v == nullptr) {
        out = fallback;
        return true;
      }
      std::string_view s = *v;
      skipSeparators(s);
      if (!parseNumber(s, out)) {
        return false;
      }
      skipSeparators(s);
      if (s.size() >= 2 && s.substr(0, 2) == "px") {
        s.remove_prefix(2);
        skipSeparators(s);
      }
      return s.empty();
    }

    bool shapePath(const XmlElement& el, std::vector<PathCommand>& out) {
      const std::string* d = el.attr("d");
      if (el.name == "path") {
        if (d == nullptr || d->find_first_not_of(" \t\n\r\f") == std::string::npos) {
          return true; // no geometry
        }
        return parsePathData(*d, out);
      }
      if (el.name == "rect") {
        double x, y, w, h;
        if (!attrNumber(el, "x", 0, x)
            || !attrNumber(el, "y", 0, y)
            || !attrNumber(el, "width", 0, w)
            || !attrNumber(el, "height", 0, h)) {
          return false;
        }
        if (w <= 0 || h <= 0) {
          return true;
        }
        double rx = 0, ry = 0;
        if (!attrNumber(el, "rx", 0, rx) || !attrNumber(el, "ry", rx, ry)) {
          return false;
        }
        rx = std::clamp(rx, 0.0, w / 2.0);
        ry = std::clamp(ry, 0.0, h / 2.0);
        if (rx == 0 || ry == 0) {
          out = {
              {.op = PathOp::Move, .x = x, .y = y},
              {.op = PathOp::Line, .x = x + w, .y = y},
              {.op = PathOp::Line, .x = x + w, .y = y + h},
              {.op = PathOp::Line, .x = x, .y = y + h},
              {.op = PathOp::Close},
          };
          return true;
        }
        constexpr double k = 0.5522847498;
        const double ax = rx * k, ay = ry * k;
        out = {
            {.op = PathOp::Move, .x = x + rx, .y = y},
            {.op = PathOp::Line, .x = x + w - rx, .y = y},
            {.op = PathOp::Cubic,
             .x = x + w,
             .y = y + ry,
             .cx1 = x + w - rx + ax,
             .cy1 = y,
             .cx2 = x + w,
             .cy2 = y + ry - ay},
            {.op = PathOp::Line, .x = x + w, .y = y + h - ry},
            {.op = PathOp::Cubic,
             .x = x + w - rx,
             .y = y + h,
             .cx1 = x + w,
             .cy1 = y + h - ry + ay,
             .cx2 = x + w - rx + ax,
             .cy2 = y + h},
            {.op = PathOp::Line, .x = x + rx, .y = y + h},
            {.op = PathOp::Cubic,
             .x = x,
             .y = y + h - ry,
             .cx1 = x + rx - ax,
             .cy1 = y + h,
             .cx2 = x,
             .cy2 = y + h - ry + ay},
            {.op = PathOp::Line, .x = x, .y = y + ry},
            {.op = PathOp::Cubic, .x = x + rx, .y = y, .cx1 = x, .cy1 = y + ry - ay, .cx2 = x + rx - ax, .cy2 = y},
            {.op = PathOp::Close},
        };
        return true;
      }
      if (el.name == "circle" || el.name == "ellipse") {
        double cx, cy, rx, ry;
        if (el.name == "circle") {
          if (!attrNumber(el, "cx", 0, cx) || !attrNumber(el, "cy", 0, cy) || !attrNumber(el, "r", 0, rx)) {
            return false;
          }
          ry = rx;
        } else {
          if (!attrNumber(el, "cx", 0, cx)
              || !attrNumber(el, "cy", 0, cy)
              || !attrNumber(el, "rx", 0, rx)
              || !attrNumber(el, "ry", 0, ry)) {
            return false;
          }
        }
        if (rx <= 0 || ry <= 0) {
          return true;
        }
        constexpr double k = 0.5522847498;
        const double kx = rx * k, ky = ry * k;
        out = {
            {.op = PathOp::Move, .x = cx + rx, .y = cy},
            {.op = PathOp::Cubic,
             .x = cx,
             .y = cy + ry,
             .cx1 = cx + rx + kx,
             .cy1 = cy,
             .cx2 = cx + rx,
             .cy2 = cy + ry - ky},
            {.op = PathOp::Cubic,
             .x = cx - rx,
             .y = cy,
             .cx1 = cx - rx - kx,
             .cy1 = cy + ry,
             .cx2 = cx - rx,
             .cy2 = cy + ry + ky},
            {.op = PathOp::Cubic,
             .x = cx,
             .y = cy - ry,
             .cx1 = cx - rx,
             .cy1 = cy - ry - ky,
             .cx2 = cx - rx - kx,
             .cy2 = cy - ry},
            {.op = PathOp::Cubic,
             .x = cx + rx,
             .y = cy,
             .cx1 = cx + rx,
             .cy1 = cy - ry + ky,
             .cx2 = cx + rx + kx,
             .cy2 = cy - ry},
            {.op = PathOp::Close},
        };
        return true;
      }
      if (el.name == "line" || el.name == "polyline" || el.name == "polygon") {
        if (el.name == "line") {
          double x1, y1, x2, y2;
          if (!attrNumber(el, "x1", 0, x1)
              || !attrNumber(el, "y1", 0, y1)
              || !attrNumber(el, "x2", 0, x2)
              || !attrNumber(el, "y2", 0, y2)) {
            return false;
          }
          out = {
              {.op = PathOp::Move, .x = x1, .y = y1},
              {.op = PathOp::Line, .x = x2, .y = y2},
          };
          return true;
        }
        const std::string* points = el.attr("points");
        if (points == nullptr) {
          return false;
        }
        std::vector<double> coords;
        if (!parseNumberList(*points, coords) || coords.size() < 4 || coords.size() % 2 != 0) {
          return false;
        }
        out.push_back({.op = PathOp::Move, .x = coords[0], .y = coords[1]});
        for (size_t i = 2; i < coords.size(); i += 2) {
          out.push_back({.op = PathOp::Line, .x = coords[i], .y = coords[i + 1]});
        }
        if (el.name == "polygon") {
          out.push_back({.op = PathOp::Close});
        }
        return true;
      }
      return false;
    }

    // ---- tree walk ----

    struct PaintState {
      Paint fill{0, 0, 0, 1};
      bool hasFill = true;
      Paint stroke{0, 0, 0, 0};
      bool hasStroke = false;
      Paint color{0, 0, 0, 1}; // currentColor
      double strokeWidth = 1;
      double fillOpacity = 1;
      double strokeOpacity = 1;
      double opacity = 1;
      int lineCap = 0;
      int lineJoin = 0;
      bool evenOdd = false;
      Mat m = kIdentity;
      // clip-path references inherited from ancestor elements.
      std::vector<ClipRef> clipRefs;
    };

    bool isShape(const std::string& name) {
      return name == "path"
          || name == "rect"
          || name == "circle"
          || name == "ellipse"
          || name == "line"
          || name == "polyline"
          || name == "polygon";
    }

    bool isContainer(const std::string& name) { return name == "g" || name == "svg"; }

    bool isSkipped(const std::string& name) {
      // Editor metadata namespaces never render; definition elements are only
      // meaningful through references we reject anyway.
      if (name.rfind("inkscape:", 0) == 0 || name.rfind("sodipodi:", 0) == 0) {
        return true;
      }
      return name == "defs"
          || name == "title"
          || name == "desc"
          || name == "metadata"
          || name == "symbol"
          || name == "marker"
          || name == "pattern"
          || name == "lineargradient"
          || name == "radialgradient"
          || name == "clippath"
          || name == "mask"
          || name == "filter";
    }

    // Attributes whose presence means rendering we do not model; reject the file.
    // `filter` is deliberately absent: filter references only add post effects
    // (drop shadows, blurs), so the cursor renders without them rather than
    // falling back wholesale. `mask` and `clip-path` change what is visible.
    bool hasUnsupportedAttr(const XmlElement& el) {
      static const char* const rejected[] = {
          "mask", "marker-start", "marker-mid", "marker-end", "stroke-dasharray", "stroke-dashoffset", "vector-effect",
      };
      for (const char* name : rejected) {
        if (el.attr(name) != nullptr) {
          return true;
        }
      }
      return false;
    }

    bool parseOpacityAttr(const std::string* v, double& out) {
      if (v == nullptr) {
        return true;
      }
      std::string_view s = *v;
      skipSeparators(s);
      if (!parseNumber(s, out)) {
        return false;
      }
      skipSeparators(s);
      return s.empty();
    }

    using StyleDecls = std::unordered_map<std::string, std::string>;

    std::string trimToken(std::string_view s) {
      while (
          !s.empty()
          && (s.front() == ' ' || s.front() == '\t' || s.front() == '\n' || s.front() == '\r' || s.front() == '\f')) {
        s.remove_prefix(1);
      }
      while (!s.empty()
             && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n' || s.back() == '\r' || s.back() == '\f')) {
        s.remove_suffix(1);
      }
      return std::string{s};
    }

    // Parses `key: value; ...` declarations. Values may not contain ';' or '{' /
    // '}' blocks; anything else malformed rejects the file.
    bool parseStyleDecls(const std::string& css, StyleDecls& out) {
      size_t pos = 0;
      while (pos <= css.size()) {
        size_t end = css.find(';', pos);
        if (end == std::string::npos) {
          end = css.size();
        }
        const std::string chunk = trimToken(std::string_view{css}.substr(pos, end - pos));
        if (!chunk.empty()) {
          const size_t colon = chunk.find(':');
          if (colon == std::string::npos) {
            return false;
          }
          std::string key = trimToken(std::string_view{chunk}.substr(0, colon));
          std::string value = trimToken(std::string_view{chunk}.substr(colon + 1));
          for (char& c : key) {
            if (c >= 'A' && c <= 'Z') {
              c = static_cast<char>(c - 'A' + 'a');
            }
          }
          if (key.empty() || value.empty()) {
            return false;
          }
          out[std::move(key)] = std::move(value);
        }
        if (end == css.size()) {
          return true;
        }
        pos = end + 1;
      }
      return true;
    }

    // Style declarations win over presentation attributes, matching CSS.
    const std::string* propValue(const StyleDecls& style, const XmlElement& el, std::string_view name) {
      if (auto it = style.find(std::string{name}); it != style.end()) {
        return &it->second;
      }
      return el.attr(name);
    }

    struct Walker {
      SvgCursorImage* image = nullptr;
      const std::unordered_map<std::string, int>* clipIds = nullptr;
      bool ok = true;

      void applyPaintAttr(const XmlElement& el, const StyleDecls& style, PaintState& st) {
        if (const std::string* v = propValue(style, el, "fill")) {
          if (isNoneToken(*v)) {
            st.hasFill = false;
          } else {
            Paint p;
            if (!parsePaint(*v, st.color, p)) {
              ok = false;
              return;
            }
            st.fill = p;
            st.hasFill = true;
          }
        }
        if (const std::string* v = propValue(style, el, "stroke")) {
          if (isNoneToken(*v)) {
            st.hasStroke = false;
          } else {
            Paint p;
            if (!parsePaint(*v, st.color, p)) {
              ok = false;
              return;
            }
            st.stroke = p;
            st.hasStroke = true;
          }
        }
        if (const std::string* v = propValue(style, el, "color")) {
          Paint p;
          if (!parsePaint(*v, st.color, p)) {
            ok = false;
            return;
          }
          st.color = p;
        }
        if (const std::string* v = propValue(style, el, "stroke-width")) {
          std::string_view s = *v;
          skipSeparators(s);
          if (!parseNumber(s, st.strokeWidth)) {
            ok = false;
            return;
          }
          skipSeparators(s);
          if (s.size() >= 2 && s.substr(0, 2) == "px") {
            s.remove_prefix(2);
            skipSeparators(s);
          }
          if (!s.empty() || st.strokeWidth < 0) {
            ok = false;
          }
        }
        if (const std::string* v = propValue(style, el, "fill-opacity")) {
          std::string_view s = *v;
          skipSeparators(s);
          double opacity;
          if (!parseNumber(s, opacity)) {
            ok = false;
            return;
          }
          skipSeparators(s);
          if (!s.empty()) {
            ok = false;
            return;
          }
          st.fillOpacity = std::clamp(opacity, 0.0, 1.0);
        }
        if (const std::string* v = propValue(style, el, "stroke-opacity")) {
          std::string_view s = *v;
          skipSeparators(s);
          double opacity;
          if (!parseNumber(s, opacity)) {
            ok = false;
            return;
          }
          skipSeparators(s);
          if (!s.empty()) {
            ok = false;
            return;
          }
          st.strokeOpacity = std::clamp(opacity, 0.0, 1.0);
        }
        if (propValue(style, el, "opacity") != nullptr) {
          double groupOpacity;
          if (!parseOpacityAttr(propValue(style, el, "opacity"), groupOpacity)) {
            ok = false;
            return;
          }
          st.opacity *= std::clamp(groupOpacity, 0.0, 1.0);
        }
        if (const std::string* v = propValue(style, el, "stroke-linecap")) {
          if (*v == "butt") {
            st.lineCap = 0;
          } else if (*v == "round") {
            st.lineCap = 1;
          } else if (*v == "square") {
            st.lineCap = 2;
          }
        }
        if (const std::string* v = propValue(style, el, "stroke-linejoin")) {
          if (*v == "miter") {
            st.lineJoin = 0;
          } else if (*v == "round") {
            st.lineJoin = 1;
          } else if (*v == "bevel") {
            st.lineJoin = 2;
          }
        }
        if (const std::string* v = propValue(style, el, "fill-rule")) {
          st.evenOdd = *v == "evenodd";
        }
      }

      void element(const XmlElement& el, PaintState st, int depth) {
        if (!ok || depth > 64) {
          return;
        }
        if (isSkipped(el.name)) {
          return;
        }
        if (!isShape(el.name) && !isContainer(el.name)) {
          ok = false; // whitelist: anything else is outside the subset
          return;
        }
        if (hasUnsupportedAttr(el)) {
          ok = false;
          return;
        }
        StyleDecls style;
        if (const std::string* css = el.attr("style")) {
          if (!parseStyleDecls(*css, style)) {
            ok = false;
            return;
          }
          // Dashing is outside the subset; dashoffset alone cannot dash.
          if (const auto it = style.find("stroke-dasharray"); it != style.end() && trimToken(it->second) != "none") {
            ok = false;
            return;
          }
        }
        if (const std::string* display = propValue(style, el, "display");
            display != nullptr && trimToken(*display) == "none") {
          return;
        }
        if (const std::string* visibility = propValue(style, el, "visibility");
            visibility != nullptr && (trimToken(*visibility) == "hidden" || trimToken(*visibility) == "collapse")) {
          return;
        }
        if (const std::string* transform = el.attr("transform")) {
          Mat local;
          if (!parseTransform(*transform, local)) {
            ok = false;
            return;
          }
          st.m = matMul(st.m, local);
        }
        // clip-path="url(#id)" resolves against <clipPath> geometry collected
        // in the pre-pass; the clip's user space is this element's, i.e. st.m
        // after its own transform.
        if (const std::string* clipPath = propValue(style, el, "clip-path")) {
          const std::string value = trimToken(*clipPath);
          if (value != "none") {
            if (value.size() < 6 || value.substr(0, 4) != "url(" || value.back() != ')') {
              ok = false;
              return;
            }
            const std::string ref = value.substr(4, value.size() - 5);
            const auto clipIt = ref.size() >= 2 && ref.front() == '#' ? clipIds->find(ref.substr(1)) : clipIds->end();
            if (clipIt == clipIds->end()) {
              ok = false;
              return;
            }
            ClipRef clipRef;
            clipRef.clip = clipIt->second;
            for (size_t i = 0; i < 6; ++i) {
              clipRef.anchor[i] = st.m[i];
            }
            st.clipRefs.push_back(clipRef);
          }
        }
        applyPaintAttr(el, style, st);
        if (!ok) {
          return;
        }
        if (isShape(el.name)) {
          std::vector<PathCommand> path;
          if (!shapePath(el, path)) {
            ok = false;
            return;
          }
          if (path.empty()) {
            return; // valid element with no visible geometry
          }
          DrawItem item;
          item.path = std::move(path);
          item.clipRefs = st.clipRefs;
          item.m[0] = st.m[0];
          item.m[1] = st.m[1];
          item.m[2] = st.m[2];
          item.m[3] = st.m[3];
          item.m[4] = st.m[4];
          item.m[5] = st.m[5];
          item.strokeWidth = st.strokeWidth;
          item.lineCap = st.lineCap;
          item.lineJoin = st.lineJoin;
          item.evenOdd = st.evenOdd;
          if (st.hasFill) {
            item.hasFill = true;
            item.fill = st.fill;
            item.fill.a = clamp01(static_cast<double>(item.fill.a) * st.opacity * st.fillOpacity);
          }
          if (st.hasStroke) {
            item.hasStroke = true;
            item.stroke = st.stroke;
            item.stroke.a = clamp01(static_cast<double>(item.stroke.a) * st.opacity * st.strokeOpacity);
          }
          image->items.push_back(std::move(item));
          return;
        }
        for (const XmlElement& child : el.children) {
          element(child, st, depth + 1);
          if (!ok) {
            return;
          }
        }
      }
    };

    // First pass: collect <clipPath> geometry anywhere in the document (they
    // usually live in <defs>, which the render walk skips). clipPathUnits
    // other than userSpaceOnUse is outside the subset.
    struct ClipCollector {
      SvgCursorImage* image = nullptr;
      std::unordered_map<std::string, int> ids;
      bool ok = true;

      void clipChild(const XmlElement& el, Mat m, int depth) {
        if (!ok || depth > 64) {
          return;
        }
        if (isSkipped(el.name)) {
          return;
        }
        if (!isShape(el.name) && el.name != "g" && el.name != "svg") {
          ok = false;
          return;
        }
        if (const std::string* transform = el.attr("transform")) {
          Mat local;
          if (!parseTransform(*transform, local)) {
            ok = false;
            return;
          }
          m = matMul(m, local);
        }
        if (isShape(el.name)) {
          std::vector<PathCommand> path;
          if (!shapePath(el, path)) {
            ok = false;
            return;
          }
          if (path.empty()) {
            return;
          }
          ClipShape shape;
          shape.path = std::move(path);
          for (size_t i = 0; i < 6; ++i) {
            shape.m[i] = m[i];
          }
          if (const std::string* rule = el.attr("clip-rule")) {
            shape.evenOdd = *rule == "evenodd";
          }
          image->clips.back().push_back(std::move(shape));
          return;
        }
        for (const XmlElement& child : el.children) {
          clipChild(child, m, depth + 1);
          if (!ok) {
            return;
          }
        }
      }

      void clipPath(const XmlElement& el) {
        const std::string* id = el.attr("id");
        if (id == nullptr || id->empty()) {
          return; // unreferenceable; harmless
        }
        if (const std::string* units = el.attr("clippathunits")) {
          std::string lowered = *units;
          for (char& c : lowered) {
            if (c >= 'A' && c <= 'Z') {
              c = static_cast<char>(c - 'A' + 'a');
            }
          }
          if (lowered != "userspaceonuse") {
            ok = false;
            return;
          }
        }
        image->clips.emplace_back();
        Mat m = kIdentity;
        if (const std::string* transform = el.attr("transform")) {
          Mat local;
          if (!parseTransform(*transform, local)) {
            ok = false;
            return;
          }
          m = matMul(m, local);
        }
        for (const XmlElement& child : el.children) {
          clipChild(child, m, 0);
          if (!ok) {
            return;
          }
        }
        ids[*id] = static_cast<int>(image->clips.size()) - 1;
      }

      void collect(const XmlElement& el) {
        if (!ok) {
          return;
        }
        if (el.name == "clippath") {
          clipPath(el);
          return;
        }
        for (const XmlElement& child : el.children) {
          collect(child);
        }
      }
    };

    bool parseViewBox(const XmlElement& svg, SvgCursorImage& image) {
      if (const std::string* vb = svg.attr("viewbox")) {
        std::vector<double> values;
        if (!parseNumberList(*vb, values) || values.size() != 4 || values[2] <= 0 || values[3] <= 0) {
          return false;
        }
        image.viewX = values[0];
        image.viewY = values[1];
        image.width = values[2];
        image.height = values[3];
        return true;
      }
      double w = 0, h = 0;
      if (!attrNumber(svg, "width", 0, w) || !attrNumber(svg, "height", 0, h)) {
        return false;
      }
      if (w <= 0 || h <= 0) {
        return false;
      }
      image.width = w;
      image.height = h;
      return true;
    }

  } // namespace

  std::optional<SvgCursorImage> parseSvgCursor(std::string_view xml) {
    XmlParser parser{.xml = xml};
    const std::vector<XmlElement> roots = parser.document();
    if (roots.size() != 1 || roots[0].name != "svg") {
      return std::nullopt;
    }
    const XmlElement& svg = roots[0];
    SvgCursorImage image;
    if (!parseViewBox(svg, image)) {
      return std::nullopt;
    }
    ClipCollector collector{.image = &image, .ids = {}, .ok = true};
    collector.collect(svg);
    if (!collector.ok) {
      return std::nullopt;
    }
    Walker walker{.image = &image, .clipIds = &collector.ids};
    PaintState state;
    walker.element(svg, state, 0);
    if (!walker.ok || image.items.empty()) {
      return std::nullopt;
    }
    return image;
  }

} // namespace umbriel::cursor_svg
