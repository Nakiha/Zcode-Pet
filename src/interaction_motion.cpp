#include "interaction_motion.h"
#include "frame_clock.h"
#include <map>

namespace {
std::map<HWND, std::map<std::wstring, InteractionFeedback>> surfaces;
void Schedule() {
  bool active = false;
  for (const auto &[window, items] : surfaces)
    if (IsWindowVisible(window))
      // 即使时间已过终点，也要等 Pulse 提交末帧后才能停；输入轮询不能提前停帧。
      for (const auto &[key, item] : items)
        active |= item.NeedsFrames();
  RequestInteractionFrames(active);
}
} // namespace
void SetInteractionFeedback(HWND window, const std::wstring &key,
                            InteractionState state, float opacity) {
  if (!window)
    return;
  auto surface = surfaces.find(window);
  if (opacity == 0 &&
      (surface == surfaces.end() || !surface->second.contains(key)))
    return;
  auto &item = surfaces[window][key];
  const auto now = VisualClockMs();
  if (item.Change(state, opacity, now)) {
    InvalidateRect(window, nullptr, FALSE);
  }
  Schedule();
}
float InteractionOpacity(HWND window, const std::wstring &key) {
  const auto surface = surfaces.find(window);
  if (surface == surfaces.end())
    return 0;
  const auto item = surface->second.find(key);
  return item == surface->second.end()
             ? 0
             : item->second.tween.Value(VisualClockMs());
}
void ClearInteractionFeedback(HWND window) {
  if (surfaces.erase(window))
    InvalidateRect(window, nullptr, FALSE);
  Schedule();
}
void PulseInteractionFeedback() {
  const auto now = VisualClockMs();
  for (auto surface = surfaces.begin(); surface != surfaces.end();) {
    const auto window = surface->first;
    if (!IsWindow(window) || !IsWindowVisible(window)) {
      surface = surfaces.erase(surface);
      continue;
    }
    bool redraw = false;
    for (auto item = surface->second.begin(); item != surface->second.end();) {
      // duration 直到末帧绘制后才清零，保证目标色和完全透明都实际提交一次。
      redraw |= item->second.Advance(now);
      if (!item->second.NeedsFrames()) {
        if (item->second.tween.target == 0) {
          item = surface->second.erase(item);
          continue;
        }
      }
      ++item;
    }
    if (redraw) {
      InvalidateRect(window, nullptr, FALSE);
      UpdateWindow(window);
    }
    if (surface->second.empty())
      surface = surfaces.erase(surface);
    else
      ++surface;
  }
  Schedule();
}
void ClearAllInteractionFeedback() {
  surfaces.clear();
  RequestInteractionFrames(false);
}
bool InteractionMotionTests() {
  InteractionTween tween;
  if (!tween.To(.075f, 0, 140) || tween.Value(0) != 0 ||
      tween.Value(140) != .075f || tween.Active(140))
    return false;
  const auto halfway = tween.Value(70);
  if (halfway <= .0375f || halfway >= .075f)
    return false;
  if (tween.To(.075f, 70, 140) || tween.started != 0)
    return false;
  tween.To(0, 70, 160);
  if (std::abs(tween.Value(70) - halfway) > .000001f || tween.Value(230) != 0)
    return false;
  const auto interrupted = tween.Value(100);
  tween.To(.13f, 100, 60);
  if (std::abs(tween.Value(100) - interrupted) > .000001f ||
      tween.Value(160) != .13f)
    return false;
  tween.To(.075f, 160, 120);
  float previous = tween.Value(160);
  for (int i = 161; i <= 280; ++i) {
    const auto value = tween.Value(i);
    if (value > previous + .000001f || value < .075f - .000001f)
      return false;
    previous = value;
  }
  InteractionFeedback tap;
  tap.Change({true, false}, .075f, 0);
  tap.Advance(140);
  tap.Change({true, true}, .23f, 200);
  tap.Change({true, false}, .075f, 205);
  tap.Advance(240);
  if (!tap.deferredRelease || !tap.NeedsFrames() ||
      tap.tween.Value(240) != .23f)
    return false;
  tap.Change({true, false}, .075f, 250);
  if (!tap.deferredRelease || tap.tween.target != .23f)
    return false;
  tap.Advance(285);
  if (tap.deferredRelease || tap.tween.target != .075f ||
      tap.tween.Value(285) != .23f)
    return false;
  tap.Advance(405);
  if (tap.NeedsFrames() || tap.tween.Value(405) != .075f)
    return false;
  tap.Change({true, true}, .23f, 500);
  tap.Change({true, false}, .075f, 505);
  tap.Change({}, 0, 510);
  if (tap.deferredRelease || tap.tween.target != 0)
    return false;
  return tween.duration > 0 && !tween.Active(280) &&
         InteractionDuration({}, {true, false}) == 140 &&
         InteractionDuration({true, false}, {true, true}) == 40 &&
         InteractionDuration({true, true}, {true, false}) == 120 &&
         InteractionDuration({true, false}, {}) == 160;
}
