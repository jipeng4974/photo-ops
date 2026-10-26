// SPDX-License-Identifier: GPL-3.0-or-later
module;
#include <cmath>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <tiffio.h>
#include <vector>
module photo_ops.io;
import photo_ops.image;
import photo_ops.output;

namespace photo_ops {
namespace {
using scalar_tiff = std::unique_ptr<TIFF, decltype(&TIFFClose)>;
constexpr auto scalar_prefix = "photo-ops-scalar-v1;";

bool valid_scalar_meaning(const std::string &value) {
  return !value.empty() && value.size() <= 4096 &&
         value.find_first_of("\n\r") == std::string::npos &&
         value.find('\0') == std::string::npos;
}
} // namespace

result<void> write_scalar_tiff_rows(const std::string &path, extent size,
                                    const std::string &meaning,
                                    const scalar_row_reader &read_row) {
  if (!size.valid() || !size.width || size.width > 65536 || size.height > 65536 ||
      size.width * size.height > 200000000 || !valid_scalar_meaning(meaning) || !read_row)
    return fail(error_code::invalid_argument, "write scalar TIFF",
                "invalid shape/meaning/reader", path);
  auto transaction = atomic_output::create(path);
  if (!transaction)
    return std::unexpected(transaction.error());
  {
    scalar_tiff file(TIFFOpen(transaction->temporary_path().c_str(), "w"), TIFFClose);
    if (!file)
      return fail(error_code::io, "write scalar TIFF", "cannot create TIFF", path);
    const auto set = [&](auto tag, auto... values) {
      return TIFFSetField(file.get(), tag, values...) != 0;
    };
    const auto description = std::string(scalar_prefix) + meaning;
    if (!(set(TIFFTAG_IMAGEWIDTH, static_cast<std::uint32_t>(size.width)) &&
          set(TIFFTAG_IMAGELENGTH, static_cast<std::uint32_t>(size.height)) &&
          set(TIFFTAG_BITSPERSAMPLE, 32) && set(TIFFTAG_SAMPLESPERPIXEL, 1) &&
          set(TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_IEEEFP) &&
          set(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK) &&
          set(TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG) &&
          set(TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT) &&
          set(TIFFTAG_COMPRESSION, COMPRESSION_ADOBE_DEFLATE) &&
          set(TIFFTAG_PREDICTOR, PREDICTOR_FLOATINGPOINT) &&
          set(TIFFTAG_ROWSPERSTRIP, TIFFDefaultStripSize(file.get(), 0)) &&
          set(TIFFTAG_IMAGEDESCRIPTION, description.c_str())))
      return fail(error_code::encode, "write scalar TIFF", "cannot set tags", path);
    std::vector<gray> row(size.width);
    for (std::size_t y = 0; y < size.height; ++y) {
      if (auto read = read_row(y, row); !read)
        return read;
      for (auto p : row)
        if (!std::isfinite(p.value))
          return fail(error_code::invalid_argument, "write scalar TIFF", "nonfinite scalar",
                      path);
      if (TIFFWriteScanline(file.get(), row.data(), y) < 0)
        return fail(error_code::io, "write scalar TIFF", "cannot write scanline", path);
    }
    if (!TIFFWriteDirectory(file.get()) || !TIFFFlush(file.get()))
      return fail(error_code::io, "write scalar TIFF", "cannot finalize TIFF", path);
  }
  return transaction->commit();
}

result<scalar_image> read_scalar_tiff(const std::string &path) {
  scalar_tiff file(TIFFOpen(path.c_str(), "r"), TIFFClose);
  if (!file)
    return fail(error_code::io, "read scalar TIFF", "cannot open TIFF", path);
  std::uint32_t width = 0, height = 0;
  std::uint16_t bits = 0, channels = 0, format = 0, planar = 0, photo = 0, orientation = 0;
  TIFFGetField(file.get(), TIFFTAG_IMAGEWIDTH, &width);
  TIFFGetField(file.get(), TIFFTAG_IMAGELENGTH, &height);
  TIFFGetFieldDefaulted(file.get(), TIFFTAG_BITSPERSAMPLE, &bits);
  TIFFGetFieldDefaulted(file.get(), TIFFTAG_SAMPLESPERPIXEL, &channels);
  TIFFGetFieldDefaulted(file.get(), TIFFTAG_SAMPLEFORMAT, &format);
  TIFFGetFieldDefaulted(file.get(), TIFFTAG_PLANARCONFIG, &planar);
  TIFFGetField(file.get(), TIFFTAG_PHOTOMETRIC, &photo);
  TIFFGetFieldDefaulted(file.get(), TIFFTAG_ORIENTATION, &orientation);
  std::uint32_t icc_size = 0;
  void *icc = nullptr;
  char *description = nullptr;
  if (!width || !height || width > 65536 || height > 65536 ||
      std::uint64_t(width) * height > 200000000 || bits != 32 || channels != 1 ||
      format != SAMPLEFORMAT_IEEEFP || planar != PLANARCONFIG_CONTIG ||
      photo != PHOTOMETRIC_MINISBLACK || orientation != ORIENTATION_TOPLEFT ||
      TIFFIsTiled(file.get()))
    return fail(error_code::unsupported_format, "read scalar TIFF",
                "requires stripped float32 numeric map", path);
  if (TIFFGetField(file.get(), TIFFTAG_ICCPROFILE, &icc_size, &icc) ||
      !TIFFGetField(file.get(), TIFFTAG_IMAGEDESCRIPTION, &description) || !description)
    return fail(error_code::color, "read scalar TIFF",
                "numeric map needs provenance and no ICC", path);
  const std::string note(description);
  if (!note.starts_with(scalar_prefix) ||
      !valid_scalar_meaning(note.substr(std::char_traits<char>::length(scalar_prefix))))
    return fail(error_code::color, "read scalar TIFF", "unknown scalar provenance", path);
  image<gray> pixels({width, height});
  auto view = pixels.view();
  for (std::size_t y = 0; y < height; ++y) {
    if (TIFFReadScanline(file.get(), view.row(y).data(), y) < 0)
      return fail(error_code::io, "read scalar TIFF", "cannot read scanline", path);
    for (auto p : view.row(y))
      if (!std::isfinite(p.value))
        return fail(error_code::decode, "read scalar TIFF", "nonfinite scalar", path);
  }
  return scalar_image{std::move(pixels),
                      note.substr(std::char_traits<char>::length(scalar_prefix))};
}
} // namespace photo_ops
