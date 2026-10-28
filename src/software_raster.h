#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

struct SoftwareTexture {
  int width = 0, height = 0;
  std::vector<uint32_t> rgba;
};
struct RasterVertex {
  float x, y, u, v;
};
inline void RasterTriangle(RasterVertex a, RasterVertex b, RasterVertex c,
                           const SoftwareTexture &texture, uint32_t light,
                           uint32_t dark, int blend, uint32_t *pixels,
                           int side) {
  float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
  if (std::abs(area) < .00001f)
    return;
  if (area < 0) {
    std::swap(b, c);
    area = -area;
  }
  const int left = std::max(0, (int)std::floor(std::min({a.x, b.x, c.x}))),
            top = std::max(0, (int)std::floor(std::min({a.y, b.y, c.y}))),
            right = std::min(side, (int)std::ceil(std::max({a.x, b.x, c.x}))),
            bottom = std::min(side, (int)std::ceil(std::max({a.y, b.y, c.y})));
  if (left >= right || top >= bottom)
    return;
  struct Edge {
    int64_t x, y, origin;
    bool inclusive;
    Edge(const RasterVertex &a, const RasterVertex &b) {
      const auto ax = (int64_t)std::llround(a.x * 256),
                 ay = (int64_t)std::llround(a.y * 256),
                 bx = (int64_t)std::llround(b.x * 256),
                 by = (int64_t)std::llround(b.y * 256);
      x = ay - by;
      y = bx - ax;
      origin = (by - ay) * ax - (bx - ax) * ay;
      inclusive = by < ay || (by == ay && bx > ax);
    }
    int64_t At(int px, int py) const {
      return x * ((int64_t)px * 256 + 128) + y * ((int64_t)py * 256 + 128) +
             origin;
    }
  };
  const Edge e0(b, c), e1(c, a), e2(a, b);
  const float au = a.u * texture.width - .5f, av = a.v * texture.height - .5f,
              bu = b.u * texture.width - .5f, bv = b.v * texture.height - .5f,
              cu = c.u * texture.width - .5f, cv = c.v * texture.height - .5f;
  const float ux = ((bu - au) * (c.y - a.y) - (cu - au) * (b.y - a.y)) / area,
              uy = ((b.x - a.x) * (cu - au) - (c.x - a.x) * (bu - au)) / area,
              vx = ((bv - av) * (c.y - a.y) - (cv - av) * (b.y - a.y)) / area,
              vy = ((b.x - a.x) * (cv - av) - (c.x - a.x) * (bv - av)) / area;
  const int lr = (light >> 16) & 255, lg = (light >> 8) & 255, lb = light & 255,
            la = (light >> 24) & 255, dr = (dark >> 16) & 255,
            dg = (dark >> 8) & 255, db = dark & 255;
  const bool plain = light == 0xffffffff && (dark & 0xffffff) == 0;
  auto sample = [&](float u, float v) {
    u = std::clamp(u, 0.f, (float)(texture.width - 1));
    v = std::clamp(v, 0.f, (float)(texture.height - 1));
    const int x = (int)u, y = (int)v, fx = (int)((u - x) * 256),
              fy = (int)((v - y) * 256),
              x1 = std::min(x + 1, texture.width - 1),
              y1 = std::min(y + 1, texture.height - 1);
    const auto p00 = texture.rgba[(size_t)y * texture.width + x],
               p01 = texture.rgba[(size_t)y * texture.width + x1],
               p10 = texture.rgba[(size_t)y1 * texture.width + x],
               p11 = texture.rgba[(size_t)y1 * texture.width + x1];
    if (p00 == p01 && p00 == p10 && p00 == p11)
      return p00;
    if (((p00 | p01 | p10 | p11) & 0xff000000) == 0)
      return (uint32_t)0;
    uint32_t result = 0;
    for (int shift : {0, 8, 16, 24}) {
      const int v0 = ((p00 >> shift) & 255) * (256 - fx) +
                     ((p01 >> shift) & 255) * fx,
                v1 = ((p10 >> shift) & 255) * (256 - fx) +
                     ((p11 >> shift) & 255) * fx;
      result |= (uint32_t)((v0 * (256 - fy) + v1 * fy + 32768) >> 16) << shift;
    }
    return result;
  };
  auto blendChannel = [&](int src, int dst, int alpha) {
    if (blend == 1)
      return std::min(255, src + dst);
    if (blend == 2)
      return std::min(255, (src * dst + dst * (255 - alpha) + 127) / 255);
    if (blend == 3)
      return std::min(255, src + (dst * (255 - src) + 127) / 255);
    return src + (dst * (255 - alpha) + 127) / 255;
  };
  auto row0 = e0.At(left, top), row1 = e1.At(left, top),
       row2 = e2.At(left, top);
  float rowU = au + ux * (left + .5f - a.x) + uy * (top + .5f - a.y),
        rowV = av + vx * (left + .5f - a.x) + vy * (top + .5f - a.y);
  for (int y = top; y < bottom; ++y) {
    auto w0 = row0, w1 = row1, w2 = row2;
    float u = rowU, v = rowV;
    auto dst = pixels + (size_t)y * side + left;
    for (int x = left; x < right; ++x, ++dst, u += ux, v += vx,
             w0 += e0.x * 256, w1 += e1.x * 256, w2 += e2.x * 256) {
      if (w0 < (e0.inclusive ? 0 : 1) || w1 < (e1.inclusive ? 0 : 1) ||
          w2 < (e2.inclusive ? 0 : 1))
        continue;
      const auto rgba = sample(u, v);
      const int alpha = ((rgba >> 24) * la + 127) / 255;
      if (!alpha)
        continue;
      const int r = rgba & 255, g = (rgba >> 8) & 255,
                blue = (rgba >> 16) & 255;
      if (plain && alpha == 255 && blend == 0) {
        *dst =
            0xff000000 | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)blue;
        continue;
      }
      const int sr =
                    ((r * lr + (255 - r) * dr + 127) / 255 * alpha + 127) / 255,
                sg =
                    ((g * lg + (255 - g) * dg + 127) / 255 * alpha + 127) / 255,
                sb = ((blue * lb + (255 - blue) * db + 127) / 255 * alpha +
                      127) /
                     255;
      const auto old = *dst;
      const int outA = alpha + (((old >> 24) * (255 - alpha) + 127) / 255);
      *dst = (uint32_t)outA << 24 |
             (uint32_t)blendChannel(sr, (old >> 16) & 255, alpha) << 16 |
             (uint32_t)blendChannel(sg, (old >> 8) & 255, alpha) << 8 |
             (uint32_t)blendChannel(sb, old & 255, alpha);
    }
    row0 += e0.y * 256;
    row1 += e1.y * 256;
    row2 += e2.y * 256;
    rowU += uy;
    rowV += vy;
  }
}
inline bool SoftwareRasterTests() {
  SoftwareTexture texture{1, 1, {0x800000ff}};
  std::vector<uint32_t> image(16);
  const RasterVertex a{0, 0, 0, 0}, b{4, 0, 0, 0}, c{4, 4, 0, 0}, d{0, 4, 0, 0};
  RasterTriangle(a, b, c, texture, 0xffffffff, 0, 0, image.data(), 4);
  RasterTriangle(a, c, d, texture, 0xffffffff, 0, 0, image.data(), 4);
  // Every pixel, including the shared diagonal, must receive exactly one
  // half-alpha red sample. This catches cracks and double-blended seams.
  for (auto pixel : image)
    if (pixel != 0x80800000)
      return false;
  std::fill(image.begin(), image.end(), 0);
  RasterTriangle(c, b, a, texture, 0xffffffff, 0, 0, image.data(), 4);
  RasterTriangle(d, c, a, texture, 0xffffffff, 0, 0, image.data(), 4);
  for (auto pixel : image)
    if (pixel != 0x80800000)
      return false;
  return true;
}
