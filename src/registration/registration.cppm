// SPDX-License-Identifier: GPL-3.0-or-later
module;
#include <cstddef>
export module photo_ops.detail.registration;
import photo_ops.image;
export import photo_ops.error;

// Internal adapter shared by focus stacking and HDR. Not a photographic entry point.
export namespace photo_ops::detail {
/// Source-to-reference similarity in pixel-center coordinates, not normalized coordinates.
struct bracket_alignment {
  double a = 1, b = 0, tx = 0, ty = 0;
  double residual_px = 0;
  double dense_residual_px = 0, dense_correlation = 1;
  std::size_t matches = 0, inliers = 0;
  std::size_t refinement_samples = 0;
  bool refined = false;
  bool identity = true;
};

result<bracket_alignment> align_bracket(image_view<const gray> reference,
                                        image_view<const gray> source);
result<bracket_alignment> refine_bracket(image_view<const gray> reference,
                                         image_view<const gray> source,
                                         bracket_alignment initial);
} // namespace photo_ops::detail
