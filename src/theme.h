#pragma once
#include <d2d1.h>
#include <wrl/client.h>

// Low-chroma pastel surfaces with a separate ink color keep status light
// without sacrificing legibility or relying on the desktop underneath.
namespace PetTheme {
inline constexpr UINT32 Ink = 0x332f42, Muted = 0x554e65, Time = 0x5b526c;
inline constexpr UINT32 Accent = 0x7963b6, Border = 0xd8cbe4;
inline constexpr UINT32 Divider = 0x9e90af;
inline constexpr UINT32 RunningShine = 0xa6acc4;
struct StatusTone {
  UINT32 fill, ink;
};
inline constexpr StatusTone Completed{0xdcefe9, 0x376f63};
inline constexpr StatusTone Running{0xdee9fa, 0x4c6594};
inline constexpr StatusTone Waiting{0xfae8d3, 0x8e5b34};
inline constexpr StatusTone Error{0xf6dee8, 0x924a64};
inline constexpr StatusTone Interrupted{0xe8e0f2, 0x725a8b};
inline constexpr StatusTone Neutral{0xe7e7e9, 0x656570};
inline constexpr StatusTone Offline{0xe2e6eb, 0x596779};
inline constexpr UINT32 Surface = 0xfaf5fc;
inline constexpr float TextReferenceBackground = .93f;

inline void PaintFeedback(ID2D1RenderTarget *target, D2D1_RECT_F bounds,
                          float opacity, float radius = 5) {
  if (opacity <= 0)
    return;
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
  if (SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(Accent, opacity),
                                              &brush))) {
    if (radius > 0)
      target->FillRoundedRectangle(D2D1::RoundedRect(bounds, radius, radius),
                                   brush.Get());
    else
      target->FillRectangle(bounds, brush.Get());
  }
}

inline void PaintSurfaceBorder(ID2D1RenderTarget *target, float width,
                               float height, float radius = 8.f) {
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> edge;
  if (SUCCEEDED(
          target->CreateSolidColorBrush(D2D1::ColorF(Border, .4f), &edge)))
    target->DrawRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(.5f, .5f, width - .5f, height - .5f),
                          radius, radius),
        edge.Get(), .75f);
}

// A milky tint balances native Acrylic visibility with a stable reading
// surface. A fully opaque palette is reserved for the unavailable-material
// fallback.
inline void PaintSurface(ID2D1RenderTarget *target, float width, float height,
                         bool materialAvailable, float radius = 8.f,
                         bool drawBorder = true) {
  using Microsoft::WRL::ComPtr;
  target->Clear(materialAvailable ? D2D1::ColorF(0, 0.f)
                                  : D2D1::ColorF(Surface));
  D2D1_GRADIENT_STOP stops[] = {
      {0, D2D1::ColorF(0xfff8fc, materialAvailable ? .56f : 1.f)},
      {1, D2D1::ColorF(0xf5f2ff, materialAvailable ? .50f : 1.f)}};
  ComPtr<ID2D1GradientStopCollection> collection;
  ComPtr<ID2D1LinearGradientBrush> gradient;
  if (SUCCEEDED(target->CreateGradientStopCollection(stops, 2, &collection)) &&
      SUCCEEDED(target->CreateLinearGradientBrush(
          D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0),
                                              D2D1::Point2F(width, height)),
          collection.Get(), &gradient))) {
    target->FillRectangle(D2D1::RectF(0, 0, width, height), gradient.Get());
  } else {
    target->Clear(D2D1::ColorF(Surface));
  }
  if (drawBorder)
    PaintSurfaceBorder(target, width, height, radius);
}
} // namespace PetTheme
