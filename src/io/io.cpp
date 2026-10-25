// SPDX-License-Identifier: GPL-3.0-or-later
module;
#include <lcms2.h>
#include <tiffio.h>
#include <turbojpeg.h>
#ifdef PHOTO_OPS_EXR
#include <OpenEXR/IexBaseExc.h>
#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfOutputFile.h>
#include <OpenEXR/ImfStandardAttributes.h>
#endif
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <expected>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <vector>
module photo_ops.io;
import photo_ops.image;
import photo_ops.color;
import photo_ops.raw;
import photo_ops.output;

namespace photo_ops {
namespace {
struct close_profile {
  void operator()(void *p) const noexcept {
    cmsCloseProfile(p);
  }
};

struct close_transform {
  void operator()(void *p) const noexcept {
    cmsDeleteTransform(p);
  }
};

using profile = std::unique_ptr<void, close_profile>;
using transform = std::unique_ptr<void, close_transform>;
using tiff = std::unique_ptr<TIFF, decltype(&TIFFClose)>;

struct close_jpeg {
  void operator()(void *p) const noexcept {
    tj3Destroy(p);
  }
};

using jpeg = std::unique_ptr<void, close_jpeg>;

struct free_jpeg {
  void operator()(unsigned char *p) const noexcept {
    tj3Free(p);
  }
};

using jpeg_bytes = std::unique_ptr<unsigned char, free_jpeg>;

std::string extension(const std::string &path) {
  const auto dot = path.find_last_of('.');
  auto ext = dot == std::string::npos ? "" : path.substr(dot);
  std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return std::tolower(c); });
  return ext;
}

result<profile> linear_profile() {
  cmsCIExyY white{0.3127, 0.3290, 1.0};
  cmsCIExyYTRIPLE primaries{{0.64, 0.33, 1}, {0.30, 0.60, 1}, {0.15, 0.06, 1}};
  auto *curve = cmsBuildGamma(nullptr, 1.0);
  if (!curve)
    return fail(error_code::color, "color conversion", "cannot allocate linear transfer curve");
  cmsToneCurve *curves[]{curve, curve, curve};
  profile result(cmsCreateRGBProfile(&white, &primaries, curves));
  cmsFreeToneCurve(curve);
  if (!result)
    return fail(error_code::color, "color conversion", "cannot create linear RGB profile");
  return result;
}

result<std::vector<unsigned char>> srgb_profile_bytes() {
  profile p(cmsCreate_sRGBProfile());
  cmsUInt32Number size = 0;
  if (!p || !cmsSaveProfileToMem(p.get(), nullptr, &size))
    return fail(error_code::color, "color conversion", "cannot create sRGB ICC profile");
  std::vector<unsigned char> bytes(size);
  if (!cmsSaveProfileToMem(p.get(), bytes.data(), &size))
    return fail(error_code::color, "color conversion", "cannot serialize sRGB ICC profile");
  return bytes;
}

result<void> to_working(image<linear_rgb> &image, const void *bytes, std::uint32_t size) {
  if (!size) {
    for (std::size_t y = 0; y < image.size().height; ++y)
      for (auto &p : image.view().row(y))
        p = {srgb_to_linear(p.r), srgb_to_linear(p.g), srgb_to_linear(p.b)};
    return {};
  }
  // Embedded profiles take precedence over the explicit untagged-sRGB fallback.
  profile source(cmsOpenProfileFromMem(bytes, size));
  if (!source || cmsGetColorSpace(source.get()) != cmsSigRgbData)
    return fail(error_code::color, "color conversion", "invalid or non-RGB ICC profile");
  auto destination = linear_profile();
  if (!destination)
    return std::unexpected(destination.error());
  transform convert(cmsCreateTransform(source.get(), TYPE_RGB_FLT, destination->get(),
                                       TYPE_RGB_FLT, INTENT_RELATIVE_COLORIMETRIC,
                                       cmsFLAGS_NOOPTIMIZE));
  if (!convert)
    return fail(error_code::color, "color conversion", "cannot construct ICC transform");
  for (std::size_t y = 0; y < image.size().height; ++y) {
    auto row = image.view().row(y);
    cmsDoTransform(convert.get(), row.data(), row.data(), row.size());
  }
  return {};
}

result<decoded_image> read_tiff(const std::string &path) {
  tiff file(TIFFOpen(path.c_str(), "r"), TIFFClose);
  if (!file)
    return fail(error_code::io, "image I/O", "cannot open TIFF: " + path);
  // Read and validate storage metadata before allocating a project image.
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint16_t bits = 0;
  std::uint16_t channels = 0;
  std::uint16_t photometric = 0;
  std::uint16_t planar_config = 0;
  std::uint16_t sample_format = 0;
  std::uint16_t orientation = 0;
  TIFFGetField(file.get(), TIFFTAG_IMAGEWIDTH, &width);
  TIFFGetField(file.get(), TIFFTAG_IMAGELENGTH, &height);
  TIFFGetFieldDefaulted(file.get(), TIFFTAG_BITSPERSAMPLE, &bits);
  TIFFGetFieldDefaulted(file.get(), TIFFTAG_SAMPLESPERPIXEL, &channels);
  TIFFGetFieldDefaulted(file.get(), TIFFTAG_PHOTOMETRIC, &photometric);
  TIFFGetFieldDefaulted(file.get(), TIFFTAG_PLANARCONFIG, &planar_config);
  TIFFGetFieldDefaulted(file.get(), TIFFTAG_SAMPLEFORMAT, &sample_format);
  TIFFGetFieldDefaulted(file.get(), TIFFTAG_ORIENTATION, &orientation);
  if (!width || !height || (bits != 8 && bits != 16) || channels != 3 ||
      photometric != PHOTOMETRIC_RGB || planar_config != PLANARCONFIG_CONTIG ||
      sample_format != SAMPLEFORMAT_UINT || orientation != ORIENTATION_TOPLEFT ||
      TIFFIsTiled(file.get()))
    return fail(error_code::unsupported_format, "read image",
                "unsupported TIFF layout (need stripped top-left 8/16-bit RGB, no alpha)");
  if (!image<linear_rgb>::can_hold({width, height}))
    return fail(error_code::decode, "read TIFF", "image dimensions exceed storage limits");
  // Decode unsigned scanlines without involving transfer functions yet.
  image<linear_rgb> pixels({width, height});
  const auto row_bytes = TIFFScanlineSize(file.get());
  if (row_bytes < static_cast<tmsize_t>(width) * 3 * (bits / 8))
    return fail(error_code::io, "image I/O", "invalid TIFF scanline size");
  std::vector<std::uint16_t> row((static_cast<std::size_t>(row_bytes) + 1) / 2);
  for (std::uint32_t y = 0; y < height; ++y) {
    if (TIFFReadScanline(file.get(), row.data(), y) < 0)
      return fail(error_code::io, "image I/O", "failed to read TIFF scanline");
    auto *samples8 = reinterpret_cast<const unsigned char *>(row.data());
    for (std::uint32_t x = 0; x < width; ++x) {
      const auto sample_index = 3 * static_cast<std::size_t>(x);
      pixels.view().at(x, y) =
          bits == 16
              ? linear_rgb{row[sample_index] / 65535.f, row[sample_index + 1] / 65535.f,
                           row[sample_index + 2] / 65535.f}
              : linear_rgb{samples8[sample_index] / 255.f, samples8[sample_index + 1] / 255.f,
                           samples8[sample_index + 2] / 255.f};
    }
  }
  // Interpret color after storage decoding; an absent ICC is an explicit sRGB assumption.
  void *icc = nullptr;
  std::uint32_t icc_size = 0;
  TIFFGetField(file.get(), TIFFTAG_ICCPROFILE, &icc_size, &icc);
  // A TIFF transfer function without ICC cannot safely be assumed sRGB.
  std::uint16_t *tr = nullptr, *tg = nullptr, *tb = nullptr;
  if (!icc_size && TIFFGetField(file.get(), TIFFTAG_TRANSFERFUNCTION, &tr, &tg, &tb))
    return fail(error_code::unsupported_format, "read image",
                "TIFF transfer function without ICC is unsupported");
  if (auto converted = to_working(pixels, icc, icc_size); !converted)
    return std::unexpected(converted.error());
  return decoded_image{std::move(pixels), input_origin::rendered_rgb,
                       icc_size ? "embedded RGB ICC -> linear sRGB/D65"
                                : "untagged TIFF: assumed sRGB -> linear sRGB/D65"};
}

result<void> check_jpeg_status(void *handle, error_code code, int status) {
  if (status < 0)
    return fail(code, "JPEG codec", tj3GetErrorStr(handle));
  return {};
}

result<decoded_image> read_jpeg(const std::string &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file)
    return fail(error_code::io, "image I/O", "cannot open JPEG: " + path);
  std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(file), {}};
  jpeg decoder(tj3Init(TJINIT_DECOMPRESS));
  if (!decoder)
    return fail(error_code::io, "image I/O", "cannot create JPEG decoder");
  if (auto checked = check_jpeg_status(decoder.get(), error_code::decode,
                                       tj3Set(decoder.get(), TJPARAM_STOPONWARNING, 1));
      !checked)
    return std::unexpected(checked.error());
  if (auto checked = check_jpeg_status(decoder.get(), error_code::decode,
                                       tj3Set(decoder.get(), TJPARAM_SAVEMARKERS, 4));
      !checked)
    return std::unexpected(checked.error());
  if (auto checked =
          check_jpeg_status(decoder.get(), error_code::decode,
                            tj3DecompressHeader(decoder.get(), bytes.data(), bytes.size()));
      !checked)
    return std::unexpected(checked.error());
  const auto width = tj3Get(decoder.get(), TJPARAM_JPEGWIDTH);
  const auto height = tj3Get(decoder.get(), TJPARAM_JPEGHEIGHT);
  if (width <= 0 || height <= 0 || tj3Get(decoder.get(), TJPARAM_PRECISION) != 8)
    return fail(error_code::unsupported_format, "read image", "expected 8-bit JPEG");
  const auto space = tj3Get(decoder.get(), TJPARAM_COLORSPACE);
  if (space == TJCS_CMYK || space == TJCS_YCCK)
    return fail(error_code::unsupported_format, "read image", "CMYK JPEG is unsupported");
  const extent dimensions{static_cast<std::size_t>(width), static_cast<std::size_t>(height)};
  if (!image<linear_rgb>::can_hold(dimensions))
    return fail(error_code::decode, "read JPEG", "image dimensions exceed storage limits");
  // The JPEG decoder returns encoded RGB; color linearization is a separate step.
  image<linear_rgb> pixels(dimensions);
  std::vector<unsigned char> rgb(static_cast<std::size_t>(width) * height * 3);
  if (auto checked = check_jpeg_status(
          decoder.get(), error_code::decode,
          tj3Decompress8(decoder.get(), bytes.data(), bytes.size(), rgb.data(), 0, TJPF_RGB));
      !checked)
    return std::unexpected(checked.error());
  for (int y = 0; y < height; ++y)
    for (int x = 0; x < width; ++x) {
      const auto sample_index = 3 * (static_cast<std::size_t>(y) * width + x);
      pixels.view().at(x, y) = {rgb[sample_index] / 255.f, rgb[sample_index + 1] / 255.f,
                                rgb[sample_index + 2] / 255.f};
    }
  // Preserve the distinction between a missing profile and a malformed embedded profile.
  unsigned char *icc_raw = nullptr;
  std::size_t icc_size = 0;
  const auto status = tj3GetICCProfile(decoder.get(), &icc_raw, &icc_size);
  jpeg_bytes icc(icc_raw);
  // TurboJPEG 3.2 reports absent ICC as an error despite setting size to 0.
  // Header parsing already rejects malformed profiles; only this exact absence
  // condition permits the explicit untagged-sRGB fallback.
  if (status < 0 && !(icc_size == 0 && !icc &&
                      std::string(tj3GetErrorStr(decoder.get())) ==
                          "tj3GetICCProfile(): No ICC profile data has been extracted"))
    if (auto checked = check_jpeg_status(decoder.get(), error_code::decode, status); !checked)
      return std::unexpected(checked.error());
  if (icc_size > std::numeric_limits<std::uint32_t>::max())
    return fail(error_code::io, "image I/O", "ICC profile too large");
  if (auto converted = to_working(pixels, icc.get(), icc_size); !converted)
    return std::unexpected(converted.error());
  return decoded_image{std::move(pixels), input_origin::rendered_rgb,
                       icc_size ? "embedded RGB ICC -> linear sRGB/D65"
                                : "untagged JPEG: assumed sRGB -> linear sRGB/D65"};
}

float encode(float v) {
  return std::clamp(linear_to_srgb(v), 0.f, 1.f);
}

result<void> write_tiff(const std::string &path, image_view<const linear_rgb> pixels) {
  tiff file(TIFFOpen(path.c_str(), "w"), TIFFClose);
  if (!file)
    return fail(error_code::io, "image I/O", "cannot create TIFF: " + path);
  auto icc = srgb_profile_bytes();
  if (!icc)
    return std::unexpected(icc.error());
  const auto set = [&](auto tag, auto... values) {
    return TIFFSetField(file.get(), tag, values...) != 0;
  };
  if (!(set(TIFFTAG_IMAGEWIDTH, static_cast<std::uint32_t>(pixels.size().width)) &&
        set(TIFFTAG_IMAGELENGTH, static_cast<std::uint32_t>(pixels.size().height)) &&
        set(TIFFTAG_BITSPERSAMPLE, 16) && set(TIFFTAG_SAMPLESPERPIXEL, 3) &&
        set(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB) &&
        set(TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG) &&
        set(TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_UINT) &&
        set(TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT) &&
        set(TIFFTAG_COMPRESSION, COMPRESSION_ADOBE_DEFLATE) &&
        set(TIFFTAG_PREDICTOR, PREDICTOR_HORIZONTAL) &&
        set(TIFFTAG_ROWSPERSTRIP, TIFFDefaultStripSize(file.get(), 0)) &&
        set(TIFFTAG_ICCPROFILE, static_cast<std::uint32_t>(icc->size()), icc->data())))
    return fail(error_code::encode, "configure TIFF", "cannot set output tags");
  std::vector<std::uint16_t> row(pixels.size().width * 3);
  for (std::size_t y = 0; y < pixels.size().height; ++y) {
    for (std::size_t x = 0; x < pixels.size().width; ++x) {
      const auto p = pixels.at(x, y);
      row[3 * x] = std::lround(65535.f * encode(p.r));
      row[3 * x + 1] = std::lround(65535.f * encode(p.g));
      row[3 * x + 2] = std::lround(65535.f * encode(p.b));
    }
    if (TIFFWriteScanline(file.get(), row.data(), y) < 0)
      return fail(error_code::io, "image I/O", "cannot write TIFF scanline");
  }
  if (!TIFFWriteDirectory(file.get()) || !TIFFFlush(file.get()))
    return fail(error_code::io, "image I/O", "cannot finalize TIFF");
  return {};
}

result<void> write_jpeg(const std::string &path, image_view<const linear_rgb> pixels,
                        int quality) {
  if (quality < 1 || quality > 100)
    return fail(error_code::invalid_argument, "write image", "JPEG quality must be 1..100");
  jpeg encoder(tj3Init(TJINIT_COMPRESS));
  if (!encoder)
    return fail(error_code::io, "image I/O", "cannot create JPEG encoder");
  if (auto checked = check_jpeg_status(encoder.get(), error_code::encode,
                                       tj3Set(encoder.get(), TJPARAM_QUALITY, quality));
      !checked)
    return std::unexpected(checked.error());
  if (auto checked = check_jpeg_status(encoder.get(), error_code::encode,
                                       tj3Set(encoder.get(), TJPARAM_SUBSAMP, TJSAMP_444));
      !checked)
    return std::unexpected(checked.error());
  auto icc = srgb_profile_bytes();
  if (!icc)
    return std::unexpected(icc.error());
  if (auto checked =
          check_jpeg_status(encoder.get(), error_code::encode,
                            tj3SetICCProfile(encoder.get(), icc->data(), icc->size()));
      !checked)
    return std::unexpected(checked.error());
  // JPEG is an output encoding boundary: transfer, quantize, then compress.
  std::vector<unsigned char> rgb(pixels.size().width * pixels.size().height * 3);
  for (std::size_t y = 0; y < pixels.size().height; ++y)
    for (std::size_t x = 0; x < pixels.size().width; ++x) {
      auto p = pixels.at(x, y);
      auto i = 3 * (y * pixels.size().width + x);
      rgb[i] = std::lround(255.f * encode(p.r));
      rgb[i + 1] = std::lround(255.f * encode(p.g));
      rgb[i + 2] = std::lround(255.f * encode(p.b));
    }
  unsigned char *raw = nullptr;
  std::size_t size = 0;
  const auto status = tj3Compress8(encoder.get(), rgb.data(), pixels.size().width, 0,
                                   pixels.size().height, TJPF_RGB, &raw, &size);
  jpeg_bytes bytes(raw);
  if (auto checked = check_jpeg_status(encoder.get(), error_code::encode, status); !checked)
    return std::unexpected(checked.error());
  std::ofstream file(path, std::ios::binary);
  if (!file.write(reinterpret_cast<const char *>(bytes.get()), size))
    return fail(error_code::io, "image I/O", "cannot write JPEG: " + path);
  file.close();
  if (!file)
    return fail(error_code::io, "image I/O", "cannot finalize JPEG: " + path);
  return {};
}
} // namespace

result<decoded_image> read_image(const std::string &path, bool half_size_raw) {
  auto read = [&]() -> result<decoded_image> {
    const auto ext = extension(path);
    if (ext == ".cr3") {
      return decode_raw(path, {half_size_raw}).transform([](image<linear_rgb> pixels) {
        return decoded_image{
            std::move(pixels), input_origin::scene_linear_raw,
            "LibRaw camera WB; linear sRGB/D65; fixed exposure; 16-bit intermediate"};
      });
    }
    if (ext == ".tif" || ext == ".tiff")
      return read_tiff(path);
    if (ext == ".jpg" || ext == ".jpeg")
      return read_jpeg(path);
    return fail(error_code::unsupported_format, "read image", "unsupported extension: " + ext);
  };
  return read().transform_error([&](error failure) {
    failure.path = path;
    return failure;
  });
}

bool exr_available() noexcept {
#ifdef PHOTO_OPS_EXR
  return true;
#else
  return false;
#endif
}

result<void> write_encoded_image(const std::string &path, image_view<const linear_rgb> pixels,
                                 output_options options) {
  if (!pixels.size().width || !pixels.size().height ||
      pixels.size().width > std::numeric_limits<int>::max() ||
      pixels.size().height > std::numeric_limits<int>::max())
    return fail(error_code::invalid_argument, "write image",
                "output image must have positive int-sized dimensions");
  for (std::size_t y = 0; y < pixels.size().height; ++y)
    for (auto p : pixels.row(y))
      if (!std::isfinite(p.r) || !std::isfinite(p.g) || !std::isfinite(p.b))
        return fail(error_code::invalid_argument, "write image",
                    "cannot encode non-finite RGB");
  switch (options.format) {
  case output_format::tiff16:
    return write_tiff(path, pixels);
  case output_format::jpeg:
    return write_jpeg(path, pixels, options.jpeg_quality);
  case output_format::exr32:
#ifdef PHOTO_OPS_EXR
    try {
      namespace exr = OPENEXR_IMF_NAMESPACE;
      exr::Header header(pixels.size().width, pixels.size().height);
      for (const auto *channel : {"R", "G", "B"})
        header.channels().insert(channel, exr::Channel(exr::FLOAT));
      exr::addChromaticities(header, exr::Chromaticities()); // Rec.709/sRGB D65
      exr::FrameBuffer frame;
      auto *base = reinterpret_cast<char *>(const_cast<linear_rgb *>(pixels.row(0).data()));
      const auto xs = sizeof(linear_rgb), ys = pixels.stride() * xs;
      frame.insert("R", exr::Slice(exr::FLOAT, base + offsetof(linear_rgb, r), xs, ys));
      frame.insert("G", exr::Slice(exr::FLOAT, base + offsetof(linear_rgb, g), xs, ys));
      frame.insert("B", exr::Slice(exr::FLOAT, base + offsetof(linear_rgb, b), xs, ys));
      exr::OutputFile file(path.c_str(), header);
      file.setFrameBuffer(frame);
      file.writePixels(pixels.size().height);
      return {};
    } catch (const IEX_NAMESPACE::BaseExc &exception) {
      return fail(error_code::io, "write EXR", exception.what(), path);
    }
#else
    return fail(error_code::unavailable, "write EXR",
                "EXR output disabled; configure with --exr=y");
#endif
  }
  return fail(error_code::invalid_argument, "write image", "unknown output format");
}

result<void> write_image(const std::string &path, image_view<const linear_rgb> pixels,
                         output_options options) {
  auto transaction = atomic_output::create(path);
  if (!transaction)
    return std::unexpected(transaction.error());
  auto encoded = write_encoded_image(transaction->temporary_path(), pixels, options);
  if (encoded)
    encoded = transaction->commit();
  return encoded.transform_error([&](error failure) {
    failure.path = path;
    return failure;
  });
}
} // namespace photo_ops
