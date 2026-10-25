// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <tiffio.h>
#include <vector>
import photo_ops.image;
import photo_ops.color;
import photo_ops.raw;
import photo_ops.io;
import photo_ops.focus_stack;

namespace {
namespace p = photo_ops;
namespace fs = std::filesystem;

void check(bool v, const char *message) {
  if (!v)
    throw std::runtime_error(message);
}

void near(float a, float b, float tolerance = 1e-5) {
  check(std::isfinite(a) && std::abs(a - b) <= tolerance, "RAW numerical mismatch");
}

template <class T> T take(p::result<T> value) {
  if (!value)
    throw std::runtime_error(value.error().operation + ": " + value.error().message);
  return std::move(*value);
}

void done(p::result<void> value) {
  if (!value)
    throw std::runtime_error(value.error().operation + ": " + value.error().message);
}

struct workspace {
  fs::path path;

  workspace() {
    auto pattern = (fs::temp_directory_path() / "photo-raw-tests-XXXXXX").string();
    check(::mkdtemp(pattern.data()), "RAW test directory");
    path = pattern;
  }

  ~workspace() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

// Tiny generated DNG: exact sensor measurements and independent EXIF calibration.
void write_dng(const std::string &path, std::array<float, 3> neutral = {.5, 1, .25},
               float black = 64, float exposure = .01, unsigned orientation = 1) {
  TIFF *file = TIFFOpen(path.c_str(), "w");
  check(file, "create numerical DNG");
  TIFFCreateEXIFDirectory(file);
  TIFFSetField(file, EXIFTAG_EXPOSURETIME, exposure);
  TIFFSetField(file, EXIFTAG_FNUMBER, 4.0);
  std::uint16_t iso = 100;
  TIFFSetField(file, EXIFTAG_ISOSPEEDRATINGS, 1, &iso);
  std::uint64_t exif = 0;
  check(TIFFWriteCustomDirectory(file, &exif), "write DNG EXIF");
  TIFFCreateDirectory(file);
  TIFFSetField(file, TIFFTAG_IMAGEWIDTH, 64);
  TIFFSetField(file, TIFFTAG_IMAGELENGTH, 64);
  TIFFSetField(file, TIFFTAG_BITSPERSAMPLE, 16);
  TIFFSetField(file, TIFFTAG_SAMPLESPERPIXEL, 1);
  TIFFSetField(file, TIFFTAG_PHOTOMETRIC, 32803);
  TIFFSetField(file, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
  TIFFSetField(file, TIFFTAG_ROWSPERSTRIP, 64);
  TIFFSetField(file, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
  TIFFSetField(file, TIFFTAG_MAKE, "photo-ops");
  TIFFSetField(file, TIFFTAG_MODEL, "numerical Bayer");
  TIFFSetField(file, TIFFTAG_ORIENTATION, orientation);
  TIFFSetField(file, TIFFTAG_EXIFIFD, exif);
  unsigned char version[]{1, 4, 0, 0};
  TIFFSetField(file, TIFFTAG_DNGVERSION, version);
  TIFFSetField(file, TIFFTAG_DNGBACKWARDVERSION, version);
  TIFFSetField(file, TIFFTAG_UNIQUECAMERAMODEL, "photo-ops numerical Bayer");
  unsigned short dimensions[]{2, 2};
  unsigned char cfa[]{0, 1, 1, 2};
  TIFFSetField(file, TIFFTAG_CFAREPEATPATTERNDIM, dimensions);
  TIFFSetField(file, TIFFTAG_CFAPATTERN, 4, cfa);
  unsigned white = 1023;
  TIFFSetField(file, TIFFTAG_BLACKLEVEL, 1, &black);
  TIFFSetField(file, TIFFTAG_WHITELEVEL, 1, &white);
  // XYZ→camera matrix. Identity is deliberately simple; LibRaw normalizes its row whites.
  float matrix[]{1, 0, 0, 0, 1, 0, 0, 0, 1};
  TIFFSetField(file, TIFFTAG_COLORMATRIX1, 9, matrix);
  TIFFSetField(file, TIFFTAG_ASSHOTNEUTRAL, 3, neutral.data());
  TIFFSetField(file, TIFFTAG_CALIBRATIONILLUMINANT1, 21);
  std::array<unsigned short, 64> row{};
  for (unsigned y = 0; y < 64; ++y) {
    for (unsigned x = 0; x < 64; ++x) {
      const unsigned c = cfa[2 * (y % 2) + x % 2];
      row[x] = x < 16 ? 60 : x < 40 ? (c == 0 ? 184 : c == 1 ? 304 : 124) : x < 56 ? 900 : 1023;
    }
    check(TIFFWriteScanline(file, row.data(), y) >= 0, "write numerical mosaic");
  }
  TIFFClose(file);
}

void color_and_storage() {
  const p::color_encoding encoding{p::rgb_primaries::rec2020, p::white_point::d65,
                                   p::transfer_function::linear,
                                   p::image_origin::scene_linear_raw, "test-normalization"};
  const auto white = p::to_xyz(p::rgb_f32{1, 1, 1}, encoding);
  near(white.x, .9504559);
  near(white.y, 1);
  near(white.z, 1.0890578);
  for (auto pixel : {p::rgb_f32{-.1, 2, .4}, p::rgb_f32{1, 0, 0}, p::rgb_f32{.2, .4, .6}}) {
    const auto restored = p::from_xyz(p::to_xyz(pixel, encoding), encoding);
    near(restored.r, pixel.r);
    near(restored.g, pixel.g);
    near(restored.b, pixel.b);
    const auto lab_restored = p::from_oklab(p::to_oklab(pixel, encoding), encoding);
    near(lab_restored.r, pixel.r, 1e-5);
    near(lab_restored.g, pixel.g, 1e-5);
    near(lab_restored.b, pixel.b, 1e-5);
  }
  check(p::to_linear_srgb(p::rgb_f32{1, 0, 0}, encoding).g < 0,
        "wide gamut conversion must retain out-of-sRGB color");
  workspace temp;
  const auto path = (temp.path / "float.tif").string();
  done(p::write_working_tiff_rows(
      path, {33, 17}, encoding,
      [](std::size_t y, std::span<p::rgb_f32> row) -> p::result<void> {
        for (std::size_t x = 0; x < row.size(); ++x)
          row[x] = {-.125f, 2.f, float(x + y) / 16};
        return {};
      }));
  auto restored = take(p::read_working_tiff(path));
  check(restored.encoding.primaries == p::rgb_primaries::rec2020 &&
            restored.encoding.normalization == "test-normalization",
        "float TIFF must retain encoding/context");
  for (std::size_t y = 0; y < 17; ++y)
    for (std::size_t x = 0; x < 33; ++x) {
      const auto q = restored.pixels.view().at(x, y);
      near(q.r, -.125, 0);
      near(q.g, 2, 0);
      near(q.b, float(x + y) / 16, 0);
    }
  check(!p::read_image(path), "legacy reader must not reinterpret float working TIFF");
  check(!p::write_working_tiff_rows(
            path, {33, 17}, encoding,
            [](std::size_t, std::span<p::rgb_f32>) -> p::result<void> { return {}; }),
        "working TIFF output cannot overwrite existing path");
  const auto failed = (temp.path / "failed.tif").string();
  check(!p::write_working_tiff_rows(
            failed, {33, 17}, encoding,
            [](std::size_t y, std::span<p::rgb_f32> row) -> p::result<void> {
              if (y == 3)
                return p::fail(p::error_code::io, "fixture", "injected read failure");
              std::ranges::fill(row, p::rgb_f32{});
              return {};
            }),
        "working stream failure must propagate");
  check(!fs::exists(failed), "working stream failure cannot publish partial output");
  TIFF *file = TIFFOpen(path.c_str(), "r+");
  check(file, "open ICC mismatch fixture");
  const std::string false_note = "photo-ops-float-v1;srgb;raw;test-normalization";
  TIFFSetField(file, TIFFTAG_IMAGEDESCRIPTION, false_note.c_str());
  TIFFRewriteDirectory(file);
  TIFFClose(file);
  check(!p::read_working_tiff(path), "working reader must reject ICC/primaries contradiction");
}

void context_and_development() {
  workspace temp;
  const auto first = (temp.path / "first.dng").string(),
             second = (temp.path / "second.dng").string();
  write_dng(first);
  write_dng(second, {.25, 1, .5});
  p::raw_sequence_options options;
  options.reference_index = 0;
  options.memory_budget_mib = 512;
  auto context =
      take(p::prepare_raw_sequence(std::vector<std::string>{first, second}, options));
  check(context.reference_index() == 0 && context.frames().size() == 2,
        "context reference/manifest");
  near(context.white_balance()[0], 2);
  near(context.white_balance()[2], 4);
  check(context.frames()[0].camera_wb != context.frames()[1].camera_wb,
        "fixture must perturb per-frame WB metadata");
  check(context.encoding().primaries == p::rgb_primaries::rec2020 &&
            context.encoding().origin == p::image_origin::scene_linear_raw,
        "RAW must carry scene-linear wide-gamut encoding");
  auto a = take(p::decode_raw_frame(first, context, 0));
  auto b = take(p::decode_raw_frame(second, context, 1));
  check(a.statistics.minimum < 0 && a.statistics.maximum > 1 &&
            a.statistics.saturated_fraction > 0,
        "RAW float path must retain signed values, HDR and sensor saturation evidence");
  const auto center = a.image.pixels.view().at(28, 28);
  near(center.r, 240.f / 959, 2e-4);
  near(center.g, 240.f / 959, 2e-4);
  near(center.b, 240.f / 959, 2e-4);
  for (std::size_t y = 0; y < 64; ++y)
    for (std::size_t x = 0; x < 64; ++x) {
      const auto u = a.image.pixels.view().at(x, y), v = b.image.pixels.view().at(x, y);
      near(u.r, v.r, 0);
      near(u.g, v.g, 0);
      near(u.b, v.b, 0);
    }
  check(a.image.encoding.normalization == context.fingerprint(),
        "pixels/proxies must identify their immutable context");
  auto again = take(p::prepare_raw_sequence(std::vector<std::string>{first, second}, options));
  check(again.fingerprint() == context.fingerprint(), "context fingerprint must be stable");
  std::vector<p::raw_frame_metadata> metadata(context.frames().begin(), context.frames().end());
  for (auto category : {0, 1, 2, 3, 4, 5}) {
    auto altered = metadata;
    if (category == 0)
      altered[1].black[0] += 1;
    if (category == 1)
      altered[1].white[2] -= 1;
    if (category == 2)
      altered[1].iso *= 2;
    if (category == 3)
      altered[1].shutter_seconds *= 2;
    if (category == 4)
      altered[1].camera += " different mode";
    if (category == 5)
      altered[1].camera_to_xyz[0] += .01;
    check(!p::prepare_raw_sequence(altered, options),
          "incompatible RAW calibration/exposure must reject before development");
  }
  auto changed_wb = metadata;
  changed_wb[1].camera_wb = {9, 1, 9, 1};
  auto frozen = take(p::prepare_raw_sequence(changed_wb, options));
  check(frozen.white_balance() == context.white_balance(),
        "non-reference WB perturbation cannot change shared development gains");
  p::focus_stack_options stack_options;
  stack_options.already_aligned = true;
  stack_options.memory_budget_mib = 512;
  stack_options.reference_index = 0;
  stack_options.scratch_directory = temp.path.string();
  stack_options.diagnostics_directory = (temp.path / "diagnostics").string();
  const auto output = (temp.path / "stack.tif").string();
  auto stacked =
      take(p::stack_focus(std::vector<std::string>{first, second}, output, stack_options));
  check(stacked.raw_context.has_value() &&
            stacked.raw_context->fingerprint() == context.fingerprint(),
        "focus stack must actually use the shared RAW context");
  auto pixels = take(p::read_working_tiff(output));
  const auto result = pixels.pixels.view().at(28, 28);
  near(result.r, center.r, 2e-5);
  near(result.g, center.g, 2e-5);
  near(result.b, center.b, 2e-5);
  check(pixels.pixels.view().at(8, 28).r < 0 && pixels.pixels.view().at(48, 28).b > 1,
        "fusion and final working TIFF must preserve negative/HDR samples");
  for (unsigned orientation : {3, 6, 8}) {
    const auto rotated_path =
        (temp.path / ("orientation-" + std::to_string(orientation) + ".dng")).string();
    write_dng(rotated_path, {.5, 1, .25}, 64, .01, orientation);
    auto rotated_context =
        take(p::prepare_raw_sequence(std::vector<std::string>{rotated_path}, options));
    auto rotated = take(p::decode_raw_frame(rotated_path, rotated_context, 0));
    const int flip = rotated_context.reference().orientation;
    for (std::size_t y = 0; y < 64; ++y)
      for (std::size_t x = 0; x < 64; ++x) {
        std::size_t ox = flip & 1 ? 63 - x : x, oy = flip & 2 ? 63 - y : y;
        if (flip & 4)
          std::swap(ox, oy);
        const auto pixel = rotated.image.pixels.view().at(ox, oy);
        const auto expected = a.image.pixels.view().at(x, y);
        near(pixel.r, expected.r, 0);
        near(pixel.g, expected.g, 0);
        near(pixel.b, expected.b, 0);
      }
  }
  const auto mismatch = (temp.path / "black.dng").string();
  write_dng(mismatch, {.5, 1, .25}, 65);
  check(!p::prepare_raw_sequence(std::vector<std::string>{first, mismatch}, options),
        "real DNG black mismatch must reject");
  std::ofstream tamper(second, std::ios::app);
  tamper << "changed";
  tamper.close();
  check(!p::decode_raw_frame(second, context, 1),
        "changed contents must invalidate frozen context");
  check(!p::decode_raw_frame(first, context, 1),
        "context frame index/path mismatch must reject");
}

void real_raw_fixtures(const fs::path &root) {
  for (const auto *relative :
       {"rawpixls/6585-eos-r8-raw.CR3", "rawpixls/6587-eos-r8-craw.CR3"}) {
    const auto path = (root / relative).string();
    auto context = take(p::prepare_raw_sequence(std::vector<std::string>{path}));
    check(context.reference().camera == "Canon EOS R8",
          "R8 fixture must actually identify the target camera");
    auto decoded = take(p::decode_raw_frame(path, context, 0));
    const auto size = decoded.image.pixels.size();
    check(size.width * size.height > 23000000 && size.width * size.height < 25000000,
          "R8 full-resolution fixture geometry");
    std::cout << relative << ": " << size.width << 'x' << size.height
              << ", range=" << decoded.statistics.minimum << ".." << decoded.statistics.maximum
              << ", negative_fraction=" << decoded.statistics.negative_fraction << '\n';
  }
  std::vector<std::string> paths;
  for (const auto *name : {"frame_000.CR3", "frame_001.CR3", "frame_002.CR3"})
    paths.push_back((root / "sood/raw" / name).string());
  auto context = take(p::prepare_raw_sequence(paths));
  std::array<double, 3> baseline{};
  for (std::size_t i = 0; i < paths.size(); ++i) {
    auto decoded = take(p::decode_raw_frame(paths[i], context, i));
    check(decoded.image.encoding.normalization == context.fingerprint(),
          "real bracket must retain one color context");
    std::array<double, 3> mean{};
    const auto view = decoded.image.pixels.view();
    for (std::size_t y = 0; y < view.size().height; y += 8)
      for (std::size_t x = 0; x < view.size().width; x += 8) {
        const auto q = view.at(x, y);
        mean[0] += q.r;
        mean[1] += q.g;
        mean[2] += q.b;
      }
    if (!i)
      baseline = mean;
    for (unsigned c = 0; c < 3; ++c)
      check(std::abs(mean[c] / mean[1] - baseline[c] / baseline[1]) < .02,
            "adjacent real Canon focus frames must retain consistent average chromaticity");
    std::cout << "Canon bracket " << i << ": range=" << decoded.statistics.minimum << ".."
              << decoded.statistics.maximum
              << ", saturation=" << decoded.statistics.saturated_fraction
              << ", mean R/G=" << mean[0] / mean[1] << ", B/G=" << mean[2] / mean[1] << '\n';
  }
}
} // namespace

void raw_sequence_tests() {
  color_and_storage();
  context_and_development();
  if (const auto *root = std::getenv("PHOTO_OPS_RAW_FIXTURES"))
    real_raw_fixtures(root);
}
