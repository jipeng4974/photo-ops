// SPDX-License-Identifier: GPL-3.0-or-later
module;
#include <algorithm>
#include <concepts>
#include <cstddef>
#include <limits>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
export module photo_ops.image;

export namespace photo_ops {
/// Legacy interleaved float32 linear-sRGB / D65; not a stable working-color ABI.
/// Wide-gamut data requires an explicit color descriptor/API (docs/raw-pipeline.md).
/// Negative and HDR values are permitted.
struct linear_rgb {
  float r{};
  float g{};
  float b{};
};

/// Untagged storage only. RGB semantics require an accompanying color_encoding.
struct rgb_f32 {
  float r{}, g{}, b{};
};

enum class rgb_primaries { srgb, rec2020 };
enum class white_point { d65 };
enum class transfer_function { linear };
enum class image_origin { scene_linear_raw, rendered_rgb };

struct color_encoding {
  rgb_primaries primaries;
  white_point white = white_point::d65;
  transfer_function transfer = transfer_function::linear;
  image_origin origin = image_origin::rendered_rgb;
  std::string
      normalization{}; // RAW: immutable context fingerprint; rendered: conversion provenance.
};

/// A linear luminance or scalar analysis sample; no transfer function is implied.
struct gray {
  float value{};
};

/// Pixel dimensions. Empty images use 0x0; other shapes must have two positive dimensions.
struct extent {
  std::size_t width{};
  std::size_t height{};

  /// Check the canonical empty/nonempty shape without allocating storage.
  bool valid() const noexcept {
    return (width == 0) == (height == 0);
  }
};

/// A crop in source pixel coordinates, with an exclusive right and bottom edge.
struct rectangle {
  std::size_t x{};
  std::size_t y{};
  std::size_t width{};
  std::size_t height{};
};

static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);

static_assert(sizeof(linear_rgb) == 3 * sizeof(float));
static_assert(sizeof(rgb_f32) == 3 * sizeof(float));

/// Borrowed pixels. Stride is measured in pixels; the owner must outlive every view.
template <class Pixel> class image_view {
public:
  image_view() = default;

  /// Borrow storage that covers every logical pixel, including inter-row padding.
  image_view(std::span<Pixel> storage, extent size, std::size_t stride)
      : storage_(storage), size_(size), stride_(stride) {
    contract_assert(valid_layout(storage, size, stride));
  }

  /// Add constness without copying pixels or changing the layout.
  template <class Other>
    requires(std::is_const_v<Pixel> && std::same_as<Other, std::remove_const_t<Pixel>>)
  image_view(image_view<Other> other)
      : image_view(other.storage(), other.size(), other.stride()) {
  }

  /// Logical pixel dimensions, excluding padding.
  extent size() const noexcept {
    return size_;
  }

  /// Number of pixel slots between successive row starts.
  std::size_t stride() const noexcept {
    return stride_;
  }

  /// Includes padding and any accessible trailing storage; row() excludes padding.
  std::span<Pixel> storage() const noexcept {
    return storage_;
  }

  /// Borrow one logical row. The caller must supply an in-range row index.
  std::span<Pixel> row(std::size_t y) const {
    contract_assert(y < size_.height);
    return storage_.subspan(y * stride_, size_.width);
  }

  /// Access an in-range pixel. A const view object can still reference mutable pixels.
  Pixel &at(std::size_t x, std::size_t y) const {
    contract_assert(x < size_.width && y < size_.height);
    return row(y)[x];
  }

  /// Borrow an in-bounds crop, retaining stride. A zero-area crop becomes an empty view.
  image_view subview(rectangle region) const {
    contract_assert(region.x <= size_.width && region.y <= size_.height);
    contract_assert(region.width <= size_.width - region.x);
    contract_assert(region.height <= size_.height - region.y);
    if (region.width == 0 || region.height == 0)
      return {};

    const auto offset = region.y * stride_ + region.x;
    return {storage_.subspan(offset), {region.width, region.height}, stride_};
  }

private:
  static bool valid_layout(std::span<Pixel> storage, extent size, std::size_t stride) noexcept {
    if (!size.valid())
      return false;
    if (size.width == 0)
      return true;

    // Subtract before multiplying, so even hostile dimensions cannot overflow the check.
    return stride >= size.width && size.width <= storage.size() &&
           size.height - 1 <= (storage.size() - size.width) / stride;
  }

  std::span<Pixel> storage_{};
  extent size_{};
  std::size_t stride_{};
};

/// A contiguous, uniquely owned image. Full-frame copies must explicitly call clone().
template <class Pixel> class image {
public:
  image() = default;

  /// Allocate a valid, representable shape. Allocation failure may throw std::bad_alloc.
  explicit image(extent size) : size_(size), pixels_(pixel_count(size)) {
  }

  image(const image &) = delete;

  image &operator=(const image &) = delete;

  /// Transfer ownership and leave the source empty.
  image(image &&other) noexcept
      : size_(std::exchange(other.size_, {})), pixels_(std::move(other.pixels_)) {
  }

  image &operator=(image &&other) noexcept {
    if (this != &other) {
      size_ = std::exchange(other.size_, {});
      pixels_ = std::move(other.pixels_);
    }
    return *this;
  }

  /// Check untrusted dimensions before calling the constructor at a file/user boundary.
  static bool can_hold(extent size) noexcept {
    return size.valid() &&
           (size.height == 0 || size.width <= std::vector<Pixel>{}.max_size() / size.height);
  }

  /// Logical pixel dimensions, excluding padding.
  extent size() const noexcept {
    return size_;
  }

  /// Borrow mutable pixels. Reacquire views after moving or replacing the owner.
  image_view<Pixel> view() & {
    return {pixels_, size_, size_.width};
  }

  /// Borrow read-only pixels; the owner must remain alive.
  image_view<const Pixel> view() const & {
    return {pixels_, size_, size_.width};
  }

  image_view<Pixel> view() && = delete;

  image_view<const Pixel> view() const && = delete;

  /// Explicitly allocate and copy all pixels; layout remains contiguous.
  image clone() const {
    image copy(size_);
    std::ranges::copy(pixels_, copy.pixels_.begin());
    return copy;
  }

private:
  static std::size_t pixel_count(extent size) {
    contract_assert(can_hold(size));
    return size.width * size.height;
  }

  extent size_{};
  std::vector<Pixel> pixels_;
};

/// Owning working RGB with mandatory, explicit color/provenance metadata.
struct working_image {
  image<rgb_f32> pixels;
  color_encoding encoding;

  working_image(image<rgb_f32> data, color_encoding color)
      : pixels(std::move(data)), encoding(std::move(color)) {
  }
};
} // namespace photo_ops
