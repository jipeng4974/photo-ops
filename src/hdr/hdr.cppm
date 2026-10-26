// SPDX-License-Identifier: GPL-3.0-or-later
module;
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>
export module photo_ops.hdr;
import photo_ops.image;
import photo_ops.detail.registration;
export import photo_ops.raw;
export import photo_ops.error;

export namespace photo_ops {
struct hdr_options {
  std::size_t memory_budget_mib = 2048;
  std::size_t tile_size = 256, proxy_long_edge = 1024;
  std::size_t reference_index =
      static_cast<std::size_t>(-1); // Local index; default median shutter.
  std::string scratch_directory, diagnostics_directory;
  bool already_aligned = false;
};

struct hdr_frame_report {
  std::size_t context_index = 0;
  double shutter_seconds = 0, exposure_scale = 1, contribution_fraction = 0;
  raw_frame_statistics raw_statistics;
  detail::bracket_alignment alignment;
};

struct hdr_profile {
  double preflight_seconds = 0, decode_seconds = 0, registration_seconds = 0;
  double analysis_seconds = 0, fusion_seconds = 0, output_seconds = 0, diagnostics_seconds = 0;
  double total_seconds = 0;
  std::size_t scratch_read_bytes = 0, scratch_write_bytes = 0, scratch_peak_file_bytes = 0;
  std::size_t tiles = 0, output_bytes = 0;
};

struct hdr_merge_report {
  extent dimensions;
  std::size_t reference_index = 0, proxy_step = 0, refinement_step = 0, tile_size = 0;
  std::size_t estimated_peak_bytes = 0, scratch_bytes = 0;
  std::optional<raw_sequence_context> raw_context;
  std::vector<hdr_frame_report> frames;
  double deghost_fraction = 0, clipped_fraction = 0, low_snr_fraction = 0;
  float minimum = 0, maximum = 0;
  hdr_profile profile;
};

/// Radiometric merge of a shutter-only Bayer RAW bracket; no CRF estimation or tone mapping.
/// Sequential decode and budgeted tile fusion preserve signed Rec.2020/D65 float measurements.
result<hdr_merge_report> merge_hdr(std::span<const std::string> inputs,
                                   const std::string &output, const hdr_options &options = {});

/// Reuse an already frozen sequence context across explicitly grouped photographic captures.
/// Indices name context members in supplied order; the context fixes the common radiance scale.
result<hdr_merge_report> merge_hdr(const raw_sequence_context &context,
                                   std::span<const std::size_t> frame_indices,
                                   const std::string &output, const hdr_options &options = {});
} // namespace photo_ops
