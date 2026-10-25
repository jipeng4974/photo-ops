// SPDX-License-Identifier: GPL-3.0-or-later
module;
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
module photo_ops.focus_stack;
import photo_ops.image;
import photo_ops.color;
import photo_ops.io;
import photo_ops.output;

namespace photo_ops {
namespace {
namespace fs = std::filesystem;
constexpr std::size_t mib = 1024 * 1024;
constexpr std::size_t levels = 4;
// Conservative support: analysis + weight smoothing + synthesis, aligned to 2^(levels-1).
constexpr std::size_t halo = 8 * ((1 << (levels - 1)) - 1) + 8;

struct scratch_run {
  fs::path path;

  ~scratch_run() {
    std::error_code ec;
    if (!path.empty())
      fs::remove_all(path, ec);
  }
};

struct frame {
  fs::path pixels, scores;
  focus_stack_alignment alignment;
};

rgb_f32 add(rgb_f32 a, rgb_f32 b) {
  return {a.r + b.r, a.g + b.g, a.b + b.b};
}

rgb_f32 mul(rgb_f32 a, float k) {
  return {a.r * k, a.g * k, a.b * k};
}

gray add(gray a, gray b) {
  return {a.value + b.value};
}

gray mul(gray a, float k) {
  return {a.value * k};
}

template <class Pixel> Pixel clamped(image_view<const Pixel> src, long x, long y) {
  return src.at(std::clamp<long>(x, 0, src.size().width - 1),
                std::clamp<long>(y, 0, src.size().height - 1));
}

template <class Pixel> Pixel bilinear(image_view<const Pixel> src, double x, double y) {
  const auto ix = static_cast<long>(std::floor(x)), iy = static_cast<long>(std::floor(y));
  const float fx = x - ix, fy = y - iy;
  return add(
      mul(add(mul(clamped(src, ix, iy), 1 - fx), mul(clamped(src, ix + 1, iy), fx)), 1 - fy),
      mul(add(mul(clamped(src, ix, iy + 1), 1 - fx), mul(clamped(src, ix + 1, iy + 1), fx)),
          fy));
}

template <class Pixel> image<Pixel> down(image_view<const Pixel> src) {
  const auto size = src.size();
  image<Pixel> horizontal({(size.width + 1) / 2, size.height});
  constexpr float w[]{1.f / 16, 4.f / 16, 6.f / 16, 4.f / 16, 1.f / 16};
  for (std::size_t y = 0; y < size.height; ++y)
    for (std::size_t x = 0; x < horizontal.size().width; ++x) {
      Pixel sum{};
      for (int k = -2; k <= 2; ++k)
        sum = add(sum, mul(clamped(src, 2 * static_cast<long>(x) + k, y), w[k + 2]));
      horizontal.view().at(x, y) = sum;
    }
  image<Pixel> output({horizontal.size().width, (size.height + 1) / 2});
  for (std::size_t y = 0; y < output.size().height; ++y)
    for (std::size_t x = 0; x < output.size().width; ++x) {
      Pixel sum{};
      for (int k = -2; k <= 2; ++k)
        sum = add(sum, mul(clamped<Pixel>(horizontal.view(), x, 2 * static_cast<long>(y) + k),
                           w[k + 2]));
      output.view().at(x, y) = sum;
    }
  return output;
}

template <class Pixel> image<Pixel> expand(image_view<const Pixel> src, extent size) {
  image<Pixel> horizontal({size.width, src.size().height});
  constexpr float w[]{1.f / 8, 4.f / 8, 6.f / 8, 4.f / 8, 1.f / 8};
  for (std::size_t y = 0; y < src.size().height; ++y)
    for (std::size_t x = 0; x < size.width; ++x) {
      Pixel sum{};
      for (int k = -2; k <= 2; ++k) {
        const long position = static_cast<long>(x) - k;
        if (position % 2 == 0)
          sum = add(sum, mul(clamped(src, position / 2, y), w[k + 2]));
      }
      horizontal.view().at(x, y) = sum;
    }
  image<Pixel> output(size);
  for (std::size_t y = 0; y < size.height; ++y)
    for (std::size_t x = 0; x < size.width; ++x) {
      Pixel sum{};
      for (int k = -2; k <= 2; ++k) {
        const long position = static_cast<long>(y) - k;
        if (position % 2 == 0)
          sum = add(sum, mul(clamped<Pixel>(horizontal.view(), x, position / 2), w[k + 2]));
      }
      output.view().at(x, y) = sum;
    }
  return output;
}

image<gray> smooth(image_view<const gray> src, int radius) {
  image<gray> horizontal(src.size()), output(src.size());
  const float scale = 1.f / (2 * radius + 1);
  for (std::size_t y = 0; y < src.size().height; ++y) {
    double sum = 0;
    for (int k = -radius; k <= radius; ++k)
      sum += clamped(src, k, y).value;
    for (std::size_t x = 0; x < src.size().width; ++x) {
      horizontal.view().at(x, y).value = sum * scale;
      sum -= clamped(src, static_cast<long>(x) - radius, y).value;
      sum += clamped(src, static_cast<long>(x) + radius + 1, y).value;
    }
  }
  for (std::size_t x = 0; x < src.size().width; ++x) {
    double sum = 0;
    for (int k = -radius; k <= radius; ++k)
      sum += clamped<gray>(horizontal.view(), x, k).value;
    for (std::size_t y = 0; y < src.size().height; ++y) {
      output.view().at(x, y).value = sum * scale;
      sum -= clamped<gray>(horizontal.view(), x, static_cast<long>(y) - radius).value;
      sum += clamped<gray>(horizontal.view(), x, static_cast<long>(y) + radius + 1).value;
    }
  }
  return output;
}

image<rgb_f32> smooth_color(image_view<const rgb_f32> src, int radius) {
  image<rgb_f32> horizontal(src.size()), output(src.size());
  const float scale = 1.f / (2 * radius + 1);
  for (std::size_t y = 0; y < src.size().height; ++y)
    for (std::size_t x = 0; x < src.size().width; ++x) {
      rgb_f32 sum{};
      for (int k = -radius; k <= radius; ++k)
        sum = add(sum, clamped(src, static_cast<long>(x) + k, y));
      horizontal.view().at(x, y) = mul(sum, scale);
    }
  for (std::size_t y = 0; y < src.size().height; ++y)
    for (std::size_t x = 0; x < src.size().width; ++x) {
      rgb_f32 sum{};
      for (int k = -radius; k <= radius; ++k)
        sum = add(sum, clamped<rgb_f32>(horizontal.view(), x, static_cast<long>(y) + k));
      output.view().at(x, y) = mul(sum, scale);
    }
  return output;
}

image<gray> focus_score(image_view<const gray> src) {
  image<gray> energy(src.size());
  for (std::size_t y = 0; y < src.size().height; ++y)
    for (std::size_t x = 0; x < src.size().width; ++x) {
      const float c = src.at(x, y).value;
      const float left = clamped(src, x - 1l, y).value, right = clamped(src, x + 1l, y).value;
      const float up = clamped(src, x, y - 1l).value, below = clamped(src, x, y + 1l).value;
      const float lap = left + right + up + below - 4 * c;
      energy.view().at(x, y).value =
          lap * lap + .25f * ((right - left) * (right - left) + (below - up) * (below - up));
    }
  return smooth(energy.view(), 2);
}

std::array<double, 2> inverse(const focus_stack_alignment &t, double x, double y) {
  const double d = t.a * t.a + t.b * t.b;
  return {(t.a * (x - t.tx) + t.b * (y - t.ty)) / d,
          (-t.b * (x - t.tx) + t.a * (y - t.ty)) / d};
}

bool valid_point(extent size, double x, double y) {
  return x >= 0 && y >= 0 && x <= size.width - 1 && y <= size.height - 1;
}

template <class Pixel> result<void> save(const fs::path &path, image_view<const Pixel> src) {
  std::ofstream file(path, std::ios::binary);
  for (std::size_t y = 0; y < src.size().height; ++y)
    file.write(reinterpret_cast<const char *>(src.row(y).data()),
               src.size().width * sizeof(Pixel));
  file.close();
  if (!file)
    return fail(error_code::io, "write focus scratch", "cannot finalize spool", path.string());
  return {};
}

template <class Pixel>
result<image<Pixel>> read_patch(const fs::path &path, extent size, rectangle region) {
  std::ifstream file(path, std::ios::binary);
  if (!file)
    return fail(error_code::io, "read focus scratch", "cannot open spool", path.string());
  image<Pixel> pixels({region.width, region.height});
  for (std::size_t y = 0; y < region.height; ++y) {
    file.seekg(((region.y + y) * size.width + region.x) * sizeof(Pixel));
    file.read(reinterpret_cast<char *>(pixels.view().row(y).data()),
              region.width * sizeof(Pixel));
    if (!file)
      return fail(error_code::io, "read focus scratch", "incomplete spool", path.string());
  }
  return pixels;
}

result<image<rgb_f32>> warp_patch(const frame &source, extent size, rectangle region) {
  double minx = size.width - 1, miny = size.height - 1, maxx = 0, maxy = 0;
  for (auto x : {region.x, region.x + region.width - 1})
    for (auto y : {region.y, region.y + region.height - 1}) {
      const auto p = inverse(source.alignment, x, y);
      minx = std::min(minx, p[0]);
      miny = std::min(miny, p[1]);
      maxx = std::max(maxx, p[0]);
      maxy = std::max(maxy, p[1]);
    }
  const auto x0 =
      static_cast<std::size_t>(std::clamp(std::floor(minx), 0., double(size.width - 1)));
  const auto y0 =
      static_cast<std::size_t>(std::clamp(std::floor(miny), 0., double(size.height - 1)));
  const auto x1 =
      static_cast<std::size_t>(std::clamp(std::ceil(maxx) + 1, 1., double(size.width)));
  const auto y1 =
      static_cast<std::size_t>(std::clamp(std::ceil(maxy) + 1, 1., double(size.height)));
  auto input = read_patch<rgb_f32>(source.pixels, size, {x0, y0, x1 - x0, y1 - y0});
  if (!input)
    return std::unexpected(input.error());
  image<rgb_f32> output({region.width, region.height});
  for (std::size_t y = 0; y < region.height; ++y)
    for (std::size_t x = 0; x < region.width; ++x) {
      const auto p = inverse(source.alignment, region.x + x, region.y + y);
      // Invalid samples replicate locally for filtering, but their weights are forced to zero.
      output.view().at(x, y) = bilinear<rgb_f32>(input->view(), p[0] - x0, p[1] - y0);
    }
  return output;
}

image<rgb_f32> proxy(image_view<const rgb_f32> pixels, std::size_t step) {
  image<rgb_f32> output(
      {(pixels.size().width + step - 1) / step, (pixels.size().height + step - 1) / step});
  for (std::size_t y = 0; y < output.size().height; ++y)
    for (std::size_t x = 0; x < output.size().width; ++x) {
      rgb_f32 sum{};
      std::size_t count = 0;
      for (std::size_t sy = y * step; sy < std::min((y + 1) * step, pixels.size().height); ++sy)
        for (std::size_t sx = x * step; sx < std::min((x + 1) * step, pixels.size().width);
             ++sx) {
          sum = add(sum, pixels.at(sx, sy));
          ++count;
        }
      output.view().at(x, y) = mul(sum, 1.f / count);
    }
  return output;
}

struct coherent_maps {
  extent size;
  std::vector<std::uint16_t> label;
  image<gray> confidence, motion;
};

result<coherent_maps> analyze(const std::vector<frame> &frames, extent full, std::size_t step,
                              std::size_t reference, const color_encoding &encoding) {
  const extent size{(full.width + step - 1) / step, (full.height + step - 1) / step};
  const auto count = size.width * size.height;
  auto ref = read_patch<rgb_f32>(frames[reference].pixels.string() + ".color", size,
                                 {0, 0, size.width, size.height});
  if (!ref)
    return std::unexpected(ref.error());
  auto low_ref = smooth_color(ref->view(), 3);
  *ref = {};
  coherent_maps maps{size, std::vector<std::uint16_t>(count, reference), image<gray>(size),
                     image<gray>(size)};
  image<gray> best(size), second(size), missing(size);
  for (std::size_t i = 0; i < frames.size(); ++i) {
    auto src = read_patch<rgb_f32>(frames[i].pixels.string() + ".color", size,
                                   {0, 0, size.width, size.height});
    if (!src)
      return std::unexpected(src.error());
    image<gray> warped(size), valid(size);
    image<rgb_f32> color(size);
    const double center = (step - 1) * .5;
    for (std::size_t y = 0; y < size.height; ++y)
      for (std::size_t x = 0; x < size.width; ++x) {
        const auto p = inverse(frames[i].alignment, x * step + center, y * step + center);
        color.view().at(x, y) =
            bilinear<rgb_f32>(src->view(), (p[0] - center) / step, (p[1] - center) / step);
        warped.view().at(x, y).value = luminance(color.view().at(x, y), encoding);
        valid.view().at(x, y).value = valid_point(full, p[0], p[1]);
      }
    auto score = focus_score(warped.view());
    auto low = smooth_color(color.view(), 3);
    for (std::size_t y = 0; y < size.height; ++y)
      for (std::size_t x = 0; x < size.width; ++x) {
        float s = score.view().at(x, y).value;
        if (!valid.view().at(x, y).value) {
          s = 0;
          missing.view().at(x, y).value = 1;
        }
        score.view().at(x, y).value = s;
        auto &first = best.view().at(x, y).value, &other = second.view().at(x, y).value;
        if (s > first) {
          other = first;
          first = s;
          maps.label[y * size.width + x] = i;
        } else if (s > other) {
          other = s;
        }
        if (valid.view().at(x, y).value && i != reference) {
          const auto r = low_ref.view().at(x, y), c = low.view().at(x, y);
          if (std::abs(c.r - r.r) > .06f + .2f * std::abs(r.r) ||
              std::abs(c.g - r.g) > .06f + .2f * std::abs(r.g) ||
              std::abs(c.b - r.b) > .06f + .2f * std::abs(r.b))
            maps.motion.view().at(x, y).value = 1;
        }
      }
    if (auto written = save<gray>(frames[i].scores, score.view()); !written)
      return std::unexpected(written.error());
  }
  for (std::size_t y = 0; y < size.height; ++y)
    for (std::size_t x = 0; x < size.width; ++x) {
      const float first = best.view().at(x, y).value;
      maps.confidence.view().at(x, y).value =
          first > 1e-10f ? std::clamp(1 - second.view().at(x, y).value / first, 0.f, 1.f) : 0;
    }
  // Three synchronous Potts iterations on the complete proxy domain, scores streamed by source.
  for (int iteration = 0; iteration < 3; ++iteration) {
    auto next = maps.label;
    image<gray> cost(size);
    for (std::size_t y = 0; y < size.height; ++y)
      for (auto &p : cost.view().row(y))
        p.value = std::numeric_limits<float>::infinity();
    for (std::size_t i = 0; i < frames.size(); ++i) {
      auto score = read_patch<gray>(frames[i].scores, size, {0, 0, size.width, size.height});
      if (!score)
        return std::unexpected(score.error());
      for (std::size_t y = 0; y < size.height; ++y)
        for (std::size_t x = 0; x < size.width; ++x) {
          int different = 0;
          for (auto [dx, dy] : {std::pair{-1, 0}, {1, 0}, {0, -1}, {0, 1}}) {
            const auto sx = std::clamp<long>(x + dx, 0, size.width - 1);
            const auto sy = std::clamp<long>(y + dy, 0, size.height - 1);
            different += maps.label[sy * size.width + sx] != i;
          }
          const float evidence =
              score->view().at(x, y).value / std::max(best.view().at(x, y).value, 1e-10f);
          const float candidate = -evidence + .12f * different;
          if (candidate < cost.view().at(x, y).value) {
            cost.view().at(x, y).value = candidate;
            next[y * size.width + x] = i;
          }
        }
    }
    maps.label = std::move(next);
  }
  // Dilate inconsistent regions in proxy space; all contributing sources then use reference.
  auto expanded = smooth(maps.motion.view(), 2);
  for (std::size_t y = 0; y < size.height; ++y)
    for (std::size_t x = 0; x < size.width; ++x) {
      maps.motion.view().at(x, y).value = expanded.view().at(x, y).value > .001f;
      if (maps.motion.view().at(x, y).value || missing.view().at(x, y).value) {
        maps.label[y * size.width + x] = reference;
        maps.confidence.view().at(x, y).value = 0;
      }
    }
  return maps;
}

image<gray> tile_weight(image_view<const rgb_f32> warped, const frame &source, extent full,
                        rectangle area, const coherent_maps &maps, std::size_t step,
                        std::size_t reference, std::size_t i, const color_encoding &encoding) {
  image<gray> ymap({area.width, area.height});
  for (std::size_t y = 0; y < area.height; ++y)
    for (std::size_t x = 0; x < area.width; ++x)
      ymap.view().at(x, y).value = luminance(warped.at(x, y), encoding);
  auto weight = focus_score(ymap.view());
  // Score smoothing gives spatially coherent evidence before multiscale mask blending.
  weight = smooth(weight.view(), 2);
  for (std::size_t y = 0; y < area.height; ++y)
    for (std::size_t x = 0; x < area.width; ++x) {
      const auto gx = area.x + x, gy = area.y + y;
      const auto mx = std::min(gx / step, maps.size.width - 1);
      const auto my = std::min(gy / step, maps.size.height - 1);
      const auto p = inverse(source.alignment, gx, gy);
      const float confidence = maps.confidence.view().at(mx, my).value;
      const float prior = maps.label[my * maps.size.width + mx] == i ? 1 : 1 - .8f * confidence;
      auto &w = weight.view().at(x, y).value;
      w = std::pow(std::max(w, 0.f) + 1e-8f, 2) * prior;
      if (!valid_point(full, p[0], p[1]))
        w = 0;
      if (maps.motion.view().at(mx, my).value)
        w = i == reference ? 1 : 0;
    }
  return weight;
}

result<image<rgb_f32>> fuse_tile(const std::vector<frame> &frames, extent full, rectangle area,
                                 const coherent_maps &maps, std::size_t step,
                                 std::size_t reference, const color_encoding &encoding) {
  std::vector<image<rgb_f32>> numerator;
  std::vector<image<gray>> denominator;
  extent level_size{area.width, area.height};
  for (std::size_t l = 0; l < levels; ++l) {
    numerator.emplace_back(level_size);
    denominator.emplace_back(level_size);
    level_size = {(level_size.width + 1) / 2, (level_size.height + 1) / 2};
  }
  // Normalize masks at the base before reducing them; otherwise motion weights and
  // ordinary focus energies can differ by orders of magnitude and create broad halos.
  image<gray> total({area.width, area.height});
  for (std::size_t i = 0; i < frames.size(); ++i) {
    auto warped = warp_patch(frames[i], full, area);
    if (!warped)
      return std::unexpected(warped.error());
    auto weight =
        tile_weight(warped->view(), frames[i], full, area, maps, step, reference, i, encoding);
    for (std::size_t y = 0; y < area.height; ++y)
      for (std::size_t x = 0; x < area.width; ++x)
        total.view().at(x, y).value += weight.view().at(x, y).value;
  }
  for (std::size_t i = 0; i < frames.size(); ++i) {
    auto warped = warp_patch(frames[i], full, area);
    if (!warped)
      return std::unexpected(warped.error());
    auto weight =
        tile_weight(warped->view(), frames[i], full, area, maps, step, reference, i, encoding);
    for (std::size_t y = 0; y < area.height; ++y)
      for (std::size_t x = 0; x < area.width; ++x)
        weight.view().at(x, y).value /= total.view().at(x, y).value;
    std::vector<image<rgb_f32>> gaussian;
    std::vector<image<gray>> weights;
    gaussian.push_back(std::move(*warped));
    weights.push_back(std::move(weight));
    for (std::size_t l = 1; l < levels; ++l) {
      gaussian.push_back(down<rgb_f32>(gaussian.back().view()));
      weights.push_back(down<gray>(weights.back().view()));
    }
    for (std::size_t l = 0; l < levels; ++l) {
      image<rgb_f32> expanded;
      if (l + 1 < levels)
        expanded = expand<rgb_f32>(gaussian[l + 1].view(), gaussian[l].size());
      for (std::size_t y = 0; y < gaussian[l].size().height; ++y)
        for (std::size_t x = 0; x < gaussian[l].size().width; ++x) {
          auto band = gaussian[l].view().at(x, y);
          if (l + 1 < levels)
            band = add(band, mul(expanded.view().at(x, y), -1));
          const float w = weights[l].view().at(x, y).value;
          auto &n = numerator[l].view().at(x, y);
          n = add(n, mul(band, w));
          denominator[l].view().at(x, y).value += w;
        }
    }
  }
  for (std::size_t l = 0; l < levels; ++l)
    for (std::size_t y = 0; y < numerator[l].size().height; ++y)
      for (std::size_t x = 0; x < numerator[l].size().width; ++x) {
        const float d = denominator[l].view().at(x, y).value;
        if (!(d > 0) || !std::isfinite(d))
          return fail(error_code::decode, "focus fusion", "invalid fusion weight sum");
        numerator[l].view().at(x, y) = mul(numerator[l].view().at(x, y), 1 / d);
      }
  auto result = std::move(numerator.back());
  for (std::size_t l = levels - 1; l-- > 0;) {
    auto expanded = expand<rgb_f32>(result.view(), numerator[l].size());
    for (std::size_t y = 0; y < expanded.size().height; ++y)
      for (std::size_t x = 0; x < expanded.size().width; ++x)
        expanded.view().at(x, y) = add(expanded.view().at(x, y), numerator[l].view().at(x, y));
    result = std::move(expanded);
  }
  // Motion fallback is exact in its coherent region, not merely an approximate blend weight.
  auto ref = read_patch<rgb_f32>(frames[reference].pixels, full, area);
  if (!ref)
    return std::unexpected(ref.error());
  for (std::size_t y = 0; y < area.height; ++y)
    for (std::size_t x = 0; x < area.width; ++x) {
      const auto mx = std::min((area.x + x) / step, maps.size.width - 1);
      const auto my = std::min((area.y + y) / step, maps.size.height - 1);
      bool fallback = maps.motion.view().at(mx, my).value;
      for (const auto &source : frames) {
        const auto p = inverse(source.alignment, area.x + x, area.y + y);
        fallback |= !valid_point(full, p[0], p[1]);
      }
      if (fallback)
        result.view().at(x, y) = ref->view().at(x, y);
    }
  return result;
}

std::string json_string(std::string_view value) {
  std::string out = "\"";
  constexpr char hex[] = "0123456789abcdef";
  for (unsigned char c : value) {
    if (c == '\\' || c == '\"') {
      out += '\\';
      out += c;
    } else if (c < 32) {
      out += "\\u00";
      out += hex[c >> 4];
      out += hex[c & 15];
    } else
      out += c;
  }
  return out + '\"';
}

template <class T, std::size_t N>
void json_array(std::ostream &file, const std::array<T, N> &values) {
  file << '[';
  for (std::size_t i = 0; i < N; ++i)
    file << (i ? "," : "") << values[i];
  file << ']';
}

result<image<linear_rgb>> preview_spool(const fs::path &path,
                                        const focus_stack_report &report) {
  const auto full = report.dimensions;
  const auto step = report.proxy_step;
  const extent size{(full.width + step - 1) / step, (full.height + step - 1) / step};
  image<linear_rgb> output(size);
  std::vector<rgb_f32> row(full.width);
  std::vector<rgb_f32> sums(size.width);
  std::ifstream file(path, std::ios::binary);
  if (!file)
    return fail(error_code::io, "focus preview", "cannot open spool", path.string());
  for (std::size_t y = 0; y < full.height; ++y) {
    file.read(reinterpret_cast<char *>(row.data()), row.size() * sizeof(rgb_f32));
    if (!file)
      return fail(error_code::io, "focus preview", "cannot read spool", path.string());
    for (std::size_t x = 0; x < full.width; ++x)
      sums[x / step] = add(sums[x / step], row[x]);
    if ((y + 1) % step == 0 || y + 1 == full.height) {
      const auto my = y / step;
      const auto rows = y + 1 - my * step;
      for (std::size_t x = 0; x < size.width; ++x) {
        const auto count = rows * std::min(step, full.width - x * step);
        output.view().at(x, my) = to_linear_srgb(mul(sums[x], 1.f / count), report.encoding);
        sums[x] = {};
      }
    }
  }
  return output;
}

result<void> diagnostics(const std::string &directory, const coherent_maps &maps,
                         const focus_stack_report &report, const fs::path &reference_pixels,
                         const fs::path &fused_pixels) {
  std::error_code ec;
  fs::create_directories(directory, ec);
  if (ec)
    return fail(error_code::io, "focus diagnostics", ec.message(), directory);
  const fs::path root(directory);
  image<linear_rgb> preview(maps.size);
  for (const auto *name : {"source-map.tif", "confidence.tif", "motion.tif"}) {
    for (std::size_t y = 0; y < maps.size.height; ++y)
      for (std::size_t x = 0; x < maps.size.width; ++x) {
        const float value =
            std::string_view(name) == "source-map.tif"
                ? maps.label[y * maps.size.width + x] / float(report.frames.size() - 1)
            : std::string_view(name) == "confidence.tif" ? maps.confidence.view().at(x, y).value
                                                         : maps.motion.view().at(x, y).value;
        preview.view().at(x, y) = {value, value, value};
      }
    if (auto written = write_image((root / name).string(), preview.view()); !written)
      return written;
  }
  for (const auto &[name, path] :
       {std::pair{"before.jpg", reference_pixels}, std::pair{"after.jpg", fused_pixels}}) {
    auto preview = preview_spool(path, report);
    if (!preview)
      return std::unexpected(preview.error());
    if (auto written =
            write_image((root / name).string(), preview->view(), {output_format::jpeg, 95});
        !written)
      return written;
  }
  auto transaction = atomic_output::create((root / "report.json").string());
  if (!transaction)
    return std::unexpected(transaction.error());
  std::ofstream file(transaction->temporary_path());
  file.imbue(std::locale::classic());
  file << std::setprecision(12);
  file << "{\n  \"algorithm\": \"focus-stack-classical-v1\",\n"
       << "  \"working_space\": \""
       << (report.raw_context ? "float32 linear-Rec.2020/D65" : "float32 linear-sRGB/D65")
       << "\",\n"
       << "  \"input_origin\": \"" << (report.raw_context ? "scene_linear_raw" : "rendered_rgb")
       << "\",\n"
       << "  \"output_encoding\": \""
       << (report.raw_context ? "float32 linear Rec.2020 ICC TIFF" : "16-bit sRGB ICC TIFF")
       << "\",\n"
       << "  \"reference_index\": " << report.reference_index
       << ",\n  \"width\": " << report.dimensions.width
       << ",\n  \"height\": " << report.dimensions.height
       << ",\n  \"tile_size\": " << report.tile_size << ",\n  \"halo\": " << halo
       << ",\n  \"proxy_step\": " << report.proxy_step
       << ",\n  \"refinement_step\": " << report.refinement_step
       << ",\n  \"estimated_peak_bytes\": " << report.estimated_peak_bytes
       << ",\n  \"scratch_bytes\": " << report.scratch_bytes
       << ",\n  \"motion_fraction\": " << report.motion_fraction
       << ",\n  \"low_confidence_fraction\": " << report.low_confidence_fraction << ",\n";
  if (report.raw_context) {
    const auto &context = *report.raw_context;
    const auto &ref = context.reference();
    file << "  \"raw_context\": {\"fingerprint\":" << json_string(context.fingerprint())
         << ",\"camera\":" << json_string(ref.camera)
         << ",\"decoder_version\":" << json_string(ref.decoder_version)
         << ",\"color_reference_index\":" << context.reference_index()
         << ",\"calibration\":\"LibRaw daylight camera-to-XYZ/D65, diagonal WB once\""
         << ",\"demosaic\":\"float directional Bayer v1\""
         << ",\"exposure_policy\":\"strict metadata match; shared green-normalized sensor "
            "white\""
         << ",\"iso\":" << ref.iso << ",\"shutter_seconds\":" << ref.shutter_seconds
         << ",\"aperture\":" << ref.aperture << ",\"white_balance\":";
    json_array(file, context.white_balance());
    file << ",\"black\":";
    json_array(file, ref.black);
    file << ",\"white\":";
    json_array(file, ref.white);
    file << ",\"camera_to_xyz\":";
    json_array(file, ref.camera_to_xyz);
    file << ",\"cfa\":";
    json_array(file, ref.cfa);
    file << ",\"raw_width\":" << ref.raw_dimensions.width
         << ",\"raw_height\":" << ref.raw_dimensions.height << ",\"active_left\":" << ref.left
         << ",\"active_top\":" << ref.top << ",\"active_width\":" << ref.active_dimensions.width
         << ",\"active_height\":" << ref.active_dimensions.height
         << ",\"orientation\":" << ref.orientation << ",\"nominal_bits\":" << ref.nominal_bits
         << "},\n";
  }
  file << "  \"frames\": [\n";
  for (std::size_t i = 0; i < report.frames.size(); ++i) {
    const auto &f = report.frames[i];
    const auto &t = f.alignment;
    file << "    {\"index\":" << i << ",\"a\":" << t.a << ",\"b\":" << t.b << ",\"tx\":" << t.tx
         << ",\"ty\":" << t.ty << ",\"matches\":" << t.matches << ",\"inliers\":" << t.inliers
         << ",\"residual_px\":" << t.residual_px
         << ",\"dense_residual_px\":" << t.dense_residual_px
         << ",\"dense_correlation\":" << t.dense_correlation
         << ",\"refinement_samples\":" << t.refinement_samples
         << ",\"refined\":" << (t.refined ? "true" : "false")
         << ",\"selected_fraction\":" << f.selected_fraction;
    if (report.raw_context) {
      const auto &m = report.raw_context->frames()[i];
      const auto &stats = f.raw_statistics;
      file << ",\"input_path\":" << json_string(m.path)
           << ",\"input_fingerprint\":" << json_string(m.input_fingerprint)
           << ",\"camera_wb_metadata\":";
      json_array(file, m.camera_wb);
      file << ",\"raw_saturated_fraction\":" << stats.saturated_fraction
           << ",\"working_negative_fraction\":" << stats.negative_fraction
           << ",\"working_minimum\":" << stats.minimum
           << ",\"working_maximum\":" << stats.maximum;
    }
    file << '}' << (i + 1 == report.frames.size() ? "\n" : ",\n");
  }
  file << "  ]\n}\n";
  file.close();
  if (!file)
    return fail(error_code::io, "focus diagnostics", "cannot finalize JSON", directory);
  return transaction->commit();
}
} // namespace

result<focus_stack_report> stack_focus(std::span<const std::string> inputs,
                                       const std::string &output,
                                       const focus_stack_options &options) {
  if (inputs.size() < 2 || inputs.size() > 256 || options.memory_budget_mib < 512 ||
      options.memory_budget_mib > 1024 * 1024 || options.tile_size < 32 ||
      options.tile_size > 4096 || options.proxy_long_edge < 32 ||
      options.proxy_long_edge > 2048 ||
      (options.reference_index != static_cast<std::size_t>(-1) &&
       options.reference_index >= inputs.size()))
    return fail(error_code::invalid_argument, "focus preflight",
                "need 2..256 inputs, budget 512..1048576 MiB, tile 32..4096, proxy 32..2048, "
                "valid reference");
  auto ext = fs::path(output).extension().string();
  std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return std::tolower(c); });
  if (ext != ".tif" && ext != ".tiff")
    return fail(error_code::unsupported_format, "focus output",
                "streaming output requires TIFF", output);
  std::error_code ec;
  if (fs::symlink_status(output, ec).type() != fs::file_type::not_found && !ec)
    return fail(error_code::io, "focus output", "output already exists", output);
  if (!options.diagnostics_directory.empty()) {
    for (auto name : {"source-map.tif", "confidence.tif", "motion.tif", "before.jpg",
                      "after.jpg", "report.json"}) {
      const auto path = fs::path(options.diagnostics_directory) / name;
      const auto status = fs::symlink_status(path, ec);
      if (!ec && status.type() != fs::file_type::not_found)
        return fail(error_code::io, "focus diagnostics", "output already exists",
                    path.string());
    }
  }
  const auto raw_path = [](const std::string &path) {
    auto suffix = fs::path(path).extension().string();
    std::ranges::transform(suffix, suffix.begin(),
                           [](unsigned char c) { return std::tolower(c); });
    return suffix == ".cr3" || suffix == ".dng";
  };
  const bool raw = raw_path(inputs.front());
  for (const auto &path : inputs)
    if (raw_path(path) != raw)
      return fail(error_code::invalid_argument, "focus preflight",
                  "mixed RAW/rendered sequence", path);
  std::optional<raw_sequence_context> context;
  if (raw) {
    raw_sequence_options settings;
    settings.reference_index = options.reference_index;
    settings.memory_budget_mib = options.memory_budget_mib;
    auto prepared = prepare_raw_sequence(inputs, settings);
    if (!prepared)
      return std::unexpected(prepared.error());
    context = std::move(*prepared);
  }
  extent size{};
  std::size_t largest_file = 0;
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    const auto &path = inputs[i];
    auto dimensions = raw ? result<extent>(context->frames()[i].output_dimensions)
                          : rendered_image_size(path);
    if (!dimensions)
      return std::unexpected(dimensions.error());
    if (size.width && (size.width != dimensions->width || size.height != dimensions->height))
      return fail(error_code::invalid_argument, "focus preflight", "input dimensions differ",
                  path);
    size = *dimensions;
    const auto bytes = fs::file_size(path, ec);
    if (ec || bytes > 1024ULL * 1024 * 1024)
      return fail(error_code::invalid_argument, "focus preflight",
                  "missing or oversized input (>1 GiB)", path);
    largest_file = std::max(largest_file, static_cast<std::size_t>(bytes));
  }
  // Deliberate implementation limits keep every byte/offset product within uint64_t.
  if (size.width < 32 || size.height < 32 || size.width > 65536 || size.height > 65536 ||
      size.width * size.height > 200000000)
    return fail(error_code::invalid_argument, "focus preflight",
                "supported dimensions: 32..65536, <=200 MP");
  const std::size_t pixels = size.width * size.height;
  const std::size_t budget = options.memory_budget_mib * mib;
  const std::size_t usable = budget * 4 / 5;
  std::size_t decode = 24 * pixels + largest_file + 128 * mib;
  if (context)
    for (const auto &m : context->frames())
      decode = std::max(decode, m.decode_reserve_bytes);
  if (decode > usable)
    return fail(
        error_code::invalid_argument, "focus memory budget",
        "single-frame decode reserve exceeds budget; tiling cannot reduce decoder memory");
  auto step = std::max<std::size_t>(
      1, (std::max(size.width, size.height) + options.proxy_long_edge - 1) /
             options.proxy_long_edge);
  auto tile = options.tile_size / 8 * 8;
  std::size_t q = 0, registration = 0, fusion = 0;
  while (true) {
    q = ((size.width + step - 1) / step) * ((size.height + step - 1) / step);
    registration = 512 * q + 128 * mib;
    fusion = 128 * q + 512 * (tile + 2 * halo) * (tile + 2 * halo) + 128 * mib;
    if (std::max(registration, fusion) <= usable)
      break;
    if (registration > usable && (size.width + step - 1) / step >= 64 &&
        (size.height + step - 1) / step >= 64) {
      step *= 2;
    } else if (fusion > usable && tile > 32) {
      tile = std::max<std::size_t>(32, (tile / 2) / 8 * 8);
    } else {
      return fail(error_code::invalid_argument, "focus memory budget",
                  "minimum proxy/tile workspace exceeds budget");
    }
  }
  auto refinement_step = std::max<std::size_t>(1, step / 2);
  auto refinement_pixels = [&] {
    return ((size.width + refinement_step - 1) / refinement_step) *
           ((size.height + refinement_step - 1) / refinement_step);
  };
  while (128 * refinement_pixels() + 128 * mib > usable)
    ++refinement_step;
  registration = std::max(registration, 128 * refinement_pixels() + 128 * mib);
  const auto reference = options.reference_index == static_cast<std::size_t>(-1)
                             ? inputs.size() / 2
                             : options.reference_index;
  const std::size_t scratch_bytes = (inputs.size() + 1) * 12 * pixels +
                                    inputs.size() * (20 * q + 4 * refinement_pixels()) +
                                    6 * pixels;
  ec.clear();
  const auto scratch_parent = options.scratch_directory.empty()
                                  ? fs::temp_directory_path(ec)
                                  : fs::path(options.scratch_directory);
  if (ec)
    return fail(error_code::io, "focus scratch", ec.message());
  const auto space = fs::space(scratch_parent, ec);
  if (ec || space.available < scratch_bytes)
    return fail(error_code::io, "focus scratch",
                "insufficient scratch space or invalid directory", scratch_parent.string());
  scratch_run scratch;
  auto pattern = (scratch_parent / "photo-ops-focus-XXXXXX").string();
  if (!::mkdtemp(pattern.data()))
    return fail(error_code::io, "focus scratch", "cannot create private run directory",
                scratch_parent.string());
  scratch.path = pattern;
  focus_stack_report report;
  report.dimensions = size;
  report.reference_index = reference;
  report.tile_size = tile;
  report.proxy_step = step;
  report.refinement_step = refinement_step;
  report.estimated_peak_bytes = std::max({decode, registration, fusion}) * 5 / 4;
  report.scratch_bytes = scratch_bytes;
  report.frames.resize(inputs.size());
  report.raw_context = std::move(context);
  if (report.raw_context)
    report.encoding = report.raw_context->encoding();
  else
    report.encoding.normalization = "rendered RGB ICC/assumed sRGB linearization";
  {
    std::ofstream manifest(scratch.path / "context.txt");
    manifest << (raw ? "linear Rec.2020/D65 scene RAW" : "linear sRGB/D65 rendered") << '\n'
             << report.encoding.normalization << '\n';
    if (report.raw_context)
      for (const auto &m : report.raw_context->frames())
        manifest << m.input_fingerprint << ' ' << m.path << '\n';
    manifest.close();
    if (!manifest)
      return fail(error_code::io, "focus scratch", "cannot write context manifest",
                  scratch.path.string());
  }
  std::vector<frame> frames(inputs.size());
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    frames[i].pixels = scratch.path / (std::to_string(i) + ".rgb");
    frames[i].scores = scratch.path / (std::to_string(i) + ".score");
  }
  image<gray> reference_proxy;
  // Decode reference first. At most one full-resolution RGB owner is alive at any time.
  for (std::size_t pass = 0; pass < inputs.size(); ++pass) {
    const auto i = pass == 0 ? reference : pass <= reference ? pass - 1 : pass;
    auto decode_frame = [&]() -> result<working_image> {
      if (report.raw_context) {
        auto developed = decode_raw_frame(inputs[i], *report.raw_context, i);
        if (!developed)
          return std::unexpected(developed.error());
        report.frames[i].raw_statistics = developed->statistics;
        return std::move(developed->image);
      }
      auto legacy = read_image(inputs[i]);
      if (!legacy)
        return std::unexpected(legacy.error());
      if (legacy->origin != input_origin::rendered_rgb)
        return fail(error_code::decode, "focus decode", "input changed after preflight",
                    inputs[i]);
      image<rgb_f32> pixels(legacy->pixels.size());
      for (std::size_t y = 0; y < pixels.size().height; ++y)
        for (std::size_t x = 0; x < pixels.size().width; ++x) {
          const auto p = legacy->pixels.view().at(x, y);
          pixels.view().at(x, y) = {p.r, p.g, p.b};
        }
      return working_image(std::move(pixels), report.encoding);
    };
    auto decoded = decode_frame();
    if (!decoded)
      return std::unexpected(decoded.error());
    if (decoded->pixels.size().width != size.width ||
        decoded->pixels.size().height != size.height ||
        decoded->encoding.normalization != report.encoding.normalization)
      return fail(error_code::decode, "focus decode", "input/context changed after preflight",
                  inputs[i]);
    for (std::size_t y = 0; y < size.height; ++y)
      for (auto p : decoded->pixels.view().row(y))
        if (!std::isfinite(p.r) || !std::isfinite(p.g) || !std::isfinite(p.b) ||
            std::max({std::abs(p.r), std::abs(p.g), std::abs(p.b)}) > (raw ? 1e6 : 16))
          return fail(error_code::invalid_argument, "focus input",
                      "non-finite or excessive rendered RGB range", inputs[i]);
    const auto fine_size = extent{(size.width + refinement_step - 1) / refinement_step,
                                  (size.height + refinement_step - 1) / refinement_step};
    image<gray> fine(fine_size);
    for (std::size_t y = 0; y < fine.size().height; ++y)
      for (std::size_t x = 0; x < fine.size().width; ++x) {
        double sum = 0;
        std::size_t count = 0;
        for (std::size_t sy = y * refinement_step;
             sy < std::min((y + 1) * refinement_step, size.height); ++sy)
          for (std::size_t sx = x * refinement_step;
               sx < std::min((x + 1) * refinement_step, size.width); ++sx) {
            sum += luminance(decoded->pixels.view().at(sx, sy), report.encoding);
            ++count;
          }
        fine.view().at(x, y).value = sum / count;
      }
    if (auto saved = save<gray>(frames[i].pixels.string() + ".fine", fine.view()); !saved)
      return std::unexpected(saved.error());
    fine = {};
    auto colors = proxy(decoded->pixels.view(), step);
    image<gray> small(colors.size());
    for (std::size_t y = 0; y < small.size().height; ++y)
      for (std::size_t x = 0; x < small.size().width; ++x)
        small.view().at(x, y).value = luminance(colors.view().at(x, y), report.encoding);
    if (auto saved = save<rgb_f32>(frames[i].pixels.string() + ".color", colors.view()); !saved)
      return std::unexpected(saved.error());
    colors = {};
    if (auto saved = save<rgb_f32>(frames[i].pixels, decoded->pixels.view()); !saved)
      return std::unexpected(saved.error());
    decoded->pixels = {}; // Release the full-resolution frame before feature extraction.
    if (auto saved = save<gray>(frames[i].pixels.string() + ".proxy", small.view()); !saved)
      return std::unexpected(saved.error());
    if (i == reference) {
      reference_proxy = std::move(small);
    } else if (!options.already_aligned) {
      auto alignment = detail::align_focus_stack(reference_proxy.view(), small.view());
      if (!alignment)
        return std::unexpected(error{alignment.error().code, alignment.error().operation,
                                     alignment.error().message, inputs[i]});
      // x_full = step*x_proxy + center; conjugate the proxy similarity into full-res.
      auto t = *alignment;
      const double center = (step - 1) * .5;
      t.tx = step * t.tx + center - (t.a - t.b) * center;
      t.ty = step * t.ty + center - (t.a + t.b) * center;
      t.residual_px *= step;
      frames[i].alignment = t;
    }
    report.frames[i].alignment = frames[i].alignment;
  }
  reference_proxy = {};
  if (!options.already_aligned) {
    const extent fine_size{(size.width + refinement_step - 1) / refinement_step,
                           (size.height + refinement_step - 1) / refinement_step};
    auto fine_ref = read_patch<gray>(frames[reference].pixels.string() + ".fine", fine_size,
                                     {0, 0, fine_size.width, fine_size.height});
    if (!fine_ref)
      return std::unexpected(fine_ref.error());
    for (std::size_t i = 0; i < inputs.size(); ++i) {
      if (frames[i].alignment.identity)
        continue;
      auto fine = read_patch<gray>(frames[i].pixels.string() + ".fine", fine_size,
                                   {0, 0, fine_size.width, fine_size.height});
      if (!fine)
        return std::unexpected(fine.error());
      auto t = frames[i].alignment;
      const double center = (refinement_step - 1) * .5;
      t.tx = (t.tx - center + (t.a - t.b) * center) / refinement_step;
      t.ty = (t.ty - center + (t.a + t.b) * center) / refinement_step;
      auto refined = detail::refine_focus_stack(fine_ref->view(), fine->view(), t);
      if (!refined)
        return std::unexpected(error{refined.error().code, refined.error().operation,
                                     refined.error().message, inputs[i]});
      t = *refined;
      t.tx = refinement_step * t.tx + center - (t.a - t.b) * center;
      t.ty = refinement_step * t.ty + center - (t.a + t.b) * center;
      t.dense_residual_px *= refinement_step;
      frames[i].alignment = t;
      report.frames[i].alignment = t;
    }
  }
  auto maps = analyze(frames, size, step, reference, report.encoding);
  if (!maps)
    return std::unexpected(maps.error());
  for (std::size_t y = 0; y < maps->size.height; ++y)
    for (std::size_t x = 0; x < maps->size.width; ++x) {
      ++report.frames[maps->label[y * maps->size.width + x]].selected_fraction;
      report.motion_fraction += maps->motion.view().at(x, y).value != 0;
      report.low_confidence_fraction += maps->confidence.view().at(x, y).value < .1f;
    }
  for (auto &f : report.frames)
    f.selected_fraction /= q;
  report.motion_fraction /= q;
  report.low_confidence_fraction /= q;
  const auto fused_path = scratch.path / "fused.rgb";
  std::fstream fused(fused_path,
                     std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
  if (!fused)
    return fail(error_code::io, "focus scratch", "cannot create fused spool",
                fused_path.string());
  for (std::size_t y = 0; y < size.height; y += tile)
    for (std::size_t x = 0; x < size.width; x += tile) {
      const auto x0 = x > halo ? x - halo : 0, y0 = y > halo ? y - halo : 0;
      const auto x1 = std::min(size.width, x + tile + halo);
      const auto y1 = std::min(size.height, y + tile + halo);
      auto result = fuse_tile(frames, size, {x0, y0, x1 - x0, y1 - y0}, *maps, step, reference,
                              report.encoding);
      if (!result)
        return std::unexpected(result.error());
      for (std::size_t row = y; row < std::min(size.height, y + tile); ++row) {
        const auto width = std::min(tile, size.width - x);
        fused.seekp((row * size.width + x) * sizeof(rgb_f32));
        fused.write(
            reinterpret_cast<const char *>(result->view().row(row - y0).data() + x - x0),
            width * sizeof(rgb_f32));
        if (!fused)
          return fail(error_code::io, "focus fusion", "cannot write fused tile",
                      fused_path.string());
      }
    }
  fused.flush();
  if (!fused)
    return fail(error_code::io, "focus fusion", "cannot flush fused spool",
                fused_path.string());
  if (!options.diagnostics_directory.empty())
    if (auto written = diagnostics(options.diagnostics_directory, *maps, report,
                                   frames[reference].pixels, fused_path);
        !written)
      return std::unexpected(written.error());
  const auto read_row = [&](std::size_t y, std::span<rgb_f32> row) -> result<void> {
    fused.seekg(y * size.width * sizeof(rgb_f32));
    fused.read(reinterpret_cast<char *>(row.data()), row.size_bytes());
    if (!fused)
      return fail(error_code::io, "focus output", "cannot read fused spool",
                  fused_path.string());
    return {};
  };
  result<void> written;
  if (raw)
    written = write_working_tiff_rows(output, size, report.encoding, read_row);
  else {
    std::vector<rgb_f32> working_row(size.width);
    written = write_tiff_rows(
        output, size, [&](std::size_t y, std::span<linear_rgb> row) -> result<void> {
          if (auto read = read_row(y, working_row); !read)
            return read;
          for (std::size_t x = 0; x < row.size(); ++x)
            row[x] = {working_row[x].r, working_row[x].g, working_row[x].b};
          return {};
        });
  }
  if (!written)
    return std::unexpected(written.error());
  return report;
}
} // namespace photo_ops
