#pragma once
#include <algorithm>
inline constexpr float RowSidePadding = 10;
struct RowLayout {
  float titleLeft = 34, titleRight = 34, timeLeft = 34, timeRight = 34,
        badgeLeft = 34, badgeRight = 34;
};
inline RowLayout LayoutRow(float width, float timeWidth, float stateWidth) {
  RowLayout r;
  r.badgeRight = width - RowSidePadding;
  r.badgeLeft = r.badgeRight - stateWidth - 14;
  r.timeRight = r.badgeLeft - 8;
  r.timeLeft = r.timeRight - timeWidth;
  r.titleRight = std::max(r.titleLeft, r.timeLeft - 10);
  return r;
}
inline bool RowLayoutTests() {
  auto shortTime = LayoutRow(312, 18, 34), longTime = LayoutRow(312, 32, 34);
  if (shortTime.badgeLeft != longTime.badgeLeft ||
      shortTime.badgeRight != longTime.badgeRight)
    return false;
  if (shortTime.titleRight <= 180 || shortTime.titleRight >= shortTime.timeLeft)
    return false;
  if (longTime.timeRight >= longTime.badgeLeft ||
      longTime.titleRight >= longTime.timeLeft)
    return false;
  return LayoutRow(400, 18, 34).titleRight - shortTime.titleRight == 88;
}
