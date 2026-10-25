// SPDX-License-Identifier: GPL-3.0-or-later
module;
#include <algorithm>
#include <cstddef>
#include <vector>
export module photo_ops.pyramid;
import photo_ops.image;
import photo_ops.color;

export namespace photo_ops {
/// Allocate a linear luminance proxy; the source remains unchanged.
[[nodiscard]] image<gray> luminance_image(image_view<const linear_rgb> source) {
  image<gray> result(source.size());
  for (std::size_t y = 0; y < source.size().height; ++y)
    for (std::size_t x = 0; x < source.size().width; ++x)
      result.view().at(x, y).value = luminance(source.at(x, y));
  return result;
}

/// Separable [1 4 6 4 1]/16, replicate borders, then sample even coordinates.
/// Odd sizes round up. No implicit gamma encoding or rescaling of luminance.
[[nodiscard]] image<gray> downsample(image_view<const gray> source) {
  const auto size = source.size();
  if (!size.width)
    return {};
  // Horizontal low-pass and decimation; keep all source rows for the vertical pass.
  image<gray> horizontal({size.width / 2 + size.width % 2, size.height});
  constexpr float weights[]{1.f / 16, 4.f / 16, 6.f / 16, 4.f / 16, 1.f / 16};
  for (std::size_t y = 0; y < size.height; ++y)
    for (std::size_t x = 0; x < horizontal.size().width; ++x) {
      float sum = 0;
      for (int k = -2; k <= 2; ++k) {
        const auto sx = std::clamp<std::ptrdiff_t>(static_cast<std::ptrdiff_t>(2 * x) + k, 0,
                                                   size.width - 1);
        sum += weights[k + 2] * source.at(sx, y).value;
      }
      horizontal.view().at(x, y).value = sum;
    }
  // Vertical low-pass and decimation complete the next Gaussian level.
  image<gray> result({horizontal.size().width, size.height / 2 + size.height % 2});
  for (std::size_t y = 0; y < result.size().height; ++y)
    for (std::size_t x = 0; x < result.size().width; ++x) {
      float sum = 0;
      for (int k = -2; k <= 2; ++k) {
        const auto sy = std::clamp<std::ptrdiff_t>(static_cast<std::ptrdiff_t>(2 * y) + k, 0,
                                                   size.height - 1);
        sum += weights[k + 2] * horizontal.view().at(x, sy).value;
      }
      result.view().at(x, y).value = sum;
    }
  return result;
}

/// Owns levels below the borrowed base; stops at 1x1 or max_levels (includes base).
[[nodiscard]] std::vector<image<gray>> gaussian_levels(image_view<const gray> base,
                                                       std::size_t max_levels) {
  std::vector<image<gray>> levels;
  while (max_levels > 1 && base.size().width &&
         (base.size().width > 1 || base.size().height > 1)) {
    levels.push_back(downsample(base));
    base = levels.back().view();
    --max_levels;
  }
  return levels;
}
} // namespace photo_ops
