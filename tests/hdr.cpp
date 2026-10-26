// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>
#include <tiffio.h>
#include <vector>
import photo_ops.image;
import photo_ops.color;
import photo_ops.raw;
import photo_ops.io;
import photo_ops.hdr;
import photo_ops.focus_stack;

namespace {
namespace p = photo_ops;
namespace fs = std::filesystem;

void check(bool value, const char *message) {
  if (!value)
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
    auto name = (fs::temp_directory_path() / "photo-hdr-tests-XXXXXX").string();
    check(::mkdtemp(name.data()), "HDR workspace");
    path = name;
  }

  ~workspace() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

using scene_function = std::function<double(double, double)>;

// Independent sensor simulator. Scene is measured in middle-shutter (.01s) signal units.
// Additive count noise precedes quantization; shutter and saturation are physical inputs.
void write_capture(const std::string &path, p::extent size, double shutter,
                   const scene_function &scene, int seed = 0, bool noise = false,
                   std::array<double, 3> camera_color = {1, 1, 1},
                   std::array<float, 3> camera_neutral = {1, 1, 1}) {
  TIFF *file = TIFFOpen(path.c_str(), "w");
  check(file, "create HDR DNG");
  TIFFCreateEXIFDirectory(file);
  TIFFSetField(file, EXIFTAG_EXPOSURETIME, float(shutter));
  TIFFSetField(file, EXIFTAG_FNUMBER, 4.);
  TIFFSetField(file, EXIFTAG_FOCALLENGTH, 85.);
  std::uint16_t iso = 100;
  TIFFSetField(file, EXIFTAG_ISOSPEEDRATINGS, 1, &iso);
  std::uint64_t exif = 0;
  check(TIFFWriteCustomDirectory(file, &exif), "HDR DNG EXIF");
  TIFFCreateDirectory(file);
  TIFFSetField(file, TIFFTAG_IMAGEWIDTH, std::uint32_t(size.width));
  TIFFSetField(file, TIFFTAG_IMAGELENGTH, std::uint32_t(size.height));
  TIFFSetField(file, TIFFTAG_BITSPERSAMPLE, 16);
  TIFFSetField(file, TIFFTAG_SAMPLESPERPIXEL, 1);
  TIFFSetField(file, TIFFTAG_PHOTOMETRIC, 32803);
  TIFFSetField(file, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
  TIFFSetField(file, TIFFTAG_ROWSPERSTRIP, size.height);
  TIFFSetField(file, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
  TIFFSetField(file, TIFFTAG_MAKE, "photo-ops");
  TIFFSetField(file, TIFFTAG_MODEL, "HDR sensor simulator");
  TIFFSetField(file, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
  TIFFSetField(file, TIFFTAG_EXIFIFD, exif);
  unsigned char version[]{1, 4, 0, 0};
  TIFFSetField(file, TIFFTAG_DNGVERSION, version);
  TIFFSetField(file, TIFFTAG_DNGBACKWARDVERSION, version);
  TIFFSetField(file, TIFFTAG_UNIQUECAMERAMODEL, "photo-ops HDR sensor simulator");
  unsigned short dimensions[]{2, 2};
  unsigned char cfa[]{0, 1, 1, 2};
  TIFFSetField(file, TIFFTAG_CFAREPEATPATTERNDIM, dimensions);
  TIFFSetField(file, TIFFTAG_CFAPATTERN, 4, cfa);
  float black = 512;
  unsigned white = 16383;
  TIFFSetField(file, TIFFTAG_BLACKLEVEL, 1, &black);
  TIFFSetField(file, TIFFTAG_WHITELEVEL, 1, &white);
  float matrix[]{1, 0, 0, 0, 1, 0, 0, 0, 1};
  TIFFSetField(file, TIFFTAG_COLORMATRIX1, 9, matrix);
  TIFFSetField(file, TIFFTAG_ASSHOTNEUTRAL, 3, camera_neutral.data());
  TIFFSetField(file, TIFFTAG_CALIBRATIONILLUMINANT1, 21);
  std::vector<std::uint16_t> row(size.width);
  for (std::size_t y = 0; y < size.height; ++y) {
    for (std::size_t x = 0; x < size.width; ++x) {
      const int noise_code = noise ? int((x * 13 + y * 7 + seed * 11) % 7) - 3 : 0;
      const unsigned c = cfa[2 * (y % 2) + x % 2];
      const double signal = scene(x, y) * camera_color[c] * shutter / .01;
      row[x] = std::clamp<long>(std::lround(black + (white - black) * signal + noise_code), 0,
                                white);
    }
    check(TIFFWriteScanline(file, row.data(), y) >= 0, "write HDR sensor row");
  }
  TIFFClose(file);
}

std::vector<std::string> captures(workspace &temp, p::extent size, const scene_function &scene,
                                  bool noise = false) {
  std::vector<std::string> paths;
  for (std::size_t i = 0; i < 3; ++i) {
    paths.push_back((temp.path / (std::to_string(i) + ".dng")).string());
    write_capture(paths.back(), size, std::array{.0025, .01, .04}[i], scene, i, noise);
  }
  return paths;
}

p::rgb_f32 expected_neutral(double signal, const p::raw_sequence_context &context) {
  const auto &m = context.reference().camera_to_xyz;
  return p::from_xyz({float(signal * (m[0] + m[1] + m[2])),
                      float(signal * (m[3] + m[4] + m[5])),
                      float(signal * (m[6] + m[7] + m[8]))},
                     context.encoding());
}

void scalar_io() {
  workspace temp;
  const auto path = (temp.path / "scalar.tif").string();
  done(p::write_scalar_tiff_rows(path, {19, 13}, "numerical regression",
                                 [](std::size_t y, std::span<p::gray> row) -> p::result<void> {
                                   for (std::size_t x = 0; x < row.size(); ++x)
                                     row[x].value = float(x) - float(y) - .5f;
                                   return {};
                                 }));
  auto scalar = take(p::read_scalar_tiff(path));
  check(scalar.meaning == "numerical regression", "scalar provenance roundtrip");
  for (std::size_t y = 0; y < 13; ++y)
    for (std::size_t x = 0; x < 19; ++x)
      check(scalar.pixels.view().at(x, y).value == float(x) - float(y) - .5f,
            "numeric scalar samples preserve signed/HDR values exactly");
  check(!p::read_image(path), "display reader must reject numeric float map");
  check(!p::write_scalar_tiff_rows(
            path, {1, 1}, "other",
            [](std::size_t, std::span<p::gray>) -> p::result<void> { return {}; }),
        "scalar no-overwrite");
  const auto failure = (temp.path / "failed.tif").string();
  check(!p::write_scalar_tiff_rows(failure, {3, 3}, "failure",
                                   [](std::size_t y, std::span<p::gray>) -> p::result<void> {
                                     if (y == 1)
                                       return p::fail(p::error_code::io, "fixture callback",
                                                      "injected row failure");
                                     return {};
                                   }),
        "scalar row failure");
  check(!fs::exists(failure), "failed scalar callback cannot publish image");
  check(!p::write_scalar_tiff_rows(failure, {1, 1}, "failure",
                                   [](std::size_t, std::span<p::gray> row) -> p::result<void> {
                                     row[0].value = std::numeric_limits<float>::quiet_NaN();
                                     return {};
                                   }),
        "scalar rejects nonfinite samples");
  check(!fs::exists(failure), "nonfinite scalar cannot publish image");
}

void radiometric_fusion() {
  workspace temp;
  const p::extent size{256, 160};
  const auto scene = [](double x, double y) {
    if (x < 16)
      return -.0005;
    if (x < 64)
      return .003 + .0004 * std::sin(.23 * x) * std::cos(.19 * y);
    if (x < 128)
      return .1 + .02 * std::sin(.11 * x) * std::cos(.13 * y);
    if (x < 208)
      return 1.6 + .15 * std::sin(.1 * x) * std::cos(.09 * y);
    return 6.;
  };
  const auto paths = captures(temp, size, scene, true);
  p::hdr_options options;
  options.already_aligned = true;
  options.tile_size = 32;
  options.memory_budget_mib = 512;
  options.scratch_directory = temp.path.string();
  options.diagnostics_directory = (temp.path / "diagnostics").string();
  const auto output = (temp.path / "hdr.tif").string();
  const auto report = take(p::merge_hdr(paths, output, options));
  check(report.reference_index == 1 && report.raw_context.has_value(),
        "HDR default reference is median shutter");
  check(report.estimated_peak_bytes <= 512 * 1024 * 1024 && report.profile.tiles == 40,
        "HDR planner and tile count");
  check(report.deghost_fraction < .005,
        "clipping and exposure differences alone must not become motion");
  auto image = take(p::read_working_tiff(output));
  auto confidence =
      take(p::read_scalar_tiff((temp.path / "diagnostics/confidence.tif").string()));
  auto clipped = take(p::read_scalar_tiff((temp.path / "diagnostics/clipping.tif").string()));
  auto shortest = take(p::decode_raw_frame(paths[0], *report.raw_context, 0));
  auto middle = take(p::decode_raw_frame(paths[1], *report.raw_context, 1));
  double shadow_error = 0, short_error = 0, highlight_error = 0, middle_error = 0;
  std::size_t shadows = 0, highlights = 0;
  for (std::size_t y = 12; y < size.height - 12; ++y)
    for (std::size_t x = 20; x < size.width - 12; ++x) {
      const auto expected = expected_neutral(scene(x, y), *report.raw_context);
      const auto actual = image.pixels.view().at(x, y);
      if (x < 56) {
        shadow_error += std::abs(actual.g - expected.g);
        short_error += std::abs(shortest.image.pixels.view().at(x, y).g - expected.g);
        ++shadows;
      }
      if (x > 140 && x < 196) {
        highlight_error += std::abs(actual.g - expected.g);
        middle_error += std::abs(middle.image.pixels.view().at(x, y).g - expected.g);
        ++highlights;
      }
    }
  std::cout << "HDR: shadow_mae=" << shadow_error / shadows
            << ", shortest=" << short_error / shadows
            << ", highlight_mae=" << highlight_error / highlights
            << ", middle=" << middle_error / highlights << std::endl;
  check(shadow_error < short_error * .5,
        "HDR shadows must improve over the shortest noisy measurement");
  check(highlight_error / highlights < .005 && highlight_error < middle_error * .05,
        "HDR must recover clipped middle-exposure highlights");
  check(image.pixels.view().at(8, 80).r < 0 && image.pixels.view().at(170, 80).g > 1,
        "HDR output must preserve signed and greater-than-one samples");
  check(confidence.pixels.view().at(170, 80).value > .5 &&
            confidence.pixels.view().at(230, 80).value == 0,
        "confidence must distinguish recovered and unrecoverable highlights");
  check(clipped.pixels.view().at(170, 80).value == 0 &&
            clipped.pixels.view().at(230, 80).value == 1,
        "clipping map must describe unmeasured radiance");
  auto low_snr = take(p::read_scalar_tiff((temp.path / "diagnostics/low-snr.tif").string()));
  check(low_snr.pixels.view().at(230, 80).value == 0 &&
            low_snr.pixels.view().at(8, 80).value == 1,
        "clipping and low-signal diagnostics must remain distinct");

  double contribution = 0;
  for (const auto &f : report.frames)
    contribution += f.contribution_fraction;
  check(std::abs(contribution - 1) < 1e-6, "HDR contribution fractions must sum to one");
  check(fs::exists(temp.path / "diagnostics/report.json"), "HDR diagnostics completion marker");
  options.tile_size = 4096;
  options.diagnostics_directory.clear();
  const auto whole = (temp.path / "whole.tif").string();
  take(p::merge_hdr(paths, whole, options));
  auto reference = take(p::read_working_tiff(whole));
  for (std::size_t y = 0; y < size.height; ++y)
    for (std::size_t x = 0; x < size.width; ++x) {
      const auto a = image.pixels.view().at(x, y), b = reference.pixels.view().at(x, y);
      check(a.r == b.r && a.g == b.g && a.b == b.b,
            "HDR tiled and whole-domain samples must agree exactly");
    }
  check(!p::merge_hdr(paths, output, options), "HDR existing output must be preserved");
  auto reordered = paths;
  std::reverse(reordered.begin(), reordered.end());
  const auto reordered_output = (temp.path / "reordered.tif").string();
  take(p::merge_hdr(reordered, reordered_output, options));
  auto reverse = take(p::read_working_tiff(reordered_output));
  for (std::size_t y = 0; y < size.height; ++y)
    for (std::size_t x = 0; x < size.width; ++x) {
      const auto a = image.pixels.view().at(x, y), b = reverse.pixels.view().at(x, y);
      check(std::abs(a.r - b.r) < 2e-6 && std::abs(a.g - b.g) < 2e-6 &&
                std::abs(a.b - b.b) < 2e-6,
            "AEB caller order cannot change radiance semantics");
    }
  p::raw_sequence_options settings;
  settings.exposure_policy = p::raw_exposure_policy::shutter_bracket;
  settings.reference_index = 0;
  auto context = take(p::prepare_raw_sequence(paths, settings));
  const auto shared = (temp.path / "shared-context.tif").string();
  const auto shared_report =
      take(p::merge_hdr(context, std::array<std::size_t, 3>{0, 1, 2}, shared, options));
  auto normalized = take(p::read_working_tiff(shared));
  check(shared_report.raw_context->fingerprint() == context.fingerprint(),
        "supplied HDR context cannot be independently rebuilt");
  for (std::size_t y = 12; y < size.height - 12; y += 9)
    for (std::size_t x = 20; x < size.width - 12; x += 9)
      check(std::abs(normalized.pixels.view().at(x, y).g * 4 - image.pixels.view().at(x, y).g) <
                2e-6,
            "shared-context HDR must retain global reference exposure units");
  check(!p::merge_hdr(context, std::array<std::size_t, 2>{0, 0},
                      (temp.path / "duplicate.tif").string(), options),
        "HDR duplicate context members reject");
  check(!p::merge_hdr(std::vector<std::string>{paths[0]}, (temp.path / "one.tif").string(),
                      options),
        "HDR needs a bracket");
  check(std::ranges::none_of(fs::directory_iterator(temp.path),
                             [](const auto &entry) {
                               return entry.path().filename().string().starts_with(
                                   "photo-ops-hdr-");
                             }),
        "HDR private scratch must be cleaned");
}

void chromatic_highlights() {
  for (const auto color :
       {std::array{1., .2, .1}, std::array{.15, 1., .4}, std::array{.2, .1, 1.}}) {
    workspace temp;
    const p::extent size{128, 96};
    std::vector<std::string> paths;
    for (std::size_t i = 0; i < 3; ++i) {
      paths.push_back((temp.path / (std::to_string(i) + ".dng")).string());
      write_capture(
          paths.back(), size, std::array{.0025, .01, .04}[i],
          [](double x, double) { return x < 64 ? .2 : 1.5; }, 0, false, color);
    }
    p::hdr_options options;
    options.already_aligned = true;
    options.memory_budget_mib = 512;
    options.scratch_directory = temp.path.string();
    const auto output = (temp.path / "color.tif").string();
    const auto report = take(p::merge_hdr(paths, output, options));
    const auto &context = *report.raw_context;
    const auto &m = context.reference().camera_to_xyz;
    auto result = take(p::read_working_tiff(output));
    for (std::size_t y = 12; y < size.height - 12; ++y)
      for (std::size_t x = 76; x < size.width - 12; ++x) {
        const double signal = 1.5;
        const auto expected =
            p::from_xyz({float(signal * (m[0] * color[0] + m[1] * color[1] + m[2] * color[2])),
                         float(signal * (m[3] * color[0] + m[4] * color[1] + m[5] * color[2])),
                         float(signal * (m[6] * color[0] + m[7] * color[1] + m[8] * color[2]))},
                        context.encoding());
        const auto actual = result.pixels.view().at(x, y);
        check(std::abs(actual.r - expected.r) < .003 &&
                  std::abs(actual.g - expected.g) < .003 &&
                  std::abs(actual.b - expected.b) < .003,
              "partial CFA-channel saturation must not bias merged highlight color");
      }
  }
}

void motion_fallback(bool bright = false, bool coarse = false) {
  workspace temp;
  const p::extent size{192, 128};
  std::vector<std::string> paths;
  for (std::size_t i = 0; i < 3; ++i) {
    const auto scene = [i, bright](double x, double y) {
      return y >= 40 && y < 88 && x >= 48 + 24 * i && x < 80 + 24 * i ? (bright ? 2. : .65)
                                                                      : .15;
    };
    paths.push_back((temp.path / (std::to_string(i) + ".dng")).string());
    write_capture(paths.back(), size, std::array{.0025, .01, .04}[i], scene);
  }
  p::hdr_options options;
  options.already_aligned = true;
  options.memory_budget_mib = 512;
  options.scratch_directory = temp.path.string();
  if (coarse)
    options.proxy_long_edge = 48;
  options.diagnostics_directory = (temp.path / "diagnostics").string();
  const auto output = (temp.path / "motion.tif").string();
  const auto report = take(p::merge_hdr(paths, output, options));
  check(report.deghost_fraction > .02, "moving object must trigger HDR deghosting");
  auto image = take(p::read_working_tiff(output));
  const std::size_t owner = bright ? 0 : 1;
  auto middle = take(p::decode_raw_frame(paths[owner], *report.raw_context, owner));
  auto mask = take(p::read_scalar_tiff((temp.path / "diagnostics/deghost.tif").string()));
  if (bright)
    check(mask.pixels.view().at(90, 64).value == 1,
          "a clipped reference inconsistent with reliable short exposure must trigger motion");
  std::size_t protected_pixels = 0;
  for (std::size_t y = 12; y < size.height - 12; ++y)
    for (std::size_t x = 12; x < size.width - 12; ++x)
      if (mask.pixels.view().at(x, y).value) {
        const auto a = image.pixels.view().at(x, y), b = middle.image.pixels.view().at(x, y);
        check(a.r == b.r && a.g == b.g && a.b == b.b,
              "HDR motion component must preserve one coherent valid exposure without ghosts");
        ++protected_pixels;
      }
  check(protected_pixels > 1000, "motion test must exercise a meaningful protected region");
}

void exposure_robust_registration() {
  workspace temp;
  const p::extent size{384, 320};

  struct spot {
    double x, y, r, a;
  };

  std::vector<spot> spots;
  std::uint32_t seed = 9;
  const auto random = [&]() {
    seed = 1664525 * seed + 1013904223;
    return double(seed) / 4294967296.;
  };
  for (int i = 0; i < 180; ++i)
    spots.push_back({random() * 384, random() * 320, 2 + random() * 5, .015 + random() * .025});
  const auto scene = [&](double x, double y) {
    double v = .03;
    for (auto s : spots) {
      const auto dx = x - s.x, dy = y - s.y;
      v += s.a * std::exp(-(dx * dx + dy * dy) / (s.r * s.r));
    }
    return v;
  };
  const std::array<double, 3> a{.998, 1, 1.002}, b{-.001, 0, .001}, tx{.35, 0, -.4},
      ty{-.25, 0, .2};
  std::vector<std::string> paths;
  for (std::size_t i = 0; i < 3; ++i) {
    paths.push_back((temp.path / (std::to_string(i) + ".dng")).string());
    write_capture(paths.back(), size, std::array{.0025, .01, .04}[i],
                  [&, i](double x, double y) {
                    return scene(a[i] * x - b[i] * y + tx[i], b[i] * x + a[i] * y + ty[i]);
                  });
  }
  p::hdr_options options;
  options.memory_budget_mib = 512;
  options.proxy_long_edge = 192;
  options.scratch_directory = temp.path.string();
  const auto report =
      take(p::merge_hdr(paths, (temp.path / "registered.tif").string(), options));
  for (std::size_t i : {0u, 2u}) {
    const auto &t = report.frames[i].alignment;
    check(t.refined, "AEB alignment must refine subpixel similarity");
    for (double y : {32., 288.})
      for (double x : {32., 352.}) {
        const auto error = std::hypot((t.a - a[i]) * x - (t.b - b[i]) * y + t.tx - tx[i],
                                      (t.b - b[i]) * x + (t.a - a[i]) * y + t.ty - ty[i]);
        std::cout << "HDR alignment " << i << ": corner_error=" << error << std::endl;
        check(error < .4, "exposure-normalized log registration must recover small breathing "
                          "and fractional motion");
      }
  }
  options.tile_size = 32;
  const auto tiled_path = (temp.path / "registered-tiles.tif").string();
  take(p::merge_hdr(paths, tiled_path, options));
  auto whole = take(p::read_working_tiff((temp.path / "registered.tif").string()));
  auto tiled = take(p::read_working_tiff(tiled_path));
  for (std::size_t y = 0; y < size.height; ++y)
    for (std::size_t x = 0; x < size.width; ++x) {
      const auto a = whole.pixels.view().at(x, y), b = tiled.pixels.view().at(x, y);
      check(std::abs(a.r - b.r) < 1e-6 && std::abs(a.g - b.g) < 1e-6 &&
                std::abs(a.b - b.b) < 1e-6,
            "fractional similarity warp must remain tile-independent");
    }
}

p::focus_exposure_matrix
matrix_captures(workspace &temp, p::extent size,
                const std::function<double(std::size_t, std::size_t, double, double)> &scene,
                bool perturbed_wb = false) {
  p::focus_exposure_matrix matrix;
  for (std::size_t f = 0; f < 2; ++f) {
    p::focus_exposure_plane plane{f == 0 ? "near" : "far", {}};
    for (std::size_t e = 0; e < 3; ++e) {
      const auto path =
          (temp.path / ("f" + std::to_string(f) + "e" + std::to_string(e) + ".dng")).string();
      write_capture(
          path, size, std::array{.0025, .01, .04}[e],
          [&, f, e](double x, double y) { return scene(f, e, x, y); }, 0, false, {1, 1, 1},
          perturbed_wb && f == 0 ? std::array<float, 3>{.5, 1, .25}
                                 : std::array<float, 3>{1, 1, 1});
      plane.exposures.push_back(path);
    }
    matrix.focus_planes.push_back(std::move(plane));
  }
  return matrix;
}

p::hdr_options plane_options(const workspace &temp) {
  p::hdr_options options;
  options.already_aligned = true;
  options.memory_budget_mib = 512;
  options.scratch_directory = temp.path.string();
  return options;
}

// Analytic Gaussian convolution of independent sinusoidal radiance: no resampling aliasing.
void matrix_static_defocus_motion() {
  struct scenario {
    double level;
    bool moving;
  };

  for (const auto [level, moving] :
       std::array{scenario{.4, false}, scenario{1.5, false}, scenario{1.5, true}}) {
    workspace temp;
    const p::extent size{256, 192};
    const auto captures = matrix_captures(
        temp, size, [level, moving](std::size_t f, std::size_t, double x, double y) {
          if (moving && f == 1 && x >= 90 && x < 126 && y >= 42 && y < 72)
            return 3.;
          const bool sharp = (y < 96) == (f == 0);
          const double variance = sharp ? 0 : 16;
          const auto wave = [variance](double frequency, double phase) {
            return std::exp(-.5 * variance * frequency * frequency) * std::sin(phase);
          };
          return level *
                 (1 + .25 * wave(2 * std::numbers::pi / 11, x * 2 * std::numbers::pi / 11) +
                  .25 * wave(2 * std::numbers::pi / 15, y * 2 * std::numbers::pi / 15) +
                  .12 * wave(std::sqrt(2.) * 2 * std::numbers::pi / 7,
                             (x + y) * 2 * std::numbers::pi / 7));
        });
    p::focus_stack_options options;
    options.already_aligned = true;
    options.memory_budget_mib = 512;
    options.tile_size = 64;
    options.scratch_directory = temp.path.string();
    options.diagnostics_directory = (temp.path / "diagnostics").string();
    const auto path = (temp.path / "static-defocus.tif").string();
    const auto report = take(p::stack_focus(captures, path, options));
    const auto near_path = (temp.path / "near.tif").string(),
               far_path = (temp.path / "far.tif").string();
    take(p::merge_hdr(*report.raw_context, std::array<std::size_t, 3>{0, 1, 2}, near_path,
                      plane_options(temp)));
    take(p::merge_hdr(*report.raw_context, std::array<std::size_t, 3>{3, 4, 5}, far_path,
                      plane_options(temp)));
    auto result = take(p::read_working_tiff(path)),
         near = take(p::read_working_tiff(near_path)),
         far = take(p::read_working_tiff(far_path));
    auto motion = take(p::read_image((temp.path / "diagnostics/motion.tif").string()));
    double error = 0, false_motion = 0;
    std::size_t count = 0;
    for (std::size_t y = 20; y < size.height - 20; ++y)
      for (std::size_t x = 20; x < size.width - 20; ++x) {
        if (std::abs(long(y) - 96) < 24 || (moving && x >= 70 && x < 146 && y >= 22 && y < 92))
          continue;
        const auto ideal = (y < 96 ? near : far).pixels.view().at(x, y).g;
        error += std::abs(result.pixels.view().at(x, y).g - ideal) / level;
        false_motion += motion.pixels.view().at(x, y).r > .5;
        ++count;
      }
    if (moving)
      for (std::size_t y = 50; y < 64; ++y)
        for (std::size_t x = 100; x < 116; ++x) {
          check(motion.pixels.view().at(x, y).r > .5,
                "interior defocus explanation must retain genuine cross-focus motion");
          const auto a = result.pixels.view().at(x, y), b = far.pixels.view().at(x, y);
          check(a.r == b.r && a.g == b.g && a.b == b.b,
                "cross-focus temporal change must preserve exact coherent reference");
        }
    std::cout << "static HDR defocus: level=" << level << ", moving=" << moving
              << ", focus_mae=" << error / count << ", false_motion=" << false_motion / count
              << std::endl;
    check(error / count < .02 && false_motion / count < .01,
          "static Gaussian defocus across HDR planes must not trigger motion/reference blur");
  }
}

void matrix_radiance_and_focus() {
  workspace temp;
  const p::extent size{256, 192};
  const auto captures = matrix_captures(
      temp, size,
      [](std::size_t f, std::size_t, double x, double y) {
        const double base = y < 64 ? .003 : y < 128 ? .15 : 1.6;
        const bool sharp = (x < 128) == (f == 0);
        return base *
               (1 + .22 * (sharp ? 1 : .08) * std::sin(.91 * x + .1) * std::cos(.83 * y + .2));
      },
      true);
  p::focus_stack_options options;
  options.already_aligned = true;
  options.memory_budget_mib = 512;
  options.tile_size = 32;
  options.scratch_directory = temp.path.string();
  options.diagnostics_directory = (temp.path / "diagnostics").string();
  const auto output = (temp.path / "matrix.tif").string();
  const auto report = take(p::stack_focus(captures, output, options));
  check(report.hdr_planes.size() == 2 && report.raw_context->frames().size() == 6 &&
            report.raw_context->reference_index() == 4,
        "matrix must freeze all originals with middle exposure of focus reference");
  for (const auto &plane : report.hdr_planes)
    check(plane.hdr.raw_context->fingerprint() == report.raw_context->fingerprint(),
          "every HDR plane must retain one global color/exposure context");
  auto result = take(p::read_working_tiff(output));
  auto hdr_options = plane_options(temp);
  const auto near_path = (temp.path / "near-hdr.tif").string(),
             far_path = (temp.path / "far-hdr.tif").string();
  take(p::merge_hdr(*report.raw_context, std::array<std::size_t, 3>{0, 1, 2}, near_path,
                    hdr_options));
  take(p::merge_hdr(*report.raw_context, std::array<std::size_t, 3>{3, 4, 5}, far_path,
                    hdr_options));
  auto near = take(p::read_working_tiff(near_path)), far = take(p::read_working_tiff(far_path));
  double error = 0, reference_error = 0;
  std::size_t count = 0;
  for (std::size_t y = 16; y < size.height - 16; ++y)
    for (std::size_t x = 16; x < size.width - 16; ++x) {
      if (std::abs(long(x) - 128) < 24 || std::abs(long(y) - 64) < 12 ||
          std::abs(long(y) - 128) < 12)
        continue;
      const auto ideal = (x < 128 ? near : far).pixels.view().at(x, y).g;
      const double scale = y < 64 ? .003 : y < 128 ? .15 : 1.6;
      error += std::abs(result.pixels.view().at(x, y).g - ideal) / scale;
      reference_error += std::abs(far.pixels.view().at(x, y).g - ideal) / scale;
      ++count;
    }
  std::cout << "matrix: relative focus_mae=" << error / count
            << ", single_plane=" << reference_error / count << std::endl;
  check(error / count < .035 && error < reference_error * .55,
        "HDR-first matrix must recover near/far detail across shadows and highlights");
  check(report.working_maximum > 1 && report.working_minimum >= -.01,
        "matrix must preserve HDR headroom");
  check(report.profile.hdr_seconds > 0 && report.profile.scalar_warp_calls > 0 &&
            report.profile.scratch_peak_file_bytes >= report.matrix_intermediate_bytes,
        "matrix profile must account for HDR stages, scalar warps and retained intermediates");
  check(result.encoding.normalization == report.raw_context->fingerprint(),
        "matrix final working TIFF must retain global context");
  for (const auto *name : {"hdr-confidence.tif", "hdr-clipping.tif", "hdr-low-snr.tif",
                           "hdr-deghost.tif", "combined-confidence.tif"})
    check(fs::exists(temp.path / "diagnostics" / name),
          "matrix combined diagnostics must be published");
  check(fs::exists(temp.path / "diagnostics/hdr-plane-0/report.json") &&
            fs::exists(temp.path / "diagnostics/hdr-plane-1/report.json"),
        "per-plane HDR evidence must accompany the final matrix report");
  options.diagnostics_directory.clear();
  options.tile_size = 4096;
  const auto whole_path = (temp.path / "whole-matrix.tif").string();
  take(p::stack_focus(captures, whole_path, options));
  auto whole = take(p::read_working_tiff(whole_path));
  for (std::size_t y = 0; y < size.height; ++y)
    for (std::size_t x = 0; x < size.width; ++x) {
      const auto a = result.pixels.view().at(x, y), b = whole.pixels.view().at(x, y);
      check(std::abs(a.r - b.r) < .0003 && std::abs(a.g - b.g) < .0003 &&
                std::abs(a.b - b.b) < .0003,
            "matrix tile and whole-domain fusion must agree across boundaries");
    }
  auto permuted = captures;
  std::rotate(permuted.focus_planes[0].exposures.begin(),
              permuted.focus_planes[0].exposures.begin() + 1,
              permuted.focus_planes[0].exposures.end());
  std::swap(permuted.focus_planes[1].exposures[1], permuted.focus_planes[1].exposures[2]);
  const auto permuted_path = (temp.path / "permuted-exposures.tif").string();
  const auto permuted_report = take(p::stack_focus(permuted, permuted_path, options));
  check(permuted_report.raw_context->reference_index() == 5,
        "matrix must locate median shutter from metadata, not column position");
  auto shuffled = take(p::read_working_tiff(permuted_path));
  auto reversed = captures;
  std::reverse(reversed.focus_planes.begin(), reversed.focus_planes.end());
  options.reference_index = 0;
  const auto reversed_path = (temp.path / "reversed-focus.tif").string();
  take(p::stack_focus(reversed, reversed_path, options));
  auto reversed_image = take(p::read_working_tiff(reversed_path));
  for (std::size_t y = 0; y < size.height; ++y)
    for (std::size_t x = 0; x < size.width; ++x) {
      const auto expected = result.pixels.view().at(x, y);
      for (const auto actual :
           {shuffled.pixels.view().at(x, y), reversed_image.pixels.view().at(x, y)})
        check(
            std::abs(actual.r - expected.r) < .0005 &&
                std::abs(actual.g - expected.g) < .0005 &&
                std::abs(actual.b - expected.b) < .0005,
            "explicit exposure/focus permutations must preserve common radiance and geometry");
    }
  options.reference_index = static_cast<std::size_t>(-1);
  auto malformed = captures;
  malformed.focus_planes[1].name = "near";
  check(!p::stack_focus(malformed, (temp.path / "bad-name.tif").string(), options),
        "duplicate focus names must reject");
  malformed = captures;
  malformed.focus_planes[1].exposures.pop_back();
  check(!p::stack_focus(malformed, (temp.path / "ragged.tif").string(), options),
        "ragged exposure grid must reject");
  malformed = captures;
  malformed.focus_planes[1].exposures[0] = malformed.focus_planes[0].exposures[0];
  check(!p::stack_focus(malformed, (temp.path / "reused.tif").string(), options),
        "reused capture must reject");
  const auto mismatched = (temp.path / "different-grid.dng").string();
  write_capture(mismatched, size, .02, [](double, double) { return .2; });
  malformed = captures;
  malformed.focus_planes[1].exposures[0] = mismatched;
  check(!p::stack_focus(malformed, (temp.path / "bad-grid.tif").string(), options),
        "different shutter grids must reject");
  check(std::ranges::none_of(fs::directory_iterator(temp.path),
                             [](const auto &entry) {
                               return entry.path().filename().string().starts_with(
                                   "photo-ops-matrix-");
                             }),
        "matrix private intermediates must be cleaned on success and failure");
}

void matrix_validity_and_motion() {
  for (bool all_invalid : {false, true}) {
    workspace temp;
    const p::extent size{128, 96};
    const auto captures =
        matrix_captures(temp, size, [all_invalid](std::size_t f, std::size_t, double, double) {
          return all_invalid || f == 1 ? 6. : .2;
        });
    p::focus_stack_options options;
    options.already_aligned = true;
    options.memory_budget_mib = 512;
    options.scratch_directory = temp.path.string();
    options.diagnostics_directory = (temp.path / "diagnostics").string();
    const auto output = (temp.path / "validity.tif").string();
    const auto report = take(p::stack_focus(captures, output, options));
    auto image = take(p::read_working_tiff(output));
    const auto source = (temp.path / "source.tif").string();
    auto hdr_options = plane_options(temp);
    const auto indices =
        all_invalid ? std::array<std::size_t, 3>{3, 4, 5} : std::array<std::size_t, 3>{0, 1, 2};
    take(p::merge_hdr(*report.raw_context, indices, source, hdr_options));
    auto expected = take(p::read_working_tiff(source));
    for (std::size_t y = 0; y < size.height; ++y)
      for (std::size_t x = 0; x < size.width; ++x) {
        const auto a = image.pixels.view().at(x, y), b = expected.pixels.view().at(x, y);
        check(std::abs(a.r - b.r) < 2e-5 && std::abs(a.g - b.g) < 2e-5 &&
                  std::abs(a.b - b.b) < 2e-5,
              "invalid HDR plane must not contaminate reliable fusion; all-invalid uses "
              "coherent reference");
      }
    auto clipping =
        take(p::read_scalar_tiff((temp.path / "diagnostics/hdr-clipping.tif").string()));
    check(clipping.pixels.view().at(64, 48).value == float(all_invalid),
          "matrix clipping diagnostics must follow the reliable selected plane or explicit "
          "fallback");
  }
  workspace temp;
  const p::extent size{192, 128};
  const auto captures =
      matrix_captures(temp, size, [](std::size_t f, std::size_t e, double x, double y) {
        const double left = f == 0 ? 48 + 24 * e : 72;
        return x >= left && x < left + 32 && y >= 40 && y < 88 ? .65 : .15;
      });
  p::focus_stack_options options;
  options.already_aligned = true;
  options.memory_budget_mib = 512;
  options.scratch_directory = temp.path.string();
  options.diagnostics_directory = (temp.path / "diagnostics").string();
  const auto output = (temp.path / "motion-matrix.tif").string();
  const auto report = take(p::stack_focus(captures, output, options));
  check(report.hdr_planes[0].hdr.deghost_fraction > .02 &&
            report.hdr_planes[1].hdr.deghost_fraction == 0,
        "motion fixture must isolate one exposure-axis deghost map");
  check(report.motion_fraction > .02,
        "HDR motion must propagate even when merged focus-plane colors agree");
  auto image = take(p::read_working_tiff(output));
  auto mask = take(p::read_image((temp.path / "diagnostics/motion.tif").string()));
  const auto reference = (temp.path / "reference-hdr.tif").string();
  auto hdr_options = plane_options(temp);
  take(p::merge_hdr(*report.raw_context, std::array<std::size_t, 3>{3, 4, 5}, reference,
                    hdr_options));
  auto ref = take(p::read_working_tiff(reference));
  for (std::size_t y = 0; y < size.height; ++y)
    for (std::size_t x = 0; x < size.width; ++x)
      if (mask.pixels.view().at(x, y).r > .5) {
        const auto a = image.pixels.view().at(x, y), b = ref.pixels.view().at(x, y);
        check(a.r == b.r && a.g == b.g && a.b == b.b,
              "HDR deghost evidence cannot be overridden by focus/defocus blending");
      }
}

void matrix_geometric_composition() {
  workspace temp;
  const p::extent size{384, 320};

  struct spot {
    double x, y, r, a;
  };

  std::vector<spot> spots;
  std::uint32_t seed = 9;
  const auto random = [&]() {
    seed = 1664525 * seed + 1013904223;
    return double(seed) / 4294967296.;
  };
  for (int i = 0; i < 180; ++i)
    spots.push_back({random() * 384, random() * 320, 2 + random() * 5, .015 + random() * .025});
  const auto scene = [&](double x, double y) {
    double v = .03;
    for (auto s : spots) {
      const auto dx = x - s.x, dy = y - s.y;
      v += s.a * std::exp(-(dx * dx + dy * dy) / (s.r * s.r));
    }
    return v;
  };
  constexpr double a = 1.006, b = .002, tx = .45, ty = -.3;
  const auto captures =
      matrix_captures(temp, size, [&](std::size_t f, std::size_t e, double x, double y) {
        if (f == 0) {
          const std::array<double, 3> aa{.998, 1, 1.002}, bb{-.001, 0, .001}, xx{.35, 0, -.4},
              yy{-.25, 0, .2};
          return scene(aa[e] * x - bb[e] * y + xx[e], bb[e] * x + aa[e] * y + yy[e]);
        }
        return scene(a * x - b * y + tx, b * x + a * y + ty);
      });
  p::focus_stack_options options;
  options.memory_budget_mib = 512;
  options.proxy_long_edge = 192;
  options.tile_size = 128;
  options.scratch_directory = temp.path.string();
  const auto report =
      take(p::stack_focus(captures, (temp.path / "registered-matrix.tif").string(), options));
  const auto &t = report.frames[0].alignment;
  check(t.refined, "matrix default geometry must refine across HDR focus planes");
  const double d = a * a + b * b, ia = a / d, ib = -b / d, itx = -(a * tx + b * ty) / d,
               ity = (b * tx - a * ty) / d;
  double maximum = 0;
  for (double y : {32., 288.})
    for (double x : {32., 352.})
      maximum = std::max(maximum, std::hypot((t.a - ia) * x - (t.b - ib) * y + t.tx - itx,
                                             (t.b - ib) * x + (t.a - ia) * y + t.ty - ity));
  std::cout << "matrix composed registration: maximum_corner_error=" << maximum << std::endl;
  check(maximum < .4,
        "HDR-first matrix must preserve composed subpixel exposure/focus geometry");
  check(report.hdr_planes[0].hdr.frames[0].alignment.refined &&
            report.hdr_planes[0].hdr.frames[2].alignment.refined,
        "matrix default path must execute within-plane AEB registration as well");
}

void matrix_cache_streaming() {
  workspace temp;
  const p::extent size{64, 48};
  p::focus_exposure_matrix matrix;
  for (std::size_t f = 0; f < 20; ++f) {
    p::focus_exposure_plane plane{"plane-" + std::to_string(f), {}};
    for (std::size_t e = 0; e < 2; ++e) {
      const auto path =
          (temp.path / ("p" + std::to_string(f) + "-" + std::to_string(e) + ".dng")).string();
      write_capture(path, size, std::array{.0025, .01}[e],
                    [f](double x, double) { return f == 0 && x < 32 ? 6. : .1; });
      plane.exposures.push_back(path);
    }
    matrix.focus_planes.push_back(std::move(plane));
  }
  p::focus_stack_options options;
  options.already_aligned = true;
  options.memory_budget_mib = 512;
  options.tile_size = 512;
  options.scratch_directory = temp.path.string();
  const auto streaming_path = (temp.path / "streaming.tif").string();
  const auto streaming = take(p::stack_focus(matrix, streaming_path, options));
  check(!streaming.tile_sources_cached, "matrix budget must exercise the streaming donor path");
  options.memory_budget_mib = 1024;
  const auto cached_path = (temp.path / "cached.tif").string();
  const auto cached = take(p::stack_focus(matrix, cached_path, options));
  check(cached.tile_sources_cached && cached.estimated_peak_bytes <= 1024 * 1024 * 1024,
        "matrix larger budget may cache sources without exceeding planner");
  check(cached.profile.warp_calls == 20 * cached.profile.tiles &&
            cached.profile.scalar_warp_calls == 20 * cached.profile.tiles,
        "cached matrix must reuse the donor-pass source and reliability buffers");
  auto a = take(p::read_working_tiff(streaming_path)),
       b = take(p::read_working_tiff(cached_path));
  for (std::size_t y = 0; y < size.height; ++y)
    for (std::size_t x = 0; x < size.width; ++x) {
      const auto p = a.pixels.view().at(x, y), q = b.pixels.view().at(x, y);
      check(p.r == q.r && p.g == q.g && p.b == q.b,
            "streaming and cached HDR reliability/donor paths must preserve identical samples");
      check(std::abs(p.g - .1) < .0001,
            "invalid partial clipping cannot leak through Gaussian/Laplacian pyramids");
    }
}

} // namespace

void hdr_tests() {
  scalar_io();
  radiometric_fusion();
  chromatic_highlights();
  motion_fallback();
  motion_fallback(false, true);
  motion_fallback(true);
  motion_fallback(true, true);
  exposure_robust_registration();
}

void matrix_tests() {
  matrix_static_defocus_motion();
  matrix_radiance_and_focus();
  matrix_validity_and_motion();
  matrix_cache_streaming();
  matrix_geometric_composition();
}
