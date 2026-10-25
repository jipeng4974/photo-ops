// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
import photo_ops.image;
import photo_ops.io;
import photo_ops.focus_stack;

namespace {
namespace p = photo_ops;
namespace fs = std::filesystem;

void check(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}

template <class T> T take(p::result<T> result) {
  if (!result)
    throw std::runtime_error(result.error().operation + ": " + result.error().message);
  return std::move(*result);
}

void done(p::result<void> result) {
  if (!result)
    throw std::runtime_error(result.error().operation + ": " + result.error().message);
}

struct workspace {
  fs::path path;

  workspace() {
    auto pattern = (fs::temp_directory_path() / "photo-defocus-tests-XXXXXX").string();
    check(::mkdtemp(pattern.data()), "defocus fixture directory");
    path = pattern;
  }

  ~workspace() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

// Independent Gaussian image-formation fixture: a defocused occluder spreads alpha over
// a focused background. The operator's blur model is three box passes, not this renderer.
p::image<p::gray> gaussian(p::image_view<const p::gray> source, double sigma) {
  const long radius = std::ceil(3 * sigma);
  std::vector<double> kernel(2 * radius + 1);
  double sum = 0;
  for (long k = -radius; k <= radius; ++k)
    sum += kernel[k + radius] = std::exp(-k * k / (2 * sigma * sigma));
  for (auto &v : kernel)
    v /= sum;
  const auto size = source.size();
  p::image<p::gray> horizontal(size), output(size);
  const auto reflect = [](long v, long n) { return v < 0 ? -v : v >= n ? 2 * n - 2 - v : v; };
  for (std::size_t y = 0; y < size.height; ++y)
    for (std::size_t x = 0; x < size.width; ++x) {
      double v = 0;
      for (long k = -radius; k <= radius; ++k)
        v += kernel[k + radius] * source.at(reflect(x + k, size.width), y).value;
      horizontal.view().at(x, y).value = v;
    }
  for (std::size_t y = 0; y < size.height; ++y)
    for (std::size_t x = 0; x < size.width; ++x) {
      double v = 0;
      for (long k = -radius; k <= radius; ++k)
        v += kernel[k + radius] * horizontal.view().at(x, reflect(y + k, size.height)).value;
      output.view().at(x, y).value = v;
    }
  return output;
}

void occlusion_case(int shape, double sigma, double baseline, double exposure = 1,
                    bool chromatic = false, bool bright_foreground = false) {
  workspace temp;
  constexpr p::extent size{385, 257};
  p::image<p::gray> alpha(size), foreground(size), background(size);
  for (std::size_t y = 0; y < size.height; ++y)
    for (std::size_t x = 0; x < size.width; ++x) {
      const double xx = x, yy = y;
      const bool mask = shape == 0 ? std::abs(xx - 120 - 12 * std::sin(yy / 38)) < 6 ||
                                         (y < 145 && std::abs(xx - 120 - (145 - yy) * .65) < 4)
                        : shape == 1 ? std::abs(xx - 96) < 5 || std::abs(xx - 239) < 5 ||
                                           std::abs(yy - 134) < 5
                                     : y > 173 + 25 * std::sin(xx / 32) && x < 270;
      alpha.view().at(x, y).value = mask;
      foreground.view().at(x, y).value =
          mask * ((bright_foreground ? .72 : .12) + .025 * std::sin(1.1 * xx + .45 * yy)) *
          exposure;
      background.view().at(x, y).value =
          ((bright_foreground ? .22 : .65) +
           (shape == 0 ? .025 : .10) * std::sin(.81 * xx + .2) * std::cos(.74 * yy + .1)) *
          exposure;
    }
  auto blurred_alpha = gaussian(alpha.view(), sigma),
       blurred_foreground = gaussian(foreground.view(), sigma),
       blurred_background = gaussian(background.view(), sigma);
  p::image<p::linear_rgb> near(size), far(size), ideal(size);
  const std::array<float, 3> fg =
      chromatic ? std::array{.65f, .9f, .45f} : std::array{1.f, .95f, .9f};
  const std::array<float, 3> bg =
      chromatic ? std::array{.8f, .9f, 1.f} : std::array{1.f, .95f, .9f};
  for (std::size_t y = 0; y < size.height; ++y)
    for (std::size_t x = 0; x < size.width; ++x) {
      const float a = alpha.view().at(x, y).value, b = blurred_alpha.view().at(x, y).value;
      const float f = foreground.view().at(x, y).value, g = background.view().at(x, y).value;
      const float bf = blurred_foreground.view().at(x, y).value,
                  bb = blurred_background.view().at(x, y).value;
      near.view().at(x, y) = {fg[0] * f + bg[0] * (1 - a) * bb,
                              fg[1] * f + bg[1] * (1 - a) * bb,
                              fg[2] * f + bg[2] * (1 - a) * bb};
      far.view().at(x, y) = {fg[0] * bf + bg[0] * (1 - b) * g, fg[1] * bf + bg[1] * (1 - b) * g,
                             fg[2] * bf + bg[2] * (1 - b) * g};
      ideal.view().at(x, y) = {fg[0] * f + bg[0] * (1 - a) * g, fg[1] * f + bg[1] * (1 - a) * g,
                               fg[2] * f + bg[2] * (1 - a) * g};
    }
  const auto near_path = (temp.path / "near.tif").string(),
             far_path = (temp.path / "far.tif").string();
  done(p::write_image(near_path, near.view()));
  done(p::write_image(far_path, far.view()));
  p::focus_stack_options options;
  options.already_aligned = true;
  options.memory_budget_mib = 512;
  options.tile_size = 128;
  options.scratch_directory = temp.path.string();
  const auto output = (temp.path / "stack.tif").string();
  auto report =
      take(p::stack_focus(std::vector<std::string>{near_path, far_path}, output, options));
  auto result = take(p::read_image(output));
  double error = 0;
  std::size_t count = 0;
  for (std::size_t y = 0; y < size.height; ++y)
    for (std::size_t x = 0; x < size.width; ++x) {
      const float a = blurred_alpha.view().at(x, y).value;
      if (a > .015 && a < .985) {
        error += std::abs(result.pixels.view().at(x, y).r - ideal.view().at(x, y).r);
        ++count;
      }
    }
  error /= count;
  std::cout << "defocus shape=" << shape << " sigma=" << sigma << " exposure=" << exposure
            << " chromatic=" << chromatic << " bright=" << bright_foreground
            << " boundary_mae=" << error
            << " guard_fraction=" << report.defocus_boundary_fraction << '\n';
  check(report.defocus_boundary_fraction > .002,
        "static occlusion must expose its boundary policy");
  check(report.motion_fraction < .001,
        "static defocus spread must be distinguished from motion");
  check(error < (sigma >= 4 ? baseline * .75 : baseline * 1.08) * exposure,
        "defocus-aware fusion must reduce halo/transparent rim across shapes and nearby blur "
        "scales");
  if (shape == 0 && bright_foreground)
    for (std::size_t y = 32; y < 224; ++y) {
      const auto x = static_cast<std::size_t>(120 + 12 * std::sin(y / 38.));
      check(result.pixels.view().at(x, y).r > .63, "bright foreground must also remain opaque");
    }
  if (shape == 0 && sigma == 7 && exposure == 1 && !chromatic && !bright_foreground) {
    // Strong geometry assertion: the fine foreground remains dark and opaque.
    for (std::size_t y = 32; y < 224; ++y) {
      const auto x = static_cast<std::size_t>(120 + 12 * std::sin(y / 38.));
      check(result.pixels.view().at(x, y).r < .19,
            "foreground branch must preserve its opaque geometry");
    }
    options.tile_size = 4096;
    const auto whole_path = (temp.path / "whole.tif").string();
    take(p::stack_focus(std::vector<std::string>{near_path, far_path}, whole_path, options));
    auto whole = take(p::read_image(whole_path));
    for (std::size_t y = 0; y < size.height; ++y)
      for (std::size_t x = 0; x < size.width; ++x)
        check(std::abs(whole.pixels.view().at(x, y).r - result.pixels.view().at(x, y).r) <
                  .0002,
              "global defocus policy must be independent of tile boundaries");
    options.proxy_long_edge = 128;
    const auto coarse_path = (temp.path / "coarse.tif").string();
    const auto coarse_report = take(
        p::stack_focus(std::vector<std::string>{near_path, far_path}, coarse_path, options));
    auto coarse = take(p::read_image(coarse_path));
    check(coarse_report.proxy_step == 4 && coarse_report.defocus_boundary_fraction > .002,
          "boundary policy must survive reduced proxies");
    for (std::size_t y = 32; y < 224; ++y) {
      const auto x = static_cast<std::size_t>(120 + 12 * std::sin(y / 38.));
      check(coarse.pixels.view().at(x, y).r < .19,
            "coarse guard interpolation must preserve full-res foreground geometry");
    }
    options.proxy_long_edge = 1024;
    options.reference_index = 0;
    const auto reversed_path = (temp.path / "reversed.tif").string();
    auto reversed_report = take(
        p::stack_focus(std::vector<std::string>{far_path, near_path}, reversed_path, options));
    auto reversed = take(p::read_image(reversed_path));
    check(reversed_report.defocus_boundary_fraction > .002,
          "reversing input order must retain automatic boundary detection");
    for (std::size_t y = 32; y < 224; ++y) {
      const auto x = static_cast<std::size_t>(120 + 12 * std::sin(y / 38.));
      check(std::abs(reversed.pixels.view().at(x, y).r - result.pixels.view().at(x, y).r) <
                .0002,
            "boundary source identity cannot assume near-to-far filename order");
    }
    // A third frame moves through a region explained as defocus by the first pair.
    for (std::size_t y = 80; y < 120; ++y)
      for (std::size_t x = 100; x < 148; ++x)
        near.view().at(x, y) = {.9, .05, .03};
    const auto moving_path = (temp.path / "moving.tif").string();
    done(p::write_image(moving_path, near.view()));
    options.reference_index = 1;
    options.tile_size = 64;
    const auto moving_output = (temp.path / "motion.tif").string();
    auto moving = take(p::stack_focus(
        std::vector<std::string>{near_path, far_path, moving_path}, moving_output, options));
    auto protected_image = take(p::read_image(moving_output));
    check(moving.motion_fraction > .005,
          "independent moving source must veto another pair's defocus explanation");
    check(std::abs(protected_image.pixels.view().at(120, 100).r - far.view().at(120, 100).r) <
              .0002,
          "true motion overlapping a defocus boundary must use the coherent reference exactly");
  }
}

void overlapping_depth_bands(int phase) {
  workspace temp;
  constexpr p::extent size{385, 257};
  p::image<p::gray> alpha(size), foreground(size), background(size);
  for (std::size_t y = 0; y < size.height; ++y)
    for (std::size_t x = 0; x < size.width; ++x) {
      const bool mask = std::abs(double(x) - 120) < 6;
      alpha.view().at(x, y).value = mask;
      foreground.view().at(x, y).value = mask * (.12 + .02 * std::sin(.7 * y));
      background.view().at(x, y).value =
          .5 + .30 * std::sin(.4 * x + .7 * phase) + .04 * std::sin(.13 * y);
    }
  std::vector<std::string> paths;
  p::image<p::linear_rgb> middle(size);
  for (int frame = 0; frame < 3; ++frame) {
    auto a = frame == 0 ? alpha.clone() : gaussian(alpha.view(), frame == 1 ? 2.5 : 7.);
    auto f =
        frame == 0 ? foreground.clone() : gaussian(foreground.view(), frame == 1 ? 2.5 : 7.);
    auto b =
        frame == 2 ? background.clone() : gaussian(background.view(), frame == 0 ? 7. : 2.5);
    p::image<p::linear_rgb> pixels(size);
    for (std::size_t y = 0; y < size.height; ++y)
      for (std::size_t x = 0; x < size.width; ++x) {
        const float v =
            f.view().at(x, y).value + (1 - a.view().at(x, y).value) * b.view().at(x, y).value;
        pixels.view().at(x, y) = {v, .95f * v, .9f * v};
      }
    paths.push_back((temp.path / (std::to_string(frame) + ".tif")).string());
    done(p::write_image(paths.back(), pixels.view()));
    if (frame == 1)
      middle = std::move(pixels);
  }
  p::focus_stack_options options;
  options.already_aligned = true;
  options.reference_index = 1;
  options.memory_budget_mib = 512;
  options.scratch_directory = temp.path.string();
  const auto output = (temp.path / "stack.tif").string();
  take(p::stack_focus(paths, output, options));
  auto result = take(p::read_image(output));
  double error = 0, middle_error = 0;
  std::size_t count = 0;
  for (std::size_t y = 32; y < 225; ++y)
    for (std::size_t x = 145; x < 170; ++x) {
      const float ideal = background.view().at(x, y).value;
      error += std::abs(result.pixels.view().at(x, y).r - ideal);
      middle_error += std::abs(middle.view().at(x, y).r - ideal);
      ++count;
    }
  std::cout << "overlapping depth bands: background_mae=" << error / count
            << ", middle_mae=" << middle_error / count << '\n';
  check(error / count < .03 && error < middle_error * .4,
        "overlapping defocus bands must preserve the sharp far source instead of the first "
        "matching pair");
}

void thin_branch_motion() {
  workspace temp;
  constexpr p::extent size{385, 257};
  std::vector<std::string> paths;
  p::image<p::linear_rgb> reference(size);
  for (int frame = 0; frame < 2; ++frame) {
    p::image<p::linear_rgb> pixels(size);
    for (std::size_t y = 0; y < size.height; ++y)
      for (std::size_t x = 0; x < size.width; ++x) {
        const double value = std::abs(double(x) - 120 - 12 * std::sin(y / 38.) - 3 * frame) < 6
                                 ? .12
                                 : .65 + .02 * std::sin(.81 * x) * std::cos(.74 * y);
        pixels.view().at(x, y) = {float(value), float(.95 * value), float(.9 * value)};
      }
    paths.push_back((temp.path / (std::to_string(frame) + ".tif")).string());
    done(p::write_image(paths.back(), pixels.view()));
    if (frame == 1)
      reference = std::move(pixels);
  }
  p::focus_stack_options options;
  options.already_aligned = true;
  options.memory_budget_mib = 512;
  options.scratch_directory = temp.path.string();
  const auto output = (temp.path / "motion.tif").string();
  const auto report = take(p::stack_focus(paths, output, options));
  check(report.motion_fraction > .03 && report.defocus_boundary_fraction < .001,
        "a thin moving branch cannot be explained as stationary defocus spread");
  auto result = take(p::read_image(output));
  check(std::abs(result.pixels.view().at(115, 120).r - reference.view().at(115, 120).r) < .0002,
        "thin moving branch must retain one coherent reference geometry");
}
} // namespace

void defocus_tests() {
  constexpr double baseline[3][3] = {{.0053228641, .0741000672, .1115963138},
                                     {.0173856537, .0686028223, .1132312357},
                                     {.0185602781, .0308463486, .0792159600}};
  for (int shape = 0; shape < 3; ++shape)
    for (int blur = 0; blur < 3; ++blur)
      occlusion_case(shape, std::array{2., 4., 7.}[blur], baseline[shape][blur]);
  occlusion_case(0, 5, .09, .3);
  occlusion_case(1, 4, .0686028223, 1, true);
  occlusion_case(2, 7, .0792159600, 1, true);
  occlusion_case(0, 4, .0741000672, 1, false, true);
  occlusion_case(2, 7, .0792159600, 1, false, true);
  thin_branch_motion();
  for (int phase = 0; phase < 3; ++phase)
    overlapping_depth_bands(phase);
}
