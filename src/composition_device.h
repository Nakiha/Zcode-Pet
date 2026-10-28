#pragma once
#include <windows.h>
#include <d3d11.h>
#include <d2d1_1.h>
#include <wrl/client.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.h>

// UI-thread-owned shared device. Panels and the pet must not create separate
// D3D devices merely because they live in different transparent HWNDs.
struct CompositionDevice {
  winrt::Windows::System::DispatcherQueueController queue{nullptr};
  winrt::Windows::UI::Composition::Compositor compositor{nullptr};
  winrt::Windows::UI::Composition::CompositionGraphicsDevice graphics{nullptr};
  Microsoft::WRL::ComPtr<ID3D11Device> d3d;
  Microsoft::WRL::ComPtr<ID2D1Device> d2d;
  HRESULT result = S_OK;
  void Initialize();
  void Shutdown() noexcept;
};
CompositionDevice &SharedCompositionDevice();
void FinishCompositionThread() noexcept;
