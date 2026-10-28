#include "panel_composition.h"
#include "composition_device.h"
#include "theme.h"
#include <d3d11.h>
#include <d2d1_1.h>
#include <dxgi1_2.h>
#include <DispatcherQueue.h>
#include <windows.ui.composition.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Composition.Desktop.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <map>
using Microsoft::WRL::ComPtr;
using namespace winrt;
using namespace winrt::Windows::UI::Composition;
using namespace winrt::Windows::Foundation::Numerics;
namespace Interop = ABI::Windows::UI::Composition;

struct PanelComposition::Impl {
  struct Node {
    ComposedSession row;
    float scale = 0;
    ContainerVisual root{nullptr};
    SpriteVisual glow{nullptr}, arc{nullptr};
    bool shimmer = false, spinner = false;
  };
  bool attempted = false, ready = false;
  HRESULT result = S_OK;
  uint64_t uploads = 0, starts = 0, updates = 0, failures = 0;
  Compositor compositor{nullptr};
  Desktop::DesktopWindowTarget target{nullptr};
  ContainerVisual root{nullptr};
  CompositionGraphicsDevice graphics{nullptr};
  CompositionPropertySet clock{nullptr};
  bool glowClock = false, spinClock = false;
  CompositionDrawingSurface arcSurface{nullptr};
  CompositionDrawingSurface baseSurface{nullptr};
  SpriteVisual baseVisual{nullptr};
  float baseWidth = 0, baseHeight = 0, baseScale = 0;
  uint64_t baseDraws = 0;
  float arcScale = 0;
  ComPtr<ID3D11Device> d3d;
  ComPtr<ID2D1Device> d2d;
  std::map<std::wstring, Node> nodes;

  void Paint(const CompositionDrawingSurface &surface, float scale,
             const std::function<void(ID2D1DeviceContext *)> &paint) {
    auto interop = surface.as<Interop::ICompositionDrawingSurfaceInterop>();
    ComPtr<ID2D1DeviceContext> canvas;
    POINT offset{};
    check_hresult(interop->BeginDraw(nullptr, IID_PPV_ARGS(&canvas), &offset));
    try {
      canvas->SetDpi(96 * scale, 96 * scale);
      canvas->SetTransform(
          D2D1::Matrix3x2F::Translation(offset.x / scale, offset.y / scale));
      canvas->Clear(D2D1::ColorF(0, 0.f));
      paint(canvas.Get());
    } catch (...) {
      interop->EndDraw();
      throw;
    }
    check_hresult(interop->EndDraw());
  }
  CompositionDrawingSurface
  Surface(int width, int height, float scale,
          const std::function<void(ID2D1DeviceContext *)> &paint) {
    using namespace winrt::Windows::Graphics::DirectX;
    auto surface = graphics.CreateDrawingSurface(
        {(float)width, (float)height},
        DirectXPixelFormat::B8G8R8A8UIntNormalized,
        DirectXAlphaMode::Premultiplied);
    Paint(surface, scale, paint);
    ++uploads;
    return surface;
  }
  ScalarKeyFrameAnimation Loop(float end, int milliseconds) {
    auto animation = compositor.CreateScalarKeyFrameAnimation();
    animation.InsertKeyFrame(0, 0, compositor.CreateLinearEasingFunction());
    animation.InsertKeyFrame(1, end, compositor.CreateLinearEasingFunction());
    animation.Duration(std::chrono::milliseconds(milliseconds));
    animation.IterationBehavior(AnimationIterationBehavior::Forever);
    return animation;
  }
  SpriteVisual Spinner(float scale) {
    const int pixels = (int)std::ceil(18 * scale);
    if (!arcSurface || arcScale != scale) {
      arcSurface =
          Surface(pixels, pixels, scale, [](ID2D1DeviceContext *canvas) {
            ComPtr<ID2D1Factory> factory;
            canvas->GetFactory(&factory);
            ComPtr<ID2D1SolidColorBrush> brush;
            check_hresult(canvas->CreateSolidColorBrush(
                D2D1::ColorF(PetTheme::Running.ink), &brush));
            ComPtr<ID2D1PathGeometry> path;
            ComPtr<ID2D1GeometrySink> sink;
            check_hresult(factory->CreatePathGeometry(&path));
            check_hresult(path->Open(&sink));
            sink->BeginFigure(D2D1::Point2F(12.8f, 9),
                              D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddArc(D2D1::ArcSegment(
                D2D1::Point2F(9, 5.2f), D2D1::SizeF(3.8f, 3.8f), 0,
                D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_LARGE));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            check_hresult(sink->Close());
            ComPtr<ID2D1StrokeStyle> stroke;
            check_hresult(factory->CreateStrokeStyle(
                D2D1::StrokeStyleProperties(
                    D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                    D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND),
                nullptr, 0, &stroke));
            canvas->DrawGeometry(path.Get(), brush.Get(), 1.8f, stroke.Get());
          });
      arcScale = scale;
    }
    auto visual = compositor.CreateSpriteVisual();
    visual.Size({(float)pixels, (float)pixels});
    auto brush = compositor.CreateSurfaceBrush(arcSurface);
    brush.Stretch(CompositionStretch::None);
    brush.HorizontalAlignmentRatio(0);
    brush.VerticalAlignmentRatio(0);
    visual.Brush(brush);
    visual.CenterPoint({9 * scale, 9 * scale, 0});
    auto angle = compositor.CreateExpressionAnimation(L"clock.angle");
    angle.SetReferenceParameter(L"clock", clock);
    visual.StartAnimation(L"RotationAngleInDegrees", angle);
    return visual;
  }
  SpriteVisual Glow(const ComposedSession &row, float scale) {
    const float left = std::floor(row.x * scale),
                top = std::floor(row.y * scale);
    const int width = (int)std::ceil((row.x + row.width) * scale) - (int)left,
              height = (int)std::ceil((row.y + row.height) * scale) - (int)top;
    const float x = row.x - left / scale, y = row.y - top / scale;
    auto surface =
        Surface(width, height, scale, [&](ID2D1DeviceContext *canvas) {
          ComPtr<ID2D1SolidColorBrush> white;
          check_hresult(
              canvas->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), &white));
          canvas->PushAxisAlignedClip(
              D2D1::RectF(x, y, x + row.width, y + row.height),
              D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
          canvas->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
          canvas->DrawTextLayout(D2D1::Point2F(x, y + row.textOffset),
                                 row.layout.Get(), white.Get(),
                                 D2D1_DRAW_TEXT_OPTIONS_CLIP);
          canvas->PopAxisAlignedClip();
        });
    auto gradient = compositor.CreateLinearGradientBrush();
    gradient.MappingMode(CompositionMappingMode::Absolute);
    constexpr UINT32 color = PetTheme::RunningShine;
    auto stop = [&](float at, BYTE alpha) {
      gradient.ColorStops().Append(compositor.CreateColorGradientStop(
          at, {alpha, (BYTE)((color >> 16) & 255), (BYTE)((color >> 8) & 255),
               (BYTE)(color & 255)}));
    };
    stop(0, 0);
    stop(.25f, 31);
    stop(.5f, 255);
    stop(.75f, 31);
    stop(1, 0);
    const float band = 56 * scale, span = row.span * scale;
    auto geometry = compositor.CreatePropertySet();
    geometry.InsertScalar(L"origin", x * scale);
    geometry.InsertScalar(L"band", band);
    geometry.InsertScalar(L"span", span);
    auto animate = [&](const wchar_t *name, const wchar_t *expression) {
      auto a = compositor.CreateExpressionAnimation(expression);
      a.SetReferenceParameter(L"clock", clock);
      a.SetReferenceParameter(L"geometry", geometry);
      gradient.StartAnimation(name, a);
    };
    animate(L"StartPoint",
            L"Vector2(geometry.origin - geometry.band + (geometry.span + "
            L"geometry.band) * clock.phase, 0)");
    animate(L"EndPoint", L"Vector2(geometry.origin + (geometry.span + "
                         L"geometry.band) * clock.phase, 0)");
    auto mask = compositor.CreateMaskBrush();
    auto coverage = compositor.CreateSurfaceBrush(surface);
    coverage.Stretch(CompositionStretch::None);
    coverage.HorizontalAlignmentRatio(0);
    coverage.VerticalAlignmentRatio(0);
    mask.Mask(coverage);
    mask.Source(gradient);
    auto visual = compositor.CreateSpriteVisual();
    visual.Size({(float)width, (float)height});
    visual.Offset({left, top, 0});
    visual.Brush(mask);
    return visual;
  }
  void Stop(Node &node) {
    if (node.arc)
      node.arc.StopAnimation(L"RotationAngleInDegrees");
    if (node.glow) {
      auto source = node.glow.Brush().as<CompositionMaskBrush>().Source();
      source.StopAnimation(L"StartPoint");
      source.StopAnimation(L"EndPoint");
    }
  }
  void Clear() {
    if (root)
      root.Children().RemoveAll();
    for (auto &[key, node] : nodes)
      Stop(node);
    nodes.clear();
    baseSurface = nullptr;
    baseVisual = nullptr;
    baseScale = 0;
    if (clock) {
      clock.StopAnimation(L"phase");
      clock.StopAnimation(L"angle");
    }
    glowClock = spinClock = false;
  }
};
PanelComposition::PanelComposition() : impl(std::make_unique<Impl>()) {}
PanelComposition::~PanelComposition() { Shutdown(); }
bool PanelComposition::Ensure(HWND window) noexcept {
  auto &s = *impl;
  if (s.attempted)
    return s.ready;
  s.attempted = true;
  try {
    auto &shared=SharedCompositionDevice();
    s.compositor=shared.compositor;
    s.graphics=shared.graphics;
    s.d3d=shared.d3d;
    s.d2d=shared.d2d;
    auto host=s.compositor.as<Interop::Desktop::ICompositorDesktopInterop>();
    check_hresult(host->CreateDesktopWindowTarget(window,TRUE,
        reinterpret_cast<Interop::Desktop::IDesktopWindowTarget**>(put_abi(s.target))));
    s.root = s.compositor.CreateContainerVisual();
    s.clock = s.compositor.CreatePropertySet();
    s.clock.InsertScalar(L"phase", 0);
    s.clock.InsertScalar(L"angle", 0);
    s.root.Clip(s.compositor.CreateInsetClip());
    s.target.Root(s.root);
    s.ready = true;
    s.result = S_OK;
  } catch (...) {
    s.result = to_hresult();
    ++s.failures;
    Shutdown();
  }
  return s.ready;
}
bool PanelComposition::Ready() const noexcept { return impl->ready; }
HRESULT PanelComposition::PaintBase(
    float width, float height, float scale,
    const std::function<void(ID2D1RenderTarget *)> &paint) noexcept {
  auto &s = *impl;
  if (!s.ready)
    return s.result;
  try {
    if (!s.baseSurface || s.baseWidth != width || s.baseHeight != height ||
        s.baseScale != scale) {
      using namespace winrt::Windows::Graphics::DirectX;
      s.baseSurface = s.graphics.CreateDrawingSurface(
          {std::ceil(width * scale), std::ceil(height * scale)},
          DirectXPixelFormat::B8G8R8A8UIntNormalized,
          DirectXAlphaMode::Premultiplied);
      if (!s.baseVisual) {
        s.baseVisual = s.compositor.CreateSpriteVisual();
        s.root.Children().InsertAtBottom(s.baseVisual);
      }
      auto brush = s.compositor.CreateSurfaceBrush(s.baseSurface);
      brush.Stretch(CompositionStretch::None);
      brush.HorizontalAlignmentRatio(0);
      brush.VerticalAlignmentRatio(0);
      s.baseVisual.Brush(brush);
      s.baseVisual.Size({width * scale, height * scale});
      s.baseWidth = width;
      s.baseHeight = height;
      s.baseScale = scale;
    }
    s.Paint(s.baseSurface, scale,
            [&](ID2D1DeviceContext *canvas) { paint(canvas); });
    ++s.baseDraws;
    return s.result = S_OK;
  } catch (...) {
    s.result = to_hresult();
    ++s.failures;
    Shutdown();
    return s.result;
  }
}
HRESULT PanelComposition::Update(const std::vector<ComposedSession> &rows,
                                 float width, float height, float scale,
                                 bool shimmer, bool spinner) noexcept {
  auto &s = *impl;
  if (!s.ready)
    return s.result;
  try {
    check_hresult(s.d3d->GetDeviceRemovedReason());
    s.root.IsVisible(true);
    s.root.Size({width * scale, height * scale});
    const bool wantGlow = shimmer && !rows.empty(),
               wantSpin = spinner && !rows.empty();
    if (wantGlow != s.glowClock) {
      if (wantGlow) {
        s.clock.StartAnimation(L"phase", s.Loop(1, 2800));
        ++s.starts;
      } else
        s.clock.StopAnimation(L"phase");
      s.glowClock = wantGlow;
    }
    if (wantSpin != s.spinClock) {
      if (wantSpin) {
        s.clock.StartAnimation(L"angle", s.Loop(360, 1200));
        ++s.starts;
      } else
        s.clock.StopAnimation(L"angle");
      s.spinClock = wantSpin;
    }
    for (auto it = s.nodes.begin(); it != s.nodes.end();) {
      if (std::none_of(rows.begin(), rows.end(), [&](const auto &r) {
            return r.identity == it->first;
          })) {
        s.Stop(it->second);
        s.root.Children().Remove(it->second.root);
        it = s.nodes.erase(it);
      } else
        ++it;
    }
    for (const auto &row : rows) {
      auto &node = s.nodes[row.identity];
      const bool changed =
          !node.root || node.row.title != row.title || node.row.x != row.x ||
          node.row.y != row.y || node.row.width != row.width ||
          node.row.height != row.height || node.scale != scale ||
          node.shimmer != shimmer || node.spinner != spinner;
      if (changed) {
        if (node.root) {
          s.Stop(node);
          s.root.Children().Remove(node.root);
        }
        node = {};
        node.row = row;
        node.scale = scale;
        node.shimmer = shimmer;
        node.spinner = spinner;
        node.root = s.compositor.CreateContainerVisual();
        if (shimmer && row.width > 0 && row.layout) {
          node.glow = s.Glow(row, scale);
          node.root.Children().InsertAtTop(node.glow);
        }
        if (spinner) {
          node.arc = s.Spinner(scale);
          node.root.Children().InsertAtTop(node.arc);
        }
        s.root.Children().InsertAtTop(node.root);
      }
      if (node.arc)
        node.arc.Offset({(18 - 9) * scale, (row.indicatorY - 9) * scale, 0});
      node.row.indicatorY = row.indicatorY;
    }
    ++s.updates;
    return s.result = S_OK;
  } catch (...) {
    s.result = to_hresult();
    ++s.failures;
    Shutdown();
    return s.result;
  }
}
void PanelComposition::Suspend() noexcept {
  try {
    impl->Clear();
    if (impl->root)
      impl->root.IsVisible(false);
  } catch (...) {
  }
}
void PanelComposition::Shutdown() noexcept {
  auto &s = *impl;
  s.ready = false;
  try {
    s.Clear();
    if (s.target) {
      s.target.Root(nullptr);
      s.target.Close();
    }
    s.arcSurface = nullptr;
    s.arcScale = 0;
    s.clock = nullptr;
    s.root = nullptr;
    s.target = nullptr;
    s.graphics = nullptr;
    s.compositor = nullptr;
    s.d2d.Reset();
    s.d3d.Reset();
  } catch (...) {
  }
}
LRESULT PanelComposition::Metric(unsigned field) const noexcept {
  const auto &s = *impl;
  return field == 0   ? s.ready
         : field == 1 ? s.result
         : field == 2 ? (LRESULT)s.uploads
         : field == 3 ? (LRESULT)s.starts
         : field == 4 ? (LRESULT)s.updates
         : field == 5 ? (LRESULT)s.nodes.size()
         : field == 6 ? (LRESULT)s.failures
                      : (LRESULT)s.baseDraws;
}
void PanelComposition::FinishThread() noexcept {
  Shutdown();
  FinishCompositionThread();
}
bool PanelCompositionTests() {
  const auto apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(apartment))
    return false;
  WNDCLASSW type{};
  type.lpfnWndProc = DefWindowProcW;
  type.hInstance = GetModuleHandleW(nullptr);
  type.lpszClassName = L"ZCodePet.CompositionTest";
  RegisterClassW(&type);
  const auto window =
      CreateWindowExW(WS_EX_TOOLWINDOW, type.lpszClassName, L"", WS_POPUP, 0, 0,
                      640, 160, nullptr, nullptr, type.hInstance, nullptr);
  PanelComposition composition;
  bool passed = window != nullptr;
  try {
    if (passed && composition.Ensure(window)) {
      ComPtr<IDWriteFactory> text;
      check_hresult(DWriteCreateFactory(
          DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
          reinterpret_cast<IUnknown **>(text.GetAddressOf())));
      ComPtr<IDWriteTextFormat> font;
      check_hresult(text->CreateTextFormat(
          L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
          DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12, L"en-US",
          &font));
      ComPtr<IDWriteTextLayout> layout;
      check_hresult(
          text->CreateTextLayout(L"Session", 7, font.Get(), 180, 34, &layout));
      std::vector<ComposedSession> rows{
          {L"one", L"Session", layout, 34, 0, 180, 34, 8, 50, 17}};
      auto update = [&] {
        check_hresult(composition.Update(rows, 312, 102, 1.75f, true, true));
      };
      update();
      check_hresult(
          composition.PaintBase(312, 102, 1.75f, [](ID2D1RenderTarget *canvas) {
            canvas->Clear(D2D1::ColorF(0, 0.f));
          }));
      passed &= composition.Metric(7) == 1;
      const auto uploads = composition.Metric(2),
                 starts = composition.Metric(3);
      passed &= uploads == 2 && starts == 2 && composition.Metric(5) == 1;
      for (int i = 0; i < 200; ++i) {
        rows[0].indicatorY = 17.f + (float)(i % 3);
        update();
      }
      passed &=
          composition.Metric(2) == uploads && composition.Metric(3) == starts;
      passed &= composition.Metric(7) == 1;
      rows[0].title = L"Renamed";
      update();
      passed &= composition.Metric(2) == uploads + 1 &&
                composition.Metric(3) == starts;
      rows[0].width = 160;
      update();
      passed &= composition.Metric(2) == uploads + 2 &&
                composition.Metric(3) == starts;
      check_hresult(composition.Update(rows, 312, 102, 2, true, true));
      passed &= composition.Metric(2) == uploads + 4 &&
                composition.Metric(3) == starts;
      composition.Suspend();
      passed &= composition.Metric(5) == 0;
      update();
      passed &=
          composition.Metric(3) == starts + 2 && composition.Metric(5) == 1;
      rows.clear();
      update();
      passed &= composition.Metric(5) == 0 && composition.Metric(6) == 0;
    } else if (passed) {
      // Headless/unsupported systems must fail closed to the existing renderer.
      passed &= !composition.Ready() && composition.Metric(5) == 0 &&
                composition.Metric(6) == 1;
    }
  } catch (...) {
    passed = false;
  }
  composition.Shutdown();
  if (window)
    DestroyWindow(window);
  composition.FinishThread();
  CoUninitialize();
  return passed;
}
