#pragma once
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>
#include <cmath>

inline LONG FloatingScreenPadding(float systemScale) {
  return (LONG)std::ceil(16 * systemScale);
}

inline RECT FloatingFootprint(POINT p, SIZE size, POINT outset = {}) {
  return {p.x - outset.x, p.y - outset.y, p.x + size.cx, p.y + size.cy};
}
inline int64_t FloatingOverlap(const RECT &a, const RECT &b, LONG gap = 0) {
  const auto width = std::max(0L, std::min(a.right, b.right + gap) -
                                     std::max(a.left, b.left - gap));
  const auto height = std::max(0L, std::min(a.bottom, b.bottom + gap) -
                                      std::max(a.top, b.top - gap));
  return (int64_t)width * height;
}
inline POINT PlaceAvoidingWindows(POINT preferred, SIZE size, const RECT &work,
                                  const std::vector<RECT> &occupied,
                                  LONG edge, LONG gap, POINT outset = {}) {
  const LONG left = work.left + edge, top = work.top + edge;
  const LONG right = std::max(left, work.right - edge - size.cx);
  const LONG bottom = std::max(top, work.bottom - edge - size.cy);
  std::vector<LONG> xs{std::clamp(preferred.x, left, right), left, right};
  std::vector<LONG> ys{std::clamp(preferred.y, top, bottom), top, bottom};
  for (const auto &r : occupied) {
    xs.insert(xs.end(), {r.left - gap - size.cx, r.right + gap + outset.x,
                          r.left + outset.x, r.right - size.cx});
    ys.insert(ys.end(), {r.top - gap - size.cy, r.bottom + gap + outset.y,
                          r.top + outset.y, r.bottom - size.cy});
  }
  POINT best{xs.front(), ys.front()};
  int64_t bestOverlap = std::numeric_limits<int64_t>::max();
  int64_t bestDistance = std::numeric_limits<int64_t>::max();
  // Edges of the occupied rectangles bound every free rectangular slot.
  // Prefer zero overlap, then the closest slot to the original anchor.
  for (auto x : xs)
    for (auto y : ys) {
      POINT candidate{std::clamp(x, left, right), std::clamp(y, top, bottom)};
      const auto rect = FloatingFootprint(candidate, size, outset);
      int64_t overlap = 0;
      for (const auto &r : occupied)
        overlap += FloatingOverlap(rect, r, gap);
      const int64_t dx = (int64_t)candidate.x - preferred.x,
                    dy = (int64_t)candidate.y - preferred.y;
      const auto distance = dx * dx + dy * dy;
      if (overlap < bestOverlap ||
          (overlap == bestOverlap && distance < bestDistance)) {
        best = candidate;
        bestOverlap = overlap;
        bestDistance = distance;
      }
    }
  return best;
}
inline bool FloatingLayoutTests() {
  auto fits = [](POINT p, SIZE size, const RECT &work,
                 const std::vector<RECT> &occupied, LONG edge, LONG gap,
                 POINT outset = POINT{}) {
    const auto r = FloatingFootprint(p, size, outset);
    if (p.x < work.left + edge || p.y < work.top + edge ||
        r.right > work.right - edge || r.bottom > work.bottom - edge)
      return false;
    for (const auto &other : occupied)
      if (FloatingOverlap(r, other, gap) != 0)
        return false;
    return true;
  };
  const RECT work{0, 0, 1200, 800};
  const std::vector<RECT> panels{{1000, 640, 1128, 768},
                                 {770, 510, 1128, 627},
                                 {708, 430, 1128, 490}};
  const SIZE toolbar{320, 64};
  auto placed = PlaceAvoidingWindows({850, 580}, toolbar, work, panels, 8, 10);
  if (!fits(placed, toolbar, work, panels, 8, 10))
    return false;
  const std::vector<RECT> nearTop{{780, 20, 908, 148}, {600, 160, 958, 277}};
  const SIZE notice{420, 60};
  placed = PlaceAvoidingWindows({488, -50}, notice, work, nearTop, 14, 10, {12, 12});
  if (!fits(placed, notice, work, nearTop, 14, 10, {12, 12}))
    return false;
  const RECT secondary{-1920, -200, 0, 880};
  const std::vector<RECT> other{{-350, 400, -10, 600}};
  placed = PlaceAvoidingWindows({-300, 450}, toolbar, secondary, other, 8, 10);
  if (!fits(placed, toolbar, secondary, other, 8, 10))
    return false;
  placed = PlaceAvoidingWindows({120, 140}, toolbar, work, {}, 8, 10);
  return placed.x == 120 && placed.y == 140;
}
