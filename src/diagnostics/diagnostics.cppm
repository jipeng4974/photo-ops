// SPDX-License-Identifier: GPL-3.0-or-later
module;
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
export module photo_ops.diagnostics;
import photo_ops.image;
import photo_ops.color;
import photo_ops.sunlight;
export import photo_ops.error;

export namespace photo_ops {
struct image_statistics {
  std::size_t pixels{}, non_finite_pixels{}, outside_unit_pixels{};
  double mean_luminance{};
  float min_channel = std::numeric_limits<float>::infinity();

  float max_channel = -std::numeric_limits<float>::infinity();
};

/// Summarize finite samples; an image with no finite samples has infinite range sentinels.
image_statistics statistics(image_view<const linear_rgb> image) {
  image_statistics result;
  for (std::size_t y = 0; y < image.size().height; ++y)
    for (auto p : image.row(y)) {
      ++result.pixels;
      if (!std::isfinite(p.r) || !std::isfinite(p.g) || !std::isfinite(p.b)) {
        ++result.non_finite_pixels;
        continue;
      }
      const auto lo = std::min({p.r, p.g, p.b}), hi = std::max({p.r, p.g, p.b});
      result.min_channel = std::min(result.min_channel, lo);
      result.max_channel = std::max(result.max_channel, hi);
      result.outside_unit_pixels += lo < 0 || hi > 1;
      result.mean_luminance += luminance(p);
    }
  const auto finite = result.pixels - result.non_finite_pixels;
  if (finite)
    result.mean_luminance /= finite;
  return result;
}

/// Write paired JPEG previews (with the same documented exposure lift), mask TIFF and JSON.
/// Each file is atomic and never overwritten; JSON is published last, not a directory
/// transaction. Image output and diagnostic I/O errors are separate.
result<void> write_sunlight_diagnostics(const std::string &directory,
                                        image_view<const linear_rgb> source,
                                        const sunlight_result &enhanced)
    pre(source.size().width > 0) pre(source.size().width == enhanced.pixels.size().width)
        pre(source.size().height == enhanced.pixels.size().height)
            pre(source.size().width == enhanced.mask.size().width)
                pre(source.size().height == enhanced.mask.size().height);
} // namespace photo_ops
