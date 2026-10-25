// SPDX-License-Identifier: GPL-3.0-or-later
module;
#include <array>
#include <cmath>
#include <cstdint>
#include <expected>
#include <lcms2.h>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <tiffio.h>
#include <vector>
module photo_ops.io;
import photo_ops.image;
import photo_ops.output;

namespace photo_ops {
namespace {
struct profile_deleter {
  void operator()(void *p) const noexcept {
    cmsCloseProfile(p);
  }
};

using icc_profile = std::unique_ptr<void, profile_deleter>;
using tiff_file = std::unique_ptr<TIFF, decltype(&TIFFClose)>;

bool valid_encoding(const color_encoding &c) {
  return (c.primaries == rgb_primaries::srgb || c.primaries == rgb_primaries::rec2020) &&
         c.white == white_point::d65 && c.transfer == transfer_function::linear &&
         (c.origin == image_origin::scene_linear_raw ||
          c.origin == image_origin::rendered_rgb) &&
         c.normalization.size() <= 1024 &&
         (c.origin != image_origin::scene_linear_raw || !c.normalization.empty()) &&
         c.normalization.find_first_of(";\n\r") == std::string::npos;
}

result<icc_profile> working_profile(const color_encoding &encoding) {
  const cmsCIExyY white{.3127, .3290, 1};
  const cmsCIExyYTRIPLE primaries =
      encoding.primaries == rgb_primaries::rec2020
          ? cmsCIExyYTRIPLE{{.708, .292, 1}, {.170, .797, 1}, {.131, .046, 1}}
          : cmsCIExyYTRIPLE{{.640, .330, 1}, {.300, .600, 1}, {.150, .060, 1}};
  auto *curve = cmsBuildGamma(nullptr, 1);
  if (!curve)
    return fail(error_code::color, "working ICC", "cannot allocate linear curve");
  cmsToneCurve *curves[]{curve, curve, curve};
  icc_profile profile(cmsCreateRGBProfile(&white, &primaries, curves));
  cmsFreeToneCurve(curve);
  if (!profile)
    return fail(error_code::color, "working ICC", "cannot create RGB profile");
  return profile;
}

std::string description(const color_encoding &c) {
  return std::string("photo-ops-float-v1;") +
         (c.primaries == rgb_primaries::rec2020 ? "rec2020" : "srgb") + ";" +
         (c.origin == image_origin::scene_linear_raw ? "raw" : "rendered") + ";" +
         c.normalization;
}

result<color_encoding> parse_description(const std::string &text) {
  const std::string prefix = "photo-ops-float-v1;";
  if (!text.starts_with(prefix))
    return fail(error_code::color, "read working TIFF", "missing explicit working encoding");
  const auto separator = text.find(';', prefix.size()), end = text.find(';', separator + 1);
  if (separator == std::string::npos || end == std::string::npos)
    return fail(error_code::color, "read working TIFF", "malformed encoding");
  const auto primary = text.substr(prefix.size(), separator - prefix.size());
  const auto origin = text.substr(separator + 1, end - separator - 1);
  if ((primary != "srgb" && primary != "rec2020") || (origin != "raw" && origin != "rendered"))
    return fail(error_code::color, "read working TIFF", "unknown working primaries or origin");
  color_encoding encoding{primary == "srgb" ? rgb_primaries::srgb : rgb_primaries::rec2020,
                          white_point::d65, transfer_function::linear,
                          origin == "raw" ? image_origin::scene_linear_raw
                                          : image_origin::rendered_rgb,
                          text.substr(end + 1)};
  if (!valid_encoding(encoding))
    return fail(error_code::color, "read working TIFF", "invalid normalization provenance");
  return encoding;
}

bool matching_profile(cmsHPROFILE profile, cmsHPROFILE expected) {
  if (cmsGetColorSpace(profile) != cmsSigRgbData)
    return false;
  for (auto tag : {cmsSigRedTRCTag, cmsSigGreenTRCTag, cmsSigBlueTRCTag}) {
    const auto *curve = static_cast<const cmsToneCurve *>(cmsReadTag(profile, tag));
    if (!curve || !cmsIsToneCurveLinear(curve))
      return false;
  }
  for (auto tag : {cmsSigRedColorantTag, cmsSigGreenColorantTag, cmsSigBlueColorantTag,
                   cmsSigMediaWhitePointTag}) {
    const auto *a = static_cast<const cmsCIEXYZ *>(cmsReadTag(profile, tag));
    const auto *b = static_cast<const cmsCIEXYZ *>(cmsReadTag(expected, tag));
    if (!a || !b || std::abs(a->X - b->X) > 5e-5 || std::abs(a->Y - b->Y) > 5e-5 ||
        std::abs(a->Z - b->Z) > 5e-5)
      return false;
  }
  return true;
}
} // namespace

result<void> write_working_tiff_rows(const std::string &path, extent size,
                                     const color_encoding &encoding,
                                     const working_row_reader &read_row) {
  if (!size.valid() || !size.width || size.width > 65536 || size.height > 65536 ||
      size.width * size.height > 200000000 || !read_row || !valid_encoding(encoding))
    return fail(error_code::invalid_argument, "write working TIFF",
                "invalid dimensions, reader or encoding", path);
  auto profile = working_profile(encoding);
  if (!profile)
    return std::unexpected(profile.error());
  cmsUInt32Number count = 0;
  if (!cmsSaveProfileToMem(profile->get(), nullptr, &count))
    return fail(error_code::color, "write working TIFF", "cannot measure ICC", path);
  std::vector<unsigned char> icc(count);
  if (!cmsSaveProfileToMem(profile->get(), icc.data(), &count))
    return fail(error_code::color, "write working TIFF", "cannot serialize ICC", path);
  auto transaction = atomic_output::create(path);
  if (!transaction)
    return std::unexpected(transaction.error());
  {
    tiff_file file(TIFFOpen(transaction->temporary_path().c_str(), "w"), TIFFClose);
    if (!file)
      return fail(error_code::io, "write working TIFF", "cannot create TIFF", path);
    const auto set = [&](auto tag, auto... values) {
      return TIFFSetField(file.get(), tag, values...) != 0;
    };
    const auto note = description(encoding);
    if (!(set(TIFFTAG_IMAGEWIDTH, static_cast<std::uint32_t>(size.width)) &&
          set(TIFFTAG_IMAGELENGTH, static_cast<std::uint32_t>(size.height)) &&
          set(TIFFTAG_BITSPERSAMPLE, 32) && set(TIFFTAG_SAMPLESPERPIXEL, 3) &&
          set(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB) &&
          set(TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG) &&
          set(TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_IEEEFP) &&
          set(TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT) &&
          set(TIFFTAG_COMPRESSION, COMPRESSION_ADOBE_DEFLATE) &&
          set(TIFFTAG_PREDICTOR, PREDICTOR_FLOATINGPOINT) &&
          set(TIFFTAG_ROWSPERSTRIP, TIFFDefaultStripSize(file.get(), 0)) &&
          set(TIFFTAG_ICCPROFILE, count, icc.data()) &&
          set(TIFFTAG_IMAGEDESCRIPTION, note.c_str())))
      return fail(error_code::encode, "write working TIFF", "cannot set output tags", path);
    std::vector<rgb_f32> row(size.width);
    for (std::size_t y = 0; y < size.height; ++y) {
      if (auto read = read_row(y, row); !read)
        return read;
      for (const auto p : row)
        if (!std::isfinite(p.r) || !std::isfinite(p.g) || !std::isfinite(p.b))
          return fail(error_code::invalid_argument, "write working TIFF",
                      "non-finite working RGB", path);
      if (TIFFWriteScanline(file.get(), row.data(), y) < 0)
        return fail(error_code::io, "write working TIFF", "cannot write scanline", path);
    }
    if (!TIFFWriteDirectory(file.get()) || !TIFFFlush(file.get()))
      return fail(error_code::io, "write working TIFF", "cannot finalize TIFF", path);
  }
  return transaction->commit();
}

result<working_image> read_working_tiff(const std::string &path) {
  tiff_file file(TIFFOpen(path.c_str(), "r"), TIFFClose);
  if (!file)
    return fail(error_code::io, "read working TIFF", "cannot open TIFF", path);
  std::uint32_t width = 0, height = 0;
  std::uint16_t bits = 0, channels = 0, format = 0, planar = 0, photometric = 0,
                orientation = 0;
  TIFFGetField(file.get(), TIFFTAG_IMAGEWIDTH, &width);
  TIFFGetField(file.get(), TIFFTAG_IMAGELENGTH, &height);
  TIFFGetFieldDefaulted(file.get(), TIFFTAG_BITSPERSAMPLE, &bits);
  TIFFGetFieldDefaulted(file.get(), TIFFTAG_SAMPLESPERPIXEL, &channels);
  TIFFGetFieldDefaulted(file.get(), TIFFTAG_SAMPLEFORMAT, &format);
  TIFFGetFieldDefaulted(file.get(), TIFFTAG_PLANARCONFIG, &planar);
  TIFFGetField(file.get(), TIFFTAG_PHOTOMETRIC, &photometric);
  TIFFGetFieldDefaulted(file.get(), TIFFTAG_ORIENTATION, &orientation);
  if (!width || !height || width > 65536 || height > 65536 ||
      std::uint64_t(width) * height > 200000000 || bits != 32 || channels != 3 ||
      format != SAMPLEFORMAT_IEEEFP || planar != PLANARCONFIG_CONTIG ||
      photometric != PHOTOMETRIC_RGB || orientation != ORIENTATION_TOPLEFT ||
      TIFFIsTiled(file.get()))
    return fail(error_code::unsupported_format, "read working TIFF",
                "requires stripped float32 RGB, top-left, <=200 MP", path);
  char *text = nullptr;
  if (!TIFFGetField(file.get(), TIFFTAG_IMAGEDESCRIPTION, &text) || !text)
    return fail(error_code::color, "read working TIFF", "missing working provenance", path);
  auto encoding = parse_description(text);
  if (!encoding)
    return std::unexpected(encoding.error());
  auto expected = working_profile(*encoding);
  if (!expected)
    return std::unexpected(expected.error());
  std::uint32_t count = 0;
  void *bytes = nullptr;
  if (!TIFFGetField(file.get(), TIFFTAG_ICCPROFILE, &count, &bytes) || !count ||
      count > 16 * 1024 * 1024)
    return fail(error_code::color, "read working TIFF", "missing or oversized ICC", path);
  icc_profile profile(cmsOpenProfileFromMem(bytes, count));
  if (!profile || !matching_profile(profile.get(), expected->get()))
    return fail(error_code::color, "read working TIFF",
                "ICC contradicts declared working encoding", path);
  image<rgb_f32> pixels({width, height});
  for (std::uint32_t y = 0; y < height; ++y) {
    if (TIFFReadScanline(file.get(), pixels.view().row(y).data(), y) < 0)
      return fail(error_code::io, "read working TIFF", "cannot read scanline", path);
    for (const auto p : pixels.view().row(y))
      if (!std::isfinite(p.r) || !std::isfinite(p.g) || !std::isfinite(p.b))
        return fail(error_code::color, "read working TIFF", "non-finite RGB", path);
  }
  return working_image(std::move(pixels), std::move(*encoding));
}
} // namespace photo_ops
