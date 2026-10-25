// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <tiffio.h>
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
    auto pattern = (fs::temp_directory_path() / "photo-focus-tests-XXXXXX").string();
    check(::mkdtemp(pattern.data()), "focus test directory");
    path = pattern;
  }

  ~workspace() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

void registration_tests() {
  struct spot {
    double x, y, radius, amplitude;
  };

  std::vector<spot> spots;
  std::uint32_t rng = 8675309;
  auto random = [&] {
    rng = 1664525 * rng + 1013904223;
    return (rng >> 8) / 16777216.;
  };
  for (int i = 0; i < 180; ++i)
    spots.push_back({random() * 400, random() * 320, 1.5 + random() * 6, .05 + random() * .3});
  auto scene = [&](double x, double y) {
    double result = .08;
    for (const auto &s : spots) {
      const double dx = x - s.x, dy = y - s.y;
      result += s.amplitude * std::exp(-(dx * dx + dy * dy) / (s.radius * s.radius));
    }
    return result;
  };
  p::image<p::gray> reference({400, 320}), source({400, 320});
  const double a = 1.015 * std::cos(.015), b = 1.015 * std::sin(.015);
  for (std::size_t y = 0; y < 320; ++y)
    for (std::size_t x = 0; x < 400; ++x) {
      reference.view().at(x, y).value = scene(x, y);
      source.view().at(x, y).value = scene(a * x - b * y + 3, b * x + a * y - 2);
    }
  // Verify registration through the photographic entry point and its diagnostics.
  workspace temp;
  p::image<p::linear_rgb> ref_rgb({400, 320}), src_rgb({400, 320});
  for (std::size_t y = 0; y < 320; ++y)
    for (std::size_t x = 0; x < 400; ++x) {
      const float r = reference.view().at(x, y).value;
      const float v = source.view().at(x, y).value;
      ref_rgb.view().at(x, y) = {r, r, r};
      src_rgb.view().at(x, y) = {v, v, v};
    }
  const std::vector<std::string> sequence{(temp.path / "ref.tif").string(),
                                          (temp.path / "src.tif").string()};
  done(p::write_image(sequence[0], ref_rgb.view()));
  done(p::write_image(sequence[1], src_rgb.view()));
  p::focus_stack_options options;
  options.reference_index = 0;
  options.proxy_long_edge = 200;
  options.memory_budget_mib = 512;
  options.tile_size = 128;
  options.scratch_directory = temp.path.string();
  const auto stacked =
      take(p::stack_focus(sequence, (temp.path / "stack.tif").string(), options));
  check(stacked.proxy_step == 2 && std::abs(stacked.frames[1].alignment.tx - 3) < .8 &&
            std::abs(stacked.frames[1].alignment.ty + 2) < .8,
        "proxy alignment must be conjugated into full-resolution pixel centers");
  const auto &transform = stacked.frames[1].alignment;
  check(std::abs(transform.a - a) < .003 && std::abs(transform.b - b) < .003,
        "stack diagnostics must recover rotation and breathing");
  check(transform.inliers >= 8 && transform.residual_px < 2,
        "stack diagnostics must expose similarity consensus and full-res residual");
  check(stacked.refinement_step == 1 && transform.refined && transform.refinement_samples > 100,
        "alignment must refine on a higher-resolution proxy with dense evidence");
  for (double y : {32., 160., 288.})
    for (double x : {32., 200., 368.}) {
      const double dx = (transform.a - a) * x - (transform.b - b) * y + transform.tx - 3;
      const double dy = (transform.b - b) * x + (transform.a - a) * y + transform.ty + 2;
      check(std::hypot(dx, dy) < .15,
            "refined breathing/rotation must have subpixel full-res geometric error");
    }
  check(transform.dense_residual_px < .7 && transform.dense_correlation > .99,
        "dense post-registration evidence must agree with known synthetic geometry");
  const auto again =
      take(p::stack_focus(sequence, (temp.path / "again.tif").string(), options));
  check(transform.a == again.frames[1].alignment.a &&
            transform.tx == again.frames[1].alignment.tx,
        "stack registration must be deterministic");

  // The opposite breathing/rotation direction plus fractional-pixel motion.
  const double small_a = .997 * std::cos(-.002), small_b = .997 * std::sin(-.002);
  for (std::size_t y = 0; y < 320; ++y)
    for (std::size_t x = 0; x < 400; ++x) {
      const float v = scene(small_a * x - small_b * y + .31, small_b * x + small_a * y - .27);
      src_rgb.view().at(x, y) = {v, v, v};
    }
  const auto fractional = (temp.path / "fractional.tif").string();
  done(p::write_image(fractional, src_rgb.view()));
  const auto micro = take(p::stack_focus(std::vector<std::string>{sequence[0], fractional},
                                         (temp.path / "micro.tif").string(), options));
  const auto &m = micro.frames[1].alignment;
  for (double y : {32., 288.})
    for (double x : {32., 368.})
      check(std::hypot((m.a - small_a) * x - (m.b - small_b) * y + m.tx - .31,
                       (m.b - small_b) * x + (m.a - small_a) * y + m.ty + .27) < .15,
            "subpixel translation and opposite rotation/breathing must be recovered");

  p::image<p::linear_rgb> flat({64, 64}), other({64, 64});
  for (std::size_t y = 0; y < 64; ++y)
    for (auto &pixel : other.view().row(y))
      pixel = {.2f, .2f, .2f};
  const std::vector<std::string> featureless{(temp.path / "flat.tif").string(),
                                             (temp.path / "other.tif").string()};
  done(p::write_image(featureless[0], flat.view()));
  done(p::write_image(featureless[1], other.view()));
  const auto failed = p::stack_focus(featureless, (temp.path / "failed.tif").string(), options);
  check(!failed && failed.error().operation == "focus registration",
        "stack must reject featureless mismatch instead of guessing identity");
  check(!fs::exists(temp.path / "failed.tif"), "failed registration cannot publish output");
}

void fusion_tests() {
  workspace temp;
  constexpr std::size_t width = 193, height = 129;
  p::image<p::linear_rgb> first({width, height}), second({width, height}),
      ideal({width, height});
  for (std::size_t y = 0; y < height; ++y)
    for (std::size_t x = 0; x < width; ++x) {
      const float texture = .12f * std::sin(.91 * x + .1) * std::cos(.83 * y + .2);
      const float base = .3f + .025f * std::sin(.03 * x);
      const float sharp = base + texture, blurred = base + .08f * texture;
      const auto color = [](float value) {
        return p::linear_rgb{value, value * .9f, value * .8f};
      };
      ideal.view().at(x, y) = color(sharp);
      first.view().at(x, y) = color(x < width / 2 ? sharp : blurred);
      second.view().at(x, y) = color(x < width / 2 ? blurred : sharp);
    }
  const std::vector<std::string> paths{(temp.path / "near.tif").string(),
                                       (temp.path / "far.tif").string()};
  done(p::write_image(paths[0], first.view()));
  done(p::write_image(paths[1], second.view()));
  p::focus_stack_options options;
  options.already_aligned = true;
  options.tile_size = 32;
  options.memory_budget_mib = 512;
  options.scratch_directory = temp.path.string();
  options.diagnostics_directory = (temp.path / "diagnostics").string();
  const auto small_path = (temp.path / "small.tif").string();
  auto report = take(p::stack_focus(paths, small_path, options));
  check(report.reference_index == 1 && report.dimensions.width == width,
        "default reference and full dimensions");
  check(report.motion_fraction < .15, "focus changes must not be classified as broad motion");
  auto stacked = take(p::read_image(small_path));
  double error = 0, blurred_error = 0;
  std::size_t count = 0;
  for (std::size_t y = 16; y < height - 16; ++y)
    for (std::size_t x = 16; x < width - 16; ++x) {
      if (std::abs(static_cast<int>(x) - static_cast<int>(width / 2)) < 20)
        continue;
      error += std::abs(stacked.pixels.view().at(x, y).r - ideal.view().at(x, y).r);
      blurred_error += std::abs((x < width / 2 ? second : first).view().at(x, y).r -
                                ideal.view().at(x, y).r);
      ++count;
    }
  check(error / count < .015 && error < blurred_error * .35,
        "fusion must recover both focus planes");
  check(fs::exists(temp.path / "diagnostics/report.json"),
        "focus diagnostics completion marker");
  check(fs::exists(temp.path / "diagnostics/source-map.tif"), "focus source map");
  options.diagnostics_directory.clear();
  options.tile_size = 4096;
  const auto whole_path = (temp.path / "whole.tif").string();
  take(p::stack_focus(paths, whole_path, options));
  auto whole = take(p::read_image(whole_path));
  for (std::size_t y = 0; y < height; ++y)
    for (std::size_t x = 0; x < width; ++x)
      check(std::abs(whole.pixels.view().at(x, y).r - stacked.pixels.view().at(x, y).r) < .0002,
            "tiled fusion must agree with whole-domain reference, including odd borders");
  // A moving bright patch straddles tile boundaries. Use the reference coherently.
  for (std::size_t y = 40; y < 84; ++y)
    for (std::size_t x = 50; x < 90; ++x)
      first.view().at(x, y) = {.85f, .12f, .08f};
  const auto moving = (temp.path / "moving.tif").string();
  done(p::write_image(moving, first.view()));
  const std::vector<std::string> motion_paths{moving, paths[1]};
  options.tile_size = 64;
  auto motion =
      take(p::stack_focus(motion_paths, (temp.path / "motion.tif").string(), options));
  check(motion.motion_fraction > .02,
        "moving region must reduce confidence and trigger fallback");
  auto protected_image = take(p::read_image((temp.path / "motion.tif").string()));
  check(std::abs(protected_image.pixels.view().at(64, 64).r - second.view().at(64, 64).r) <
            .0002,
        "moving region must preserve one coherent source exactly");
  const auto before = fs::file_size(small_path);
  check(!p::stack_focus(paths, small_path, options), "existing output must be rejected");
  check(fs::file_size(small_path) == before, "existing output must be preserved");
  options.memory_budget_mib = 64;
  check(!p::stack_focus(paths, (temp.path / "budget.tif").string(), options),
        "insufficient budget fails");
  check(!fs::exists(temp.path / "budget.tif"), "budget failure cannot publish an output");
  for (const auto &entry : fs::directory_iterator(temp.path))
    check(!entry.path().filename().string().starts_with("photo-ops-focus-"),
          "scratch must be cleaned up");
}

void memory_and_identity_tests() {
  workspace temp;
  p::image<p::linear_rgb> flat({65, 49});
  for (std::size_t y = 0; y < 49; ++y)
    for (auto &pixel : flat.view().row(y))
      pixel = {.2f, .3f, .4f};
  const auto source = (temp.path / "flat.tif").string();
  done(p::write_image(source, flat.view()));
  p::focus_stack_options options;
  options.memory_budget_mib = 512;
  options.scratch_directory = temp.path.string();
  const auto two = take(p::stack_focus(std::vector<std::string>(2, source),
                                       (temp.path / "two.tif").string(), options));
  const auto four = take(p::stack_focus(std::vector<std::string>(4, source),
                                        (temp.path / "four.tif").string(), options));
  for (const auto &frame : four.frames)
    check(frame.alignment.identity, "identical flat inputs use identity registration");
  check(two.estimated_peak_bytes == four.estimated_peak_bytes &&
            four.scratch_bytes > two.scratch_bytes,
        "frame count must grow scratch, not full-res resident RGB");
  auto result = take(p::read_image((temp.path / "four.tif").string()));
  for (std::size_t y = 0; y < 49; ++y)
    for (auto pixel : result.pixels.view().row(y))
      check(std::abs(pixel.r - .2f) < .0002 && std::abs(pixel.g - .3f) < .0002 &&
                std::abs(pixel.b - .4f) < .0002,
            "flat identity stack preserves all channels");

  const auto huge = (temp.path / "huge.tif").string();
  TIFF *file = TIFFOpen(huge.c_str(), "w");
  check(file != nullptr, "create large header fixture");
  TIFFSetField(file, TIFFTAG_IMAGEWIDTH, 10000);
  TIFFSetField(file, TIFFTAG_IMAGELENGTH, 10000);
  TIFFSetField(file, TIFFTAG_BITSPERSAMPLE, 8);
  TIFFSetField(file, TIFFTAG_SAMPLESPERPIXEL, 3);
  TIFFSetField(file, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
  TIFFSetField(file, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
  TIFFSetField(file, TIFFTAG_ROWSPERSTRIP, 1);
  std::vector<unsigned char> row(30000);
  check(TIFFWriteScanline(file, row.data(), 0) >= 0, "write only one large-fixture scanline");
  TIFFClose(file);
  const auto rejected = p::stack_focus(std::vector<std::string>(2, huge),
                                       (temp.path / "budget.tif").string(), options);
  check(!rejected && rejected.error().operation == "focus memory budget",
        "header budget must reject a huge decoder before allocating its full image");
  check(!fs::exists(temp.path / "budget.tif"), "budget preflight cannot publish output");
}

void streaming_tests() {
  workspace temp;
  const auto failed = (temp.path / "failed.tif").string();
  auto written = p::write_tiff_rows(
      failed, {32, 32}, [](std::size_t y, std::span<p::linear_rgb> row) -> p::result<void> {
        if (y == 3)
          return p::fail(p::error_code::io, "fixture", "source read failure");
        for (auto &pixel : row)
          pixel = {.2, .3, .4};
        return {};
      });
  check(!written && !fs::exists(failed),
        "stream source failure must not publish a partial TIFF");
  check(fs::is_empty(temp.path), "stream source failure must clean its temporary file");
}
} // namespace

void focus_stack_tests() {
  registration_tests();
  fusion_tests();
  memory_and_identity_tests();
  streaming_tests();
}
