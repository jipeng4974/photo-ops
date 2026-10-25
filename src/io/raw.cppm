// SPDX-License-Identifier: GPL-3.0-or-later
module;
#include <string>
export module photo_ops.raw;
import photo_ops.image;
export import photo_ops.error;

export namespace photo_ops {
struct raw_options {
  bool half_size = false;
};

// Legacy single-frame path: camera WB, linear sRGB/D65, 16-bit intermediate.
// Not sequence-consistent; see docs/raw-pipeline.md for the wide-gamut/context contract.
/// Decode failures carry a stage and path; malformed input is never a contract violation.
[[nodiscard]] result<image<linear_rgb>> decode_raw(const std::string &path,
                                                   raw_options options = {});
} // namespace photo_ops
