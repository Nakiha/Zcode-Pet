#pragma once
#include "popup_motion.h"
#include <string>

struct InteractionTween {
  float from = 0, target = 0;
  double started = 0, duration = 0;
  bool Active(double now) const {
    return duration > 0 && now < started + duration;
  }
  float Value(double now) const {
    if (!Active(now))
      return target;
    const auto progress = (float)std::clamp((now - started) / duration, 0., 1.);
    return from + (target - from) * CssCubicBezier(progress, 0, 0, .58f, 1);
  }
  bool To(float next, double now, double milliseconds) {
    if (next == target)
      return false;
    from = Value(now);
    target = next;
    started = now;
    duration = std::abs(target - from) < .00001f ? 0 : milliseconds;
    return true;
  }
};
struct InteractionState {
  bool hovered = false, pressed = false;
  bool operator==(const InteractionState &) const = default;
};
inline double InteractionDuration(InteractionState previous,
                                  InteractionState next) {
  if (previous.pressed != next.pressed)
    return next.pressed ? 40. : 120.;
  return next.hovered ? 140. : 160.;
}

struct InteractionFeedback {
  InteractionTween tween;
  InteractionState state;
  double pressedAt = 0, releaseAt = 0;
  float releasedOpacity = 0;
  bool deferredRelease = false;
  bool Change(InteractionState next, float opacity, double now) {
    if (deferredRelease && next == state && opacity == releasedOpacity)
      return false;
    const auto previous = state;
    state = next;
    if (next.pressed && !previous.pressed)
      pressedAt = now;
    // 极短点击也要先呈现明确的按下色；只延后视觉回退，不延后动作。
    if (previous.pressed && !next.pressed && next.hovered &&
        now < pressedAt + 85) {
      deferredRelease = true;
      releaseAt = pressedAt + 85;
      releasedOpacity = opacity;
      return true;
    }
    deferredRelease = false;
    return tween.To(opacity, now, InteractionDuration(previous, next));
  }
  bool Advance(double now) {
    if (deferredRelease && now >= releaseAt) {
      deferredRelease = false;
      tween.To(releasedOpacity, now, 120);
    }
    const bool redraw = tween.duration > 0;
    if (!tween.Active(now))
      tween.duration = 0;
    return redraw;
  }
  bool NeedsFrames() const { return tween.duration > 0 || deferredRelease; }
};

void SetInteractionFeedback(HWND window, const std::wstring &key,
                            InteractionState state, float opacity);
float InteractionOpacity(HWND window, const std::wstring &key);
void ClearInteractionFeedback(HWND window);
void PulseInteractionFeedback();
void ClearAllInteractionFeedback();
bool InteractionMotionTests();
