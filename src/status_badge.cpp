#include <windows.h>
#include "status_badge.h"
#include <wrl/client.h>
#include <vector>
#include <deque>
#include <cmath>
using Microsoft::WRL::ComPtr;
ID2D1StrokeStyle *RoundStatusStroke(ID2D1Factory *graphics) {
  static ComPtr<ID2D1Factory> owner;
  static ComPtr<ID2D1StrokeStyle> style;
  if (owner.Get() != graphics) {
    style.Reset();
    owner = graphics;
  }
  if (!style)
    graphics->CreateStrokeStyle(
        D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                    D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND),
        nullptr, 0, &style);
  return style.Get();
}
void DrawCheckMark(ID2D1RenderTarget *target, ID2D1Factory *graphics,
                   ID2D1Brush *brush, float x, float y, float scale,
                   float strokeWidth) {
  static ComPtr<ID2D1Factory> owner;
  static ComPtr<ID2D1PathGeometry> path;
  if (owner.Get() != graphics) {
    path.Reset();
    owner = graphics;
  }
  if (!path) {
    ComPtr<ID2D1PathGeometry> created;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(graphics->CreatePathGeometry(&created)) ||
        FAILED(created->Open(&sink)))
      return;
    // 同一个 figure 内连续折线才有连接处；独立 DrawLine 的平端帽会在拐角留洞。
    sink->BeginFigure(D2D1::Point2F(-2, 0), D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLine(D2D1::Point2F(-.5f, 1.5f));
    sink->AddLine(D2D1::Point2F(2, -1.5f));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    if (FAILED(sink->Close()))
      return;
    path = created;
  }
  const auto style = RoundStatusStroke(graphics);
  D2D1_MATRIX_3X2_F previous;
  target->GetTransform(&previous);
  target->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) *
                       D2D1::Matrix3x2F::Translation(x, y) * previous);
  target->DrawGeometry(path.Get(), brush, strokeWidth / scale, style);
  target->SetTransform(previous);
}
namespace {
class Outlines final : public IDWriteTextRenderer {
  ULONG refs = 1;
  ID2D1Factory *factory;

public:
  std::vector<ComPtr<ID2D1Geometry>> runs;
  explicit Outlines(ID2D1Factory *value) : factory(value) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **result) override {
    if (!result)
      return E_POINTER;
    *result = nullptr;
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IDWritePixelSnapping) ||
        iid == __uuidof(IDWriteTextRenderer)) {
      *result = static_cast<IDWriteTextRenderer *>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override {
    auto count = --refs;
    if (!count)
      delete this;
    return count;
  }
  HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void *,
                                                    BOOL *value) override {
    *value = TRUE;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetCurrentTransform(void *,
                                                DWRITE_MATRIX *value) override {
    *value = {1, 0, 0, 1, 0, 0};
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void *, FLOAT *value) override {
    *value = 1;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DrawGlyphRun(void *, FLOAT x, FLOAT y,
                                         DWRITE_MEASURING_MODE,
                                         const DWRITE_GLYPH_RUN *run,
                                         const DWRITE_GLYPH_RUN_DESCRIPTION *,
                                         IUnknown *) override {
    ComPtr<ID2D1PathGeometry> path;
    HRESULT hr = factory->CreatePathGeometry(&path);
    if (FAILED(hr))
      return hr;
    ComPtr<ID2D1GeometrySink> sink;
    hr = path->Open(&sink);
    if (FAILED(hr))
      return hr;
    hr = run->fontFace->GetGlyphRunOutline(
        run->fontEmSize, run->glyphIndices, run->glyphAdvances,
        run->glyphOffsets, run->glyphCount, run->isSideways,
        (run->bidiLevel & 1) != 0, sink.Get());
    HRESULT closed = sink->Close();
    if (FAILED(hr))
      return hr;
    if (FAILED(closed))
      return closed;
    ComPtr<ID2D1TransformedGeometry> positioned;
    hr = factory->CreateTransformedGeometry(
        path.Get(), D2D1::Matrix3x2F::Translation(x, y), &positioned);
    if (SUCCEEDED(hr))
      runs.push_back(positioned);
    return hr;
  }
  HRESULT STDMETHODCALLTYPE DrawUnderline(void *, FLOAT, FLOAT,
                                          const DWRITE_UNDERLINE *,
                                          IUnknown *) override {
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DrawStrikethrough(void *, FLOAT, FLOAT,
                                              const DWRITE_STRIKETHROUGH *,
                                              IUnknown *) override {
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DrawInlineObject(void *context, FLOAT x, FLOAT y,
                                             IDWriteInlineObject *object,
                                             BOOL sideways, BOOL rtl,
                                             IUnknown *effect) override {
    return object->Draw(context, this, x, y, sideways, rtl, effect);
  }
};
ComPtr<ID2D1GeometryGroup> LayoutOutlines(ID2D1Factory *graphics,
                                          IDWriteTextLayout *layout) {
  auto renderer = new Outlines(graphics);
  HRESULT hr = layout->Draw(nullptr, renderer, 0, 0);
  std::vector<ID2D1Geometry *> raw;
  for (auto &run : renderer->runs)
    raw.push_back(run.Get());
  ComPtr<ID2D1GeometryGroup> glyphs;
  if (SUCCEEDED(hr) && !raw.empty())
    graphics->CreateGeometryGroup(D2D1_FILL_MODE_WINDING, raw.data(),
                                  (UINT32)raw.size(), &glyphs);
  renderer->Release();
  return glyphs;
}
struct Shape {
  ComPtr<ID2D1Geometry> glyphs;
  ComPtr<ID2D1RoundedRectangleGeometry> pill;
  D2D1_RECT_F textBounds{};
};
Shape Build(ID2D1Factory *graphics, IDWriteFactory *text,
            const std::wstring &label, float width, float height) {
  Shape result;
  ComPtr<IDWriteTextFormat> font;
  if (FAILED(text->CreateTextFormat(
          L"Segoe UI Variable", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
          DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.5f, L"zh-CN",
          &font)))
    return result;
  font->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  ComPtr<IDWriteTextLayout> layout;
  if (FAILED(text->CreateTextLayout(label.c_str(), (UINT32)label.size(),
                                    font.Get(), width, height, &layout)))
    return result;
  auto glyphs = LayoutOutlines(graphics, layout.Get());
  if (!glyphs)
    return result;
  D2D1_RECT_F ink{};
  if (FAILED(glyphs->GetBounds(nullptr, &ink)))
    return result;
  // Center the actual glyph ink, including fallback CJK faces, rather than
  // the line box's asymmetric ascent/descent and baseline padding.
  float dx = (width - ink.left - ink.right) / 2,
        dy = (height - ink.top - ink.bottom) / 2;
  auto transform = D2D1::Matrix3x2F::Translation(dx, dy);
  result.textBounds = {ink.left + dx, ink.top + dy, ink.right + dx,
                       ink.bottom + dy};
  ComPtr<ID2D1TransformedGeometry> centered;
  if (FAILED(graphics->CreateTransformedGeometry(glyphs.Get(), transform,
                                                 &centered)))
    return {};
  if (FAILED(graphics->CreateRoundedRectangleGeometry(
          D2D1::RoundedRect(D2D1::RectF(0, 0, width, height), height / 2,
                            height / 2),
          &result.pill)))
    return {};
  result.glyphs = centered;
  return result;
}
} // namespace
bool TextInkBounds(ID2D1Factory *graphics, IDWriteTextLayout *layout,
                   D2D1_RECT_F &bounds) {
  auto glyphs = LayoutOutlines(graphics, layout);
  return glyphs && SUCCEEDED(glyphs->GetBounds(nullptr, &bounds)) &&
         bounds.right > bounds.left && bounds.bottom > bounds.top;
}
float CenteredTextOriginY(ID2D1Factory *graphics, IDWriteTextLayout *layout,
                          float height) {
  D2D1_RECT_F ink{};
  if (TextInkBounds(graphics, layout, ink))
    return (height - ink.top - ink.bottom) / 2;
  DWRITE_TEXT_METRICS metrics{};
  if (SUCCEEDED(layout->GetMetrics(&metrics)))
    return (height - metrics.height) / 2 - metrics.top;
  return 0;
}
bool DrawStatusBadge(ID2D1RenderTarget *target, ID2D1Factory *graphics,
                     IDWriteFactory *text, const std::wstring &label,
                     const D2D1_RECT_F &bounds, D2D1_COLOR_F fill,
                     D2D1_COLOR_F ink) {
  struct Entry {
    ID2D1Factory *graphics;
    IDWriteFactory *text;
    std::wstring label;
    float width, height;
    Shape shape;
  };
  static std::deque<Entry> cache;
  const float width = bounds.right - bounds.left,
              height = bounds.bottom - bounds.top;
  Shape shape;
  for (const auto &entry : cache)
    if (entry.graphics == graphics && entry.text == text &&
        entry.label == label && entry.width == width &&
        entry.height == height) {
      shape = entry.shape;
      break;
    }
  if (!shape.glyphs) {
    shape = Build(graphics, text, label, width, height);
    if (shape.glyphs) {
      cache.push_front({graphics, text, label, width, height, shape});
      if (cache.size() > 32)
        cache.pop_back();
    }
  }
  if (!shape.glyphs || !shape.pill)
    return false;
  ComPtr<ID2D1SolidColorBrush> brush;
  if (FAILED(target->CreateSolidColorBrush(fill, &brush)))
    return false;
  D2D1_MATRIX_3X2_F previous;
  target->GetTransform(&previous);
  target->SetTransform(D2D1::Matrix3x2F::Translation(bounds.left, bounds.top) *
                       previous);
  target->FillGeometry(shape.pill.Get(), brush.Get());
  brush->SetColor(ink);
  target->FillGeometry(shape.glyphs.Get(), brush.Get());
  target->SetTransform(previous);
  return true;
}
bool StatusBadgeTests() {
  ComPtr<ID2D1Factory> graphics;
  ComPtr<IDWriteFactory> text;
  if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                               graphics.GetAddressOf())) ||
      FAILED(DWriteCreateFactory(
          DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
          reinterpret_cast<IUnknown **>(text.GetAddressOf()))))
    return false;
  auto shape = Build(graphics.Get(), text.Get(), L"等待回答", 64, 22);
  if (!shape.glyphs || !shape.pill)
    return false;
  float whole = 0, glyphArea = 0;
  shape.pill->ComputeArea(nullptr, &whole);
  shape.glyphs->ComputeArea(nullptr, &glyphArea);
  if (!(glyphArea > 10 && glyphArea < whole))
    return false;
  if (std::abs((shape.textBounds.left + shape.textBounds.right) / 2 - 32) >=
          .01f ||
      std::abs((shape.textBounds.top + shape.textBounds.bottom) / 2 - 11) >=
          .01f)
    return false;
  // Exercise actual CJK/Latin fallback and the trimming inline object used by
  // session titles. A trimmed label must still yield visible, bounded ink.
  ComPtr<IDWriteTextFormat> font;
  if (FAILED(text->CreateTextFormat(
          L"Segoe UI Variable", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
          DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12, L"zh-CN",
          &font)))
    return false;
  font->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  ComPtr<IDWriteInlineObject> ellipsis;
  if (FAILED(text->CreateEllipsisTrimmingSign(font.Get(), &ellipsis)))
    return false;
  DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
  font->SetTrimming(&trim, ellipsis.Get());
  for (const auto &label : {L"桌宠状态提示更新", L"Casual Greeting Check-In",
                            L"chat 同步到远端版本", L"gyp 43m"}) {
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(text->CreateTextLayout(label, (UINT32)wcslen(label), font.Get(),
                                      80, 34, &layout)))
      return false;
    D2D1_RECT_F ink{};
    if (!TextInkBounds(graphics.Get(), layout.Get(), ink) || ink.right > 80.5f)
      return false;
    float offset = CenteredTextOriginY(graphics.Get(), layout.Get(), 34);
    if (ink.top + offset < 0 || ink.bottom + offset > 34 ||
        std::abs(ink.top + offset - (34 - ink.bottom - offset)) > .01f)
      return false;
  }
  return true;
}
