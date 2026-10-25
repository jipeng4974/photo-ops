// SPDX-License-Identifier: GPL-3.0-or-later
module;
#include <cmath>
export module photo_ops.color;
import photo_ops.image;

export namespace photo_ops {
/// Decode the extended sRGB transfer function, retaining negative and HDR values.
inline float srgb_to_linear(float v) noexcept {
  return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

/// Encode linear RGB with the extended sRGB transfer function; no clipping occurs.
inline float linear_to_srgb(float v) noexcept {
  return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.f / 2.4f) - 0.055f;
}

/// Relative Rec.709 luminance of linear-sRGB / D65, without a tone curve.
inline float luminance(linear_rgb p) noexcept {
  return 0.2126f * p.r + 0.7152f * p.g + 0.0722f * p.b;
}

/// Perceptual OKLab coordinates, using the D65 white point.
struct oklab {
  float l{}, a{}, b{};
};

/// Convert linear-sRGB / D65 to OKLab; signed cube roots support negative channels.
inline oklab to_oklab(linear_rgb p) noexcept {
  const auto l = std::cbrt(0.4122214708f * p.r + 0.5363325363f * p.g + 0.0514459929f * p.b);
  const auto m = std::cbrt(0.2119034982f * p.r + 0.6806995451f * p.g + 0.1073969566f * p.b);
  const auto s = std::cbrt(0.0883024619f * p.r + 0.2817188376f * p.g + 0.6299787005f * p.b);
  return {0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s,
          1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s,
          0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s};
}

/// Invert OKLab into linear-sRGB / D65 without gamut clipping.
inline linear_rgb from_oklab(oklab p) noexcept {
  const auto l = p.l + 0.3963377774f * p.a + 0.2158037573f * p.b;
  const auto m = p.l - 0.1055613458f * p.a - 0.0638541728f * p.b;
  const auto s = p.l - 0.0894841775f * p.a - 1.2914855480f * p.b;
  return {4.0767416621f * l * l * l - 3.3077115913f * m * m * m + 0.2309699292f * s * s * s,
          -1.2684380046f * l * l * l + 2.6097574011f * m * m * m - 0.3413193965f * s * s * s,
          -0.0041960863f * l * l * l - 0.7034186147f * m * m * m + 1.7076147010f * s * s * s};
}

/// Tristimulus values relative to the D65 reference white; signed/HDR values are valid.
struct xyz_d65 {
  float x{}, y{}, z{};
};

inline xyz_d65 to_xyz(rgb_f32 p, const color_encoding &encoding) noexcept {
  if (encoding.primaries == rgb_primaries::rec2020)
    return {.636958048f * p.r + .144616904f * p.g + .168880975f * p.b,
            .262700212f * p.r + .677998072f * p.g + .059301716f * p.b,
            .028072693f * p.g + 1.060985058f * p.b};
  return {.412390799f * p.r + .357584339f * p.g + .180480788f * p.b,
          .212639006f * p.r + .715168679f * p.g + .072192315f * p.b,
          .019330819f * p.r + .119194780f * p.g + .950532152f * p.b};
}

inline rgb_f32 from_xyz(xyz_d65 p, const color_encoding &encoding) noexcept {
  if (encoding.primaries == rgb_primaries::rec2020)
    return {1.716651188f * p.x - .355670784f * p.y - .253366281f * p.z,
            -.666684352f * p.x + 1.616481237f * p.y + .015768546f * p.z,
            .017639857f * p.x - .042770613f * p.y + .942103121f * p.z};
  return {3.240969942f * p.x - 1.537383178f * p.y - .498610760f * p.z,
          -.969243636f * p.x + 1.875967502f * p.y + .041555057f * p.z,
          .055630080f * p.x - .203976959f * p.y + 1.056971514f * p.z};
}

inline float luminance(rgb_f32 p, const color_encoding &encoding) noexcept {
  return to_xyz(p, encoding).y;
}

/// Explicit, unclipped working-to-preview conversion. Rendering/quantization is separate.
inline linear_rgb to_linear_srgb(rgb_f32 p, const color_encoding &encoding) noexcept {
  const auto rgb = from_xyz(to_xyz(p, encoding), color_encoding{rgb_primaries::srgb});
  return {rgb.r, rgb.g, rgb.b};
}

inline oklab to_oklab(rgb_f32 p, const color_encoding &encoding) noexcept {
  return to_oklab(to_linear_srgb(p, encoding));
}

inline rgb_f32 from_oklab(oklab p, const color_encoding &encoding) noexcept {
  const auto rgb = from_oklab(p);
  return from_xyz(to_xyz(rgb_f32{rgb.r, rgb.g, rgb.b}, color_encoding{rgb_primaries::srgb}),
                  encoding);
}
} // namespace photo_ops
