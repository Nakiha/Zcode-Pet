#include "spine_pet.h"
#include "composition_device.h"
#include "software_raster.h"
#include "pet_canvas.h"
#include <spine/spine.h>
#include <spine/SkeletonRenderer.h>
#include <d3dcompiler.h>
#include <d3d11_1.h>
#include <dxgi1_3.h>
#include <wincodec.h>
#include <windows.ui.composition.interop.h>
#include <winrt/Windows.UI.Composition.Desktop.h>
#include <winrt/Windows.Foundation.h>
#include <fstream>
#include <cmath>
#include <algorithm>
#include <array>
#include <vector>
#include <map>
using Microsoft::WRL::ComPtr;
using namespace winrt;
using namespace winrt::Windows::UI::Composition;
namespace Interop = ABI::Windows::UI::Composition;
namespace fs = std::filesystem;
namespace {
fs::path WindowsPath(const char *path) {
  return fs::path(std::u8string_view(reinterpret_cast<const char8_t *>(path)));
}
std::string Utf8(const fs::path &path) {
  const auto s = path.u8string();
  return {reinterpret_cast<const char *>(s.data()), s.size()};
}
struct WindowsSpineExtension : spine::DefaultSpineExtension {
  char *_readFile(const spine::String &name, int *length) override {
    std::ifstream input(WindowsPath(name.buffer()),
                        std::ios::binary | std::ios::ate);
    if (!input) {
      *length = 0;
      return nullptr;
    }
    const auto size = input.tellg();
    if (size < 0 || size > 64 * 1024 * 1024) {
      *length = 0;
      return nullptr;
    }
    *length = (int)size;
    auto data = (char *)_alloc((size_t)*length + 1, __FILE__, __LINE__);
    input.seekg(0);
    input.read(data, *length);
    data[*length] = 0;
    return data;
  }
};
struct Texture : SoftwareTexture {
  ComPtr<ID3D11ShaderResourceView> view;
};
struct TextureLoader : spine::TextureLoader {
  ComPtr<IWICImagingFactory> wic;
  ID3D11Device *device;
  explicit TextureLoader(ID3D11Device *graphics) : device(graphics) {
    check_hresult(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                   CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)));
  }
  void load(spine::AtlasPage &page, const spine::String &path) override {
    ComPtr<IWICBitmapDecoder> decoder;
    check_hresult(wic->CreateDecoderFromFilename(
        WindowsPath(path.buffer()).c_str(), nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnLoad, &decoder));
    ComPtr<IWICBitmapFrameDecode> frame;
    check_hresult(decoder->GetFrame(0, &frame));
    ComPtr<IWICFormatConverter> converter;
    check_hresult(wic->CreateFormatConverter(&converter));
    check_hresult(converter->Initialize(
        frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
        nullptr, 0, WICBitmapPaletteTypeCustom));
    UINT width = 0, height = 0;
    check_hresult(frame->GetSize(&width, &height));
    if (!width || !height || width > 8192 || height > 8192)
      throw_hresult(E_INVALIDARG);
    std::vector<BYTE> pixels((size_t)width * height * 4);
    check_hresult(converter->CopyPixels(nullptr, width * 4, (UINT)pixels.size(),
                                        pixels.data()));
    if (page.pma)
      for (size_t i = 0; i < pixels.size(); i += 4) {
        const auto a = pixels[i + 3];
        if (a)
          for (int k = 0; k < 3; ++k)
            pixels[i + k] = (BYTE)std::min(255, (int)pixels[i + k] * 255 / a);
      }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA source{pixels.data(), width * 4, 0};
    ComPtr<ID3D11Texture2D> texture;
    auto resource = std::make_unique<Texture>();
    resource->width = (int)width;
    resource->height = (int)height;
    resource->rgba.resize((size_t)width * height);
    memcpy(resource->rgba.data(), pixels.data(), pixels.size());
    if (device) {
      check_hresult(device->CreateTexture2D(&desc, &source, &texture));
      check_hresult(device->CreateShaderResourceView(texture.Get(), nullptr,
                                                     &resource->view));
    }
    page.width = (int)width;
    page.height = (int)height;
    page.texture = resource.release();
  }
  void unload(void *texture) override {
    delete static_cast<Texture *>(texture);
  }
};
constexpr char Shader[] = R"(
Texture2D image:register(t0); SamplerState sampleImage:register(s0);
struct V { float2 p:POSITION;float2 uv:TEXCOORD0;float4 light:COLOR0;float4 dark:COLOR1; };
struct P { float4 p:SV_POSITION;float2 uv:TEXCOORD0;float4 light:COLOR0;float4 dark:COLOR1; };
P vs(V v){ P o;o.p=float4(v.p,0,1);o.uv=v.uv;o.light=v.light;o.dark=v.dark;return o; }
float4 ps(P p):SV_TARGET {float4 t=image.Sample(sampleImage,p.uv);float a=t.a*p.light.a;
 return float4((t.rgb*p.light.rgb+(1-t.rgb)*p.dark.rgb)*a,a);}
)";
struct Vertex {
  float x, y, u, v;
  std::array<float, 4> light, dark;
};
struct PipelineScope {
  ID3D11DeviceContext1 *context;
  ComPtr<ID3DDeviceContextState> previous;
  PipelineScope(ID3D11DeviceContext1 *ctx, ID3DDeviceContextState *state)
      : context(ctx) {
    context->SwapDeviceContextState(state, &previous);
  }
  ~PipelineScope() { context->SwapDeviceContextState(previous.Get(), nullptr); }
};
std::array<float, 4> Color(uint32_t c) {
  return {(float)((c >> 16) & 255) / 255, (float)((c >> 8) & 255) / 255,
          (float)(c & 255) / 255, (float)(c >> 24) / 255};
}
} // namespace
spine::SpineExtension *spine::getDefaultExtension() {
  return new WindowsSpineExtension();
}
struct SpinePet::Impl {
  bool ready = false;
  bool hardware = false;
  HWND window = nullptr;
  PetCanvasBuffer canvas;
  HRESULT result = S_OK;
  uint64_t frames = 0, changes = 0, errors = 0;
  int vertices = 0, batches = 0, side = 0;
  std::string clip;
  std::map<std::wstring, std::string> aliases;
  int fps = 30, mixFps = 60;
  std::unique_ptr<TextureLoader> loader;
  std::unique_ptr<spine::Atlas> atlas;
  std::unique_ptr<spine::SkeletonData> data;
  std::unique_ptr<spine::Skeleton> skeleton;
  std::unique_ptr<spine::AnimationStateData> mixes;
  std::unique_ptr<spine::AnimationState> state;
  spine::SkeletonRenderer renderer;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<ID3D11DeviceContext1> context1;
  ComPtr<ID3DDeviceContextState> pipelineState;
  ComPtr<IDXGISwapChain1> swap;
  ComPtr<ID3D11RenderTargetView> target;
  ComPtr<ID3D11Texture2D> backBuffer;
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps;
  ComPtr<ID3D11InputLayout> layout;
  ComPtr<ID3D11Buffer> vb, ib;
  size_t vertexCapacity = 0, indexCapacity = 0;
  ComPtr<ID3D11SamplerState> sampler;
  ComPtr<ID3D11RasterizerState> raster;
  ComPtr<ID3D11DepthStencilState> depth;
  std::array<ComPtr<ID3D11BlendState>, 4> blend;
  Desktop::DesktopWindowTarget host{nullptr};
  SpriteVisual visual{nullptr};
  Compositor compositor{nullptr};
  void Resize(int pixels) {
    context->OMSetRenderTargets(0, nullptr, nullptr);
    target.Reset();
    backBuffer.Reset();
    if (swap)
      check_hresult(
          swap->ResizeBuffers(0, pixels, pixels, DXGI_FORMAT_UNKNOWN, 0));
    else {
      ComPtr<IDXGIDevice> dxgi;
      check_hresult(device.As(&dxgi));
      ComPtr<IDXGIAdapter> adapter;
      check_hresult(dxgi->GetAdapter(&adapter));
      ComPtr<IDXGIFactory2> factory;
      check_hresult(adapter->GetParent(IID_PPV_ARGS(&factory)));
      DXGI_SWAP_CHAIN_DESC1 desc{};
      desc.Width = desc.Height = pixels;
      desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
      desc.SampleDesc.Count = 1;
      desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
      desc.BufferCount = 2;
      desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
      desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
      check_hresult(factory->CreateSwapChainForComposition(device.Get(), &desc,
                                                           nullptr, &swap));
      winrt::Windows::UI::Composition::ICompositionSurface surface{nullptr};
      check_hresult(
          compositor.as<Interop::ICompositorInterop>()
              ->CreateCompositionSurfaceForSwapChain(
                  swap.Get(), reinterpret_cast<Interop::ICompositionSurface **>(
                                  put_abi(surface))));
      auto brush = compositor.CreateSurfaceBrush(surface);
      brush.Stretch(CompositionStretch::Fill);
      visual.Brush(brush);
      ComPtr<IDXGISwapChain2> limit;
      if (SUCCEEDED(swap.As(&limit)))
        limit->SetMaximumFrameLatency(1);
    }
    check_hresult(swap->GetBuffer(0, IID_PPV_ARGS(&backBuffer)));
    check_hresult(
        device->CreateRenderTargetView(backBuffer.Get(), nullptr, &target));
    visual.Size({(float)pixels, (float)pixels});
    side = pixels;
  }
  void Buffers(size_t v, size_t i) {
    auto ensure = [&](ComPtr<ID3D11Buffer> &buffer, size_t &capacity,
                      size_t count, size_t stride, UINT bind) {
      if (count <= capacity)
        return;
      capacity = std::max((size_t)4096, count * 2);
      buffer.Reset();
      D3D11_BUFFER_DESC desc{};
      desc.ByteWidth = (UINT)(capacity * stride);
      desc.Usage = D3D11_USAGE_DYNAMIC;
      desc.BindFlags = bind;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
      check_hresult(device->CreateBuffer(&desc, nullptr, &buffer));
    };
    ensure(vb, vertexCapacity, v, sizeof(Vertex), D3D11_BIND_VERTEX_BUFFER);
    ensure(ib, indexCapacity, i, sizeof(uint16_t), D3D11_BIND_INDEX_BUFFER);
  }
  void Draw(float delta, int pixels, const fs::path *capture = nullptr) {
    if (!hardware) {
      SoftwareDraw(delta, pixels, capture);
      return;
    }
    check_hresult(device->GetDeviceRemovedReason());
    PipelineScope pipeline(context1.Get(), pipelineState.Get());
    if (side != pixels)
      Resize(pixels);
    const float dt = std::clamp(delta, 0.f, .1f);
    skeleton->update(dt);
    state->update(dt);
    state->apply(*skeleton);
    skeleton->updateWorldTransform(spine::Physics_Update);
    auto commands = renderer.render(*skeleton);
    size_t nv = 0, ni = 0;
    for (auto c = commands; c; c = c->next) {
      nv += c->numVertices;
      ni += c->numIndices;
    }
    if (!nv || !ni)
      throw_hresult(E_FAIL);
    Buffers(nv, ni);
    D3D11_MAPPED_SUBRESOURCE mappedVertices{}, mappedIndices{};
    check_hresult(
        context->Map(vb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedVertices));
    check_hresult(
        context->Map(ib.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedIndices));
    auto vertex = (Vertex *)mappedVertices.pData;
    auto index = (uint16_t *)mappedIndices.pData;
    const float fit =
                    .9f / (std::max(data->getWidth(), data->getHeight()) * .5f),
                cx = data->getX() + data->getWidth() / 2,
                cy = data->getY() + data->getHeight() / 2;
    for (auto c = commands; c; c = c->next) {
      for (int i = 0; i < c->numVertices; ++i)
        *vertex++ = {(c->positions[i * 2] - cx) * fit,
                     (c->positions[i * 2 + 1] - cy) * fit,
                     c->uvs[i * 2],
                     c->uvs[i * 2 + 1],
                     Color(c->colors[i]),
                     Color(c->darkColors[i])};
      memcpy(index, c->indices, (size_t)c->numIndices * sizeof(uint16_t));
      index += c->numIndices;
    }
    context->Unmap(vb.Get(), 0);
    context->Unmap(ib.Get(), 0);
    const float clear[4]{};
    context->ClearRenderTargetView(target.Get(), clear);
    context->OMSetRenderTargets(1, target.GetAddressOf(), nullptr);
    context->OMSetDepthStencilState(depth.Get(), 0);
    D3D11_VIEWPORT viewport{0, 0, (float)pixels, (float)pixels, 0, 1};
    context->RSSetViewports(1, &viewport);
    context->RSSetState(raster.Get());
    context->IASetInputLayout(layout.Get());
    UINT stride = sizeof(Vertex), offset = 0;
    context->IASetVertexBuffers(0, 1, vb.GetAddressOf(), &stride, &offset);
    context->IASetIndexBuffer(ib.Get(), DXGI_FORMAT_R16_UINT, 0);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vs.Get(), nullptr, 0);
    context->PSSetShader(ps.Get(), nullptr, 0);
    context->PSSetSamplers(0, 1, sampler.GetAddressOf());
    UINT startIndex = 0;
    INT baseVertex = 0;
    batches = 0;
    for (auto c = commands; c; c = c->next) {
      auto texture = static_cast<Texture *>(c->texture);
      if (texture) {
        context->PSSetShaderResources(0, 1, texture->view.GetAddressOf());
        context->OMSetBlendState(blend[(int)c->blendMode].Get(), nullptr,
                                 0xffffffff);
        context->DrawIndexed(c->numIndices, startIndex, baseVertex);
        ++batches;
      }
      startIndex += c->numIndices;
      baseVertex += c->numVertices;
    }
    ID3D11ShaderResourceView *none = nullptr;
    context->PSSetShaderResources(0, 1, &none);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    if (capture)
      Save(*capture);
    const auto hr = swap->Present(0, DXGI_PRESENT_DO_NOT_WAIT);
    if (hr != DXGI_ERROR_WAS_STILL_DRAWING)
      check_hresult(hr);
    vertices = (int)nv;
    ++frames;
  }
  void Save(const fs::path &path) {
    D3D11_TEXTURE2D_DESC desc{};
    backBuffer->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    check_hresult(device->CreateTexture2D(&desc, nullptr, &staging));
    context->CopyResource(staging.Get(), backBuffer.Get());
    D3D11_MAPPED_SUBRESOURCE source{};
    check_hresult(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &source));
    std::vector<BYTE> pixels((size_t)side * side * 4);
    for (int y = 0; y < side; ++y)
      for (int x = 0; x < side; ++x) {
        const auto from = (BYTE *)source.pData + y * source.RowPitch + x * 4;
        auto to = pixels.data() + ((size_t)y * side + x) * 4;
        to[3] = from[3];
        for (int k = 0; k < 3; ++k)
          to[k] =
              from[3] ? (BYTE)std::min(255, (int)from[k] * 255 / from[3]) : 0;
      }
    context->Unmap(staging.Get(), 0);
    SaveStraight(pixels, path);
  }
  void SaveStraight(const std::vector<BYTE> &pixels, const fs::path &path) {
    ComPtr<IWICStream> stream;
    check_hresult(loader->wic->CreateStream(&stream));
    check_hresult(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE));
    ComPtr<IWICBitmapEncoder> encoder;
    check_hresult(
        loader->wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder));
    check_hresult(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
    ComPtr<IWICBitmapFrameEncode> frame;
    check_hresult(encoder->CreateNewFrame(&frame, nullptr));
    check_hresult(frame->Initialize(nullptr));
    check_hresult(frame->SetSize(side, side));
    auto format = GUID_WICPixelFormat32bppBGRA;
    check_hresult(frame->SetPixelFormat(&format));
    check_hresult(frame->WritePixels(side, side * 4, (UINT)pixels.size(),
                                     const_cast<BYTE *>(pixels.data())));
    check_hresult(frame->Commit());
    check_hresult(encoder->Commit());
  }
  void SoftwareDraw(float delta, int pixels, const fs::path *capture) {
    HDC screen = GetDC(nullptr);
    const bool valid = canvas.Ensure(screen, pixels);
    ReleaseDC(nullptr, screen);
    if (!valid)
      throw_hresult(E_OUTOFMEMORY);
    side = pixels;
    const float dt = std::clamp(delta, 0.f, .1f);
    skeleton->update(dt);
    state->update(dt);
    state->apply(*skeleton);
    skeleton->updateWorldTransform(spine::Physics_Update);
    auto commands = renderer.render(*skeleton);
    auto output = (uint32_t *)canvas.pixels;
    memset(output, 0, (size_t)side * side * 4);
    const float fit =
                    .9f * side / std::max(data->getWidth(), data->getHeight()),
                cx = data->getX() + data->getWidth() / 2,
                cy = data->getY() + data->getHeight() / 2;
    vertices = batches = 0;
    for (auto command = commands; command; command = command->next) {
      const auto texture = static_cast<Texture *>(command->texture);
      if (!texture)
        continue;
      ++batches;
      vertices += command->numVertices;
      for (int i = 0; i + 2 < command->numIndices; i += 3) {
        auto vertex = [&](uint16_t index) {
          return RasterVertex{
              side / 2.f + (command->positions[index * 2] - cx) * fit,
              side / 2.f - (command->positions[index * 2 + 1] - cy) * fit,
              command->uvs[index * 2], command->uvs[index * 2 + 1]};
        };
        RasterTriangle(
            vertex(command->indices[i]), vertex(command->indices[i + 1]),
            vertex(command->indices[i + 2]), *texture, command->colors[0],
            command->darkColors[0], (int)command->blendMode, output, side);
      }
    }
    if (capture) {
      std::vector<BYTE> captured((size_t)side * side * 4);
      for (size_t i = 0; i < (size_t)side * side; ++i) {
        const auto from = (BYTE *)output + i * 4;
        auto to = captured.data() + i * 4;
        to[3] = from[3];
        for (int k = 0; k < 3; ++k)
          to[k] =
              from[3] ? (BYTE)std::min(255, (int)from[k] * 255 / from[3]) : 0;
      }
      SaveStraight(captured, *capture);
    }
    KeepRectangularPetHitArea(output, (size_t)side * side);
    POINT origin{};
    SIZE size{side, side};
    BLENDFUNCTION layerBlend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    check_bool(UpdateLayeredWindow(window, nullptr, nullptr, &size, canvas.dc,
                                   &origin, 0, &layerBlend, ULW_ALPHA));
    ++frames;
  }
};
SpinePet::SpinePet() : impl(std::make_unique<Impl>()) {}
SpinePet::~SpinePet() { Shutdown(); }
bool SpinePet::Initialize(HWND window, const fs::path &folder,
                          bool hardware) noexcept {
  auto &s = *impl;
  try {
    s.hardware = hardware;
    s.window = window;
    if (hardware) {
      auto &shared = SharedCompositionDevice();
      s.device = shared.d3d;
      s.compositor = shared.compositor;
      s.device->GetImmediateContext(&s.context);
      check_hresult(s.context.As(&s.context1));
      ComPtr<ID3D11Device1> device1;
      check_hresult(s.device.As(&device1));
      const auto level = s.device->GetFeatureLevel();
      check_hresult(device1->CreateDeviceContextState(
          0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), nullptr,
          &s.pipelineState));
    } else {
      auto style = GetWindowLongPtrW(window, GWL_EXSTYLE);
      SetWindowLongPtrW(window, GWL_EXSTYLE,
                        (style & ~WS_EX_NOREDIRECTIONBITMAP) | WS_EX_LAYERED);
      SetWindowPos(window, nullptr, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                       SWP_FRAMECHANGED);
    }
    s.loader = std::make_unique<TextureLoader>(s.device.Get());
    s.atlas = std::make_unique<spine::Atlas>(
        Utf8(folder / L"leimi.atlas").c_str(), s.loader.get());
    spine::SkeletonJson parser(s.atlas.get());
    s.data.reset(
        parser.readSkeletonDataFile(Utf8(folder / L"remilia.json").c_str()));
    if (!s.data)
      throw_hresult(E_INVALIDARG);
    s.skeleton = std::make_unique<spine::Skeleton>(s.data.get());
    s.mixes = std::make_unique<spine::AnimationStateData>(s.data.get());
    s.mixes->setDefaultMix(.22f);
    s.state = std::make_unique<spine::AnimationState>(s.mixes.get());
    const auto config = folder / L"animations.ini";
    const std::pair<const wchar_t *, const wchar_t *> defaults[] = {
        {L"idle", L"a"},    {L"working", L"d"}, {L"waiting", L"e"},
        {L"success", L"c"}, {L"paused", L"b"},  {L"thinking", L"b"}};
    for (const auto &[logical, original] : defaults) {
      wchar_t value[128]{};
      GetPrivateProfileStringW(L"animations", logical, original, value, 128,
                               config.c_str());
      const auto native = fs::path(value).u8string();
      std::string name(reinterpret_cast<const char *>(native.data()),
                       native.size());
      s.aliases[logical] =
          s.data->findAnimation(name.c_str()) ? name : Utf8(fs::path(original));
    }
    wchar_t mix[32]{};
    GetPrivateProfileStringW(L"mixing", L"seconds", L"0.22", mix, 32,
                             config.c_str());
    s.mixes->setDefaultMix(std::clamp(wcstof(mix, nullptr), 0.f, 1.f));
    s.fps = std::clamp(
        (int)GetPrivateProfileIntW(L"playback", L"fps", 30, config.c_str()), 15,
        60);
    s.mixFps = std::clamp(
        (int)GetPrivateProfileIntW(L"playback", L"mix_fps", 60, config.c_str()),
        30, 120);
    if (hardware) {
      ComPtr<ID3DBlob> vs, ps, error;
      check_hresult(D3DCompile(Shader, sizeof(Shader), nullptr, nullptr,
                               nullptr, "vs", "vs_4_0",
                               D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs, &error));
      check_hresult(D3DCompile(Shader, sizeof(Shader), nullptr, nullptr,
                               nullptr, "ps", "ps_4_0",
                               D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps, &error));
      check_hresult(s.device->CreateVertexShader(
          vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &s.vs));
      check_hresult(s.device->CreatePixelShader(
          ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &s.ps));
      D3D11_INPUT_ELEMENT_DESC fields[] = {
          {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
           D3D11_INPUT_PER_VERTEX_DATA, 0},
          {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,
           D3D11_INPUT_PER_VERTEX_DATA, 0},
          {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16,
           D3D11_INPUT_PER_VERTEX_DATA, 0},
          {"COLOR", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32,
           D3D11_INPUT_PER_VERTEX_DATA, 0}};
      check_hresult(s.device->CreateInputLayout(
          fields, 4, vs->GetBufferPointer(), vs->GetBufferSize(), &s.layout));
      D3D11_SAMPLER_DESC sample{};
      sample.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
      sample.AddressU = sample.AddressV = sample.AddressW =
          D3D11_TEXTURE_ADDRESS_CLAMP;
      sample.MaxLOD = D3D11_FLOAT32_MAX;
      check_hresult(s.device->CreateSamplerState(&sample, &s.sampler));
      D3D11_RASTERIZER_DESC raster{};
      raster.FillMode = D3D11_FILL_SOLID;
      raster.CullMode = D3D11_CULL_NONE;
      raster.DepthClipEnable = TRUE;
      check_hresult(s.device->CreateRasterizerState(&raster, &s.raster));
      D3D11_DEPTH_STENCIL_DESC depth{};
      depth.DepthEnable = FALSE;
      check_hresult(s.device->CreateDepthStencilState(&depth, &s.depth));
      const D3D11_BLEND sources[] = {D3D11_BLEND_ONE, D3D11_BLEND_ONE,
                                     D3D11_BLEND_DEST_COLOR, D3D11_BLEND_ONE};
      const D3D11_BLEND destinations[] = {
          D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA,
          D3D11_BLEND_INV_SRC_COLOR};
      for (int i = 0; i < 4; ++i) {
        D3D11_BLEND_DESC blend{};
        auto &b = blend.RenderTarget[0];
        b.BlendEnable = TRUE;
        b.SrcBlend = sources[i];
        b.DestBlend = destinations[i];
        b.BlendOp = D3D11_BLEND_OP_ADD;
        b.SrcBlendAlpha = D3D11_BLEND_ONE;
        b.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        b.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        b.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        check_hresult(s.device->CreateBlendState(&blend, &s.blend[i]));
      }
      check_hresult(
          s.compositor.as<Interop::Desktop::ICompositorDesktopInterop>()
              ->CreateDesktopWindowTarget(
                  window, TRUE,
                  reinterpret_cast<Interop::Desktop::IDesktopWindowTarget **>(
                      put_abi(s.host))));
      s.visual = s.compositor.CreateSpriteVisual();
      s.host.Root(s.visual);
    }
    s.ready = true;
    s.result = S_OK;
    return SetClip("a");
  } catch (...) {
    s.result = to_hresult();
    ++s.errors;
    Shutdown();
    return false;
  }
}
bool SpinePet::Ready() const noexcept { return impl->ready; }
bool SpinePet::SetClip(const std::string &name, bool loop) noexcept {
  auto &s = *impl;
  if (!s.ready)
    return false;
  if (s.clip == name)
    return true;
  try {
    if (!s.data->findAnimation(name.c_str()))
      return false;
    auto entry = s.state->setAnimation(0, name.c_str(), loop);
    entry->setMixAttachmentThreshold(.5f);
    entry->setMixDrawOrderThreshold(.5f);
    s.clip = name;
    ++s.changes;
    return true;
  } catch (...) {
    s.result = to_hresult();
    ++s.errors;
    return false;
  }
}
bool SpinePet::Render(float delta, int side) noexcept {
  if (!impl->ready)
    return false;
  try {
    impl->Draw(delta, side);
    return true;
  } catch (...) {
    impl->result = to_hresult();
    ++impl->errors;
    return false;
  }
}
bool SpinePet::SetState(const std::wstring &logical) noexcept {
  const auto found = impl->aliases.find(logical);
  return SetClip(found == impl->aliases.end() ? "a" : found->second);
}
int SpinePet::DesiredFps() const noexcept {
  const auto &s = *impl;
  auto entry = s.state ? s.state->getCurrent(0) : nullptr;
  return entry && entry->getMixingFrom() &&
                 entry->getMixTime() < entry->getMixDuration()
             ? s.mixFps
             : s.fps;
}
bool SpinePet::Snapshot(const fs::path &path, int side) noexcept {
  try {
    impl->Draw(0, side, &path);
    return true;
  } catch (...) {
    impl->result = to_hresult();
    ++impl->errors;
    return false;
  }
}
std::vector<std::string> SpinePet::Clips() const {
  std::vector<std::string> names;
  if (impl->data) {
    auto &animations = impl->data->getAnimations();
    for (size_t i = 0; i < animations.size(); ++i)
      names.emplace_back(animations[i]->getName().buffer());
  }
  return names;
}
std::string SpinePet::CurrentClip() const { return impl->clip; }
void SpinePet::Shutdown() noexcept {
  auto &s = *impl;
  s.ready = false;
  try {
    if (s.host) {
      s.host.Root(nullptr);
      s.host.Close();
    }
    s.host = nullptr;
    s.visual = nullptr;
  } catch (...) {
  }
  s.target.Reset();
  s.backBuffer.Reset();
  s.swap.Reset();
  if (s.context1 && s.pipelineState) {
    PipelineScope pipeline(s.context1.Get(), s.pipelineState.Get());
    s.context->ClearState();
  }
  s.state.reset();
  s.mixes.reset();
  s.skeleton.reset();
  s.data.reset();
  s.atlas.reset();
  s.loader.reset();
  s.context.Reset();
  s.device.Reset();
  s.compositor = nullptr;
  s.context1.Reset();
  s.pipelineState.Reset();
  s.vs.Reset();
  s.ps.Reset();
  s.layout.Reset();
  s.vb.Reset();
  s.ib.Reset();
  s.sampler.Reset();
  s.raster.Reset();
  s.depth.Reset();
  for (auto &b : s.blend)
    b.Reset();
  s.vertexCapacity = s.indexCapacity = 0;
  s.side = 0;
  s.clip.clear();
  s.canvas.Reset();
}
LRESULT SpinePet::Metric(unsigned f) const noexcept {
  const auto &s = *impl;
  return f == 0   ? s.ready
         : f == 1 ? s.result
         : f == 2 ? (LRESULT)s.frames
         : f == 3 ? s.vertices
         : f == 4 ? s.batches
         : f == 5 ? (LRESULT)s.changes
         : f == 6 ? (LRESULT)s.errors
                  : s.ready && s.hardware;
}
bool RenderSpinePreview(const fs::path &folder, const fs::path &output,
                        bool hardware) {
  const auto hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(hr))
    return false;
  WNDCLASSW type{};
  type.lpfnWndProc = DefWindowProcW;
  type.hInstance = GetModuleHandleW(nullptr);
  type.lpszClassName = L"ZCodePet.SpinePreview";
  RegisterClassW(&type);
  auto window = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW,
                                type.lpszClassName, L"", WS_POPUP, 0, 0, 512,
                                512, nullptr, nullptr, type.hInstance, nullptr);
  SpinePet pet;
  bool okay = pet.Initialize(window, folder, hardware);
  fs::create_directories(output);
  for (const auto &name : pet.Clips()) {
    okay &= pet.SetClip(name);
    for (int i = 0; i < 45; ++i)
      okay &= pet.Render(1.f / 30, 512);
    okay &= pet.Snapshot(output / (name + ".png"), 512);
  }
  pet.Shutdown();
  DestroyWindow(window);
  FinishCompositionThread();
  CoUninitialize();
  return okay;
}
bool SpineModelTests(const fs::path &folder) {
  struct TestTextures : spine::TextureLoader {
    void load(spine::AtlasPage &page, const spine::String &) override {
      page.texture = &page;
    }
    void unload(void *) override {}
  } textures;
  try {
    spine::Atlas atlas(Utf8(folder / L"leimi.atlas").c_str(), &textures);
    spine::SkeletonJson parser(&atlas);
    std::unique_ptr<spine::SkeletonData> data(
        parser.readSkeletonDataFile(Utf8(folder / L"remilia.json").c_str()));
    if (!data || data->getAnimations().size() < 5)
      return false;
    spine::Skeleton skeleton(data.get());
    spine::AnimationStateData mixes(data.get());
    mixes.setDefaultMix(.22f);
    spine::AnimationState state(&mixes);
    spine::SkeletonRenderer renderer;
    for (const auto name : {"a", "d", "e", "c", "b"}) {
      if (!data->findAnimation(name))
        return false;
      auto entry = state.setAnimation(0, name, true);
      if (name != std::string("a") &&
          (!entry->getMixingFrom() ||
           std::abs(entry->getMixDuration() - .22f) > .001f))
        return false;
      for (int i = 0; i < 20; ++i) {
        skeleton.update(1.f / 60);
        state.update(1.f / 60);
        state.apply(skeleton);
        skeleton.updateWorldTransform(spine::Physics_Update);
        auto commands = renderer.render(skeleton);
        int count = 0;
        for (auto c = commands; c; c = c->next)
          for (int k = 0; k < c->numVertices * 2; ++k) {
            if (!std::isfinite(c->positions[k]))
              return false;
            ++count;
          }
        if (count < 100)
          return false;
      }
    }
    std::unique_ptr<spine::SkeletonData> missing(
        parser.readSkeletonDataFile(Utf8(folder / L"missing.json").c_str()));
    return !missing;
  } catch (...) {
    return false;
  }
}
bool SpineLifecycleTests(const fs::path &folder) {
  const auto hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(hr))
    return false;
  WNDCLASSW type{};
  type.lpfnWndProc = DefWindowProcW;
  type.hInstance = GetModuleHandleW(nullptr);
  type.lpszClassName = L"ZCodePet.SpineLifecycle";
  RegisterClassW(&type);
  auto window = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW,
                                type.lpszClassName, L"", WS_POPUP, 0, 0, 224,
                                224, nullptr, nullptr, type.hInstance, nullptr);
  bool okay = window != nullptr;
  SpinePet pet;
  DWORD baseline = 0;
  for (int cycle = 0; okay && cycle < 8; ++cycle) {
    okay = pet.Initialize(window, folder, false) && pet.DesiredFps() == 30;
    // An animation that has never been applied has no visible source pose to
    // mix from. Match the app startup: draw idle before changing state.
    okay &= pet.Render(0, 224);
    okay &= pet.SetState(L"working") && pet.DesiredFps() == 60;
    for (int frame = 0; frame < 6 && okay; ++frame)
      okay &= pet.Render(.05f, 224);
    okay &= pet.DesiredFps() == 30 && pet.Metric(6) == 0;
    pet.Shutdown();
    const auto resources = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    if (cycle == 0)
      baseline = resources;
    else
      okay &= resources <= baseline + 1;
  }
  okay &= !pet.Initialize(window, folder / L"missing", false) && !pet.Ready();
  pet.Shutdown();
  if (window)
    DestroyWindow(window);
  CoUninitialize();
  return okay;
}
