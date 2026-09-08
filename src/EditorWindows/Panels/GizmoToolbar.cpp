#include "Engine.h"
namespace GizmoToolbar {
enum class Icon {
  Translate,
  Rotate,
  Scale,
  Bounds,
  Universal,
  Mesh,
  GizmoToggle,
  GridToggle,
  SnapToggle,
  LocalMode,
  WorldMode,
  UiWorldToggle
};

ImVec4 ScaleColor(const ImVec4 &c, float s) {
  return ImVec4(std::clamp(c.x * s, 0.0f, 1.0f),
                std::clamp(c.y * s, 0.0f, 1.0f),
                std::clamp(c.z * s, 0.0f, 1.0f), c.w);
}

bool TextButton(const char *label, bool active, const ImVec2 &size,
                       ImU32 base, ImU32 hover, ImU32 activeCol, ImU32 accent,
                       ImU32 textColor) {
  ImGui::PushStyleColor(ImGuiCol_Button, active ? accent : base);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, active ? accent : hover);
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, active ? accent : activeCol);
  ImGui::PushStyleColor(ImGuiCol_Text,
                        ImGui::ColorConvertU32ToFloat4(textColor));
  ImGui::SetNextItemAllowOverlap();
  bool pressed = ImGui::Button(label, size);
  ImGui::PopStyleColor(4);
  return pressed;
}

static void GetIconBounds(const ImVec2 &min, const ImVec2 &max, ImVec2 &outMin,
                          ImVec2 &outMax) {
  float size = std::min(max.x - min.x, max.y - min.y);
  ImVec2 center = ImVec2((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
  outMin = ImVec2(center.x - size * 0.5f, center.y - size * 0.5f);
  outMax = ImVec2(center.x + size * 0.5f, center.y + size * 0.5f);
}

struct SvgPathSpec {
  const char *d;
  bool stroke;
};

struct SvgIconSpec {
  float viewW;
  float viewH;
  const SvgPathSpec *paths;
  int pathCount;
};

struct SvgSubpath {
  std::vector<ImVec2> points;
  bool closed = false;
  bool stroke = false;
};

struct SvgIconCache {
  bool built = false;
  std::vector<SvgSubpath> subpaths;
};

static bool IsCommandChar(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static void SkipSvgSeparators(const char *&s) {
  while (*s) {
    if (*s == ' ' || *s == '\n' || *s == '\t' || *s == '\r' || *s == ',') {
      ++s;
      continue;
    }
    break;
  }
}

static bool ParseSvgNumber(const char *&s, float &out) {
  SkipSvgSeparators(s);
  if (!*s)
    return false;
  char *end = nullptr;
  out = strtof(s, &end);
  if (end == s)
    return false;
  s = end;
  return true;
}

static ImVec2 SvgLerp(const ImVec2 &a, const ImVec2 &b, float t) {
  return ImVec2(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t);
}

static ImVec2 SvgCubic(const ImVec2 &p0, const ImVec2 &p1, const ImVec2 &p2,
                       const ImVec2 &p3, float t) {
  ImVec2 a = SvgLerp(p0, p1, t);
  ImVec2 b = SvgLerp(p1, p2, t);
  ImVec2 c = SvgLerp(p2, p3, t);
  ImVec2 d = SvgLerp(a, b, t);
  ImVec2 e = SvgLerp(b, c, t);
  return SvgLerp(d, e, t);
}

static void AppendSvgCubic(std::vector<ImVec2> &pts, const ImVec2 &p0,
                           const ImVec2 &p1, const ImVec2 &p2, const ImVec2 &p3,
                           int segments) {
  for (int i = 1; i <= segments; ++i) {
    float t = static_cast<float>(i) / static_cast<float>(segments);
    pts.push_back(SvgCubic(p0, p1, p2, p3, t));
  }
}

static float SvgVectorAngle(const ImVec2 &u, const ImVec2 &v) {
  float dot = u.x * v.x + u.y * v.y;
  float det = u.x * v.y - u.y * v.x;
  return std::atan2(det, dot);
}

static void AppendSvgArc(std::vector<ImVec2> &pts, const ImVec2 &start,
                         const ImVec2 &end, float rx, float ry,
                         float xAxisRotationDeg, bool largeArc, bool sweep) {
  if (rx == 0.0f || ry == 0.0f) {
    pts.push_back(end);
    return;
  }

  float phi = xAxisRotationDeg * (IM_PI / 180.0f);
  float cosPhi = std::cos(phi);
  float sinPhi = std::sin(phi);

  float dx2 = (start.x - end.x) * 0.5f;
  float dy2 = (start.y - end.y) * 0.5f;
  float x1p = cosPhi * dx2 + sinPhi * dy2;
  float y1p = -sinPhi * dx2 + cosPhi * dy2;

  rx = std::fabs(rx);
  ry = std::fabs(ry);

  float lambda = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
  if (lambda > 1.0f) {
    float s = std::sqrt(lambda);
    rx *= s;
    ry *= s;
  }

  float rx2 = rx * rx;
  float ry2 = ry * ry;
  float x1p2 = x1p * x1p;
  float y1p2 = y1p * y1p;

  float denom = (rx2 * y1p2 + ry2 * x1p2);
  float num = rx2 * ry2 - rx2 * y1p2 - ry2 * x1p2;
  float coef = 0.0f;
  if (denom > 0.0f) {
    float sign = (largeArc == sweep) ? -1.0f : 1.0f;
    coef = sign * std::sqrt(std::max(0.0f, num / denom));
  }

  float cxp = coef * (rx * y1p / ry);
  float cyp = coef * (-ry * x1p / rx);

  float cx = cosPhi * cxp - sinPhi * cyp + (start.x + end.x) * 0.5f;
  float cy = sinPhi * cxp + cosPhi * cyp + (start.y + end.y) * 0.5f;

  ImVec2 v1((x1p - cxp) / rx, (y1p - cyp) / ry);
  ImVec2 v2((-x1p - cxp) / rx, (-y1p - cyp) / ry);

  float startAngle = std::atan2(v1.y, v1.x);
  float deltaAngle = SvgVectorAngle(v1, v2);
  if (!sweep && deltaAngle > 0.0f)
    deltaAngle -= 2.0f * IM_PI;
  if (sweep && deltaAngle < 0.0f)
    deltaAngle += 2.0f * IM_PI;

  float absDelta = std::fabs(deltaAngle);
  int segments =
      std::max(4, static_cast<int>(std::ceil(absDelta / (IM_PI / 8.0f))));
  for (int i = 1; i <= segments; ++i) {
    float t = static_cast<float>(i) / static_cast<float>(segments);
    float angle = startAngle + deltaAngle * t;
    float cosA = std::cos(angle);
    float sinA = std::sin(angle);
    float x = cx + cosPhi * rx * cosA - sinPhi * ry * sinA;
    float y = cy + sinPhi * rx * cosA + cosPhi * ry * sinA;
    pts.push_back(ImVec2(x, y));
  }
}

static void FinalizeSvgSubpath(std::vector<SvgSubpath> &out,
                               std::vector<ImVec2> &current, bool closed,
                               bool stroke) {
  if (current.size() < 2) {
    current.clear();
    return;
  }
  bool shouldClose = closed || !stroke;
  if (shouldClose && current.size() > 2) {
    if (current.front().x != current.back().x ||
        current.front().y != current.back().y) {
      current.push_back(current.front());
    }
  }
  SvgSubpath sub;
  sub.points = std::move(current);
  sub.closed = shouldClose;
  sub.stroke = stroke;
  out.push_back(std::move(sub));
  current.clear();
}

static void ParseSvgPathData(const char *d, std::vector<SvgSubpath> &out,
                             bool stroke) {
  const char *s = d;
  char cmd = 0;
  ImVec2 cur(0, 0);
  ImVec2 start(0, 0);
  ImVec2 lastCtrl(0, 0);
  bool hasCtrl = false;
  std::vector<ImVec2> current;

  while (*s) {
    SkipSvgSeparators(s);
    if (!*s)
      break;
    if (IsCommandChar(*s)) {
      cmd = *s++;
    } else if (!cmd) {
      break;
    }

    bool relative = (cmd >= 'a' && cmd <= 'z');
    char upper =
        static_cast<char>(std::toupper(static_cast<unsigned char>(cmd)));

    if (upper == 'M') {
      float x, y;
      if (!ParseSvgNumber(s, x) || !ParseSvgNumber(s, y))
        break;
      if (relative) {
        cur.x += x;
        cur.y += y;
      } else {
        cur = ImVec2(x, y);
      }
      FinalizeSvgSubpath(out, current, false, stroke);
      current.push_back(cur);
      start = cur;
      hasCtrl = false;

      while (ParseSvgNumber(s, x) && ParseSvgNumber(s, y)) {
        ImVec2 p = relative ? ImVec2(cur.x + x, cur.y + y) : ImVec2(x, y);
        cur = p;
        current.push_back(cur);
      }
      continue;
    }

    if (upper == 'Z') {
      FinalizeSvgSubpath(out, current, true, stroke);
      cur = start;
      hasCtrl = false;
      continue;
    }

    if (upper == 'L') {
      float x, y;
      while (ParseSvgNumber(s, x) && ParseSvgNumber(s, y)) {
        cur = relative ? ImVec2(cur.x + x, cur.y + y) : ImVec2(x, y);
        current.push_back(cur);
      }
      hasCtrl = false;
      continue;
    }

    if (upper == 'H') {
      float x;
      while (ParseSvgNumber(s, x)) {
        cur.x = relative ? cur.x + x : x;
        current.push_back(cur);
      }
      hasCtrl = false;
      continue;
    }

    if (upper == 'V') {
      float y;
      while (ParseSvgNumber(s, y)) {
        cur.y = relative ? cur.y + y : y;
        current.push_back(cur);
      }
      hasCtrl = false;
      continue;
    }

    if (upper == 'C') {
      float x1, y1, x2, y2, x, y;
      while (ParseSvgNumber(s, x1) && ParseSvgNumber(s, y1) &&
             ParseSvgNumber(s, x2) && ParseSvgNumber(s, y2) &&
             ParseSvgNumber(s, x) && ParseSvgNumber(s, y)) {
        ImVec2 p1 = relative ? ImVec2(cur.x + x1, cur.y + y1) : ImVec2(x1, y1);
        ImVec2 p2 = relative ? ImVec2(cur.x + x2, cur.y + y2) : ImVec2(x2, y2);
        ImVec2 p3 = relative ? ImVec2(cur.x + x, cur.y + y) : ImVec2(x, y);
        AppendSvgCubic(current, cur, p1, p2, p3, 12);
        cur = p3;
        lastCtrl = p2;
        hasCtrl = true;
      }
      continue;
    }

    if (upper == 'S') {
      float x2, y2, x, y;
      while (ParseSvgNumber(s, x2) && ParseSvgNumber(s, y2) &&
             ParseSvgNumber(s, x) && ParseSvgNumber(s, y)) {
        ImVec2 p1 = hasCtrl ? ImVec2(cur.x * 2.0f - lastCtrl.x,
                                     cur.y * 2.0f - lastCtrl.y)
                            : cur;
        ImVec2 p2 = relative ? ImVec2(cur.x + x2, cur.y + y2) : ImVec2(x2, y2);
        ImVec2 p3 = relative ? ImVec2(cur.x + x, cur.y + y) : ImVec2(x, y);
        AppendSvgCubic(current, cur, p1, p2, p3, 12);
        cur = p3;
        lastCtrl = p2;
        hasCtrl = true;
      }
      continue;
    }

    if (upper == 'A') {
      float rx, ry, xRot, largeFlag, sweepFlag, x, y;
      while (ParseSvgNumber(s, rx) && ParseSvgNumber(s, ry) &&
             ParseSvgNumber(s, xRot) && ParseSvgNumber(s, largeFlag) &&
             ParseSvgNumber(s, sweepFlag) && ParseSvgNumber(s, x) &&
             ParseSvgNumber(s, y)) {
        ImVec2 end = relative ? ImVec2(cur.x + x, cur.y + y) : ImVec2(x, y);
        AppendSvgArc(current, cur, end, rx, ry, xRot, largeFlag != 0.0f,
                     sweepFlag != 0.0f);
        cur = end;
        hasCtrl = false;
      }
      continue;
    }

    hasCtrl = false;
  }

  FinalizeSvgSubpath(out, current, false, stroke);
}

static float SvgArea(const std::vector<ImVec2> &pts) {
  if (pts.size() < 3)
    return 0.0f;
  float a = 0.0f;
  for (size_t i = 0; i + 1 < pts.size(); ++i) {
    a += pts[i].x * pts[i + 1].y - pts[i + 1].x * pts[i].y;
  }
  return a * 0.5f;
}

static bool SvgPointInTri(const ImVec2 &p, const ImVec2 &a, const ImVec2 &b,
                          const ImVec2 &c) {
  float ab = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
  float bc = (c.x - b.x) * (p.y - b.y) - (c.y - b.y) * (p.x - b.x);
  float ca = (a.x - c.x) * (p.y - c.y) - (a.y - c.y) * (p.x - c.x);
  bool hasNeg = (ab < 0.0f) || (bc < 0.0f) || (ca < 0.0f);
  bool hasPos = (ab > 0.0f) || (bc > 0.0f) || (ca > 0.0f);
  return !(hasNeg && hasPos);
}

static void SvgTriangulate(const std::vector<ImVec2> &pts,
                           std::vector<ImVec2> &outTris) {
  outTris.clear();
  if (pts.size() < 3)
    return;

  std::vector<ImVec2> poly = pts;
  if (poly.front().x == poly.back().x && poly.front().y == poly.back().y) {
    poly.pop_back();
  }
  int n = static_cast<int>(poly.size());
  if (n < 3)
    return;

  std::vector<int> idx(n);
  float area = SvgArea(poly);
  if (area > 0.0f) {
    for (int i = 0; i < n; ++i)
      idx[i] = i;
  } else {
    for (int i = 0; i < n; ++i)
      idx[i] = n - 1 - i;
  }

  int guard = 0;
  while (idx.size() > 2 && guard < 10000) {
    bool earFound = false;
    int m = static_cast<int>(idx.size());
    for (int i = 0; i < m; ++i) {
      int i0 = idx[(i + m - 1) % m];
      int i1 = idx[i];
      int i2 = idx[(i + 1) % m];
      const ImVec2 &a = poly[i0];
      const ImVec2 &b = poly[i1];
      const ImVec2 &c = poly[i2];

      float cross = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
      if (cross <= 0.0f)
        continue;

      bool anyInside = false;
      for (int j = 0; j < m; ++j) {
        int ii = idx[j];
        if (ii == i0 || ii == i1 || ii == i2)
          continue;
        if (SvgPointInTri(poly[ii], a, b, c)) {
          anyInside = true;
          break;
        }
      }
      if (anyInside)
        continue;

      outTris.push_back(a);
      outTris.push_back(b);
      outTris.push_back(c);
      idx.erase(idx.begin() + i);
      earFound = true;
      break;
    }
    if (!earFound)
      break;
    ++guard;
  }
}

static void BuildSvgIconCache(const SvgIconSpec &spec, SvgIconCache &cache) {
  if (cache.built)
    return;
  for (int i = 0; i < spec.pathCount; ++i) {
    ParseSvgPathData(spec.paths[i].d, cache.subpaths, spec.paths[i].stroke);
  }
  cache.built = true;
}

static ImVec2 SvgTransformPoint(const ImVec2 &p, const ImVec2 &min,
                                const ImVec2 &max, float viewW, float viewH,
                                float scaleFactor) {
  float size = std::min(max.x - min.x, max.y - min.y) * scaleFactor;
  ImVec2 center = ImVec2((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
  float scale = size / std::max(viewW, viewH);
  ImVec2 offset = ImVec2(center.x - (viewW * scale) * 0.5f,
                         center.y - (viewH * scale) * 0.5f);
  return ImVec2(offset.x + p.x * scale, offset.y + p.y * scale);
}

static void DrawSvgIcon(ImDrawList *drawList, const SvgIconSpec &spec,
                        SvgIconCache &cache, const ImVec2 &min,
                        const ImVec2 &max, ImU32 color, float strokeScale,
                        float scaleFactor) {
  BuildSvgIconCache(spec, cache);
  std::vector<ImVec2> tris;
  for (const SvgSubpath &sub : cache.subpaths) {
    if (sub.points.size() < 2)
      continue;
    if (sub.stroke) {
      drawList->PathClear();
      for (const ImVec2 &p : sub.points) {
        drawList->PathLineTo(SvgTransformPoint(p, min, max, spec.viewW,
                                               spec.viewH, scaleFactor));
      }
      ImDrawFlags flags = sub.closed ? ImDrawFlags_Closed : 0;
      drawList->PathStroke(color, flags, strokeScale);
    } else {
      SvgTriangulate(sub.points, tris);
      if (tris.empty())
        continue;
      for (size_t i = 0; i + 2 < tris.size(); i += 3) {
        ImVec2 a = SvgTransformPoint(tris[i], min, max, spec.viewW, spec.viewH,
                                     scaleFactor);
        ImVec2 b = SvgTransformPoint(tris[i + 1], min, max, spec.viewW,
                                     spec.viewH, scaleFactor);
        ImVec2 c = SvgTransformPoint(tris[i + 2], min, max, spec.viewW,
                                     spec.viewH, scaleFactor);
        drawList->AddTriangleFilled(a, b, c, color);
      }
    }
  }
}

static const SvgPathSpec kTranslateSvgPaths[] = {
    {"M12 3L12.3123 2.60957L12 2.35969L11.6877 2.60957L12 3ZM11.5 9C11.5 "
     "9.27614 11.7239 9.5 12 9.5C12.2761 9.5 12.5 9.27614 12.5 9H11.5ZM16.3123 "
     "5.80957L12.3123 2.60957L11.6877 3.39043L15.6877 6.59043L16.3123 "
     "5.80957ZM11.6877 2.60957L7.68765 5.80957L8.31235 6.59043L12.3123 "
     "3.39043L11.6877 2.60957ZM11.5 3V9H12.5V3H11.5Z",
     true},
    {"M21 12L21.3904 12.3123L21.6403 12L21.3904 11.6877L21 12ZM15 11.5C14.7239 "
     "11.5 14.5 11.7239 14.5 12C14.5 12.2761 14.7239 12.5 15 12.5L15 "
     "11.5ZM18.1904 16.3123L21.3904 12.3123L20.6096 11.6877L17.4096 "
     "15.6877L18.1904 16.3123ZM21.3904 11.6877L18.1904 7.68765L17.4096 "
     "8.31235L20.6096 12.3123L21.3904 11.6877ZM21 11.5L15 11.5L15 12.5L21 "
     "12.5L21 11.5Z",
     true},
    {"M12 21L12.3123 21.3904L12 21.6403L11.6877 21.3904L12 21ZM11.5 15C11.5 "
     "14.7239 11.7239 14.5 12 14.5C12.2761 14.5 12.5 14.7239 12.5 "
     "15H11.5ZM16.3123 18.1904L12.3123 21.3904L11.6877 20.6096L15.6877 "
     "17.4096L16.3123 18.1904ZM11.6877 21.3904L7.68765 18.1904L8.31235 "
     "17.4096L12.3123 20.6096L11.6877 21.3904ZM11.5 21V15H12.5V21H11.5Z",
     true},
    {"M3 12L2.60957 12.3123L2.35969 12L2.60957 11.6877L3 12ZM9 11.5C9.27614 "
     "11.5 9.5 11.7239 9.5 12C9.5 12.2761 9.27614 12.5 9 12.5L9 11.5ZM5.80956 "
     "16.3123L2.60957 12.3123L3.39043 11.6877L6.59043 15.6877L5.80956 "
     "16.3123ZM2.60957 11.6877L5.80957 7.68765L6.59043 8.31235L3.39043 "
     "12.3123L2.60957 11.6877ZM3 11.5L9 11.5L9 12.5L3 12.5L3 11.5Z",
     true}};

static const SvgPathSpec kRotateSvgPaths[] = {
    {"M11.2797426,15.9868494 L10.1464466,14.8535534 C9.95118446,14.6582912 "
     "9.95118446,14.3417088 10.1464466,14.1464466 C10.3417088,13.9511845 "
     "10.6582912,13.9511845 10.8535534,14.1464466 L12.8535534,16.1464466 "
     "C13.0488155,16.3417088 13.0488155,16.6582912 12.8535534,16.8535534 "
     "L10.8535534,18.8535534 C10.6582912,19.0488155 10.3417088,19.0488155 "
     "10.1464466,18.8535534 C9.95118446,18.6582912 9.95118446,18.3417088 "
     "10.1464466,18.1464466 L11.3044061,16.9884871 C10.3667147,16.9573314 "
     "9.46306739,16.8635462 8.61196501,16.7145167 C9.33747501,19.2936084 "
     "10.6229353,21 12,21 C14.0051086,21 15.8160018,17.3821896 "
     "15.9868494,12.7202574 L14.8535534,13.8535534 C14.6582912,14.0488155 "
     "14.3417088,14.0488155 14.1464466,13.8535534 C13.9511845,13.6582912 "
     "13.9511845,13.3417088 14.1464466,13.1464466 L16.1464466,11.1464466 "
     "C16.3417088,10.9511845 16.6582912,10.9511845 16.8535534,11.1464466 "
     "L18.8535534,13.1464466 C19.0488155,13.3417088 19.0488155,13.6582912 "
     "18.8535534,13.8535534 C18.6582912,14.0488155 18.3417088,14.0488155 "
     "18.1464466,13.8535534 L16.9884871,12.6955939 C16.8167229,17.8651676 "
     "14.7413901,22 12,22 C9.97580598,22 8.3147521,19.7456544 "
     "7.515026,16.484974 C4.2543456,15.6852479 2,14.024194 2,12 C2,9.97580598 "
     "4.2543456,8.3147521 7.515026,7.515026 C8.3147521,4.2543456 9.97580598,2 "
     "12,2 C13.5021775,2 14.8263891,3.23888365 15.7433738,5.30744582 "
     "C15.8552836,5.55989543 15.7413536,5.8552671 15.4889039,5.96717692 "
     "C15.2364543,6.07908673 14.9410827,5.96515672 14.8291729,5.71270711 "
     "C14.0550111,3.96632921 13.0221261,3 12,3 C10.6229353,3 "
     "9.33747501,4.70639159 8.61196501,7.28548333 C9.67174589,7.09991387 "
     "10.812997,7 12,7 C17.4892085,7 22,9.13669069 22,12 C22,13.5021775 "
     "20.7611164,14.8263891 18.6925542,15.7433738 C18.4401046,15.8552836 "
     "18.1447329,15.7413536 18.0328231,15.4889039 C17.9209133,15.2364543 "
     "18.0348433,14.9410827 18.2872929,14.8291729 C20.0336708,14.0550111 "
     "21,13.0221261 21,12 C21,9.89274656 17.0042017,8 12,8 C10.6991081,8 "
     "9.46636321,8.12791023 8.35424759,8.35424759 C8.12791023,9.46636321 "
     "8,10.6991081 8,12 C8,13.3008919 8.12791023,14.5336368 "
     "8.35424759,15.6457524 C9.25899447,15.8298862 10.2435788,15.9488767 "
     "11.2797426,15.9868494 Z M7.28548333,8.61196501 C4.70639159,9.33747501 "
     "3,10.6229353 3,12 C3,13.3770647 4.70639159,14.662525 "
     "7.28548333,15.388035 C7.09991387,14.3282541 7,13.187003 7,12 "
     "C7,10.812997 7.09991387,9.67174589 7.28548333,8.61196501 "
     "L7.28548333,8.61196501 Z",
     true}};

static const SvgPathSpec kScaleSvgPaths[] = {
    {"M20,19.2928932 L20,16.5 C20,16.2238576 20.2238576,16 20.5,16 "
     "C20.7761424,16 21,16.2238576 21,16.5 L21,20.5 C21,20.7761424 "
     "20.7761424,21 20.5,21 L16.5,21 C16.2238576,21 16,20.7761424 16,20.5 "
     "C16,20.2238576 16.2238576,20 16.5,20 L19.2928932,20 "
     "L16.1464466,16.8535534 C15.9511845,16.6582912 15.9511845,16.3417088 "
     "16.1464466,16.1464466 C16.3417088,15.9511845 16.6582912,15.9511845 "
     "16.8535534,16.1464466 L20,19.2928932 Z M4,4.70710678 L4,7.5 "
     "C4,7.77614237 3.77614237,8 3.5,8 C3.22385763,8 3,7.77614237 3,7.5 L3,3.5 "
     "C3,3.22385763 3.22385763,3 3.5,3 L7.5,3 C7.77614237,3 8,3.22385763 8,3.5 "
     "C8,3.77614237 7.77614237,4 7.5,4 L4.70710678,4 L7.85355339,7.14644661 "
     "C8.04881554,7.34170876 8.04881554,7.65829124 7.85355339,7.85355339 "
     "C7.65829124,8.04881554 7.34170876,8.04881554 7.14644661,7.85355339 "
     "L4,4.70710678 Z M4.70710678,20 L7.5,20 C7.77614237,20 8,20.2238576 "
     "8,20.5 C8,20.7761424 7.77614237,21 7.5,21 L3.5,21 C3.22385763,21 "
     "3,20.7761424 3,20.5 L3,16.5 C3,16.2238576 3.22385763,16 3.5,16 "
     "C3.77614237,16 4,16.2238576 4,16.5 L4,19.2928932 L7.14644661,16.1464466 "
     "C7.34170876,15.9511845 7.65829124,15.9511845 7.85355339,16.1464466 "
     "C8.04881554,16.3417088 8.04881554,16.6582912 7.85355339,16.8535534 "
     "L4.70710678,20 Z M19.2928932,4 L16.5,4 C16.2238576,4 16,3.77614237 "
     "16,3.5 C16,3.22385763 16.2238576,3 16.5,3 L20.5,3 C20.7761424,3 "
     "21,3.22385763 21,3.5 L21,7.53112887 C21,7.80727125 20.7761424,8.03112887 "
     "20.5,8.03112887 C20.2238576,8.03112887 20,7.80727125 20,7.53112887 "
     "L20,4.70710678 L16.8535534,7.85355339 C16.6582912,8.04881554 "
     "16.3417088,8.04881554 16.1464466,7.85355339 C15.9511845,7.65829124 "
     "15.9511845,7.34170876 16.1464466,7.14644661 L19.2928932,4 L19.2928932,4 "
     "Z M8,10.4949109 C8,9.11668583 9.11540994,7.99843045 "
     "10.4936306,7.99491906 L13.4936306,7.98727573 C14.8807119,7.98726762 "
     "16,9.10655574 16,10.4872676 L16,13.5 C16,14.8807119 14.8807119,16 "
     "13.5,16 L10.5,16 C9.11928813,16 8,14.8807119 8,13.5 L8,10.4949109 Z "
     "M9,10.4949109 L9,13.5 C9,14.3284271 9.67157288,15 10.5,15 L13.5,15 "
     "C14.3284271,15 15,14.3284271 15,13.5 L15,10.4872676 C15,9.65884049 "
     "14.3284271,8.98726762 13.5,8.98726762 L10.4961784,8.99491581 "
     "C9.66924596,8.99702265 9,9.66797587 9,10.4949109 Z",
     true}};

static const SvgPathSpec kBoundsSvgPaths[] = {
    {"M11 13.6V21H3.6C3.26863 21 3 20.7314 3 20.4V13H10.4C10.7314 13 11 "
     "13.2686 11 13.6Z",
     true},
    {"M11 21H14", true},
    {"M3 13V10", true},
    {"M6 3H3.6C3.26863 3 3 3.26863 3 3.6V6", true},
    {"M14 3H10", true},
    {"M21 10V14", true},
    {"M18 3H20.4C20.7314 3 21 3.26863 21 3.6V6", true},
    {"M18 21H20.4C20.7314 21 21 20.7314 21 20.4V18", true},
    {"M11 10H14V13", true}};

static const SvgPathSpec kMeshSvgPaths[] = {
    {"M363.6 36.48c-22.2 0-40 17.8-40 40 0 22.23 17.8 40.02 40 40.02s40-17.79 "
     "40-40.02c0-22.2-17.8-40-40-40zm-56.7 51.97c-53.2 18.95-108.7 34.95-169 "
     "45.25 1.8 4.6 2.8 9.6 2.8 14.8 0 4.8-.8 9.4-2.4 13.6 96.2 12.9 182.8 36 "
     "257.8 71.9 1.6-5.9 4.5-11.3 8.3-15.9-71.2-34.3-152.4-57.2-241.5-70.7 "
     "53.2-10.6 102.8-25.4 150.4-42.2-3-5.2-5.2-10.79-6.4-16.75zm97.8 "
     "28.85c-4.3 4.3-9.2 8-14.6 10.8 15.3 24.8 26 50.6 31.8 77.8 4.3-1.5 9-2.4 "
     "13.8-2.4 1.4 0 2.8.1 4.1.2-6.3-30.3-18.2-59.1-35.1-86.4zm-305 8.2c-12.81 "
     "0-23 10.2-23 23s10.19 23 23 23c12.8 0 23-10.2 23-23s-10.2-23-23-23zm34.7 "
     "44.6c-3.2 5.2-7.5 9.6-12.6 12.9 32.1 32.6 66.1 65.9 120.6 80.4 "
     "0-.9-.1-1.9-.1-2.8 0-5.3 1.3-10.3 "
     "3.5-14.8-49.5-13.5-80-43.8-111.4-75.7zm-57 12.7c-21.76 67.8-27.12 "
     "137.2-32.29 206 2.13-.5 4.34-.7 6.6-.7 3.99 0 7.81.7 11.35 2.1 5.19-68.4 "
     "10.57-136 31.29-201.1-6.18-.8-11.94-3-16.95-6.3zm358.3 38.7c-12.8 0-23 "
     "10.2-23 23s10.2 23 23 23 23-10.2 23-23-10.2-23-23-23zm-41 22.2c-28.4 "
     "5.8-56.6 10.8-86 10.5.4 2.1.6 4.2.6 6.4 0 4-.7 7.9-2.1 11.5 32 .6 62-4.7 "
     "91.2-10.8-2.4-5.1-3.7-10.8-3.7-16.8zm-118.9 1.4c-8.7 0-15.5 6.8-15.5 "
     "15.5s6.8 15.5 15.5 15.5 15.5-6.8 15.5-15.5-6.8-15.5-15.5-15.5zM399 "
     "262.7c-55.6 45.9-106.6 94.4-143.1 150.7 5.9 1.8 11.2 5 15.6 9.1 "
     "34.9-53.5 84.2-100.8 138.8-145.9-4.7-3.7-8.6-8.5-11.3-13.9zm-152 "
     "15c-47.9 46.4-109.6 83.2-172.85 119.5 4.36 4.2 7.56 9.6 9.05 15.6C146.8 "
     "376.4 210 338.9 260 290.1c-5.4-2.9-9.9-7.2-13-12.4zm179.4 6.7c1.3 28.8 6 "
     "57.3 14.3 85.2 4.8-3.4 10.7-5.6 "
     "17-6-7.6-26-11.9-52.3-13.2-79.1-2.9.7-5.8 1-8.8 1-3.2 "
     "0-6.3-.4-9.3-1.1zm33.3 97.1c-8.4 0-15 6.6-15 15s6.6 15 15 15 15-6.6 "
     "15-15-6.6-15-15-15zM51.71 406.1c-8.07 0-14.42 6.4-14.42 14.4 0 8.1 6.35 "
     "14.5 14.42 14.5s14.42-6.4 "
     "14.42-14.5c0-8-6.35-14.4-14.42-14.4zm376.49.3c-44.7 24.5-93.8 32.6-144.9 "
     "35.6.9 3.4 1.4 6.9 1.4 10.5 0 2.6-.3 5.1-.7 7.5 53.1-3.1 105.8-11.6 "
     "154.3-38.5-4.7-4-8.2-9.2-10.1-15.1zM83.91 416.8c.14 1.2.22 2.4.22 3.7 0 "
     "5-1.15 9.7-3.19 14l121.86 20.3c-.1-.8-.1-1.5-.1-2.3 0-5.4 1.1-10.6 "
     "3-15.4zm159.79 12.7c-12.8 0-23 10.2-23 23s10.2 23 23 23 23-10.2 "
     "23-23-10.2-23-23-23z",
     true}};

static const SvgPathSpec kGizmoToggleSvgPaths[] = {
    {"M2 17h1v5h5v1H2zm21 0h-1v5h-5v1h6zM3 3h5V2H2v6h1zm20-1h-6v1h5v5h1zm-9.75 "
     "12h-1.5a.75.75 0 0 1-.75-.75v-1.5a.75.75 0 0 1 .75-.75h1.5a.75.75 0 0 1 "
     ".75.75v1.5a.75.75 0 0 1-.75.75zM13 12h-1v1h1zm7 0h-5v1h5zm-10 0H5v1h5zm3 "
     "8v-5h-1v5zm-1-10h1V5h-1z",
     false}};

static const SvgPathSpec kGridToggleSvgPaths[] = {
    {"M47.547,63.547V448.453a16,16,0,0,0,16,16H448.453a16,16,0,0,0,16-16V63."
     "547a16,16,0,0,0-16-16H63.547A16,16,0,0,0,47.547,63.547Zm288.6,16h96.3v96."
     "3h-96.3Zm0,128.3h96.3v96.3h-96.3Zm0,128.3h96.3v96.3h-96.3Zm-128.3-256."
     "6h96.3v96.3h-96.3Zm0,128.3h96.3v96.3h-96.3Zm0,128.3h96.3v96.3h-96.3Zm-"
     "128.3-256.6h96.3v96.3h-96.3Zm0,128.3h96.3v96.3h-96.3Zm0,128.3h96.3v96.3h-"
     "96.3Z",
     true}};

static const SvgPathSpec kSnapToggleSvgPaths[] = {
    {"M13.3,7.7l8.1,8.1c1.5,1.5,1.5,3.9,0,5.4c-1.5,1.5-3.9,1.5-5.4,0l-8.1-8.1l-"
     "4.7,4.7l8.1,8.1 c4.1,4.1,10.7,4.1,14.8,0s4.1-10.7,0-14.8L18,3L13.3,7.7z",
     true}};

static const SvgPathSpec kLocalModeSvgPaths[] = {
    {"M8 10C9.10457 10 10 9.10457 10 8C10 6.89543 9.10457 6 8 6C6.89543 6 6 "
     "6.89543 6 8C6 9.10457 6.89543 10 8 10Z",
     false},
    {"M2.08296 7C2.50448 4.48749 4.48749 2.50448 7 2.08296V0H9V2.08296C11.5125 "
     "2.50448 13.4955 4.48749 13.917 7H16V9H13.917C13.4955 11.5125 11.5125 "
     "13.4955 9 13.917V16H7V13.917C4.48749 13.4955 2.50448 11.5125 2.08296 "
     "9H0V7H2.08296ZM4 8C4 5.79086 5.79086 4 8 4C10.2091 4 12 5.79086 12 8C12 "
     "10.2091 10.2091 12 8 12C5.79086 12 4 10.2091 4 8Z",
     false}};

static const SvgPathSpec kWorldModeSvgPaths[] = {
    {"M19.5 6L18.0333 7.1C17.6871 7.35964 17.2661 7.5 16.8333 "
     "7.5H13.475C12.8775 7.5 12.3312 7.83761 12.064 8.37206V8.37206C11.7342 "
     "9.03161 11.9053 9.83161 12.476 10.2986L14.476 11.9349C16.0499 13.2227 "
     "16.8644 15.22 16.6399 17.2412L16.6199 17.4206C16.5403 18.1369 16.3643 "
     "18.8392 16.0967 19.5083L15.5 21",
     true},
    {"M2.5 10.5L5.7381 9.96032C7.09174 9.73471 8.26529 10.9083 8.03968 "
     "12.2619L7.90517 13.069C7.66434 14.514 8.3941 15.9471 9.70437 "
     "16.6022V16.6022C10.7535 17.1268 11.2976 18.3097 11.0131 19.4476L10.5 "
     "21.5",
     true},
    {"M12 2.5C6.75329 2.5 2.5 6.75329 2.5 12C2.5 17.2467 6.75329 21.5 12 "
     "21.5C17.2467 21.5 21.5 17.2467 21.5 12C21.5 6.75329 17.2467 2.5 12 2.5Z",
     true}};

static const SvgPathSpec kUiWorldToggleSvgPaths[] = {
    {"M1 1 L17 1 L17 17 L1 17 L1 1 Z M20 7 L23 7 L23 23 L7 23 L7 20 L7 20",
     true}};

static const SvgIconSpec kTranslateSvg = {24.0f, 24.0f, kTranslateSvgPaths, 4};
static const SvgIconSpec kRotateSvg = {24.0f, 24.0f, kRotateSvgPaths, 1};
static const SvgIconSpec kScaleSvg = {24.0f, 24.0f, kScaleSvgPaths, 1};
static const SvgIconSpec kBoundsSvg = {24.0f, 24.0f, kBoundsSvgPaths, 9};
static const SvgIconSpec kMeshSvg = {512.0f, 512.0f, kMeshSvgPaths, 1};
static const SvgIconSpec kGizmoToggleSvg = {20.0f, 20.0f, kGizmoToggleSvgPaths,
                                            1};
static const SvgIconSpec kGridToggleSvg = {512.0f, 512.0f, kGridToggleSvgPaths,
                                           1};
static const SvgIconSpec kSnapToggleSvg = {32.0f, 32.0f, kSnapToggleSvgPaths,
                                           1};
static const SvgIconSpec kLocalModeSvg = {16.0f, 16.0f, kLocalModeSvgPaths, 1};
static const SvgIconSpec kWorldModeSvg = {24.0f, 24.0f, kWorldModeSvgPaths, 3};
static const SvgIconSpec kUiWorldToggleSvg = {24.0f, 24.0f,
                                              kUiWorldToggleSvgPaths, 1};

static SvgIconCache gTranslateSvgCache;
static SvgIconCache gRotateSvgCache;
static SvgIconCache gScaleSvgCache;
static SvgIconCache gBoundsSvgCache;
static SvgIconCache gMeshSvgCache;
static SvgIconCache gGizmoToggleSvgCache;
static SvgIconCache gGridToggleSvgCache;
static SvgIconCache gSnapToggleSvgCache;
static SvgIconCache gLocalModeSvgCache;
static SvgIconCache gWorldModeSvgCache;
static SvgIconCache gUiWorldToggleSvgCache;

static void DrawTranslateIcon(ImDrawList *drawList, const ImVec2 &min,
                              const ImVec2 &max, ImU32 lineColor,
                              ImU32 accentColor) {
  (void)accentColor;
  DrawSvgIcon(drawList, kTranslateSvg, gTranslateSvgCache, min, max, lineColor,
              1.15f, 0.8f);
}

static void DrawRotateIcon(ImDrawList *drawList, const ImVec2 &min,
                           const ImVec2 &max, ImU32 lineColor,
                           ImU32 accentColor) {
  (void)accentColor;
  DrawSvgIcon(drawList, kRotateSvg, gRotateSvgCache, min, max, lineColor, 1.2f,
              0.8f);
}

static void DrawScaleIcon(ImDrawList *drawList, const ImVec2 &min,
                          const ImVec2 &max, ImU32 lineColor,
                          ImU32 accentColor) {
  (void)accentColor;
  DrawSvgIcon(drawList, kScaleSvg, gScaleSvgCache, min, max, lineColor, 1.2f,
              0.8f);
}

static void DrawBoundsIcon(ImDrawList *drawList, const ImVec2 &min,
                           const ImVec2 &max, ImU32 lineColor,
                           ImU32 accentColor) {
  (void)accentColor;
  float size = std::min(max.x - min.x, max.y - min.y) * 0.8f;
  DrawSvgIcon(drawList, kBoundsSvg, gBoundsSvgCache, min, max, lineColor,
              size * 0.06f, 0.82f);
}

static void DrawUniversalIcon(ImDrawList *drawList, const ImVec2 &min,
                              const ImVec2 &max, ImU32 lineColor,
                              ImU32 accentColor) {
  (void)accentColor;
  DrawSvgIcon(drawList, kRotateSvg, gRotateSvgCache, min, max, lineColor, 1.1f,
              0.85f);
  DrawSvgIcon(drawList, kTranslateSvg, gTranslateSvgCache, min, max, lineColor,
              1.1f, 0.62f);
}

static void DrawMeshIcon(ImDrawList *drawList, const ImVec2 &min,
                         const ImVec2 &max, ImU32 lineColor,
                         ImU32 accentColor) {
  (void)accentColor;
  DrawSvgIcon(drawList, kMeshSvg, gMeshSvgCache, min, max, lineColor, 1.0f,
              0.78f);
}

static void DrawGizmoToggleIcon(ImDrawList *drawList, const ImVec2 &min,
                                const ImVec2 &max, ImU32 lineColor,
                                ImU32 accentColor) {
  (void)accentColor;
  ImVec2 iconMin, iconMax;
  GetIconBounds(min, max, iconMin, iconMax);
  float size = iconMax.x - iconMin.x;
  float thickness = std::max(1.0f, size * 0.08f);

  auto T = [&](float x, float y) {
    return ImVec2(iconMin.x + (x / 24.0f) * size,
                  iconMin.y + (y / 24.0f) * size);
  };

  // Corner brackets
  drawList->AddLine(T(2, 2), T(8, 2), lineColor, thickness);
  drawList->AddLine(T(2, 2), T(2, 8), lineColor, thickness);
  drawList->AddLine(T(16, 2), T(22, 2), lineColor, thickness);
  drawList->AddLine(T(22, 2), T(22, 8), lineColor, thickness);
  drawList->AddLine(T(2, 22), T(8, 22), lineColor, thickness);
  drawList->AddLine(T(2, 16), T(2, 22), lineColor, thickness);
  drawList->AddLine(T(16, 22), T(22, 22), lineColor, thickness);
  drawList->AddLine(T(22, 16), T(22, 22), lineColor, thickness);

  // Crosshair
  drawList->AddLine(T(5, 12), T(10, 12), lineColor, thickness);
  drawList->AddLine(T(14, 12), T(19, 12), lineColor, thickness);
  drawList->AddLine(T(12, 5), T(12, 10), lineColor, thickness);
  drawList->AddLine(T(12, 14), T(12, 19), lineColor, thickness);

  // Center square
  float half = 1.1f;
  ImVec2 c = T(12, 12);
  drawList->AddRectFilled(
      ImVec2(c.x - (half / 24.0f) * size, c.y - (half / 24.0f) * size),
      ImVec2(c.x + (half / 24.0f) * size, c.y + (half / 24.0f) * size),
      lineColor, 1.5f);
}

static void DrawGridToggleIcon(ImDrawList *drawList, const ImVec2 &min,
                               const ImVec2 &max, ImU32 lineColor,
                               ImU32 accentColor) {
  (void)accentColor;
  DrawSvgIcon(drawList, kGridToggleSvg, gGridToggleSvgCache, min, max,
              lineColor, 0.8f, 0.72f);
}

static void DrawSnapToggleIcon(ImDrawList *drawList, const ImVec2 &min,
                               const ImVec2 &max, ImU32 lineColor,
                               ImU32 accentColor) {
  (void)accentColor;
  ImVec2 iconMin, iconMax;
  GetIconBounds(min, max, iconMin, iconMax);
  float size = iconMax.x - iconMin.x;
  float thickness = std::max(1.0f, size * 0.075f);
  DrawSvgIcon(drawList, kSnapToggleSvg, gSnapToggleSvgCache, min, max,
              lineColor, thickness, 0.78f);

  auto T = [&](float x, float y) {
    return ImVec2(iconMin.x + (x / 32.0f) * size,
                  iconMin.y + (y / 32.0f) * size);
  };
  auto DrawRotRect = [&](float cx, float cy, float w, float h) {
    float hx = w * 0.5f;
    float hy = h * 0.5f;
    float c = 0.70710678f;
    float s = 0.70710678f;
    ImVec2 corners[4] = {ImVec2(-hx, -hy), ImVec2(hx, -hy), ImVec2(hx, hy),
                         ImVec2(-hx, hy)};
    drawList->PathClear();
    for (int i = 0; i < 4; ++i) {
      float rx = corners[i].x * c - corners[i].y * s;
      float ry = corners[i].x * s + corners[i].y * c;
      drawList->PathLineTo(T(cx + rx, cy + ry));
    }
    drawList->PathStroke(lineColor, ImDrawFlags_Closed, thickness);
  };

  DrawRotRect(6.95f, 16.8f, 6.7f, 3.8f);
  DrawRotRect(17.05f, 6.7f, 6.7f, 3.8f);
}

static void DrawLocalModeIcon(ImDrawList *drawList, const ImVec2 &min,
                              const ImVec2 &max, ImU32 lineColor,
                              ImU32 accentColor) {
  (void)accentColor;
  ImVec2 iconMin, iconMax;
  GetIconBounds(min, max, iconMin, iconMax);
  ImVec2 center =
      ImVec2((iconMin.x + iconMax.x) * 0.5f, (iconMin.y + iconMax.y) * 0.5f);
  float size = iconMax.x - iconMin.x;
  float outerR = size * 0.36f;
  float dotR = size * 0.08f;
  float thickness = std::max(1.0f, size * 0.08f);

  drawList->AddCircle(center, outerR, lineColor, 28, thickness);
  drawList->AddCircleFilled(center, dotR, lineColor, 12);

  const float tickLen = size * 0.12f;
  for (int i = 0; i < 4; ++i) {
    float angle = (IM_PI * 0.5f) * static_cast<float>(i);
    ImVec2 dir(std::cos(angle), std::sin(angle));
    ImVec2 a = ImVec2(center.x + dir.x * (outerR - tickLen * 0.2f),
                      center.y + dir.y * (outerR - tickLen * 0.2f));
    ImVec2 b = ImVec2(center.x + dir.x * (outerR + tickLen),
                      center.y + dir.y * (outerR + tickLen));
    drawList->AddLine(a, b, lineColor, thickness);
  }
}

static void DrawWorldModeIcon(ImDrawList *drawList, const ImVec2 &min,
                              const ImVec2 &max, ImU32 lineColor,
                              ImU32 accentColor) {
  (void)accentColor;
  DrawSvgIcon(drawList, kWorldModeSvg, gWorldModeSvgCache, min, max, lineColor,
              1.0f, 0.8f);
}

static void DrawUiWorldToggleIcon(ImDrawList *drawList, const ImVec2 &min,
                                  const ImVec2 &max, ImU32 lineColor,
                                  ImU32 accentColor) {
  (void)accentColor;
  DrawSvgIcon(drawList, kUiWorldToggleSvg, gUiWorldToggleSvgCache, min, max,
              lineColor, 0.9f, 0.8f);

  ImVec2 iconMin, iconMax;
  GetIconBounds(min, max, iconMin, iconMax);
  float size = iconMax.x - iconMin.x;
  auto T = [&](float x, float y) {
    return ImVec2(iconMin.x + (x / 24.0f) * size,
                  iconMin.y + (y / 24.0f) * size);
  };

  ImVec2 boxMin = T(1, 1);
  ImVec2 boxMax = T(17, 17);
  float fontSize = size * 0.38f;
  ImVec2 textSize = ImGui::CalcTextSize("2D");
  float textScale = fontSize / ImGui::GetFontSize();
  ImVec2 scaledTextSize(textSize.x * textScale, textSize.y * textScale);
  ImVec2 textPos(boxMin.x + (boxMax.x - boxMin.x - scaledTextSize.x) * 0.5f,
                 boxMin.y + (boxMax.y - boxMin.y - scaledTextSize.y) * 0.5f -
                     size * 0.02f);

  ImFont *font = ImGui::GetFont();
  const ImVec2 offsets[] = {ImVec2(-0.6f, 0.0f), ImVec2(0.6f, 0.0f),
                            ImVec2(0.0f, -0.6f), ImVec2(0.0f, 0.6f)};
  for (const ImVec2 &off : offsets) {
    drawList->AddText(font, fontSize,
                      ImVec2(textPos.x + off.x, textPos.y + off.y), lineColor,
                      "2D");
  }
}

static void DrawIcon(Icon icon, ImDrawList *drawList, const ImVec2 &min,
                     const ImVec2 &max, ImU32 lineColor, ImU32 accentColor) {
  switch (icon) {
  case Icon::Translate:
    DrawTranslateIcon(drawList, min, max, lineColor, accentColor);
    break;
  case Icon::Rotate:
    DrawRotateIcon(drawList, min, max, lineColor, accentColor);
    break;
  case Icon::Scale:
    DrawScaleIcon(drawList, min, max, lineColor, accentColor);
    break;
  case Icon::Bounds:
    DrawBoundsIcon(drawList, min, max, lineColor, accentColor);
    break;
  case Icon::Universal:
    DrawUniversalIcon(drawList, min, max, lineColor, accentColor);
    break;
  case Icon::Mesh:
    DrawMeshIcon(drawList, min, max, lineColor, accentColor);
    break;
  case Icon::GizmoToggle:
    DrawGizmoToggleIcon(drawList, min, max, lineColor, accentColor);
    break;
  case Icon::GridToggle:
    DrawGridToggleIcon(drawList, min, max, lineColor, accentColor);
    break;
  case Icon::SnapToggle:
    DrawSnapToggleIcon(drawList, min, max, lineColor, accentColor);
    break;
  case Icon::LocalMode:
    DrawLocalModeIcon(drawList, min, max, lineColor, accentColor);
    break;
  case Icon::WorldMode:
    DrawWorldModeIcon(drawList, min, max, lineColor, accentColor);
    break;
  case Icon::UiWorldToggle:
    DrawUiWorldToggleIcon(drawList, min, max, lineColor, accentColor);
    break;
  }
}

bool IconButton(const char *id, Icon icon, bool active,
                       const ImVec2 &size, ImU32 baseColor, ImU32 hoverColor,
                       ImU32 activeColor, ImU32 accentColor, ImU32 iconColor) {
  ImGui::PushID(id);
  ImGui::SetNextItemAllowOverlap();
  ImGui::InvisibleButton("##btn", size);
  bool hovered = ImGui::IsItemHovered();
  bool pressed = ImGui::IsItemClicked();
  ImVec2 min = ImGui::GetItemRectMin();
  ImVec2 max = ImGui::GetItemRectMax();
  float rounding = 9.0f;

  ImDrawList *drawList = ImGui::GetWindowDrawList();
  ImU32 bg = active ? activeColor : (hovered ? hoverColor : baseColor);

  ImVec4 bgCol = ImGui::ColorConvertU32ToFloat4(bg);
  ImU32 top = ImGui::GetColorU32(ScaleColor(bgCol, 1.07f));
  ImU32 bottom = ImGui::GetColorU32(ScaleColor(bgCol, 0.93f));
  drawList->AddRectFilledMultiColor(min, max, top, top, bottom, bottom);
  drawList->AddRect(min, max,
                    ImGui::GetColorU32(ImVec4(1, 1, 1, active ? 0.35f : 0.18f)),
                    rounding);

  ImDrawListFlags prevFlags = drawList->Flags;
  drawList->Flags |=
      ImDrawListFlags_AntiAliasedLines | ImDrawListFlags_AntiAliasedFill;
  DrawIcon(icon, drawList, min, max, iconColor, accentColor);
  drawList->Flags = prevFlags;

  ImGui::PopID();
  return pressed;
}

bool TextButton(const char *id, const char *label, bool active,
                       const ImVec2 &size, ImU32 baseColor, ImU32 hoverColor,
                       ImU32 activeColor, ImU32 borderColor, ImVec4 textColor) {
  ImGui::PushID(id);
  ImGui::SetNextItemAllowOverlap();
  ImGui::InvisibleButton("##btn", size);
  bool hovered = ImGui::IsItemHovered();
  bool pressed = ImGui::IsItemClicked();
  ImVec2 min = ImGui::GetItemRectMin();
  ImVec2 max = ImGui::GetItemRectMax();
  float rounding = 8.0f;

  ImDrawList *drawList = ImGui::GetWindowDrawList();
  ImU32 bg = active ? activeColor : (hovered ? hoverColor : baseColor);

  ImVec4 bgCol = ImGui::ColorConvertU32ToFloat4(bg);
  ImU32 top = ImGui::GetColorU32(ScaleColor(bgCol, 1.06f));
  ImU32 bottom = ImGui::GetColorU32(ScaleColor(bgCol, 0.94f));
  drawList->AddRectFilledMultiColor(min, max, top, top, bottom, bottom);
  drawList->AddRect(min, max, borderColor, rounding);

  ImVec2 textSize = ImGui::CalcTextSize(label);
  ImVec2 textPos = ImVec2(min.x + (size.x - textSize.x) * 0.5f,
                          min.y + (size.y - textSize.y) * 0.5f - 1.0f);
  drawList->AddText(textPos, ImGui::GetColorU32(textColor), label);

  ImGui::PopID();
  return pressed;
}

bool ModeButton(const char *label, bool active, const ImVec2 &size,
                       ImVec4 baseColor, ImVec4 activeColor, ImVec4 textColor) {
  ImGui::PushStyleColor(ImGuiCol_Button, active ? activeColor : baseColor);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                        active ? activeColor : baseColor);
  ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                        active ? activeColor : baseColor);
  ImGui::PushStyleColor(ImGuiCol_Text, textColor);
  ImGui::SetNextItemAllowOverlap();
  bool pressed = ImGui::Button(label, size);
  ImGui::PopStyleColor(4);
  return pressed;
}
} // namespace GizmoToolbar


