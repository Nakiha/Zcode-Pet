#pragma once
#include <algorithm>
#include <array>
// Linear source-over model. Coverage calibration/gamma remains experimental.
inline std::array<float, 4> EncodeCoverage(std::array<float, 3> c,
                                           std::array<float, 3> text,
                                           float background, float strength) {
  float gray = (c[0] + c[1] + c[2]) / 3;
  for (auto &v : c)
    v = gray + (v - gray) * strength;
  float alpha = std::max({c[0], c[1], c[2]});
  std::array<float, 4> out{};
  for (int k = 0; k < 3; ++k)
    out[k] = c[k] * text[k] + (alpha - c[k]) * background;
  out[3] = alpha;
  return out;
}
inline bool CoverageTests() {
  for (float r : {0.f, .2f, .7f, 1.f})
    for (float g : {0.f, .4f, 1.f})
      for (float b : {0.f, .6f, 1.f})
        for (float bg : {0.f, .5f, 1.f}) {
          std::array<float, 3> c{r, g, b}, t{.1f, .2f, .3f};
          auto p = EncodeCoverage(c, t, bg, 1);
          for (int k = 0; k < 3; ++k) {
            if (p[k] < 0 || p[k] > p[3] + .00001f)
              return false;
            float actual = p[k] + (1 - p[3]) * bg,
                  expected = c[k] * t[k] + (1 - c[k]) * bg;
            if (abs(actual - expected) > .00001f)
              return false;
          }
        }
  return true;
}
