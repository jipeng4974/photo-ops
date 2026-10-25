// SPDX-License-Identifier: GPL-3.0-or-later
module;
#include <cstddef>
#include <string>
export module photo_ops.sunlight;
import photo_ops.image;
export import photo_ops.error;

export namespace photo_ops {
/// Conservative local enhancement; strength is in [0,2], with 0 an exact identity.
struct sunlight_options {
  float strength = 1.f;
};

/// A heuristic result, not a calibrated probability or semantic sunlight classification.
struct sunlight_report {
  bool applied = false;
  std::string decision = "no_coherent_sunlight";
  std::size_t rejected_neutral_regions{};
  std::size_t candidate_regions{};
  float selected_area_fraction{};
  float confidence{};
  float luminance_contrast_ev{};
  float chroma_deficit{};
  float maximum_chroma_gain = 1.f;
  float selected_relative_chroma{};
  /// Mask-weighted measured changes, after all pixel-level guards and gamut constraints.
  double mean_relative_chroma_gain = 1;
  double mean_luminance_change_ev = 0;
  double changed_pixel_fraction = 0;
};

/// Owns the enhanced image and a full-resolution soft mask in [0,1].
struct sunlight_result {
  image<linear_rgb> pixels;
  image<gray> mask;
  sunlight_report report;
};

/// Analyze coherent illumination and enhance existing color evidence in one dominant region.
/// Inputs remain untouched. Output owns an explicit full-frame copy and full-resolution mask.
/// Negative/HDR pixels remain unchanged; no global tone mapping, dehazing or sharpening occurs.
/// Empty/non-finite data and invalid options return errors. No candidate is a successful no-op.
result<sunlight_result> enhance_sunlight(image_view<const linear_rgb> source,
                                         sunlight_options options = {});
} // namespace photo_ops
