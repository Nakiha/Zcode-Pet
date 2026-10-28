#pragma once
#include <windows.h>
#include <algorithm>

inline POINT ClampPetToWorkArea(POINT requested, SIZE size, const RECT &work) {
  return {std::clamp(requested.x, work.left,
                     std::max(work.left, work.right - size.cx)),
          std::clamp(requested.y, work.top,
                     std::max(work.top, work.bottom - size.cy))};
}
inline bool PetPositionTests() {
  const RECT primary{0, 0, 1920, 1040};
  const SIZE size{224, 224};
  const auto topLeft = ClampPetToWorkArea({-80, -20}, size, primary);
  if (topLeft.x != 0 || topLeft.y != 0)
    return false;
  // Cursor travel past an edge must keep returning the same in-bounds point.
  for (LONG x : {1696L, 1800L, 1920L, 2400L}) {
    const auto right = ClampPetToWorkArea({x, 400}, size, primary);
    if (right.x != 1696 || right.y != 400)
      return false;
  }
  const auto bottom = ClampPetToWorkArea({800, 1100}, size, primary);
  if (bottom.x != 800 || bottom.y != 816)
    return false;
  const RECT secondary{-1920, -200, 0, 880};
  const auto other = ClampPetToWorkArea({-10, 900}, size, secondary);
  if (other.x != -224 || other.y != 656)
    return false;
  const auto large = ClampPetToWorkArea({10, 10}, {2400, 1300}, primary);
  return large.x == 0 && large.y == 0;
}
