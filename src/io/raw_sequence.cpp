// SPDX-License-Identifier: GPL-3.0-or-later
module;
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <libraw/libraw.h>
#include <limits>
#include <locale>
#include <sstream>
#include <string>
#include <vector>
module photo_ops.raw;
import photo_ops.image;
import photo_ops.color;

namespace photo_ops {
namespace {
constexpr std::size_t mib = 1024 * 1024;

// Versioned, reproducible identity for local scratch provenance, not a cryptographic digest.
struct fingerprint {
  std::uint64_t value = 14695981039346656037ULL;

  void add(std::string_view bytes) {
    for (unsigned char b : bytes) {
      value ^= b;
      value *= 1099511628211ULL;
    }
  }

  std::string hex() const {
    std::ostringstream out;
    out << "fnv1a64:" << std::hex << std::setfill('0') << std::setw(16) << value;
    return out.str();
  }
};

result<std::string> file_fingerprint(const std::string &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file)
    return fail(error_code::io, "RAW identity", "cannot open input", path);
  fingerprint hash;
  std::array<char, 65536> bytes{};
  while (file.read(bytes.data(), bytes.size()) || file.gcount())
    hash.add(std::string_view(bytes.data(), file.gcount()));
  if (!file.eof())
    return fail(error_code::io, "RAW identity", "cannot read complete input", path);
  return hash.hex();
}

bool same_size(extent a, extent b) {
  return a.width == b.width && a.height == b.height;
}

bool compatible(const raw_frame_metadata &a, const raw_frame_metadata &b,
                bool shutter_bracket = false) {
  const auto near = [](double x, double y) {
    return std::abs(x - y) <= 1e-6 * std::max(std::abs(x), std::abs(y));
  };
  return a.camera == b.camera && a.decoder_version == b.decoder_version &&
         same_size(a.raw_dimensions, b.raw_dimensions) &&
         same_size(a.active_dimensions, b.active_dimensions) &&
         same_size(a.output_dimensions, b.output_dimensions) && a.left == b.left &&
         a.top == b.top && a.orientation == b.orientation && a.nominal_bits == b.nominal_bits &&
         a.cfa == b.cfa && a.black == b.black && a.white == b.white &&
         a.camera_to_xyz == b.camera_to_xyz && near(a.iso, b.iso) &&
         (shutter_bracket || near(a.shutter_seconds, b.shutter_seconds)) &&
         near(a.aperture, b.aperture);
}

result<void> validate(const raw_frame_metadata &m) {
  const auto dimensions = m.raw_dimensions;
  if (!dimensions.width || !dimensions.height || dimensions.width > 65536 ||
      dimensions.height > 65536 || dimensions.width * dimensions.height > 200000000 ||
      m.active_dimensions.width < 8 || m.active_dimensions.height < 8 ||
      m.left > dimensions.width || m.top > dimensions.height ||
      m.active_dimensions.width > dimensions.width - m.left ||
      m.active_dimensions.height > dimensions.height - m.top || m.orientation < 0 ||
      m.orientation > 7 || m.nominal_bits < 8 || m.nominal_bits > 16 || m.camera.empty() ||
      m.input_fingerprint.empty() || m.decoder_version != LibRaw::version() ||
      !std::isfinite(m.iso) || !std::isfinite(m.aperture) ||
      !std::isfinite(m.shutter_seconds) || m.iso <= 0 || m.aperture <= 0 ||
      m.shutter_seconds <= 0)
    return fail(error_code::invalid_argument, "RAW metadata",
                "unsupported geometry, exposure or decoder identity", m.path);
  const auto reserve = 40 * dimensions.width * dimensions.height + m.file_bytes + 256 * mib;
  if (!m.file_bytes || m.file_bytes > 1024 * mib || m.decode_reserve_bytes != reserve)
    return fail(error_code::invalid_argument, "RAW metadata",
                "inconsistent decoder reserve/file size", m.path);
  const extent output = m.orientation & 4
                            ? extent{m.active_dimensions.height, m.active_dimensions.width}
                            : m.active_dimensions;
  if (!same_size(m.output_dimensions, output))
    return fail(error_code::invalid_argument, "RAW metadata",
                "inconsistent orientation dimensions", m.path);
  std::array<int, 3> counts{};
  for (auto c : m.cfa) {
    if (c > 3)
      return fail(error_code::unsupported_format, "RAW metadata", "unsupported CFA", m.path);
    ++counts[c == 3 ? 1 : c];
  }
  if (counts != std::array{1, 2, 1} || (m.cfa[0] % 2) != (m.cfa[3] % 2) ||
      (m.cfa[1] % 2) != (m.cfa[2] % 2))
    return fail(error_code::unsupported_format, "RAW metadata",
                "only regular 2x2 RGB Bayer is supported", m.path);
  for (std::size_t c = 0; c < 4; ++c)
    if (!std::isfinite(m.black[c]) || !std::isfinite(m.white[c]) || m.black[c] < 0 ||
        m.white[c] <= m.black[c] || m.white[c] > 65535)
      return fail(error_code::invalid_argument, "RAW metadata",
                  "invalid black/white calibration", m.path);
  double matrix_energy = 0;
  for (auto v : m.camera_to_xyz) {
    if (!std::isfinite(v))
      return fail(error_code::color, "RAW metadata", "non-finite camera characterization",
                  m.path);
    matrix_energy += v * v;
  }
  const auto &t = m.camera_to_xyz;
  const double determinant = t[0] * (t[4] * t[8] - t[5] * t[7]) -
                             t[1] * (t[3] * t[8] - t[5] * t[6]) +
                             t[2] * (t[3] * t[7] - t[4] * t[6]);
  if (matrix_energy < .01 || matrix_energy > 100 || std::abs(determinant) < 1e-6)
    return fail(error_code::color, "RAW metadata",
                "missing or singular camera characterization", m.path);
  return {};
}

result<raw_frame_metadata> metadata(LibRaw &decoder, const std::string &path) {
  const auto &d = decoder.imgdata;
  const auto &s = d.sizes;
  const auto &c = d.color;
  if (!d.rawdata.raw_image || d.idata.colors != 3 || d.idata.filters < 1000 || c.cblack[4] ||
      c.cblack[5] || c.as_shot_wb_applied || s.pixel_aspect != 1 ||
      s.raw_pitch < 2U * s.raw_width || decoder.error_count())
    return fail(error_code::unsupported_format, "RAW metadata",
                "requires intact integer Bayer mosaic, simple channel black, unapplied WB and "
                "square pixels",
                path);
  raw_frame_metadata m;
  m.path = path;
  m.decoder_version = LibRaw::version();
  m.camera = std::string(d.idata.make) + " " + d.idata.model;
  m.raw_dimensions = {s.raw_width, s.raw_height};
  m.active_dimensions = {s.width, s.height};
  m.output_dimensions = s.flip & 4 ? extent{s.height, s.width} : extent{s.width, s.height};
  m.left = s.left_margin;
  m.top = s.top_margin;
  m.orientation = s.flip;
  m.nominal_bits = c.raw_bps
                       ? c.raw_bps
                       : static_cast<unsigned>(std::ceil(std::log2(double(c.maximum) + 1)));
  for (unsigned y = 0; y < 2; ++y)
    for (unsigned x = 0; x < 2; ++x)
      m.cfa[2 * y + x] = decoder.COLOR(y + s.top_margin, x + s.left_margin);
  // Validate repeated Bayer phase before interpreting any raw samples.
  for (unsigned y = 0; y < 8; ++y)
    for (unsigned x = 0; x < 2; ++x)
      if (m.cfa[2 * (y % 2) + x] !=
          static_cast<unsigned>(decoder.COLOR(y + s.top_margin, x + s.left_margin)))
        return fail(error_code::unsupported_format, "RAW metadata",
                    "non-Bayer repeating filter layout", path);
  for (unsigned channel = 0; channel < 4; ++channel) {
    m.black[channel] = double(c.black) + c.cblack[channel];
    m.white[channel] = c.maximum;
    m.camera_wb[channel] = c.cam_mul[channel];
  }
  if (m.camera_wb[3] == 0)
    m.camera_wb[3] = m.camera_wb[1];
  // LibRaw rgb_cam maps WB-balanced camera RGB to linear sRGB. Compose with sRGB→XYZ.
  // cam_xyz is the opposite-direction DNG-style matrix despite its historical field name.
  constexpr double xyz[3][3] = {{.412390799, .357584339, .180480788},
                                {.212639006, .715168679, .072192315},
                                {.019330819, .119194780, .950532152}};
  for (unsigned row = 0; row < 3; ++row)
    for (unsigned column = 0; column < 3; ++column)
      for (unsigned k = 0; k < 3; ++k)
        m.camera_to_xyz[3 * row + column] += xyz[row][k] * c.rgb_cam[k][column];
  for (unsigned k = 0; k < 3; ++k)
    if (std::abs(c.rgb_cam[k][3]) > 1e-8)
      return fail(error_code::unsupported_format, "RAW metadata",
                  "four-channel characterization unsupported", path);
  m.iso = d.other.iso_speed;
  m.shutter_seconds = d.other.shutter;
  m.aperture = d.other.aperture;
  return m;
}

result<void> open_budgeted(LibRaw &decoder, const std::string &path, std::size_t budget_mib) {
  if (budget_mib < 512 || budget_mib > 1024 * 1024)
    return fail(error_code::invalid_argument, "RAW memory budget",
                "budget must be 512..1048576 MiB", path);
  const int status = decoder.open_file(path.c_str());
  if (status != LIBRAW_SUCCESS)
    return fail(error_code::decode, "open RAW sequence", libraw_strerror(status), path);
  const auto &s = decoder.imgdata.sizes;
  std::error_code ec;
  const auto bytes = std::filesystem::file_size(path, ec);
  const std::size_t pixels = std::size_t(s.raw_width) * s.raw_height;
  if (ec || !pixels || pixels > 200000000 || bytes > 1024ULL * mib)
    return fail(error_code::invalid_argument, "RAW memory budget",
                "invalid or oversized input geometry/file", path);
  if (40 * pixels + bytes + 256 * mib > budget_mib * mib * 4 / 5)
    return fail(error_code::invalid_argument, "focus memory budget",
                "RAW single-frame decode reserve exceeds budget", path);
  decoder.imgdata.rawparams.max_raw_memory_mb = budget_mib * 4 / 5;
  return {};
}
} // namespace

result<raw_frame_metadata> inspect_raw_frame(const std::string &path, std::size_t budget_mib) {
  LibRaw decoder;
  if (auto opened = open_budgeted(decoder, path, budget_mib); !opened)
    return std::unexpected(opened.error());
  auto identity = file_fingerprint(path);
  if (!identity)
    return std::unexpected(identity.error());
  if (const auto status = decoder.unpack(); status != LIBRAW_SUCCESS)
    return fail(error_code::decode, "unpack RAW sequence", libraw_strerror(status), path);
  auto after = file_fingerprint(path);
  if (!after)
    return std::unexpected(after.error());
  if (*after != *identity)
    return fail(error_code::invalid_argument, "RAW identity", "input changed during unpack",
                path);
  auto m = metadata(decoder, path);
  if (!m)
    return std::unexpected(m.error());
  m->input_fingerprint = *identity;
  std::error_code ec;
  m->file_bytes = std::filesystem::file_size(path, ec);
  if (ec)
    return fail(error_code::io, "RAW identity", ec.message(), path);
  m->decode_reserve_bytes =
      40 * m->raw_dimensions.width * m->raw_dimensions.height + m->file_bytes + 256 * mib;
  if (auto valid = validate(*m); !valid)
    return std::unexpected(valid.error());
  return m;
}

result<raw_sequence_context> prepare_raw_sequence(std::span<const raw_frame_metadata> frames,
                                                  raw_sequence_options options) {
  if (frames.empty() || frames.size() > 256 ||
      (options.reference_index != static_cast<std::size_t>(-1) &&
       options.reference_index >= frames.size()))
    return fail(error_code::invalid_argument, "RAW context",
                "need 1..256 inputs and valid reference");
  const auto reference = options.reference_index == static_cast<std::size_t>(-1)
                             ? frames.size() / 2
                             : options.reference_index;
  const auto &ref = frames[reference];
  if (options.memory_budget_mib < 512 || options.memory_budget_mib > 1024 * 1024)
    return fail(error_code::invalid_argument, "RAW memory budget",
                "budget must be 512..1048576 MiB");
  if (options.exposure_policy != raw_exposure_policy::fixed &&
      options.exposure_policy != raw_exposure_policy::shutter_bracket)
    return fail(error_code::invalid_argument, "RAW context", "unknown exposure policy");
  for (const auto &m : frames) {
    if (m.decode_reserve_bytes > options.memory_budget_mib * mib * 4 / 5)
      return fail(error_code::invalid_argument, "focus memory budget",
                  "RAW decode reserve exceeds budget", m.path);
    if (auto valid = validate(m); !valid)
      return std::unexpected(valid.error());
    if (!compatible(ref, m, options.exposure_policy == raw_exposure_policy::shutter_bracket))
      return fail(error_code::invalid_argument, "RAW context",
                  "camera/mode/geometry/black/white/color/exposure mismatch", m.path);
  }
  for (const auto &m : frames) {
    const double gain = ref.shutter_seconds / m.shutter_seconds;
    if (!std::isfinite(gain) || gain < 1. / 65536 || gain > 65536)
      return fail(error_code::invalid_argument, "RAW context",
                  "shutter normalization exceeds 16 EV from reference", m.path);
  }
  auto wb = options.white_balance;
  if (std::ranges::all_of(wb, [](double v) { return v == 0; }))
    wb = ref.camera_wb;
  for (auto v : wb)
    if (!std::isfinite(v) || v <= 0)
      return fail(error_code::color, "RAW context",
                  "reference WB unavailable; explicit positive multipliers required", ref.path);
  const auto green = wb[1];
  for (auto &v : wb) {
    v /= green;
    if (v < .01 || v > 100)
      return fail(error_code::color, "RAW context", "unsupported WB gain range", ref.path);
  }
  fingerprint hash;
  std::ostringstream canonical;
  canonical.imbue(std::locale::classic());
  canonical << "photo-ops-raw-float-ha-v2;Rec2020-D65;"
            << static_cast<int>(options.exposure_policy) << ';' << reference << ';'
            << std::hexfloat;
  for (auto v : wb)
    canonical << v << ';';
  for (const auto &m : frames) {
    canonical << m.path << ';' << m.input_fingerprint << ';' << m.decoder_version << ';'
              << m.camera << ';' << m.raw_dimensions.width << ',' << m.raw_dimensions.height
              << ',' << m.left << ',' << m.top << ',' << m.active_dimensions.width << ','
              << m.active_dimensions.height << ',' << m.orientation << ',' << m.nominal_bits
              << ';' << m.iso << ',' << m.shutter_seconds << ',' << m.aperture << ';';
    for (auto c : m.cfa)
      canonical << c << ',';
    for (auto v : m.black)
      canonical << v << ',';
    for (auto v : m.white)
      canonical << v << ',';
    for (auto v : m.camera_to_xyz)
      canonical << v << ',';
  }
  hash.add(canonical.str());
  return raw_sequence_context(std::vector<raw_frame_metadata>(frames.begin(), frames.end()),
                              reference, wb, hash.hex(), options.exposure_policy,
                              options.memory_budget_mib);
}

result<raw_sequence_context> prepare_raw_sequence(std::span<const std::string> paths,
                                                  raw_sequence_options options) {
  if (paths.empty() || paths.size() > 256)
    return fail(error_code::invalid_argument, "RAW context", "need 1..256 inputs");
  std::vector<raw_frame_metadata> frames;
  for (const auto &path : paths) {
    auto m = inspect_raw_frame(path, options.memory_budget_mib);
    if (!m)
      return std::unexpected(m.error());
    frames.push_back(std::move(*m));
  }
  return prepare_raw_sequence(frames, options);
}

result<developed_raw_frame> decode_raw_frame(const std::string &path,
                                             const raw_sequence_context &context,
                                             std::size_t frame_index,
                                             raw_decode_options options) {
  if (frame_index >= context.frames().size() || path != context.frames()[frame_index].path)
    return fail(error_code::invalid_argument, "RAW context",
                "input is not the indexed context member", path);
  const auto &expected = context.frames()[frame_index];
  const auto support_bytes = options.sensor_support ? 8 * expected.output_dimensions.width *
                                                          expected.output_dimensions.height
                                                    : 0;
  if (expected.decode_reserve_bytes + support_bytes > context.memory_budget_mib() * mib * 4 / 5)
    return fail(error_code::invalid_argument, "RAW memory budget",
                "sensor-support decode reserve exceeds context budget", path);
  auto identity = file_fingerprint(path);
  if (!identity)
    return std::unexpected(identity.error());
  if (*identity != expected.input_fingerprint)
    return fail(error_code::invalid_argument, "RAW context",
                "input contents changed after preflight", path);
  LibRaw decoder;
  // The context was already budgeted. Repeat the geometry/file reserve check before unpack.
  const auto budget_mib = (expected.decode_reserve_bytes * 5 / 4 + mib - 1) / mib;
  if (auto opened = open_budgeted(decoder, path, std::max<std::size_t>(512, budget_mib));
      !opened)
    return std::unexpected(opened.error());
  if (const auto status = decoder.unpack(); status != LIBRAW_SUCCESS)
    return fail(error_code::decode, "unpack RAW frame", libraw_strerror(status), path);
  auto after = file_fingerprint(path);
  if (!after)
    return std::unexpected(after.error());
  if (*after != expected.input_fingerprint)
    return fail(error_code::invalid_argument, "RAW identity", "input changed during unpack",
                path);
  auto actual = metadata(decoder, path);
  if (!actual)
    return std::unexpected(actual.error());
  if (!compatible(expected, *actual))
    return fail(error_code::invalid_argument, "RAW context",
                "calibration changed after preflight", path);
  const auto &ref = context.reference();
  const auto size = ref.active_dimensions;
  const auto cfa = [&](long x, long y) { return ref.cfa[2 * (y & 1) + (x & 1)]; };
  image<gray> mosaic(size), green(size);
  image<raw_sensor_range> sensor_native(options.sensor_support ? size : extent{});
  auto mosaic_view = mosaic.view(), green_view = green.view();
  const auto mosaic_pixels = mosaic_view.storage(), green_pixels = green_view.storage();
  const auto mosaic_stride = mosaic_view.stride(), green_stride = green_view.stride();
  const auto &white_balance = context.white_balance();
  const auto &encoding = context.encoding();
  raw_frame_statistics statistics;
  std::size_t saturated = 0;
  const auto *samples = decoder.imgdata.rawdata.raw_image;
  const auto pitch = decoder.imgdata.sizes.raw_pitch / 2;
  for (std::size_t y = 0; y < size.height; ++y)
    for (std::size_t x = 0; x < size.width; ++x) {
      const auto c = cfa(x, y);
      const double raw = samples[(y + ref.top) * pitch + x + ref.left];
      saturated += raw >= ref.white[c];
      if (options.sensor_support) {
        const float signal = (raw - ref.black[c]) / (ref.white[c] - ref.black[c]);
        sensor_native.view().at(x, y) = {signal, signal};
      }
      mosaic_pixels[y * mosaic_stride + x].value =
          (raw - ref.black[c]) / (ref.white[c] - ref.black[c]) * white_balance[c];
    }
  const auto reflect = [](long v, long n) { return v < 0 ? -v : v >= n ? 2 * n - 2 - v : v; };
  const auto at = [&](long x, long y) {
    return mosaic_pixels[reflect(y, size.height) * mosaic_stride + reflect(x, size.width)]
        .value;
  };
  // Directional green interpolation in float; second differences are measured on the same CFA
  // channel.
  for (long y = 0; y < static_cast<long>(size.height); ++y)
    for (long x = 0; x < static_cast<long>(size.width); ++x) {
      const float center = at(x, y);
      if (cfa(x, y) % 2) {
        green_pixels[y * green_stride + x].value = center;
        continue;
      }
      const float horizontal = (at(x - 1, y) + at(x + 1, y)) * .5f +
                               (2 * center - at(x - 2, y) - at(x + 2, y)) * .25f;
      const float vertical = (at(x, y - 1) + at(x, y + 1)) * .5f +
                             (2 * center - at(x, y - 2) - at(x, y + 2)) * .25f;
      const float dh = std::abs(at(x - 1, y) - at(x + 1, y)) +
                       std::abs(2 * center - at(x - 2, y) - at(x + 2, y));
      const float dv = std::abs(at(x, y - 1) - at(x, y + 1)) +
                       std::abs(2 * center - at(x, y - 2) - at(x, y + 2));
      green_pixels[y * green_stride + x].value = dh < dv   ? horizontal
                                                 : dv < dh ? vertical
                                                           : (horizontal + vertical) * .5f;
    }
  const auto difference = [&](long x, long y) {
    x = reflect(x, size.width);
    y = reflect(y, size.height);
    return mosaic_pixels[y * mosaic_stride + x].value -
           green_pixels[y * green_stride + x].value;
  };
  image<raw_sensor_range> sensor_support;
  if (options.sensor_support) {
    // Separable support extrema. Release each temporary before the RGB output allocation:
    // relative to ordinary development the peak grows by at most 8*output_pixels bytes.
    image<raw_sensor_range> horizontal(size);
    auto source = sensor_native.view(), middle = horizontal.view();
    for (long y = 0; y < static_cast<long>(size.height); ++y) {
      const auto input = source.row(y);
      auto row = middle.row(y);
      for (long x = 0; x < static_cast<long>(size.width); ++x) {
        raw_sensor_range value{std::numeric_limits<float>::infinity(),
                               -std::numeric_limits<float>::infinity()};
        for (int k = -3; k <= 3; ++k) {
          const auto p = input[reflect(x + k, size.width)];
          value.minimum = std::min(value.minimum, p.minimum);
          value.maximum = std::max(value.maximum, p.maximum);
        }
        row[x] = value;
      }
    }
    sensor_native = {};
    sensor_support = image<raw_sensor_range>(ref.output_dimensions);
    auto result = sensor_support.view();
    for (long y = 0; y < static_cast<long>(size.height); ++y)
      for (long x = 0; x < static_cast<long>(size.width); ++x) {
        raw_sensor_range value{std::numeric_limits<float>::infinity(),
                               -std::numeric_limits<float>::infinity()};
        for (int k = -3; k <= 3; ++k) {
          const auto p = middle.at(x, reflect(y + k, size.height));
          value.minimum = std::min(value.minimum, p.minimum);
          value.maximum = std::max(value.maximum, p.maximum);
        }
        std::size_t ox = ref.orientation & 1 ? size.width - 1 - x : x;
        std::size_t oy = ref.orientation & 2 ? size.height - 1 - y : y;
        if (ref.orientation & 4)
          std::swap(ox, oy);
        result.at(ox, oy) = value;
      }
  }
  const double exposure_scale = context.exposure_scale(frame_index);
  image<rgb_f32> output(ref.output_dimensions);
  auto output_view = output.view();
  const auto output_pixels = output_view.storage();
  const auto output_stride = output_view.stride();
  statistics.minimum = std::numeric_limits<float>::infinity();
  statistics.maximum = -statistics.minimum;
  std::size_t negatives = 0;
  for (long y = 0; y < static_cast<long>(size.height); ++y)
    for (long x = 0; x < static_cast<long>(size.width); ++x) {
      const float g = green_pixels[y * green_stride + x].value;
      std::array<double, 3> camera{0, g, 0};
      const auto c = cfa(x, y);
      if (c % 2 == 0) {
        camera[c] = at(x, y);
        camera[2 - c] = g + .25f * (difference(x - 1, y - 1) + difference(x + 1, y - 1) +
                                    difference(x - 1, y + 1) + difference(x + 1, y + 1));
      } else {
        const auto horizontal = cfa(x - 1, y);
        camera[horizontal] = g + .5f * (difference(x - 1, y) + difference(x + 1, y));
        camera[2 - horizontal] = g + .5f * (difference(x, y - 1) + difference(x, y + 1));
      }
      const auto &m = ref.camera_to_xyz;
      const xyz_d65 xyz{
          static_cast<float>(m[0] * camera[0] + m[1] * camera[1] + m[2] * camera[2]),
          static_cast<float>(m[3] * camera[0] + m[4] * camera[1] + m[5] * camera[2]),
          static_cast<float>(m[6] * camera[0] + m[7] * camera[1] + m[8] * camera[2])};
      auto pixel = from_xyz(xyz, encoding);
      if (exposure_scale != 1) {
        pixel.r *= exposure_scale;
        pixel.g *= exposure_scale;
        pixel.b *= exposure_scale;
      }
      if (!std::isfinite(pixel.r) || !std::isfinite(pixel.g) || !std::isfinite(pixel.b))
        return fail(error_code::color, "RAW float development", "non-finite camera conversion",
                    path);
      statistics.minimum = std::min({statistics.minimum, pixel.r, pixel.g, pixel.b});
      statistics.maximum = std::max({statistics.maximum, pixel.r, pixel.g, pixel.b});
      negatives += pixel.r < 0 || pixel.g < 0 || pixel.b < 0;
      std::size_t ox = ref.orientation & 1 ? size.width - 1 - x : x;
      std::size_t oy = ref.orientation & 2 ? size.height - 1 - y : y;
      if (ref.orientation & 4)
        std::swap(ox, oy);
      output_pixels[oy * output_stride + ox] = pixel;
    }
  const double count = size.width * size.height;
  statistics.saturated_fraction = saturated / count;
  statistics.negative_fraction = negatives / count;
  return developed_raw_frame{working_image(std::move(output), context.encoding()), statistics,
                             std::move(sensor_support)};
}
} // namespace photo_ops
