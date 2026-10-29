#pragma once
#include <d2d1.h>
#include <cmath>
// Choose a stable physical-pixel origin before DirectWrite sees the drawing
// surface's atlas translation. Do not let it snap the baseline a second time.
inline D2D1_POINT_2F TextRasterOrigin(ID2D1RenderTarget *target, float x, float y) {
  float dx = 96, dy = 96;
  target->GetDpi(&dx, &dy);
  return {std::round(x * dx / 96) * 96 / dx,
          std::round(y * dy / 96) * 96 / dy};
}
inline constexpr auto StableTextOptions = static_cast<D2D1_DRAW_TEXT_OPTIONS>(
    D2D1_DRAW_TEXT_OPTIONS_CLIP | D2D1_DRAW_TEXT_OPTIONS_NO_SNAP);
