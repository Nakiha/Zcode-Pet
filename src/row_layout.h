#pragma once
#include <algorithm>
inline constexpr int DefaultSessionRows = 3;
inline int ClampSessionRows(int rows) { return std::clamp(rows, 1, 10); }
inline int VisibleSessionRows(int sessions, int requested, int capacity = 10) {
  return std::clamp(sessions, 1,
                    std::min(ClampSessionRows(requested), std::max(1, capacity)));
}
inline constexpr float RowSidePadding = 10;
inline constexpr float RowTitleLeft = 34, RowIndicatorCenter = 18;
struct RowLayout {
  float titleLeft = RowTitleLeft, titleRight = RowTitleLeft,
        timeLeft = RowTitleLeft, timeRight = RowTitleLeft,
        badgeLeft = RowTitleLeft, badgeRight = RowTitleLeft;
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
  if (VisibleSessionRows(0, 3) != 1 || VisibleSessionRows(40, 5) != 5 ||
      VisibleSessionRows(2, 10) != 2 || VisibleSessionRows(40, 10, 4) != 4 ||
      VisibleSessionRows(40, -1) != 1 || VisibleSessionRows(40, 999) != 10 ||
      VisibleSessionRows(40, 5, 0) != 1)
    return false;
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
