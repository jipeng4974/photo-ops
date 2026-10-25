// SPDX-License-Identifier: GPL-3.0-or-later
module;
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>
export module photo_ops.focus_stack;
import photo_ops.image;
export import photo_ops.raw;
export import photo_ops.error;

export namespace photo_ops {
/// Source-to-reference similarity in pixel-center coordinates, not normalized coordinates.
struct focus_stack_alignment {
  double a = 1, b = 0, tx = 0, ty = 0;
  double residual_px = 0;
  double dense_residual_px = 0, dense_correlation = 1;
  std::size_t matches = 0, inliers = 0;
  std::size_t refinement_samples = 0;
  bool refined = false;
  bool identity = true;
};

struct focus_stack_options {
  std::size_t memory_budget_mib = 2048;
  std::string scratch_directory; // Empty uses the system temporary directory.
  std::string diagnostics_directory;
  std::size_t tile_size = 256; // Rounded down to a multiple of 8, minimum 32.
  std::size_t proxy_long_edge = 1024;
  std::size_t reference_index = static_cast<std::size_t>(-1); // Default: middle input.
  bool already_aligned = false; // Explicit caller assertion; never an automatic fallback.
};

struct focus_stack_frame_report {
  focus_stack_alignment alignment;
  double selected_fraction = 0;
  raw_frame_statistics raw_statistics;
};

struct focus_stack_report {
  extent dimensions;
  std::size_t reference_index = 0;
  std::size_t tile_size = 0, proxy_step = 0;
  std::size_t refinement_step = 0;
  std::size_t estimated_peak_bytes = 0, scratch_bytes = 0;
  double motion_fraction = 0, low_confidence_fraction = 0;
  std::vector<focus_stack_frame_report> frames;
  color_encoding encoding{rgb_primaries::srgb};
  std::optional<raw_sequence_context> raw_context;
};

/// Stack same-sized rendered TIFF/JPEG or compatible RAW Bayer brackets atomically.
/// RAW uses one frozen context, signed scene-linear Rec.2020/D65 and float32 ICC TIFF.
/// Rendered RGB retains the compatibility linear-sRGB / 16-bit TIFF output path.
/// Memory is planned before unpack/decode; full-resolution frames spool sequentially.
/// The reference supplies uncovered borders and motion regions. Mixed RAW/rendered fails.
result<focus_stack_report> stack_focus(std::span<const std::string> inputs,
                                       const std::string &output,
                                       const focus_stack_options &options = {});
} // namespace photo_ops

// Shared by this module's implementation units; not exported to consumers.
namespace photo_ops::detail {
/// Internal registration for focus stacking; views are linear-sRGB luminance proxies.
/// Rejects insufficient evidence and excessive deformation instead of guessing identity.
result<focus_stack_alignment> align_focus_stack(image_view<const gray> reference,
                                                image_view<const gray> source);
result<focus_stack_alignment> refine_focus_stack(image_view<const gray> reference,
                                                 image_view<const gray> source,
                                                 focus_stack_alignment initial);

} // namespace photo_ops::detail
