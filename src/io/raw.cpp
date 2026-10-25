// SPDX-License-Identifier: GPL-3.0-or-later
module;
#include <libraw/libraw.h>
#include <memory>
#include <string>
module photo_ops.raw;
import photo_ops.image;

namespace photo_ops {
namespace {
void configure_linear_decode(libraw_output_params_t &parameters, raw_options options) {
  parameters.output_bps = 16;
  parameters.output_color = 1; // sRGB primaries; override the transfer function separately.
  parameters.gamm[0] = 1.0;
  parameters.gamm[1] = 1.0;

  // Keep exposure and processing deterministic across a focus bracket.
  parameters.no_auto_bright = 1;
  parameters.adjust_maximum_thr = 0;
  parameters.use_camera_wb = 1;
  parameters.use_auto_wb = 0;
  parameters.user_qual = 3; // AHD demosaic.
  parameters.half_size = options.half_size;
  parameters.highlight = 0;
  parameters.fbdd_noiserd = 0;
  parameters.threshold = 0;
}
} // namespace

result<image<linear_rgb>> decode_raw(const std::string &path, raw_options options) {
  LibRaw decoder;
  const auto failure = [&](int status, const char *stage) {
    return fail(error_code::decode, stage, libraw_strerror(status), path);
  };

  if (const auto status = decoder.open_file(path.c_str()); status != LIBRAW_SUCCESS)
    return failure(status, "open RAW");
  if (const auto status = decoder.unpack(); status != LIBRAW_SUCCESS)
    return failure(status, "unpack RAW");

  configure_linear_decode(decoder.imgdata.params, options);
  if (const auto status = decoder.dcraw_process(); status != LIBRAW_SUCCESS)
    return failure(status, "develop RAW");

  int status = LIBRAW_SUCCESS;
  using bitmap_owner =
      std::unique_ptr<libraw_processed_image_t, decltype(&LibRaw::dcraw_clear_mem)>;
  bitmap_owner bitmap(decoder.dcraw_make_mem_image(&status), &LibRaw::dcraw_clear_mem);
  if (status != LIBRAW_SUCCESS)
    return failure(status, "create RAW bitmap");
  if (!bitmap || bitmap->type != LIBRAW_IMAGE_BITMAP || bitmap->colors != 3 ||
      bitmap->bits != 16)
    return fail(error_code::decode, "create RAW bitmap", "expected 16-bit RGB bitmap", path);

  const extent dimensions{bitmap->width, bitmap->height};
  if (!image<linear_rgb>::can_hold(dimensions) || dimensions.width == 0)
    return fail(error_code::decode, "create RAW bitmap", "invalid image dimensions", path);

  image<linear_rgb> pixels(dimensions);
  const auto *samples = reinterpret_cast<const unsigned short *>(bitmap->data);
  for (std::size_t y = 0; y < dimensions.height; ++y) {
    auto row = pixels.view().row(y);
    for (std::size_t x = 0; x < dimensions.width; ++x) {
      const auto index = 3 * (y * dimensions.width + x);
      row[x] = {samples[index] / 65535.f, samples[index + 1] / 65535.f,
                samples[index + 2] / 65535.f};
    }
  }
  return pixels;
}
} // namespace photo_ops
