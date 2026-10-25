// SPDX-License-Identifier: GPL-3.0-or-later
#include <lcms2.h>
#include <tiffio.h>
#include <turbojpeg.h>
#ifdef PHOTO_OPS_EXR
#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfHeader.h>
#include <OpenEXR/ImfInputFile.h>
#endif
#include <array>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>
import photo_ops.image;
import photo_ops.color;
import photo_ops.pyramid;
import photo_ops.diagnostics;
import photo_ops.io;
import photo_ops.output;

void sunlight_tests();
void focus_stack_tests();
void raw_sequence_tests();

namespace p = photo_ops;

namespace {
int checks = 0;

void require(bool ok, const char *message) {
  ++checks;
  if (!ok)
    throw std::runtime_error(message);
}

void near(float a, float b, float tolerance = 1e-5f) {
  require(std::isfinite(a) && std::abs(a - b) <= tolerance, "numerical mismatch");
}

template <class T> T take(p::result<T> result) {
  if (!result)
    throw std::runtime_error(result.error().operation + ": " + result.error().message);
  return std::move(*result);
}

void succeeds(p::result<void> result) {
  if (!result)
    throw std::runtime_error(result.error().operation + ": " + result.error().message);
  ++checks;
}

template <class F> void rejects(F &&operation) {
  const auto result = operation();
  require(!result.has_value(), "expected recoverable failure");
  require(!result.error().operation.empty() && !result.error().message.empty(),
          "failure must carry useful context");
}

template <class F> void contract_fails(F &&operation) {
  const auto child = fork();
  require(child >= 0, "fork contract probe");
  if (child == 0) {
    // These children deliberately abort. RLIMIT_CORE alone does not prevent a piped
    // systemd-coredump handler from recording them and triggering desktop notifications.
    // Disable dumpability only here; unexpected crashes in the test runner remain visible.
    if (prctl(PR_SET_DUMPABLE, 0) != 0)
      _exit(2);

    const rlimit no_core{0, 0};
    setrlimit(RLIMIT_CORE, &no_core);
    (void)freopen("/dev/null", "w", stderr);
    operation();
    _exit(0);
  }
  int status = 0;
  require(waitpid(child, &status, 0) == child, "wait contract probe");
  require(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT,
          "contract must terminate instead of returning or throwing");
}

void image_tests() {
  static_assert(!std::is_copy_constructible_v<p::image<p::linear_rgb>>);
  static_assert(std::is_convertible_v<p::image_view<p::gray>, p::image_view<const p::gray>>);
  static_assert(!std::is_convertible_v<p::image_view<const p::gray>, p::image_view<p::gray>>);
  std::array<p::gray, 14> memory{};
  p::image_view<p::gray> view(memory, {3, 3}, 5); // last row needs no trailing padding
  view.at(2, 1).value = 7;
  auto crop = view.subview({1, 1, 2, 2});
  require(crop.stride() == 5, "crop must preserve stride");
  crop.at(1, 0).value = 9;
  near(memory[7].value, 9);
  contract_fails([&] { (void)view.at(3, 0); });
  contract_fails([&] { (void)view.at(0, 3); });
  contract_fails([&] { (void)view.subview({2, 2, 2, 2}); });
  contract_fails([&] { p::image_view<p::gray> bad(memory, {3, 4}, 5); });
  contract_fails([&] { p::image_view<p::gray> bad(memory, {3, 2}, 2); });
  contract_fails([] { p::image<p::gray> bad({1, 0}); });
  contract_fails([] { p::image<p::gray> bad({std::numeric_limits<std::size_t>::max(), 2}); });
  p::image<p::gray> source({3, 2});
  source.view().at(0, 0).value = 0.4;
  auto copied = source.clone();
  copied.view().at(0, 0).value = 0.8;
  near(source.view().at(0, 0).value, 0.4);
  auto moved = std::move(source);
  require(source.size().width == 0 && source.view().size().height == 0,
          "moved-from image must be empty");
  near(moved.view().at(0, 0).value, 0.4);
}

void color_tests() {
  near(p::srgb_to_linear(0.5f), 0.21404114f);
  near(p::srgb_to_linear(0.04045f), 0.003130805f);
  near(p::linear_to_srgb(0.0031308f), 0.040449936f);
  for (float value : {-0.1f, 0.f, 0.001f, 0.18f, 0.5f, 1.f, 4.f})
    near(p::srgb_to_linear(p::linear_to_srgb(value)), value, 2e-6);
  near(p::luminance({1, 1, 1}), 1);
  near(p::luminance({1, 0, 0}), 0.2126);
  const auto red = p::to_oklab({1, 0, 0});
  near(red.l, 0.62795536f);
  near(red.a, 0.22486306f);
  near(red.b, 0.12584630f);
  for (auto rgb : {p::linear_rgb{0, 0, 0}, {1, 1, 1}, {0.1, 0.4, 0.8}, {-0.01, 1.2, 0.2}}) {
    const auto back = p::from_oklab(p::to_oklab(rgb));
    near(back.r, rgb.r);
    near(back.g, rgb.g);
    near(back.b, rgb.b);
  }
}

void pyramid_tests() {
  p::image<p::gray> constant({5, 3});
  for (std::size_t y = 0; y < 3; ++y)
    for (auto &pixel : constant.view().row(y))
      pixel.value = 0.37;
  auto levels = p::gaussian_levels(constant.view(), 20);
  require(levels.size() == 3, "odd pyramid should end at 1x1");
  require(levels[0].size().width == 3 && levels[0].size().height == 2,
          "odd dimensions round up");
  for (const auto &level : levels)
    for (std::size_t y = 0; y < level.size().height; ++y)
      for (auto pixel : level.view().row(y))
        near(pixel.value, 0.37);
  p::image<p::gray> impulse({5, 5});
  impulse.view().at(2, 2).value = 1;
  auto filtered = p::downsample(impulse.view());
  near(filtered.view().at(1, 1).value, 36.f / 256);
  near(filtered.view().at(0, 1).value, 6.f / 256);
  near(filtered.view().at(0, 0).value, 1.f / 256);
  require(p::gaussian_levels(impulse.view(), 1).empty(), "one level should borrow only base");
  p::image<p::gray> thin({1, 7});
  auto thin_levels = p::gaussian_levels(thin.view(), 20);
  require(thin_levels.size() == 3, "single-column pyramid");
  p::image<p::gray> empty;
  require(p::downsample(empty.view()).size().width == 0, "empty downsample");
}

struct temporary_directory {
  std::filesystem::path path;

  temporary_directory() {
    std::string pattern =
        (std::filesystem::temp_directory_path() / "photo-ops-tests-XXXXXX").string();
    auto *result = mkdtemp(pattern.data());
    if (!result)
      throw std::runtime_error("cannot create test directory");
    path = result;
  }

  ~temporary_directory() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};

void atomic_output_tests() {
  temporary_directory temp;
  const auto destination = (temp.path / "result.json").string();
  std::string abandoned;
  {
    auto staged = take(p::atomic_output::create(destination));
    abandoned = staged.temporary_path();
    std::ofstream(abandoned) << "partial";
    require(!std::filesystem::exists(destination), "uncommitted output must be invisible");
  }
  require(!std::filesystem::exists(abandoned), "aborted temporary output must be removed");
  {
    auto first = take(p::atomic_output::create(destination));
    auto second = take(p::atomic_output::create(destination));
    std::ofstream(first.temporary_path()) << "complete";
    std::ofstream(second.temporary_path()) << "loser";
    succeeds(first.commit());
    rejects([&] { return second.commit(); });
    rejects([&] { return first.commit(); });
    std::string contents;
    std::ifstream(destination) >> contents;
    require(contents == "complete", "late competing writer must preserve published bytes");
  }
  const auto link = temp.path / "dangling.json";
  std::filesystem::create_symlink(temp.path / "absent.json", link);
  {
    auto staged = take(p::atomic_output::create(link.string()));
    std::ofstream(staged.temporary_path()) << "replacement";
    rejects([&] { return staged.commit(); });
  }
  require(std::filesystem::is_symlink(link), "dangling output symlink must be preserved");
  p::image<p::linear_rgb> pixel({1, 1});
  for (auto format :
       {p::output_format::tiff16, p::output_format::jpeg, p::output_format::exr32}) {
    const auto path =
        (temp.path / ("encoded-" + std::to_string(static_cast<int>(format)))).string();
    if (format != p::output_format::exr32 || p::exr_available()) {
      succeeds(p::write_image(path, pixel.view(), {format}));
      const auto size = std::filesystem::file_size(path);
      rejects([&] { return p::write_image(path, pixel.view(), {format}); });
      require(std::filesystem::file_size(path) == size,
              "encoder must preserve existing output");
    }
  }
  const auto failed = (temp.path / "invalid.jpg").string();
  rejects([&] { return p::write_image(failed, pixel.view(), {p::output_format::jpeg, 101}); });
  require(!std::filesystem::exists(failed), "failed encoding must not publish a destination");
  // Exercise an actual short write/finalization failure, not only invalid arguments.
  const auto short_write = (temp.path / "short-write.jpg").string();
  const auto child = ::fork();
  require(child >= 0, "fork short-write test");
  if (child == 0) {
    const rlimit limit{1, 1};
    if (::setrlimit(RLIMIT_FSIZE, &limit) != 0)
      ::_exit(2);
    std::signal(SIGXFSZ, SIG_IGN);
    const auto written = p::write_image(short_write, pixel.view(), {p::output_format::jpeg});
    ::_exit(!written && written.error().code == p::error_code::io ? 0 : 3);
  }
  int status = 0;
  require(::waitpid(child, &status, 0) == child && WIFEXITED(status) &&
              WEXITSTATUS(status) == 0,
          "short write must return an I/O failure");
  require(!std::filesystem::exists(short_write), "short write must not publish partial output");
  rejects(
      [&] { return p::write_image((temp.path / "missing/out.tif").string(), pixel.view()); });
  for (const auto &entry : std::filesystem::directory_iterator(temp.path))
    require(entry.path().filename().string().find(".tmp-") == std::string::npos,
            "all temporary outputs must be cleaned up");
}

void io_tests() {
  temporary_directory temp;
  p::image<p::linear_rgb> image({10, 9});
  for (std::size_t y = 0; y < 9; ++y)
    for (std::size_t x = 0; x < 10; ++x)
      image.view().at(x, y) = {x / 10.f, y / 9.f, 0.18};
  // Non-contiguous source exercises encoder strides.
  const auto crop = image.view().subview({2, 1, 5, 6});
  const auto tiff_path = (temp.path / "roundtrip.tif").string();
  succeeds(p::write_image(tiff_path, crop));
  auto decoded = take(p::read_image(tiff_path));
  require(decoded.origin == p::input_origin::rendered_rgb,
          "TIFF is rendered, not recovered RAW");
  require(decoded.color_note.starts_with("embedded"), "output ICC required");
  require(decoded.pixels.size().width == 5 && decoded.pixels.size().height == 6,
          "TIFF dimensions");
  for (std::size_t y = 0; y < 6; ++y)
    for (std::size_t x = 0; x < 5; ++x) {
      auto a = crop.at(x, y), b = decoded.pixels.view().at(x, y);
      near(a.r, b.r, 0.0002);
      near(a.g, b.g, 0.0002);
      near(a.b, b.b, 0.0002);
    }
  TIFF *file = TIFFOpen(tiff_path.c_str(), "r");
  require(file != nullptr, "open output TIFF");
  std::uint16_t bits = 0;
  TIFFGetField(file, TIFFTAG_BITSPERSAMPLE, &bits);
  require(bits == 16, "default output must be 16-bit");
  TIFFClose(file);
  const auto jpg_path = (temp.path / "roundtrip.jpg").string();
  succeeds(p::write_image(jpg_path, crop, {p::output_format::jpeg, 100}));
  auto jpg = take(p::read_image(jpg_path));
  require(jpg.pixels.size().height == 6, "JPEG dimensions");
  near(jpg.pixels.view().at(2, 2).r, crop.at(2, 2).r, 0.025);
  require(jpg.color_note.starts_with("embedded"), "JPEG output ICC required");
  // Real PCB fixtures exposed TurboJPEG's absent-ICC error return. Keep a
  // minimal synthetic reproduction so this branch stays covered offline.
  const auto untagged_jpg_path = (temp.path / "untagged.jpg").string();
  auto encoder = tj3Init(TJINIT_COMPRESS);
  require(encoder != nullptr, "create JPEG fixture encoder");
  require(tj3Set(encoder, TJPARAM_QUALITY, 100) == 0, "fixture JPEG quality");
  require(tj3Set(encoder, TJPARAM_SUBSAMP, TJSAMP_444) == 0, "fixture JPEG sampling");
  unsigned char sample[]{128, 128, 128};
  unsigned char *jpeg_data = nullptr;
  std::size_t jpeg_size = 0;
  const auto jpeg_status =
      tj3Compress8(encoder, sample, 1, 0, 1, TJPF_RGB, &jpeg_data, &jpeg_size);
  tj3Destroy(encoder);
  require(jpeg_status == 0, "compress untagged JPEG");
  std::ofstream(untagged_jpg_path, std::ios::binary)
      .write(reinterpret_cast<const char *>(jpeg_data), jpeg_size);
  tj3Free(jpeg_data);
  auto untagged_jpg = take(p::read_image(untagged_jpg_path));
  require(untagged_jpg.color_note.starts_with("untagged"), "untagged JPEG fallback");
  near(untagged_jpg.pixels.view().at(0, 0).r, 0.2158605f, 0.005);
  const auto corrupt_path = (temp.path / "corrupt.jpg").string();
  std::ofstream(corrupt_path) << "this is not JPEG";
  rejects([&] { return p::read_image(corrupt_path); });
  rejects([&] { return p::read_image((temp.path / "missing.cr3").string()); });
  rejects([&] { return p::read_image((temp.path / "missing.tif").string()); });
  rejects([&] { return p::write_image(jpg_path, crop, {p::output_format::jpeg, 101}); });
  // Independent untagged 8-bit fixture validates decode transfer separately from encode.
  const auto plain = (temp.path / "untagged.tif").string();
  file = TIFFOpen(plain.c_str(), "w");
  require(file != nullptr, "create untagged fixture");
  TIFFSetField(file, TIFFTAG_IMAGEWIDTH, 1);
  TIFFSetField(file, TIFFTAG_IMAGELENGTH, 1);
  TIFFSetField(file, TIFFTAG_SAMPLESPERPIXEL, 3);
  TIFFSetField(file, TIFFTAG_BITSPERSAMPLE, 8);
  TIFFSetField(file, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
  TIFFSetField(file, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
  unsigned char data[]{128, 64, 255};
  require(TIFFWriteScanline(file, data, 0) >= 0, "write untagged fixture");
  TIFFClose(file);
  auto untagged = take(p::read_image(plain));
  near(untagged.pixels.view().at(0, 0).r, 0.2158605f);
  require(untagged.color_note.starts_with("untagged"), "sRGB assumption must be reported");
  // Same samples, but linear RGB ICC: 128 must stay near 0.502, not 0.216.
  const auto linear_path = (temp.path / "linear.tif").string();
  std::filesystem::copy_file(plain, linear_path);
  cmsCIExyY white{0.3127, 0.3290, 1};
  cmsCIExyYTRIPLE primaries{{0.64, 0.33, 1}, {0.30, 0.60, 1}, {0.15, 0.06, 1}};
  auto *curve = cmsBuildGamma(nullptr, 1);
  cmsToneCurve *curves[]{curve, curve, curve};
  auto profile = cmsCreateRGBProfile(&white, &primaries, curves);
  cmsFreeToneCurve(curve);
  require(profile != nullptr, "create linear fixture ICC");
  cmsUInt32Number profile_size = 0;
  require(cmsSaveProfileToMem(profile, nullptr, &profile_size), "measure fixture ICC");
  std::vector<unsigned char> profile_data(profile_size);
  require(cmsSaveProfileToMem(profile, profile_data.data(), &profile_size),
          "serialize fixture ICC");
  cmsCloseProfile(profile);
  file = TIFFOpen(linear_path.c_str(), "r+");
  require(file != nullptr, "open linear TIFF fixture");
  require(TIFFSetField(file, TIFFTAG_ICCPROFILE, profile_size, profile_data.data()),
          "attach fixture ICC");
  require(TIFFRewriteDirectory(file), "rewrite fixture directory");
  TIFFClose(file);
  auto linear = take(p::read_image(linear_path));
  near(linear.pixels.view().at(0, 0).r, 128.f / 255, 0.0002);
  // Reject unsupported orientation instead of quietly returning mirrored pixels.
  file = TIFFOpen(linear_path.c_str(), "r+");
  require(file != nullptr, "open orientation fixture");
  require(TIFFSetField(file, TIFFTAG_ORIENTATION, ORIENTATION_BOTLEFT), "set orientation");
  require(TIFFRewriteDirectory(file), "rewrite orientation fixture");
  TIFFClose(file);
  rejects([&] { return p::read_image(linear_path); });
  image.view().at(0, 0) = {-0.1, 2.f, 0.18};
  const auto clipped_path = (temp.path / "clipped.tif").string();
  succeeds(p::write_image(clipped_path, image.view()));
  auto clipped = take(p::read_image(clipped_path));
  near(clipped.pixels.view().at(0, 0).r, 0.f, 0.0002);
  near(clipped.pixels.view().at(0, 0).g, 1.f, 0.0002);
  const auto exr_path = (temp.path / "range.exr").string();
  if (p::exr_available()) {
    succeeds(p::write_image(exr_path, image.view(), {p::output_format::exr32}));
    require(std::filesystem::file_size(exr_path) > 0, "EXR output");
#ifdef PHOTO_OPS_EXR
    namespace exr = OPENEXR_IMF_NAMESPACE;
    exr::InputFile file(exr_path.c_str());
    require(file.header().channels().findChannel("R")->type == exr::FLOAT,
            "EXR must use float32");
    std::array<float, 90> red{};
    exr::FrameBuffer buffer;
    buffer.insert("R", exr::Slice(exr::FLOAT, reinterpret_cast<char *>(red.data()),
                                  sizeof(float), 10 * sizeof(float)));
    file.setFrameBuffer(buffer);
    file.readPixels(0, 8);
    near(red[0], -0.1f, 0);
    std::array<float, 90> green{};
    exr::FrameBuffer green_buffer;
    green_buffer.insert("G", exr::Slice(exr::FLOAT, reinterpret_cast<char *>(green.data()),
                                        sizeof(float), 10 * sizeof(float)));
    file.setFrameBuffer(green_buffer);
    file.readPixels(0, 8);
    near(green[0], 2.f, 0);
#endif
  } else
    rejects([&] { return p::write_image(exr_path, image.view(), {p::output_format::exr32}); });
  const auto stats = p::statistics(image.view());
  require(stats.outside_unit_pixels == 1, "HDR/negative diagnostics");
  image.view().at(0, 0).r = std::numeric_limits<float>::quiet_NaN();
  require(p::statistics(image.view()).non_finite_pixels == 1, "NaN diagnostics");
  rejects([&] { return p::write_image(tiff_path, image.view()); });
}
} // namespace

int main() {
  try {
    image_tests();
    color_tests();
    pyramid_tests();
    atomic_output_tests();
    io_tests();
    sunlight_tests();
    focus_stack_tests();
    raw_sequence_tests();
    std::cout << checks << " checks passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "failed after " << checks << " checks: " << e.what() << '\n';
    return 1;
  }
}
