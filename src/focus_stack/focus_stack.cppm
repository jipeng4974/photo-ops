// SPDX-License-Identifier: GPL-3.0-or-later
module;
#include <cstddef>
#include <span>
#include <string>
#include <vector>
export module photo_ops.focus_stack;
import photo_ops.image;
export import photo_ops.error;

export namespace photo_ops {
/// Source-to-reference similarity in pixel-center coordinates, not normalized coordinates.
struct focus_stack_alignment {
  double a = 1, b = 0, tx = 0, ty = 0;
  double residual_px = 0;
  std::size_t matches = 0, inliers = 0;
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
};

struct focus_stack_report {
  extent dimensions;
  std::size_t reference_index = 0;
  std::size_t tile_size = 0, proxy_step = 0;
  std::size_t estimated_peak_bytes = 0, scratch_bytes = 0;
  double motion_fraction = 0, low_confidence_fraction = 0;
  std::vector<focus_stack_frame_report> frames;
};

/// Stack consistently developed, same-sized RGB TIFF/JPEG frames into atomic 16-bit TIFF.
/// Working pixels are explicitly float32 linear-sRGB/D65, rendered provenance; no RAW
/// development or implicit exposure/WB matching. CR3 is rejected until its float/context
/// pipeline exists. Memory is planned before decode; full-resolution frames spool to disk.
/// Registration, coherent selection, motion fallback and tiled multiband fusion belong to
/// this one module. The reference frame supplies uncovered borders and motion regions.
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

} // namespace photo_ops::detail
