// SPDX-License-Identifier: GPL-3.0-or-later
#include <cmath>
#include <expected>
#include <limits>
#include <stdexcept>
import photo_ops.image;
import photo_ops.color;
import photo_ops.sunlight;

namespace {
namespace photo = photo_ops;

void check(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

photo::image<photo::linear_rgb> scene(photo::linear_rgb foreground = {0.34f, 0.30f, 0.25f}) {
  photo::image<photo::linear_rgb> input({96, 64});
  for (std::size_t y = 0; y < 64; ++y) {
    for (std::size_t x = 0; x < 96; ++x) {
      input.view().at(x, y) = {0.05f, 0.045f, 0.04f};
      if (x >= 24 && x < 72 && y >= 20 && y < 48)
        input.view().at(x, y) = foreground;
    }
  }
  return input;
}

void unchanged(photo::image_view<const photo::linear_rgb> before,
               photo::image_view<const photo::linear_rgb> after) {
  for (std::size_t y = 0; y < before.size().height; ++y) {
    for (std::size_t x = 0; x < before.size().width; ++x) {
      const auto a = before.at(x, y), b = after.at(x, y);
      check(a.r == b.r && a.g == b.g && a.b == b.b, "no-op must preserve pixels exactly");
    }
  }
}
} // namespace

void sunlight_tests() {
  auto input = scene();
  auto enhanced = photo::enhance_sunlight(input.view());
  check(enhanced.has_value(), "sunlight result");
  check(enhanced->report.applied, "coherent colored bright region should be selected");
  check(enhanced->report.selected_area_fraction > 0.1f, "coherent region area");
  const auto original = input.view().at(48, 32);
  const auto changed = enhanced->pixels.view().at(48, 32);
  check(photo::luminance(changed) > photo::luminance(original), "sunlit midtone lift");
  const auto before_lab = photo::to_oklab(original);
  const auto after_lab = photo::to_oklab(changed);
  check(std::hypot(after_lab.a, after_lab.b) > std::hypot(before_lab.a, before_lab.b),
        "enhance existing color evidence");
  check(enhanced->mask.view().at(48, 32).value > 0.9f, "mask interior");
  check(enhanced->mask.view().at(0, 0).value == 0, "mask excludes distant shadows");
  const auto shadow = enhanced->pixels.view().at(0, 0);
  check(shadow.r == 0.05f && shadow.g == 0.045f && shadow.b == 0.04f, "preserve shadows");

  auto zero = photo::enhance_sunlight(input.view(), {.strength = 0});
  check(zero.has_value() && !zero->report.applied, "zero strength disables application");
  unchanged(input.view(), zero->pixels.view());

  auto neutral = scene({0.4f, 0.4f, 0.4f});
  auto neutral_result = photo::enhance_sunlight(neutral.view());
  check(neutral_result.has_value(), "neutral result");
  const auto neutral_pixel = neutral_result->pixels.view().at(48, 32);
  check(std::abs(neutral_pixel.r - neutral_pixel.b) < 1e-5f, "do not invent neutral warmth");

  auto clipped = scene({1.2f, 1.1f, 1.f});
  auto protected_result = photo::enhance_sunlight(clipped.view());
  check(protected_result.has_value(), "HDR input");
  unchanged(clipped.view(), protected_result->pixels.view());

  // Uniform illumination, a tiny specular highlight, and neutral/blue skies are no-ops.
  photo::image<photo::linear_rgb> flat({96, 64});
  for (std::size_t y = 0; y < 64; ++y)
    for (auto &pixel : flat.view().row(y))
      pixel = {0.2f, 0.2f, 0.2f};
  auto flat_result = photo::enhance_sunlight(flat.view());
  check(flat_result.has_value() && !flat_result->report.applied, "flat scene is not sunlight");
  unchanged(flat.view(), flat_result->pixels.view());
  flat.view().at(48, 32) = {1, 1, 1};
  auto specular = photo::enhance_sunlight(flat.view());
  check(specular.has_value() && !specular->report.applied, "reject tiny highlight");

  for (auto sky : {photo::linear_rgb{0.7f, 0.7f, 0.7f}, {0.1f, 0.3f, 0.7f}}) {
    auto landscape = scene();
    for (std::size_t y = 0; y < 64; ++y)
      for (auto &pixel : landscape.view().row(y))
        pixel = y < 20 ? sky : photo::linear_rgb{0.04f, 0.04f, 0.04f};
    auto result = photo::enhance_sunlight(landscape.view());
    check(result.has_value() && !result->report.applied, "reject top-connected sky");
  }

  // Real landscape review found bright detached clouds beneath blue sky could be selected.
  auto cloudy = scene();
  for (std::size_t y = 0; y < 64; ++y)
    for (std::size_t x = 0; x < 96; ++x)
      cloudy.view().at(x, y) = y < 40 ? photo::linear_rgb{0.1f, 0.25f, 0.6f}
                                      : photo::linear_rgb{0.03f, 0.03f, 0.03f};
  for (std::size_t y = 10; y < 30; ++y)
    for (std::size_t x = 20; x < 76; ++x)
      cloudy.view().at(x, y) = {0.7f, 0.7f, 0.7f};
  auto clouds = photo::enhance_sunlight(cloudy.view());
  check(clouds.has_value() && !clouds->report.applied, "exclude bright cloud in connected sky");

  // Bright horizontal water-like bands at the lower edge must not win region selection.
  auto water = scene();
  for (std::size_t y = 0; y < 64; ++y)
    for (std::size_t x = 0; x < 96; ++x) {
      const float ripple = y % 2 == 0 ? 0.15f : 0.f;
      water.view().at(x, y) =
          x >= 20 && x < 76 && y >= 40
              ? photo::linear_rgb{0.35f + ripple, 0.30f + ripple, 0.25f + ripple}
              : photo::linear_rgb{0.03f, 0.03f, 0.03f};
    }
  auto reflections = photo::enhance_sunlight(water.view());
  check(reflections.has_value() && !reflections->report.applied,
        "reject lower-edge horizontal specular bands");

  // Invalid external data must return errors, never terminate via a contract.
  check(!photo::enhance_sunlight(input.view(), {.strength = -1}), "reject invalid strength");
  input.view().at(0, 0).r = std::numeric_limits<float>::quiet_NaN();
  check(!photo::enhance_sunlight(input.view()), "reject NaN image");
  photo::image<photo::linear_rgb> empty;
  check(!photo::enhance_sunlight(empty.view()), "reject empty image");
}
