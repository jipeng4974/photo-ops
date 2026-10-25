// SPDX-License-Identifier: GPL-3.0-or-later
module;
#include <cmath>
export module photo_ops.color;
import photo_ops.image;

export namespace photo_ops {
/// Decode the extended sRGB transfer function, retaining negative and HDR values.
float srgb_to_linear(float v) noexcept {
  return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

/// Encode linear RGB with the extended sRGB transfer function; no clipping occurs.
float linear_to_srgb(float v) noexcept {
  return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.f / 2.4f) - 0.055f;
}

/// Relative Rec.709 luminance of linear-sRGB / D65, without a tone curve.
float luminance(linear_rgb p) noexcept {
  return 0.2126f * p.r + 0.7152f * p.g + 0.0722f * p.b;
}

/// Perceptual OKLab coordinates, using the D65 white point.
struct oklab {
  float l{}, a{}, b{};
};

/// Convert linear-sRGB / D65 to OKLab; signed cube roots support negative channels.
oklab to_oklab(linear_rgb p) noexcept {
  const auto l = std::cbrt(0.4122214708f * p.r + 0.5363325363f * p.g + 0.0514459929f * p.b);
  const auto m = std::cbrt(0.2119034982f * p.r + 0.6806995451f * p.g + 0.1073969566f * p.b);
  const auto s = std::cbrt(0.0883024619f * p.r + 0.2817188376f * p.g + 0.6299787005f * p.b);
  return {0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s,
          1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s,
          0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s};
}

/// Invert OKLab into linear-sRGB / D65 without gamut clipping.
linear_rgb from_oklab(oklab p) noexcept {
  const auto l = p.l + 0.3963377774f * p.a + 0.2158037573f * p.b;
  const auto m = p.l - 0.1055613458f * p.a - 0.0638541728f * p.b;
  const auto s = p.l - 0.0894841775f * p.a - 1.2914855480f * p.b;
  return {4.0767416621f * l * l * l - 3.3077115913f * m * m * m + 0.2309699292f * s * s * s,
          -1.2684380046f * l * l * l + 2.6097574011f * m * m * m - 0.3413193965f * s * s * s,
          -0.0041960863f * l * l * l - 0.7034186147f * m * m * m + 1.7076147010f * s * s * s};
}
} // namespace photo_ops
