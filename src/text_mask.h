#pragma once
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <deque>
#include <cmath>

// Rasterize a stationary title once at its real subpixel origin. Only its
// alpha is reused; the moving highlight brush still advances every frame.
inline bool PaintTextMask(ID2D1RenderTarget *canvas, IDWriteTextLayout *layout,
                          ID2D1Brush *light, float x, float originY,
                          D2D1_RECT_F clip, float scale) {
  using Microsoft::WRL::ComPtr;
  struct Entry {
    ComPtr<ID2D1RenderTarget> owner;
    ComPtr<IDWriteTextLayout> layout;
    ComPtr<ID2D1Bitmap> mask;
    float x, y, scale;
    D2D1_RECT_F clip, bounds;
  };
  static std::deque<Entry> entries;
  // Drop old device resources promptly when a window render target changes.
  while (!entries.empty() && entries.front().owner.Get() != canvas)
    entries.pop_front();
  for (const auto &entry : entries)
    if (entry.layout.Get() == layout && entry.x == x && entry.y == originY &&
        entry.scale == scale && entry.clip.left == clip.left &&
        entry.clip.top == clip.top && entry.clip.right == clip.right &&
        entry.clip.bottom == clip.bottom) {
      const auto previous = canvas->GetAntialiasMode();
      canvas->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
      canvas->FillOpacityMask(entry.mask.Get(), light,
                              D2D1_OPACITY_MASK_CONTENT_GRAPHICS, &entry.bounds,
                              nullptr);
      canvas->SetAntialiasMode(previous);
      return true;
    }
  const float left = std::floor(clip.left * scale) / scale,
              top = std::floor(clip.top * scale) / scale;
  const auto pixels = D2D1::SizeU(
      (UINT32)(std::ceil(clip.right * scale) - std::floor(clip.left * scale)),
      (UINT32)(std::ceil(clip.bottom * scale) - std::floor(clip.top * scale)));
  if (!pixels.width || !pixels.height)
    return false;
  const auto size = D2D1::SizeF(pixels.width / scale, pixels.height / scale);
  ComPtr<ID2D1BitmapRenderTarget> surface;
  ComPtr<ID2D1SolidColorBrush> white;
  if (FAILED(canvas->CreateCompatibleRenderTarget(
          &size, &pixels, nullptr, D2D1_COMPATIBLE_RENDER_TARGET_OPTIONS_NONE,
          &surface)) ||
      FAILED(surface->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), &white)))
    return false;
  surface->BeginDraw();
  surface->Clear(D2D1::ColorF(0, 0.f));
  surface->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
  surface->DrawTextLayout(D2D1::Point2F(x - left, originY - top), layout,
                          white.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
  Entry entry{
      canvas,  layout,
      {},      x,
      originY, scale,
      clip,    D2D1::RectF(left, top, left + size.width, top + size.height)};
  if (FAILED(surface->EndDraw()) || FAILED(surface->GetBitmap(&entry.mask)))
    return false;
  entries.push_front(std::move(entry));
  if (entries.size() > 12)
    entries.pop_back();
  const auto previous = canvas->GetAntialiasMode();
  canvas->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
  canvas->FillOpacityMask(entries.front().mask.Get(), light,
                          D2D1_OPACITY_MASK_CONTENT_GRAPHICS,
                          &entries.front().bounds, nullptr);
  canvas->SetAntialiasMode(previous);
  return true;
}
