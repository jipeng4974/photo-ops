// SPDX-License-Identifier: GPL-3.0-or-later
module;
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <expected>
#include <limits>
#include <numeric>
#include <vector>
module photo_ops.sunlight;
import photo_ops.image;
import photo_ops.color;

namespace photo_ops {
namespace {
// Analysis is bounded independently of capture resolution. No semantic model is involved.
constexpr std::size_t analysis_limit = 256;

constexpr float minimum_region_fraction = 0.01f;

constexpr float maximum_region_fraction = 0.55f;

float smooth_step(float low, float high, float value) pre(low < high) {
  const float position = std::clamp((value - low) / (high - low), 0.f, 1.f);
  return position * position * (3.f - 2.f * position);
}

float chroma(oklab pixel) {
  return std::hypot(pixel.a, pixel.b);
}

struct analysis_grid {
  extent size;
  std::size_t scale;
  std::vector<linear_rgb> colors;
  std::vector<float> log_luminance;
};

result<analysis_grid> make_analysis_grid(image_view<const linear_rgb> source) {
  const auto dimensions = source.size();
  const auto longest = std::max(dimensions.width, dimensions.height);
  const auto scale = 1 + (longest - 1) / analysis_limit;
  const extent size{1 + (dimensions.width - 1) / scale, 1 + (dimensions.height - 1) / scale};
  analysis_grid grid{size, scale, std::vector<linear_rgb>(size.width * size.height), {}};
  std::vector<std::size_t> counts(grid.colors.size());

  // Area averaging retains coherent illumination and suppresses isolated sensor highlights.
  for (std::size_t y = 0; y < dimensions.height; ++y) {
    const auto row = source.row(y);
    for (std::size_t x = 0; x < dimensions.width; ++x) {
      const auto pixel = row[x];
      if (!std::isfinite(pixel.r) || !std::isfinite(pixel.g) || !std::isfinite(pixel.b))
        return fail(error_code::invalid_argument, "analyze sunlight", "non-finite RGB input");
      const auto index = (y / scale) * size.width + x / scale;
      auto &sum = grid.colors[index];
      sum.r += pixel.r;
      sum.g += pixel.g;
      sum.b += pixel.b;
      ++counts[index];
    }
  }

  grid.log_luminance.reserve(grid.colors.size());
  for (std::size_t index = 0; index < grid.colors.size(); ++index) {
    auto &pixel = grid.colors[index];
    const float count = static_cast<float>(counts[index]);
    pixel = {pixel.r / count, pixel.g / count, pixel.b / count};
    grid.log_luminance.push_back(std::log2(std::max(luminance(pixel), 1e-8f)));
  }
  return grid;
}

// Separable box filtering is sufficient at this small analysis resolution.
std::vector<float> blur(const std::vector<float> &source, extent size, int radius) {
  std::vector<float> horizontal(source.size());
  std::vector<float> output(source.size());
  for (std::size_t y = 0; y < size.height; ++y) {
    for (std::size_t x = 0; x < size.width; ++x) {
      float sum = 0;
      for (int offset = -radius; offset <= radius; ++offset) {
        const auto sample = std::clamp<std::ptrdiff_t>(static_cast<std::ptrdiff_t>(x) + offset,
                                                       0, size.width - 1);
        sum += source[y * size.width + sample];
      }
      horizontal[y * size.width + x] = sum / (2 * radius + 1);
    }
  }
  for (std::size_t y = 0; y < size.height; ++y) {
    for (std::size_t x = 0; x < size.width; ++x) {
      float sum = 0;
      for (int offset = -radius; offset <= radius; ++offset) {
        const auto sample = std::clamp<std::ptrdiff_t>(static_cast<std::ptrdiff_t>(y) + offset,
                                                       0, size.height - 1);
        sum += horizontal[sample * size.width + x];
      }
      output[y * size.width + x] = sum / (2 * radius + 1);
    }
  }
  return output;
}

float quantile(std::vector<float> values, float fraction) {
  std::ranges::sort(values);
  return values[static_cast<std::size_t>(fraction * (values.size() - 1))];
}

// Seed a conservative sky exclusion from the top edge. Blue sky connects otherwise
// detached bright clouds; neutral foreground is not excluded merely for being bright.
std::vector<bool> sky_exclusion(const analysis_grid &grid, float median) {
  std::vector<bool> eligible(grid.colors.size());
  std::vector<bool> sky(grid.colors.size());
  std::vector<std::size_t> queue;
  for (std::size_t index = 0; index < grid.colors.size(); ++index) {
    const auto pixel = grid.colors[index];
    const auto perceptual = to_oklab(pixel);
    const bool blue = pixel.b > 1.15f * pixel.r && pixel.b > 1.05f * pixel.g;
    const bool bright_neutral = chroma(perceptual) / std::max(perceptual.l, 0.01f) < 0.06f &&
                                grid.log_luminance[index] > median + 0.4f;
    eligible[index] = blue || bright_neutral;
    if (index < grid.size.width && eligible[index]) {
      sky[index] = true;
      queue.push_back(index);
    }
  }
  for (std::size_t next = 0; next < queue.size(); ++next) {
    const auto index = queue[next];
    const auto x = index % grid.size.width;
    const auto y = index / grid.size.width;
    const auto visit = [&](std::size_t neighbor) {
      if (eligible[neighbor] && !sky[neighbor]) {
        sky[neighbor] = true;
        queue.push_back(neighbor);
      }
    };
    if (x > 0)
      visit(index - 1);
    if (x + 1 < grid.size.width)
      visit(index + 1);
    if (y > 0)
      visit(index - grid.size.width);
    if (y + 1 < grid.size.height)
      visit(index + grid.size.width);
  }
  return sky;
}

struct region_selection {
  std::vector<float> mask;
  sunlight_report report;
  float threshold{};
};

region_selection select_region(const analysis_grid &grid) {
  region_selection selected{std::vector<float>(grid.colors.size()), {}, 0};
  const auto illumination = blur(grid.log_luminance, grid.size, 2);
  const float median = quantile(illumination, 0.5f);
  const float upper = quantile(illumination, 0.85f);
  if (upper - quantile(illumination, 0.15f) < 0.35f)
    return selected; // Flat illumination is not evidence of direct sunlight.

  const auto sky = sky_exclusion(grid, median);
  selected.threshold = median + 0.3f * (upper - median);
  // Use the darker half as an explicit chromatic reference, rather than equating
  // low saturation with missing sunlight. Both luminance and chroma separation matter.
  float shadow_lightness = 0;
  float shadow_chroma = 0;
  std::size_t shadow_count = 0;
  for (std::size_t index = 0; index < grid.colors.size(); ++index) {
    if (illumination[index] > median)
      continue;
    const auto perceptual = to_oklab(grid.colors[index]);
    shadow_lightness += perceptual.l;
    shadow_chroma += chroma(perceptual);
    ++shadow_count;
  }
  contract_assert(shadow_count > 0);
  shadow_lightness /= shadow_count;
  shadow_chroma /= shadow_count;

  const auto surroundings = blur(illumination, grid.size, 12);
  std::vector<bool> visited(illumination.size());
  float best_score = 0;

  // A four-connected bright region must occupy a meaningful area and contrast with shadows.
  for (std::size_t seed = 0; seed < illumination.size(); ++seed) {
    if (sky[seed] || visited[seed] || illumination[seed] <= selected.threshold)
      continue;
    std::vector<std::size_t> region{seed};
    visited[seed] = true;
    for (std::size_t next = 0; next < region.size(); ++next) {
      const auto index = region[next];
      const auto x = index % grid.size.width;
      const auto y = index / grid.size.width;
      const auto visit = [&](std::size_t neighbor) {
        if (!sky[neighbor] && !visited[neighbor] &&
            illumination[neighbor] > selected.threshold) {
          visited[neighbor] = true;
          region.push_back(neighbor);
        }
      };
      if (x > 0)
        visit(index - 1);
      if (x + 1 < grid.size.width)
        visit(index + 1);
      if (y > 0)
        visit(index - grid.size.width);
      if (y + 1 < grid.size.height)
        visit(index + grid.size.width);
    }

    const float area = static_cast<float>(region.size()) / illumination.size();
    if (area < minimum_region_fraction || area > maximum_region_fraction)
      continue;

    float average_light = 0;
    float local_contrast = 0;
    float average_chroma = 0;
    float average_lightness = 0;
    std::size_t blue_pixels = 0;
    bool touches_top = false;
    bool touches_bottom = false;
    float horizontal_variation = 0;
    float vertical_variation = 0;
    float mean_row = 0;
    for (const auto index : region) {
      const auto x = index % grid.size.width;
      const auto y = index / grid.size.width;
      touches_bottom = touches_bottom || y + 1 == grid.size.height;
      mean_row += y;
      if (x + 1 < grid.size.width)
        horizontal_variation +=
            std::abs(grid.log_luminance[index + 1] - grid.log_luminance[index]);
      if (y + 1 < grid.size.height)
        vertical_variation +=
            std::abs(grid.log_luminance[index + grid.size.width] - grid.log_luminance[index]);
      const auto pixel = grid.colors[index];
      const auto perceptual = to_oklab(pixel);
      average_light += illumination[index];
      local_contrast += illumination[index] - surroundings[index];
      average_chroma += chroma(perceptual);
      average_lightness += perceptual.l;
      blue_pixels += pixel.b > 1.15f * pixel.r && pixel.b > 1.05f * pixel.g;
      touches_top = touches_top || index < grid.size.width;
    }
    const float count = static_cast<float>(region.size());
    average_light /= count;
    local_contrast /= count;
    average_chroma /= count;
    average_lightness /= count;

    // A conservative sky heuristic: reject blue regions and neutral top-connected regions.
    // This is deliberately inspectable; textured clouds can still fool the first version.
    const float relative_chroma = average_chroma / std::max(average_lightness, 0.01f);
    if (blue_pixels > 0.6f * count || (touches_top && relative_chroma < 0.04f))
      continue;
    // Water glints commonly make horizontal bands: large variation between rows,
    // little variation along a row, and contact with the lower image boundary.
    if (touches_bottom && mean_row / count > 0.5f * grid.size.height &&
        vertical_variation > 1.4f * horizontal_variation)
      continue;

    const float contrast = average_light - median;
    if (contrast < 0.3f || local_contrast < 0.04f)
      continue;

    ++selected.report.candidate_regions;
    const float confidence = smooth_step(0.3f, 1.5f, contrast) * smooth_step(0.f, 0.04f, area) *
                             smooth_step(0.f, 0.25f, local_contrast);
    const float score = confidence * std::sqrt(area);
    if (score <= best_score)
      continue;

    best_score = score;
    std::ranges::fill(selected.mask, 0.f);
    for (const auto index : region)
      selected.mask[index] = 1.f;
    selected.report.selected_area_fraction = area;
    selected.report.confidence = confidence;
    selected.report.luminance_contrast_ev = contrast;
    // Strong illumination with little relative color evidence receives the greater gain.
    const float color_separation = (average_chroma - shadow_chroma) /
                                   std::max(average_lightness - shadow_lightness, 0.01f);
    selected.report.chroma_deficit = (1.f - smooth_step(0.04f, 0.25f, color_separation)) *
                                     (1.f - smooth_step(0.12f, 0.25f, relative_chroma));
  }

  if (selected.report.confidence < 0.08f) {
    std::ranges::fill(selected.mask, 0.f);
    return selected;
  }
  selected.mask = blur(selected.mask, grid.size, 2);
  selected.report.applied = true;
  return selected;
}

float interpolate_mask(const std::vector<float> &mask, const analysis_grid &grid, std::size_t x,
                       std::size_t y) {
  const float grid_x =
      std::clamp((x + 0.5f) / grid.scale - 0.5f, 0.f, static_cast<float>(grid.size.width - 1));
  const float grid_y =
      std::clamp((y + 0.5f) / grid.scale - 0.5f, 0.f, static_cast<float>(grid.size.height - 1));
  const auto left = static_cast<std::size_t>(grid_x);
  const auto top = static_cast<std::size_t>(grid_y);
  const auto right = std::min(left + 1, grid.size.width - 1);
  const auto bottom = std::min(top + 1, grid.size.height - 1);
  const auto a = std::lerp(mask[top * grid.size.width + left],
                           mask[top * grid.size.width + right], grid_x - left);
  const auto b = std::lerp(mask[bottom * grid.size.width + left],
                           mask[bottom * grid.size.width + right], grid_x - left);
  return std::lerp(a, b, grid_y - top);
}

linear_rgb enhance_pixel(linear_rgb source, float amount, float chroma_gain) {
  const float minimum = std::min({source.r, source.g, source.b});
  const float maximum = std::max({source.r, source.g, source.b});
  if (minimum < 0 || maximum >= 1 || amount == 0)
    return source;

  auto perceptual = to_oklab(source);
  const float color_evidence = smooth_step(0.005f, 0.06f, chroma(perceptual));
  const float headroom = 1.f - smooth_step(0.65f, 0.98f, maximum);
  const float weight = amount * headroom;
  perceptual.a *= 1.f + chroma_gain * weight * color_evidence;
  perceptual.b *= 1.f + chroma_gain * weight * color_evidence;
  perceptual.b += 0.002f * weight * color_evidence; // Subtle warmth, never added to neutrals.
  perceptual.l *= std::exp2(0.06f * weight / 3.f);  // At most 0.06 EV at unit strength.
  const auto candidate = from_oklab(perceptual);

  // Reduce the whole proposed change to fit the output gamut instead of clipping channels.
  float blend = 1.f;
  for (const auto &[before, after] :
       {std::pair{source.r, candidate.r}, {source.g, candidate.g}, {source.b, candidate.b}}) {
    const float delta = after - before;
    if (delta > 0)
      blend = std::min(blend, (1.f - before) / delta);
    if (delta < 0)
      blend = std::min(blend, -before / delta);
  }
  return {std::lerp(source.r, candidate.r, blend), std::lerp(source.g, candidate.g, blend),
          std::lerp(source.b, candidate.b, blend)};
}
} // namespace

result<sunlight_result> enhance_sunlight(image_view<const linear_rgb> source,
                                         sunlight_options options) {
  if (source.size().width == 0 || !std::isfinite(options.strength) || options.strength < 0 ||
      options.strength > 2)
    return fail(error_code::invalid_argument, "enhance sunlight",
                "need a nonempty image and finite strength in [0,2]");

  auto grid = make_analysis_grid(source);
  if (!grid)
    return std::unexpected(grid.error());
  auto selection = select_region(*grid);
  if (options.strength == 0)
    selection.report.applied = false;

  sunlight_result output{image<linear_rgb>(source.size()), image<gray>(source.size()),
                         selection.report};
  const float chroma_gain = 0.18f * selection.report.chroma_deficit;
  output.report.maximum_chroma_gain =
      output.report.applied ? 1.f + options.strength * chroma_gain : 1.f;
  for (std::size_t y = 0; y < source.size().height; ++y) {
    auto pixels = output.pixels.view().row(y);
    auto mask = output.mask.view().row(y);
    for (std::size_t x = 0; x < source.size().width; ++x) {
      const auto original = source.at(x, y);
      float weight = 0;
      if (output.report.applied) {
        const float light = std::log2(std::max(luminance(original), 1e-8f));
        weight = interpolate_mask(selection.mask, *grid, x, y) *
                 smooth_step(selection.threshold - 0.75f, selection.threshold + 0.25f, light);
      }
      mask[x].value = weight;
      pixels[x] = enhance_pixel(original, weight * options.strength, chroma_gain);
    }
  }
  return output;
}
} // namespace photo_ops
