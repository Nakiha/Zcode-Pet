#include "composition_device.h"
#include <DispatcherQueue.h>
#include <windows.ui.composition.interop.h>
#include <winrt/Windows.Foundation.h>
#include <dxgi1_2.h>
#include <memory>
using namespace winrt;
using Microsoft::WRL::ComPtr;
namespace {
std::unique_ptr<CompositionDevice> shared;
}
void CompositionDevice::Initialize() {
  if (!winrt::Windows::System::DispatcherQueue::GetForCurrentThread()) {
    DispatcherQueueOptions options{sizeof(options), DQTYPE_THREAD_CURRENT,
                                   DQTAT_COM_NONE};
    check_hresult(CreateDispatcherQueueController(
        options,
        reinterpret_cast<ABI::Windows::System::IDispatcherQueueController **>(
            put_abi(queue))));
  }
  compositor = winrt::Windows::UI::Composition::Compositor();
  check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                  D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                  D3D11_SDK_VERSION, &d3d, nullptr, nullptr));
  ComPtr<IDXGIDevice> dxgi;
  check_hresult(d3d.As(&dxgi));
  check_hresult(D2D1CreateDevice(dxgi.Get(), nullptr, &d2d));
  check_hresult(
      compositor.as<ABI::Windows::UI::Composition::ICompositorInterop>()
          ->CreateGraphicsDevice(
              d2d.Get(),
              reinterpret_cast<
                  ABI::Windows::UI::Composition::ICompositionGraphicsDevice **>(
                  put_abi(graphics))));
}
CompositionDevice &SharedCompositionDevice() {
  if (!shared) {
    shared = std::make_unique<CompositionDevice>();
    try {
      shared->Initialize();
    } catch (...) {
      shared->result = to_hresult();
    }
  }
  check_hresult(shared->result);
  return *shared;
}
void CompositionDevice::Shutdown() noexcept {
  try {
    graphics = nullptr;
    if (compositor)
      compositor.Close();
    compositor = nullptr;
    d2d.Reset();
    d3d.Reset();
    if (queue) {
      auto finished = queue.ShutdownQueueAsync();
      while (finished.Status() ==
             winrt::Windows::Foundation::AsyncStatus::Started) {
        MSG msg{};
        if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
          if (msg.message != WM_QUIT) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
          }
        } else
          MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
      }
      queue = nullptr;
    }
  } catch (...) {
    queue = nullptr;
  }
}
void FinishCompositionThread() noexcept {
  if (shared) {
    shared->Shutdown();
    shared.reset();
  }
}
