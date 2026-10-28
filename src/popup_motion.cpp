#include "popup_motion.h"
#include "frame_clock.h"
#include <map>

namespace {
struct Motion {
  POINT anchor{};
  int slide = 0;
  PopupTween tween;
  double lastFrame = 0;
};
std::map<HWND, Motion> motions;
double totalFrameMs = 0;
unsigned frameCount = 0;
double Now() {
  static const double ticksPerMs = [] {
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    return frequency.QuadPart / 1000.;
  }();
  LARGE_INTEGER counter;
  QueryPerformanceCounter(&counter);
  return counter.QuadPart / ticksPerMs;
}
Motion &State(HWND window) {
  auto [i, added] = motions.try_emplace(window);
  if (added) {
    RECT r;
    GetWindowRect(window, &r);
    i->second.anchor = {r.left, r.top};
    i->second.tween.Snap(IsWindowVisible(window) != FALSE);
  }
  return i->second;
}
int SlideFor(HWND window, POINT anchor, float scale) {
  RECT bounds;
  GetWindowRect(window, &bounds);
  MONITORINFO monitor{sizeof(monitor)};
  GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
  const int distance = std::max(1, (int)std::lround(6 * scale));
  const LONG roomBelow = monitor.rcWork.bottom - anchor.y - (bounds.bottom - bounds.top);
  if (roomBelow >= distance)
    return distance;
  return -(int)std::min<LONG>(distance, std::max(0L, anchor.y - monitor.rcWork.top));
}
void Apply(HWND window, const Motion &motion, double now) {
  const int y = motion.anchor.y +
                (int)std::lround(motion.slide * (1 - motion.tween.Value(now)));
  RECT r;
  GetWindowRect(window, &r);
  if (r.left != motion.anchor.x || r.top != y)
    SetWindowPos(window, nullptr, motion.anchor.x, y, 0, 0,
                   SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}
void UpdateActivity() {
  const double now = Now();
  bool active = false;
  for (const auto &[window, motion] : motions)
    active |= IsWindow(window) && motion.tween.Active(now);
  RequestMotionFrames(active);
}
void Tick(HWND window) {
  auto i = motions.find(window);
  if (i == motions.end() || !IsWindow(window)) {
    return;
  }
  auto &motion = i->second;
  const double now = Now();
  if (motion.lastFrame > 0) {
    totalFrameMs += now - motion.lastFrame;
    ++frameCount;
  }
  motion.lastFrame = now;
  Apply(window, motion, now);
  if (!motion.tween.Active(now)) {
    motion.lastFrame = 0;
    if (motion.tween.target == 0) {
      ShowWindow(window, SW_HIDE);
      SetWindowPos(window, nullptr, motion.anchor.x, motion.anchor.y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
  }
}
} // namespace

void PlacePopup(HWND window, POINT anchor, SIZE size) {
  auto &motion = State(window);
  const double now = Now();
  const bool active = motion.tween.Active(now);
  if (!active)
    motion.tween.Snap(motion.tween.target > .5f);
  motion.anchor = anchor;
  const int y = anchor.y + (active
      ? (int)std::lround(motion.slide * (1 - motion.tween.Value(now))) : 0);
  SetWindowPos(window, HWND_TOPMOST, anchor.x, y, size.cx, size.cy,
                 SWP_NOACTIVATE);
  UpdateActivity();
}
void SnapPopupPlacement(HWND window) {
  auto &motion = State(window);
  motion.tween.Snap(motion.tween.target > .5f);
  motion.lastFrame = 0;
  SetWindowPos(window, nullptr, motion.anchor.x, motion.anchor.y, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
  UpdateActivity();
}
void ShowPopupAnimated(HWND window, POINT anchor, float scale) {
  auto &motion = State(window);
  if (motion.tween.target == 1 && IsWindowVisible(window))
    return;
  if (!IsWindowVisible(window))
    motion.tween.Snap(false);
  motion.anchor = anchor;
  motion.slide = SlideFor(window, anchor, scale);
  const double now = Now();
  motion.tween.Start(true, now);
  motion.lastFrame = 0;
  Apply(window, motion, now);
  SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
  InvalidateRect(window, nullptr, FALSE);
  UpdateWindow(window);
  RequestMotionFrames(true);
}
void HidePopupAnimated(HWND window, float scale) {
  if (!IsWindowVisible(window))
    return;
  auto &motion = State(window);
  if (motion.tween.target == 0)
    return;
  motion.slide = SlideFor(window, motion.anchor, scale);
  motion.tween.Start(false, Now());
  motion.lastFrame = 0;
  RequestMotionFrames(true);
}
void HidePopupImmediately(HWND window) {
  State(window).tween.Snap(false);
  ShowWindow(window, SW_HIDE);
  UpdateActivity();
}
bool PopupTargetVisible(HWND window) {
  auto i = motions.find(window);
  return i == motions.end() ? IsWindowVisible(window) != FALSE
                            : i->second.tween.target > .5f;
}
RECT PopupAnimationBounds(HWND window) {
  RECT bounds;
  GetWindowRect(window, &bounds);
  auto i = motions.find(window);
  if (i != motions.end() && i->second.tween.Active(Now())) {
    const auto &motion = i->second;
    const LONG height = bounds.bottom - bounds.top;
    bounds.top = std::min(motion.anchor.y, motion.anchor.y + motion.slide);
    bounds.bottom = std::max(motion.anchor.y, motion.anchor.y + motion.slide) + height;
  }
  return bounds;
}
void ClearPopupAnimations() {
  motions.clear();
  RequestMotionFrames(false);
}
void PulsePopupAnimations() {
  for (const auto &[window, motion] : motions) {
    (void)motion;
    if (IsWindowVisible(window))
      Tick(window);
  }
  UpdateActivity();
}
double PopupAverageFrameMs() {
  return frameCount ? totalFrameMs / frameCount : 0;
}
