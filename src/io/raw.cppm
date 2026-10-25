// SPDX-License-Identifier: GPL-3.0-or-later
module;
#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <utility>
#include <vector>
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
result<image<linear_rgb>> decode_raw(const std::string &path, raw_options options = {});

/// Calibration after unpack (some Canon black levels are unavailable at header time).
struct raw_frame_metadata {
  std::string path, input_fingerprint, decoder_version, camera;
  extent raw_dimensions, active_dimensions, output_dimensions;
  std::size_t left = 0, top = 0;
  int orientation = 0;
  unsigned nominal_bits = 0;
  std::array<unsigned, 4> cfa{}; // 2x2 in native active-area coordinates: R=0,G1=1,B=2,G2=3.
  std::array<double, 4> black{}, white{}, camera_wb{};
  std::array<double, 9> camera_to_xyz{}; // D65 daylight characterization, row-major.
  double iso = 0, shutter_seconds = 0, aperture = 0;
  std::size_t file_bytes = 0, decode_reserve_bytes = 0;
};

/// Fixed exposure for focus brackets; shutter-only AEB is an explicit radiometric contract.
enum class raw_exposure_policy { fixed, shutter_bracket };

struct raw_sequence_options {
  std::size_t reference_index = static_cast<std::size_t>(-1);
  std::array<double, 4> white_balance{}; // All zero: use reference camera WB, normalized to G1.
  std::size_t memory_budget_mib = 2048;
  raw_exposure_policy exposure_policy = raw_exposure_policy::fixed;
};

/// Frozen small calibration/manifest value. Pixels are decoded sequentially elsewhere.
class raw_sequence_context {
public:
  inline std::span<const raw_frame_metadata> frames() const noexcept {
    return frames_;
  }

  inline const raw_frame_metadata &reference() const noexcept {
    return frames_[reference_];
  }

  inline std::size_t reference_index() const noexcept {
    return reference_;
  }

  inline const std::array<double, 4> &white_balance() const noexcept {
    return wb_;
  }

  inline const color_encoding &encoding() const noexcept {
    return encoding_;
  }

  inline raw_exposure_policy exposure_policy() const noexcept {
    return exposure_policy_;
  }

  inline double exposure_scale(std::size_t frame) const {
    contract_assert(frame < frames_.size());
    return exposure_policy_ == raw_exposure_policy::fixed
               ? 1.
               : reference().shutter_seconds / frames_[frame].shutter_seconds;
  }

  inline std::size_t memory_budget_mib() const noexcept {
    return memory_budget_mib_;
  }

  inline const std::string &fingerprint() const noexcept {
    return encoding_.normalization;
  }

private:
  std::vector<raw_frame_metadata> frames_;
  std::size_t reference_;
  std::array<double, 4> wb_;
  color_encoding encoding_;
  raw_exposure_policy exposure_policy_;
  std::size_t memory_budget_mib_;

  raw_sequence_context(std::vector<raw_frame_metadata> frames, std::size_t reference,
                       std::array<double, 4> wb, std::string fingerprint,
                       raw_exposure_policy policy, std::size_t budget_mib)
      : frames_(std::move(frames)), reference_(reference), wb_(wb),
        encoding_{rgb_primaries::rec2020, white_point::d65, transfer_function::linear,
                  image_origin::scene_linear_raw, std::move(fingerprint)},
        exposure_policy_(policy), memory_budget_mib_(budget_mib) {
  }

  friend result<raw_sequence_context> prepare_raw_sequence(std::span<const raw_frame_metadata>,
                                                           raw_sequence_options);
};

/// Unpack one Bayer mosaic to obtain complete calibration, then release it. No development.
/// The conservative per-frame reserve is checked before unpack; unknown sensor modes fail.
result<raw_frame_metadata> inspect_raw_frame(const std::string &path,
                                             std::size_t memory_budget_mib = 2048);
result<raw_sequence_context> prepare_raw_sequence(std::span<const raw_frame_metadata> frames,
                                                  raw_sequence_options options = {});
result<raw_sequence_context> prepare_raw_sequence(std::span<const std::string> paths,
                                                  raw_sequence_options options = {});

struct raw_frame_statistics {
  double saturated_fraction = 0, negative_fraction = 0;
  float minimum = 0, maximum = 0;
};

/// Unscaled, pre-WB (raw-black)/(white-black) extrema over the demosaic's 7x7 support.
/// A saturated contributing CFA sample invalidates the complete developed RGB measurement.
struct raw_sensor_range {
  float minimum = 0, maximum = 0;
};

static_assert(sizeof(raw_sensor_range) == 8);

struct raw_decode_options {
  bool sensor_support = false; // Optional 8-byte/pixel measurement map, budgeted before unpack.
};

struct developed_raw_frame {
  working_image image;
  raw_frame_statistics statistics;
  photo_ops::image<raw_sensor_range>
      sensor_support; // Empty unless requested; output orientation.
};

/// Sensor-native float normalization and directional Bayer demosaic, then camera→XYZ→2020.
/// Retains signed/HDR samples; validates identity and metadata against the frozen manifest.
/// No dcraw_process(), auto WB, auto brightness, integer RGB intermediate or gamut clipping.
result<developed_raw_frame> decode_raw_frame(const std::string &path,
                                             const raw_sequence_context &context,
                                             std::size_t frame_index,
                                             raw_decode_options options = {});
} // namespace photo_ops
