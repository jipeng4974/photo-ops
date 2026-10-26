// SPDX-License-Identifier: GPL-3.0-or-later
module;
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>
export module photo_ops.focus_stack;
import photo_ops.image;
import photo_ops.hdr;
import photo_ops.detail.registration;
export import photo_ops.raw;
export import photo_ops.error;

export namespace photo_ops {
/// Inspectable geometry; registration remains internal to photographic operations.
using focus_stack_alignment = detail::bracket_alignment;

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
  raw_frame_statistics
      raw_statistics; // Direct RAW only; HDR planes use hdr_planes exposure reports.
};

/// Stage wall times include their nested scratch/warp/focus/pyramid measurements.
struct focus_stack_profile {
  double preflight_seconds = 0, decode_seconds = 0, registration_seconds = 0,
         analysis_seconds = 0;
  double fusion_seconds = 0, output_seconds = 0, diagnostics_seconds = 0, total_seconds = 0;
  double scratch_read_seconds = 0, scratch_write_seconds = 0, warp_seconds = 0;
  double focus_score_seconds = 0, pyramid_seconds = 0;
  double matrix_preflight_seconds = 0, hdr_seconds = 0;
  std::size_t scratch_read_bytes = 0, scratch_write_bytes = 0, scratch_peak_file_bytes = 0,
              output_bytes = 0;
  std::size_t decoded_frames = 0, tiles = 0, warp_calls = 0, focus_evaluations = 0,
              peak_tile_cache_bytes = 0;
  std::size_t scalar_warp_calls = 0, hdr_scratch_peak_file_bytes = 0;
};

struct focus_exposure_plane {
  std::string name;
  std::vector<std::string>
      exposures; // Explicit capture grouping; never inferred from filenames.
};

struct focus_exposure_matrix {
  std::vector<focus_exposure_plane> focus_planes;
};

struct focus_hdr_plane_report {
  std::string name;
  hdr_merge_report hdr;
};

struct focus_stack_report {
  extent dimensions;
  std::size_t reference_index = 0;
  std::size_t tile_size = 0, proxy_step = 0;
  std::size_t refinement_step = 0;
  std::size_t estimated_peak_bytes = 0, scratch_bytes = 0;
  double motion_fraction = 0, low_confidence_fraction = 0, defocus_boundary_fraction = 0;
  float working_minimum = 0, working_maximum = 0;
  std::vector<focus_stack_frame_report> frames;
  std::vector<focus_hdr_plane_report> hdr_planes; // Empty for ordinary flat focus brackets.
  std::size_t matrix_intermediate_bytes = 0;
  color_encoding encoding{rgb_primaries::srgb};
  std::optional<raw_sequence_context> raw_context;
  focus_stack_profile profile;
  bool tile_sources_cached = false;
  std::size_t tile_cache_capacity_bytes = 0;
};

/// Stack same-sized rendered TIFF/JPEG or compatible RAW Bayer brackets atomically.
/// RAW uses one frozen context, signed scene-linear Rec.2020/D65 and float32 ICC TIFF.
/// Rendered RGB retains the compatibility linear-sRGB / 16-bit TIFF output path.
/// Memory is planned before unpack/decode; full-resolution frames spool sequentially.
/// The reference supplies uncovered borders and motion regions. Mixed RAW/rendered fails.
result<focus_stack_report> stack_focus(std::span<const std::string> inputs,
                                       const std::string &output,
                                       const focus_stack_options &options = {});
/// Explicit HDR-first focus/exposure capture matrix, one frozen RAW context for all originals.
result<focus_stack_report> stack_focus(const focus_exposure_matrix &matrix,
                                       const std::string &output,
                                       const focus_stack_options &options = {});
} // namespace photo_ops

// Shared by this module's implementation units, not exported as photographic stages.
namespace photo_ops::detail {
struct hdr_plane_input {
  std::string image, confidence, motion, clipping, low_snr, diagnostics_source;
};

result<focus_stack_report> stack_hdr_planes(std::span<const hdr_plane_input> planes,
                                            const std::string &output,
                                            const focus_stack_options &options,
                                            focus_stack_report seed);
} // namespace photo_ops::detail
