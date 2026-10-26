// SPDX-License-Identifier: GPL-3.0-or-later
module;
#include <functional>
#include <span>
#include <string>
export module photo_ops.io;
import photo_ops.image;
export import photo_ops.error;

export namespace photo_ops {
/// Distinguish RAW development from linearization of an already rendered photograph.
enum class input_origin { scene_linear_raw, rendered_rgb };

/// Owns working pixels and the provenance of their transfer/color conversion.
struct decoded_image {
  image<linear_rgb> pixels;
  input_origin origin;
  std::string color_note;
};

/// Untagged RGB is explicitly assumed sRGB, reported in color_note.
/// TIFF: stripped, contiguous 8/16-bit RGB, top-left, no alpha; ICC supported.
/// JPEG: 8-bit RGB/grayscale, ICC supported; stored orientation (no EXIF rotation).
/// Return a recoverable format/decode/I/O error; resource exhaustion may still throw.
result<decoded_image> read_image(const std::string &path, bool half_size_raw = false);

/// Header-only dimensions for budget preflight; does not decode image pixels.
/// Supports the same rendered TIFF/JPEG inputs as read_image; RAW is deliberately excluded.
result<extent> rendered_image_size(const std::string &path);

/// Stream one float32 linear-sRGB/D65 row at a time into atomic, ICC-tagged 16-bit TIFF.
/// The callback fills the supplied row or returns an error; no full-frame output allocation.
using image_row_reader = std::function<result<void>(std::size_t, std::span<linear_rgb>)>;
result<void> write_tiff_rows(const std::string &path, extent dimensions,
                             const image_row_reader &read_row);

/// Output encodings are separate from the linear working representation.
enum class output_format { tiff16, exr32, jpeg };

/// TIFF is the default; jpeg_quality is checked only for JPEG output.
struct output_options {
  output_format format = output_format::tiff16;
  int jpeg_quality = 95;
};

/// TIFF/JPEG encode sRGB, embed ICC, and clip to [0,1]; no implicit tone map.
/// EXR preserves finite negative/HDR values in float32 linear sRGB/D65.
/// Atomically publishes a completed file; never overwrites an existing path.
/// Reject unsupported options, invalid output data and I/O failures through the result.
result<void> write_image(const std::string &path, image_view<const linear_rgb> pixels,
                         output_options options = {});

/// Report whether this build includes the optional OpenEXR backend.
bool exr_available() noexcept;
/// Lossless working storage: float32 linear RGB TIFF + matching ICC and provenance tag.
/// Signed/HDR values are preserved. Encoding is mandatory; no rendering or gamut clipping.
using working_row_reader = std::function<result<void>(std::size_t, std::span<rgb_f32>)>;
result<void> write_working_tiff_rows(const std::string &path, extent dimensions,
                                     const color_encoding &encoding,
                                     const working_row_reader &read_row);
/// Reads the explicit working TIFF format above; rejects unknown/contradictory ICC metadata.
result<working_image> read_working_tiff(const std::string &path);
/// Numeric scalar TIFF: float32, no transfer/ICC, explicit semantic provenance.
/// These samples are measurements/maps, not sRGB display values.
using scalar_row_reader = std::function<result<void>(std::size_t, std::span<gray>)>;

struct scalar_image {
  image<gray> pixels;
  std::string meaning;
};

result<void> write_scalar_tiff_rows(const std::string &path, extent dimensions,
                                    const std::string &meaning,
                                    const scalar_row_reader &read_row);
result<scalar_image> read_scalar_tiff(const std::string &path);
} // namespace photo_ops
