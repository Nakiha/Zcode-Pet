#pragma once
#include <windows.h>
#include <dwrite.h>
#include <d2d1.h>
#include <wrl/client.h>
#include <memory>
#include <string>
#include <vector>
#include <functional>

struct ComposedSession {
  std::wstring identity, title;
  Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
  float x = 0, y = 0, width = 0, height = 0, textOffset = 0, span = 0,
        indicatorY = 0;
};
// The compositor owns animation time. Updating a row's age or hover must never
// restart its repeating animations or rasterize its stationary glyph mask.
class PanelComposition {
  struct Impl;
  std::unique_ptr<Impl> impl;

public:
  PanelComposition();
  ~PanelComposition();
  bool Ensure(HWND window) noexcept;
  bool Ready() const noexcept;
  HRESULT Update(const std::vector<ComposedSession> &rows, float width,
                 float height, float scale, bool shimmer,
                 bool spinner) noexcept;
  HRESULT
  PaintBase(float width, float height, float scale,
            const std::function<void(ID2D1RenderTarget *)> &paint) noexcept;
  void Suspend() noexcept;
  void Shutdown() noexcept;
  void FinishThread() noexcept;
  LRESULT Metric(unsigned field) const noexcept;
};
bool PanelCompositionTests();
