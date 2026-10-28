#pragma once
#include <windows.h>
#include <d2d1.h>
#include <wrl/client.h>
#include <map>

namespace UiRenderer {
inline std::map<HWND, D2D1_RENDER_TARGET_TYPE> backends;
inline HRESULT Create(ID2D1Factory *factory, HWND window, float dpi,
                        D2D1_SIZE_U size, ID2D1HwndRenderTarget **result) {
  HRESULT hr = E_FAIL;
  for (auto type : {D2D1_RENDER_TARGET_TYPE_HARDWARE, D2D1_RENDER_TARGET_TYPE_SOFTWARE}) {
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> created;
    const auto properties = D2D1::RenderTargetProperties(
        type, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                D2D1_ALPHA_MODE_PREMULTIPLIED), dpi, dpi);
    hr = factory->CreateHwndRenderTarget(properties,
        D2D1::HwndRenderTargetProperties(window, size, D2D1_PRESENT_OPTIONS_IMMEDIATELY),
        &created);
    if (SUCCEEDED(hr)) {
      backends[window] = type;
      *result = created.Detach();
      return hr;
    }
  }
  backends.erase(window);
  return hr;
}
inline LRESULT Backend(HWND window) {
  auto i = backends.find(window);
  return i == backends.end() ? -1 : (LRESULT)i->second;
}
inline void Forget(HWND window) { backends.erase(window); }
} // namespace UiRenderer
