#pragma once
#include <windows.h>
#include <algorithm>
#include <cmath>

inline constexpr UINT PopupBackendQuery = WM_APP + 25;
inline constexpr UINT PopupFrameTimeQuery = WM_APP + 26;

inline float CssCubicBezier(float progress, float x1, float y1, float x2,
                            float y2) {
  if (progress <= 0)
    return 0;
  if (progress >= 1)
    return 1;
  auto coordinate = [](float t, float a, float b) {
    const float inverse = 1 - t;
    return 3 * inverse * inverse * t * a + 3 * inverse * t * t * b + t * t * t;
  };
  // CSS time is the curve's x coordinate, not its Bezier parameter.
  // Invert x first, then sample y; substituting progress for t changes easing.
  float low = 0, high = 1;
  for (int i = 0; i < 22; ++i) {
    const float t = (low + high) / 2;
    if (coordinate(t, x1, x2) < progress)
      low = t;
    else
      high = t;
  }
  return coordinate((low + high) / 2, y1, y2);
}

struct PopupTween {
  double started = 0, duration = 0;
  float from = 0, target = 0;
  bool Active(double now) const {
    return duration > 0 && now - started < duration;
  }
  float Value(double now) const {
    if (!Active(now))
      return target;
    const float t = (float)std::clamp((now - started) / duration, 0., 1.);
    const float eased = CssCubicBezier(t, .42f, 0, 1, 1);
    return from + (target - from) * eased;
  }
  void Snap(bool shown) {
    from = target = shown ? 1.f : 0.f;
    duration = 0;
  }
  void Start(bool shown, double now) {
    const float current = Value(now), next = shown ? 1.f : 0.f;
    from = current;
    target = next;
    started = now;
    duration = (shown ? 220. : 110.) * std::abs(next - current);
  }
};

struct PopupPosition {
  double x = 0, y = 0;
};
struct PopupPlacementTween {
  PopupPosition from{}, target{};
  double started = 0, duration = 0;
  bool Active(double now) const {
    return duration > 0 && now < started + duration;
  }
  PopupPosition Value(double now) const {
    if (!Active(now))
      return target;
    const auto t = (float)std::clamp((now - started) / duration, 0., 1.);
    const auto eased = CssCubicBezier(t, 0, 0, .58f, 1);
    return {from.x + (target.x - from.x) * eased,
            from.y + (target.y - from.y) * eased};
  }
  void Snap(PopupPosition position) {
    from = target = position;
    duration = 0;
  }
  bool Start(PopupPosition next, double now) {
    if (target.x == next.x && target.y == next.y)
      return false;
    from = Value(now);
    target = next;
    started = now;
    duration = std::hypot(target.x - from.x, target.y - from.y) < .01 ? 0 : 200;
    return true;
  }
  void Freeze(double now) { Snap(Value(now)); }
};

inline bool PopupPlacementTests() {
  PopupPlacementTween t;
  t.Snap({100, 100});
  if (!t.Start({100, 300}, 1000) || t.Value(1000).y != 100)
    return false;
  const auto midpoint = t.Value(1100);
  if (midpoint.y <= 200 || midpoint.y >= 300 || midpoint.x != 100)
    return false;
  if (t.Start({100, 300}, 1100) || t.started != 1000)
    return false;
  t.Start({300, 200}, 1100);
  if (std::abs(t.Value(1100).y - midpoint.y) > .00001 ||
      t.Value(1100).x != midpoint.x)
    return false;
  auto previous = t.Value(1100);
  for (int i = 1101; i <= 1300; ++i) {
    const auto value = t.Value(i);
    if (value.x < previous.x || value.y > previous.y || value.x > 300 ||
        value.y < 200)
      return false;
    previous = value;
  }
  if (t.Active(1300) || t.Value(1300).x != 300 || t.Value(1300).y != 200)
    return false;
  t.Start({10, 20}, 1400);
  const auto frozen = t.Value(1450);
  t.Freeze(1450);
  if (t.Active(1450) || t.Value(1700).x != frozen.x ||
      t.Value(1700).y != frozen.y)
    return false;
  t.Snap({50, 60});
  return t.Value(2000).x == 50 && t.Value(2000).y == 60;
}

void PlacePopup(HWND window, POINT anchor, SIZE size);
void RepositionPopupAnimated(HWND window, POINT anchor, SIZE size);
void SnapPopupPlacement(HWND window);
void FreezePopupPlacement(HWND window);
void ShowPopupAnimated(HWND window, POINT anchor, float scale);
void HidePopupAnimated(HWND window, float scale);
void HidePopupImmediately(HWND window);
bool PopupTargetVisible(HWND window);
RECT PopupAnimationBounds(HWND window);
void ClearPopupAnimations();
void ForgetPopupAnimation(HWND window);
void PulsePopupAnimations();
double PopupAverageFrameMs();
LRESULT PopupReflowMetric(WPARAM field);

inline bool PopupMotionTests() {
  if (!PopupPlacementTests())
    return false;
  // Reference samples of CSS ease-in, including the inverse x mapping.
  auto easeIn = [](float t) { return CssCubicBezier(t, .42f, 0, 1, 1); };
  if (std::abs(easeIn(.25f) - .0934647f) > .0001f ||
      std::abs(easeIn(.5f) - .3153568f) > .0001f ||
      std::abs(easeIn(.75f) - .6218619f) > .0001f)
    return false;
  float previous = 0;
  for (int i = 0; i <= 100; ++i) {
    const float value = easeIn(i / 100.f);
    if (value < previous || value < 0 || value > 1)
      return false;
    previous = value;
  }
  if (easeIn(0) != 0 || easeIn(1) != 1 || 1 - easeIn(.9f) <= 3 * easeIn(.1f))
    return false;
  PopupTween t;
  t.Start(true, 1000);
  if (!t.Active(1000) || t.Value(1000) != 0 || t.Value(1110) <= .31f ||
      t.Value(1110) >= .32f || t.Active(1220) || t.Value(1220) != 1)
    return false;
  // Reverse an unfinished exit without snapping to the start of a new entry.
  t.Start(false, 1300);
  const float midway = t.Value(1350);
  t.Start(true, 1350);
  if (std::abs(t.Value(1350) - midway) > .0001f || t.Value(1370) < midway ||
      t.Value(1800) != 1)
    return false;
  t.Start(false, 1900);
  if (t.Value(2010) != 0 || t.Active(2010))
    return false;
  t.Snap(true);
  return !t.Active(2000) && t.Value(2000) == 1;
}
