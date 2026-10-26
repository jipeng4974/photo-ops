// SPDX-License-Identifier: GPL-3.0-or-later
module;
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <numeric>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
module photo_ops.hdr;
import photo_ops.image;
import photo_ops.color;
import photo_ops.io;
import photo_ops.output;

namespace photo_ops {
namespace {
namespace fs = std::filesystem;
using clock_type = std::chrono::steady_clock;
constexpr std::size_t mib = 1024 * 1024;

double seconds(clock_type::time_point start) {
  return std::chrono::duration<double>(clock_type::now() - start).count();
}

struct scratch_run {
  fs::path path;

  ~scratch_run() {
    std::error_code ec;
    if (!path.empty())
      fs::remove_all(path, ec);
  }
};

struct measurement {
  rgb_f32 rgb;
  raw_sensor_range sensor;
};

struct fused_measurement {
  rgb_f32 rgb;
  gray confidence, deghost, clipping, low_snr;
};

static_assert(sizeof(measurement) == 20 && sizeof(fused_measurement) == 28);

struct frame {
  fs::path pixels, proxy, fine, aligned;
  double gain = 1, levels = 1, shutter = 1;
  detail::bracket_alignment transform;
};

struct motion_maps {
  extent size;
  image<gray> motion;
  std::vector<std::uint16_t> owner;
};

template <class Pixel>
result<void> save(const fs::path &path, image_view<const Pixel> data, hdr_profile &profile) {
  std::ofstream file(path, std::ios::binary);
  if (!file)
    return fail(error_code::io, "HDR scratch", "cannot create spool", path.string());
  for (std::size_t y = 0; y < data.size().height; ++y) {
    const auto row = data.row(y);
    file.write(reinterpret_cast<const char *>(row.data()), row.size_bytes());
    profile.scratch_write_bytes += row.size_bytes();
  }
  file.close();
  if (!file)
    return fail(error_code::io, "HDR scratch", "cannot finish spool", path.string());
  return {};
}

template <class Pixel>
result<image<Pixel>> patch(const fs::path &path, extent full, rectangle area,
                           hdr_profile &profile) {
  image<Pixel> result({area.width, area.height});
  std::ifstream file(path, std::ios::binary);
  if (!file)
    return fail(error_code::io, "HDR scratch", "cannot open spool", path.string());
  auto view = result.view();
  for (std::size_t y = 0; y < area.height; ++y) {
    auto row = view.row(y);
    file.seekg(((area.y + y) * full.width + area.x) * sizeof(Pixel));
    file.read(reinterpret_cast<char *>(row.data()), row.size_bytes());
    profile.scratch_read_bytes += row.size_bytes();
    if (!file)
      return fail(error_code::io, "HDR scratch", "short spool read", path.string());
  }
  return result;
}

std::array<double, 2> inverse(const detail::bracket_alignment &t, double x, double y) {
  const auto d = t.a * t.a + t.b * t.b;
  return {(t.a * (x - t.tx) + t.b * (y - t.ty)) / d,
          (-t.b * (x - t.tx) + t.a * (y - t.ty)) / d};
}

bool inside(extent size, double x, double y) {
  return x >= 0 && y >= 0 && x <= size.width - 1 && y <= size.height - 1;
}

measurement sample(image_view<const measurement> src, double x, double y) {
  const auto size = src.size();
  const long ix = std::floor(x), iy = std::floor(y);
  const double fx = x - ix, fy = y - iy;
  std::array<double, 3> rgb{};
  raw_sensor_range range{std::numeric_limits<float>::infinity(),
                         -std::numeric_limits<float>::infinity()};
  for (long dy = 0; dy < 2; ++dy) {
    const auto row = src.row(std::clamp<long>(iy + dy, 0, size.height - 1));
    for (long dx = 0; dx < 2; ++dx) {
      const double w = (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy);
      if (w == 0)
        continue;
      const auto p = row[std::clamp<long>(ix + dx, 0, size.width - 1)];
      rgb[0] += w * p.rgb.r;
      rgb[1] += w * p.rgb.g;
      rgb[2] += w * p.rgb.b;
      range.minimum = std::min(range.minimum, p.sensor.minimum);
      range.maximum = std::max(range.maximum, p.sensor.maximum);
    }
  }
  return {{float(rgb[0]), float(rgb[1]), float(rgb[2])}, range};
}

result<image<measurement>> warp(const frame &source, extent full, rectangle area,
                                hdr_profile &profile) {
  double left = full.width - 1, right = 0, top = full.height - 1, bottom = 0;
  for (auto y : {area.y, area.y + area.height - 1})
    for (auto x : {area.x, area.x + area.width - 1}) {
      const auto p = inverse(source.transform, x, y);
      left = std::min(left, p[0]);
      right = std::max(right, p[0]);
      top = std::min(top, p[1]);
      bottom = std::max(bottom, p[1]);
    }
  const auto x0 = std::clamp<long>(std::floor(left), 0, full.width - 1);
  const auto y0 = std::clamp<long>(std::floor(top), 0, full.height - 1);
  const auto x1 = std::clamp<long>(std::ceil(right) + 1, x0 + 1, full.width);
  const auto y1 = std::clamp<long>(std::ceil(bottom) + 1, y0 + 1, full.height);
  auto input = patch<measurement>(
      source.pixels, full,
      {std::size_t(x0), std::size_t(y0), std::size_t(x1 - x0), std::size_t(y1 - y0)}, profile);
  if (!input)
    return std::unexpected(input.error());
  image<measurement> result({area.width, area.height});
  const auto src = input->view();
  auto dst = result.view();
  for (std::size_t y = 0; y < area.height; ++y) {
    auto row = dst.row(y);
    for (std::size_t x = 0; x < area.width; ++x) {
      const auto p = inverse(source.transform, area.x + x, area.y + y);
      row[x] = sample(src, p[0] - x0, p[1] - y0);
      if (!inside(full, p[0], p[1]))
        row[x].sensor = {-1, 2};
    }
  }
  return result;
}

double quality(raw_sensor_range p, double levels) {
  const double shadow = std::clamp((p.minimum * levels - 2) / 14., 0., 1.);
  const double highlight = std::clamp((.995 - p.maximum) / .095, 0., 1.);
  return std::min(shadow, highlight);
}

double variance(raw_sensor_range p, const frame &f) {
  // Conservative signal-dependent engineering model, not a calibrated camera noise profile.
  return f.gain * f.gain * (16. / (f.levels * f.levels) + std::max(0.f, p.maximum) / f.levels);
}

double proxy_luminance(rgb_f32 p, const color_encoding &encoding) {
  return std::log1p(16. * std::max(0.f, luminance(p, encoding)));
}

image<gray> luminance_proxy(image_view<const measurement> input,
                            const color_encoding &encoding) {
  image<gray> output(input.size());
  auto result = output.view();
  for (std::size_t y = 0; y < input.size().height; ++y) {
    const auto row = input.row(y);
    auto dst = result.row(y);
    for (std::size_t x = 0; x < row.size(); ++x)
      dst[x].value = proxy_luminance(row[x].rgb, encoding);
  }
  return output;
}

image<measurement> make_proxy(const developed_raw_frame &input, std::size_t step) {
  const auto rgb = input.image.pixels.view();
  const auto support = input.sensor_support.view();
  const auto size = rgb.size();
  image<measurement> result({(size.width + step - 1) / step, (size.height + step - 1) / step});
  auto out = result.view();
  for (std::size_t y = 0; y < out.size().height; ++y) {
    auto row = out.row(y);
    for (std::size_t x = 0; x < row.size(); ++x) {
      std::array<double, 3> sum{};
      std::size_t count = 0;
      raw_sensor_range range{std::numeric_limits<float>::infinity(),
                             -std::numeric_limits<float>::infinity()};
      for (std::size_t sy = y * step; sy < std::min((y + 1) * step, size.height); ++sy) {
        const auto source = rgb.row(sy);
        const auto sensor = support.row(sy);
        for (std::size_t sx = x * step; sx < std::min((x + 1) * step, size.width); ++sx) {
          const auto p = source[sx];
          sum[0] += p.r;
          sum[1] += p.g;
          sum[2] += p.b;
          ++count;
          range.minimum = std::min(range.minimum, sensor[sx].minimum);
          range.maximum = std::max(range.maximum, sensor[sx].maximum);
        }
      }
      row[x] = {{float(sum[0] / count), float(sum[1] / count), float(sum[2] / count)}, range};
    }
  }
  return result;
}

image<gray> fine_proxy(image_view<const rgb_f32> rgb, std::size_t step,
                       const color_encoding &encoding) {
  const auto size = rgb.size();
  image<gray> result({(size.width + step - 1) / step, (size.height + step - 1) / step});
  auto out = result.view();
  for (std::size_t y = 0; y < out.size().height; ++y) {
    auto row = out.row(y);
    for (std::size_t x = 0; x < row.size(); ++x) {
      double sum = 0;
      std::size_t count = 0;
      for (std::size_t sy = y * step; sy < std::min((y + 1) * step, size.height); ++sy) {
        const auto input = rgb.row(sy);
        for (std::size_t sx = x * step; sx < std::min((x + 1) * step, size.width); ++sx) {
          sum += std::max(0.f, luminance(input[sx], encoding));
          ++count;
        }
      }
      row[x].value = std::log1p(16. * sum / count);
    }
  }
  return result;
}

result<image<measurement>> aligned_proxy(const frame &f, extent full, extent small,
                                         std::size_t step, hdr_profile &profile) {
  auto src = patch<measurement>(f.proxy, small, {0, 0, small.width, small.height}, profile);
  if (!src)
    return std::unexpected(src.error());
  image<measurement> result(small);
  auto out = result.view();
  const double center = (step - 1) * .5;
  for (std::size_t y = 0; y < small.height; ++y)
    for (std::size_t x = 0; x < small.width; ++x) {
      const auto p = inverse(f.transform, step * x + center, step * y + center);
      auto m = sample(src->view(), (p[0] - center) / step, (p[1] - center) / step);
      if (!inside(full, p[0], p[1]))
        m.sensor = {-1, 2};
      out.at(x, y) = m;
    }
  return result;
}

result<motion_maps> analyze(std::vector<frame> &frames, extent full, extent small,
                            std::size_t step, std::size_t reference, double color_noise_gain,
                            hdr_profile &profile) {
  const auto count = small.width * small.height;
  image<measurement> anchor(small);
  std::vector<std::uint16_t> anchor_owner(count, reference);
  std::vector<float> anchor_quality(count, -1);
  for (std::size_t pass = 0; pass < frames.size(); ++pass) {
    const auto i = pass == 0 ? reference : pass <= reference ? pass - 1 : pass;
    auto aligned = aligned_proxy(frames[i], full, small, step, profile);
    if (!aligned)
      return std::unexpected(aligned.error());
    if (auto written = save<measurement>(frames[i].aligned, aligned->view(), profile); !written)
      return std::unexpected(written.error());
    for (std::size_t y = 0; y < small.height; ++y)
      for (std::size_t x = 0; x < small.width; ++x) {
        const auto at = y * small.width + x;
        const auto m = aligned->view().at(x, y);
        const auto q = quality(m.sensor, frames[i].levels);
        if (pass == 0 || (anchor_quality[at] < .25 && q > anchor_quality[at])) {
          anchor.view().at(x, y) = m;
          anchor_owner[at] = i;
          anchor_quality[at] = q;
        }
      }
  }
  motion_maps maps{small, image<gray>(small), std::vector<std::uint16_t>(count, reference)};
  for (std::size_t i = 0; i < frames.size(); ++i) {
    auto src = patch<measurement>(frames[i].aligned, small, {0, 0, small.width, small.height},
                                  profile);
    if (!src)
      return std::unexpected(src.error());
    for (std::size_t y = 0; y < small.height; ++y)
      for (std::size_t x = 0; x < small.width; ++x) {
        const auto at = y * small.width + x;
        const auto a = anchor.view().at(x, y), b = src->view().at(x, y);
        if (anchor_quality[at] <= .1)
          continue;
        // A clipped sample still provides a lower bound on pre-WB scene signal.
        // A reliable measurement below that bound is temporal inconsistency, not
        // ordinary clipping. Compare sensor support before any color-matrix mixing.
        const auto position = inverse(frames[i].transform, x * step + (step - 1) * .5,
                                      y * step + (step - 1) * .5);
        if (b.sensor.maximum >= .995 && inside(full, position[0], position[1])) {
          const double lower = .995 * frames[i].gain;
          const double observed = a.sensor.maximum * frames[anchor_owner[at]].gain;
          const double guard = .08 * std::max(std::abs(lower), std::abs(observed)) +
                               8 * std::sqrt(variance(a.sensor, frames[anchor_owner[at]]) +
                                             variance(b.sensor, frames[i]));
          if (observed + guard < lower)
            maps.motion.view().at(x, y).value = 1;
        }
        if (quality(b.sensor, frames[i].levels) <= .1)
          continue;
        const double noise = 8 * color_noise_gain *
                             std::sqrt(variance(a.sensor, frames[anchor_owner[at]]) +
                                       variance(b.sensor, frames[i]));
        const auto differs = [&](double u, double v) {
          return std::abs(u - v) > .08 * std::max(std::abs(u), std::abs(v)) + noise;
        };
        if (differs(a.rgb.r, b.rgb.r) || differs(a.rgb.g, b.rgb.g) || differs(a.rgb.b, b.rgb.b))
          maps.motion.view().at(x, y).value = 1;
      }
  }
  image<gray> expanded(small);
  for (std::size_t y = 0; y < small.height; ++y)
    for (std::size_t x = 0; x < small.width; ++x) {
      float value = 0;
      for (long dy = -2; dy <= 2; ++dy)
        for (long dx = -2; dx <= 2; ++dx)
          value = std::max(value, maps.motion.view()
                                      .at(std::clamp<long>(x + dx, 0, small.width - 1),
                                          std::clamp<long>(y + dy, 0, small.height - 1))
                                      .value);
      expanded.view().at(x, y).value = value;
    }
  maps.motion = std::move(expanded);
  std::vector<std::uint32_t> component(count);
  std::vector<std::size_t> queue;
  queue.reserve(count);
  std::uint32_t components = 0;
  for (std::size_t at = 0; at < count; ++at) {
    if (component[at] || maps.motion.view().at(at % small.width, at / small.width).value == 0)
      continue;
    ++components;
    queue.clear();
    queue.push_back(at);
    component[at] = components;
    for (std::size_t head = 0; head < queue.size(); ++head) {
      const auto p = queue[head], x = p % small.width, y = p / small.width;
      for (auto [dx, dy] : {std::pair{-1, 0}, {1, 0}, {0, -1}, {0, 1}}) {
        const long nx = static_cast<long>(x) + dx, ny = static_cast<long>(y) + dy;
        if (nx < 0 || ny < 0 || nx >= static_cast<long>(small.width) ||
            ny >= static_cast<long>(small.height))
          continue;
        const auto next = std::size_t(ny) * small.width + nx;
        if (!component[next] && maps.motion.view().at(nx, ny).value != 0) {
          component[next] = components;
          queue.push_back(next);
        }
      }
    }
  }
  if (!components)
    return maps;
  std::vector<std::size_t> best_coverage(components + 1);
  std::vector<double> best_quality(components + 1, -1);
  std::vector<std::uint16_t> owner(components + 1, reference);
  for (std::size_t pass = 0; pass < frames.size(); ++pass) {
    const auto i = pass == 0 ? reference : pass <= reference ? pass - 1 : pass;
    auto src = patch<measurement>(frames[i].aligned, small, {0, 0, small.width, small.height},
                                  profile);
    if (!src)
      return std::unexpected(src.error());
    std::vector<std::size_t> coverage(components + 1);
    std::vector<double> qualities(components + 1);
    for (std::size_t y = 0; y < small.height; ++y)
      for (std::size_t x = 0; x < small.width; ++x) {
        const auto id = component[y * small.width + x];
        if (!id)
          continue;
        const double q = quality(src->view().at(x, y).sensor, frames[i].levels);
        coverage[id] += q > .1;
        qualities[id] += q;
      }
    for (std::uint32_t id = 1; id <= components; ++id)
      if (coverage[id] > best_coverage[id] ||
          (coverage[id] == best_coverage[id] && qualities[id] > best_quality[id] + 1e-6)) {
        best_coverage[id] = coverage[id];
        best_quality[id] = qualities[id];
        owner[id] = i;
      }
  }
  for (std::size_t at = 0; at < count; ++at)
    if (component[at])
      maps.owner[at] = owner[component[at]];
  return maps;
}

result<image<fused_measurement>> fuse_tile(const std::vector<frame> &frames, extent full,
                                           rectangle area, const motion_maps &maps,
                                           std::size_t step, std::size_t reference,
                                           hdr_merge_report &report) {
  const extent size{area.width, area.height};
  image<measurement> fallback(size);
  image<std::uint16_t> fallback_owner(size);
  image<std::array<double, 4>> sum(size);
  image<gray> confidence(size), shadow_signal(size);
  std::vector<image<gray>> weights;
  weights.reserve(frames.size());
  for (std::size_t i = 0; i < frames.size(); ++i)
    weights.emplace_back(size);
  ++report.profile.tiles;
  for (std::size_t pass = 0; pass < frames.size(); ++pass) {
    const auto i = pass == 0 ? reference : pass <= reference ? pass - 1 : pass;
    auto source = warp(frames[i], full, area, report.profile);
    if (!source)
      return std::unexpected(source.error());
    for (std::size_t y = 0; y < size.height; ++y) {
      const auto src = source->view().row(y);
      auto fallback_row = fallback.view().row(y);
      auto owner_row = fallback_owner.view().row(y);
      auto accumulator = sum.view().row(y);
      auto weight_row = weights[i].view().row(y);
      auto quality_row = confidence.view().row(y);
      auto shadow_row = shadow_signal.view().row(y);
      for (std::size_t x = 0; x < size.width; ++x) {
        const auto p = src[x];
        const auto mx = std::min((area.x + x) / step, maps.size.width - 1),
                   my = std::min((area.y + y) / step, maps.size.height - 1);
        const bool motion = maps.motion.view().at(mx, my).value != 0;
        const double q = quality(p.sensor, frames[i].levels);
        const double w = static_cast<float>(q / std::max(variance(p.sensor, frames[i]), 1e-24));
        weight_row[x].value = w;
        quality_row[x].value = std::max<double>(quality_row[x].value, q);
        if (p.sensor.maximum < .995)
          shadow_row[x].value =
              std::max<double>(shadow_row[x].value, p.sensor.minimum * frames[i].levels);
        auto &a = accumulator[x];
        a[0] += w * p.rgb.r;
        a[1] += w * p.rgb.g;
        a[2] += w * p.rgb.b;
        a[3] += w;
        if (pass == 0) {
          fallback_row[x] = p;
          owner_row[x] = i;
          continue;
        }
        const auto position = inverse(frames[i].transform, area.x + x, area.y + y);
        if (!inside(full, position[0], position[1]))
          continue;
        if (motion) {
          if (maps.owner[my * maps.size.width + mx] == i) {
            fallback_row[x] = p;
            owner_row[x] = i;
          }
        } else {
          const bool usable = p.sensor.maximum < .995,
                     old_usable = fallback_row[x].sensor.maximum < .995;
          const auto old = owner_row[x];
          if ((usable && !old_usable) ||
              (usable && old_usable && frames[i].shutter > frames[old].shutter) ||
              (!usable && !old_usable && frames[i].shutter < frames[old].shutter)) {
            fallback_row[x] = p;
            owner_row[x] = i;
          }
        }
      }
    }
  }
  image<fused_measurement> result(size);
  auto out = result.view();
  for (std::size_t y = 0; y < size.height; ++y) {
    auto row = out.row(y);
    const auto acc = sum.view().row(y);
    const auto selected = fallback.view().row(y);
    const auto owners = fallback_owner.view().row(y);
    const auto qualities = confidence.view().row(y), shadows = shadow_signal.view().row(y);
    for (std::size_t x = 0; x < size.width; ++x) {
      const auto mx = std::min((area.x + x) / step, maps.size.width - 1),
                 my = std::min((area.y + y) / step, maps.size.height - 1);
      const bool motion = maps.motion.view().at(mx, my).value != 0;
      const auto p = selected[x];
      const auto &a = acc[x];
      if (!std::isfinite(a[3]))
        return fail(error_code::decode, "HDR fusion", "nonfinite measurement weights");
      const bool use_source = motion || a[3] <= 0;
      const auto rgb =
          use_source ? p.rgb
                     : rgb_f32{float(a[0] / a[3]), float(a[1] / a[3]), float(a[2] / a[3])};
      const float q = motion     ? quality(p.sensor, frames[owners[x]].levels)
                      : a[3] > 0 ? qualities[x].value
                                 : 0;
      const bool clipped = p.sensor.maximum >= .995;
      const bool low_snr =
          motion ? p.sensor.minimum * frames[owners[x]].levels < 16 : shadows[x].value < 16;
      row[x] = {rgb, {q}, {float(motion)}, {float(clipped)}, {float(low_snr)}};
      if (use_source)
        report.frames[owners[x]].contribution_fraction += 1;
      else
        for (std::size_t i = 0; i < frames.size(); ++i)
          report.frames[i].contribution_fraction += weights[i].view().at(x, y).value / a[3];
      report.clipped_fraction += clipped;
      report.low_snr_fraction += low_snr;
      report.minimum = std::min({report.minimum, rgb.r, rgb.g, rgb.b});
      report.maximum = std::max({report.maximum, rgb.r, rgb.g, rgb.b});
    }
  }
  return result;
}

std::string json_string(std::string_view value) {
  std::string out = "\"";
  constexpr char digits[] = "0123456789abcdef";
  for (unsigned char c : value) {
    if (c == '\\' || c == '"') {
      out += '\\';
      out += c;
    } else if (c < 32) {
      out += "\\u00";
      out += digits[c >> 4];
      out += digits[c & 15];
    } else
      out += c;
  }
  return out + '"';
}

template <class Pixel>
result<void> spool_row(std::fstream &file, std::size_t y, std::span<Pixel> row,
                       hdr_profile &profile) {
  file.seekg(y * row.size_bytes());
  file.read(reinterpret_cast<char *>(row.data()), row.size_bytes());
  profile.scratch_read_bytes += row.size_bytes();
  if (!file)
    return fail(error_code::io, "HDR output", "cannot read fused spool row");
  return {};
}

constexpr std::array<const char *, 4> map_names{"confidence.tif", "deghost.tif", "clipping.tif",
                                                "low-snr.tif"};
constexpr std::array<const char *, 4> map_meanings{"hdr-confidence;", "hdr-deghost;",
                                                   "hdr-clipping;", "hdr-low-snr;"};

result<void> diagnostics(const fs::path &root, hdr_merge_report &report,
                         const motion_maps &maps, std::array<std::fstream, 4> &scalar_spools) {
  const auto start = clock_type::now();
  std::error_code ec;
  fs::create_directories(root, ec);
  if (ec)
    return fail(error_code::io, "HDR diagnostics", ec.message(), root.string());
  const auto &context = *report.raw_context;
  for (std::size_t i = 0; i < map_names.size(); ++i) {
    auto written =
        write_scalar_tiff_rows((root / map_names[i]).string(), report.dimensions,
                               std::string(map_meanings[i]) + context.fingerprint(),
                               [&](std::size_t y, std::span<gray> row) {
                                 return spool_row(scalar_spools[i], y, row, report.profile);
                               });
    if (!written)
      return written;
  }
  auto owner = write_scalar_tiff_rows((root / "deghost-source.tif").string(), maps.size,
                                      "hdr-deghost-source-index;" + context.fingerprint(),
                                      [&](std::size_t y, std::span<gray> row) -> result<void> {
                                        for (std::size_t x = 0; x < row.size(); ++x)
                                          row[x].value = maps.owner[y * maps.size.width + x];
                                        return {};
                                      });
  if (!owner)
    return owner;
  report.profile.diagnostics_seconds = seconds(start);
  report.profile.total_seconds += report.profile.diagnostics_seconds;
  auto transaction = atomic_output::create((root / "report.json").string());
  if (!transaction)
    return std::unexpected(transaction.error());
  std::ofstream file(transaction->temporary_path());
  file.imbue(std::locale::classic());
  file << std::setprecision(14);
  const auto &ref = context.reference();
  const auto &p = report.profile;
  file << "{\n\"algorithm\":\"hdr-radiometric-v1\",\"working_space\":\"float32 scene-linear "
          "Rec.2020/D65\",\n"
       << "\"normalization\":\"reference-shutter signal units\",\"tone_mapping\":false,\n"
       << "\"width\":" << report.dimensions.width << ",\"height\":" << report.dimensions.height
       << ",\"reference_index\":" << report.reference_index
       << ",\"proxy_step\":" << report.proxy_step
       << ",\"refinement_step\":" << report.refinement_step
       << ",\"tile_size\":" << report.tile_size
       << ",\"estimated_peak_bytes\":" << report.estimated_peak_bytes
       << ",\"scratch_bytes\":" << report.scratch_bytes
       << ",\"deghost_fraction\":" << report.deghost_fraction
       << ",\"clipped_fraction\":" << report.clipped_fraction
       << ",\"low_snr_fraction\":" << report.low_snr_fraction
       << ",\"minimum\":" << report.minimum << ",\"maximum\":" << report.maximum;
  file << ",\n\"raw_context\":{\"fingerprint\":" << json_string(context.fingerprint())
       << ",\"camera\":" << json_string(ref.camera)
       << ",\"decoder_version\":" << json_string(ref.decoder_version)
       << ",\"exposure_policy\":\"shutter_bracket\",\"reference_index\":"
       << context.reference_index() << ",\"shutter_seconds\":" << ref.shutter_seconds
       << ",\"iso\":" << ref.iso << ",\"aperture\":" << ref.aperture;
  const auto array = [&](const char *name, const auto &values) {
    file << ",\"" << name << "\":[";
    bool first = true;
    for (auto value : values) {
      if (!first)
        file << ',';
      first = false;
      file << value;
    }
    file << ']';
  };
  array("white_balance", context.white_balance());
  array("black", ref.black);
  array("white", ref.white);
  array("camera_to_xyz", ref.camera_to_xyz);
  file << "},\n\"frames\":[";
  for (std::size_t i = 0; i < report.frames.size(); ++i) {
    const auto &f = report.frames[i];
    const auto &m = context.frames()[f.context_index];
    const auto &t = f.alignment;
    if (i)
      file << ',';
    file << "{\"path\":" << json_string(m.path) << ",\"context_index\":" << f.context_index
         << ",\"shutter_seconds\":" << f.shutter_seconds
         << ",\"exposure_scale\":" << f.exposure_scale
         << ",\"ev_from_reference\":" << -std::log2(f.exposure_scale)
         << ",\"contribution_fraction\":" << f.contribution_fraction
         << ",\"sensor_saturated_fraction\":" << f.raw_statistics.saturated_fraction
         << ",\"minimum\":" << f.raw_statistics.minimum
         << ",\"maximum\":" << f.raw_statistics.maximum << ",\"a\":" << t.a << ",\"b\":" << t.b
         << ",\"tx\":" << t.tx << ",\"ty\":" << t.ty << ",\"matches\":" << t.matches
         << ",\"inliers\":" << t.inliers << ",\"residual_px\":" << t.residual_px
         << ",\"dense_residual_px\":" << t.dense_residual_px
         << ",\"dense_correlation\":" << t.dense_correlation
         << ",\"refined\":" << (t.refined ? "true" : "false") << '}';
  }
  file << "],\n\"profile\":{\"preflight_seconds\":" << p.preflight_seconds
       << ",\"decode_seconds\":" << p.decode_seconds
       << ",\"registration_seconds\":" << p.registration_seconds
       << ",\"analysis_seconds\":" << p.analysis_seconds
       << ",\"fusion_seconds\":" << p.fusion_seconds
       << ",\"output_seconds\":" << p.output_seconds
       << ",\"diagnostics_seconds\":" << p.diagnostics_seconds
       << ",\"total_seconds\":" << p.total_seconds
       << ",\"scratch_read_bytes\":" << p.scratch_read_bytes
       << ",\"scratch_write_bytes\":" << p.scratch_write_bytes
       << ",\"scratch_peak_file_bytes\":" << p.scratch_peak_file_bytes
       << ",\"output_bytes\":" << p.output_bytes << ",\"tiles\":" << p.tiles << "}\n}\n";
  file.close();
  if (!file)
    return fail(error_code::io, "HDR diagnostics", "cannot finish report JSON", root.string());
  return transaction->commit();
}

result<void> validate_options(std::size_t count, const std::string &output,
                              const hdr_options &options) {
  if (count < 2 || count > 32 || options.memory_budget_mib < 512 ||
      options.memory_budget_mib > 1024 * 1024 || options.tile_size < 32 ||
      options.tile_size > 4096 || options.proxy_long_edge < 32 ||
      options.proxy_long_edge > 2048 ||
      (options.reference_index != static_cast<std::size_t>(-1) &&
       options.reference_index >= count))
    return fail(error_code::invalid_argument, "HDR preflight",
                "need 2..32 frames, budget 512..1048576 MiB, tile 32..4096, proxy 32..2048 and "
                "valid reference");
  auto extension = fs::path(output).extension().string();
  std::ranges::transform(extension, extension.begin(),
                         [](unsigned char c) { return std::tolower(c); });
  if (extension != ".tif" && extension != ".tiff")
    return fail(error_code::unsupported_format, "HDR output",
                "streaming HDR output requires float TIFF", output);
  std::error_code ec;
  if (fs::symlink_status(output, ec).type() != fs::file_type::not_found && !ec)
    return fail(error_code::io, "HDR output", "output already exists", output);
  if (!options.diagnostics_directory.empty()) {
    const fs::path root(options.diagnostics_directory);
    for (const auto *name : {"confidence.tif", "deghost.tif", "clipping.tif", "low-snr.tif",
                             "deghost-source.tif", "report.json"}) {
      const auto status = fs::symlink_status(root / name, ec);
      if (!ec && status.type() != fs::file_type::not_found)
        return fail(error_code::io, "HDR diagnostics", "output already exists",
                    (root / name).string());
    }
  }
  return {};
}

std::size_t choose_reference(const raw_sequence_context &context,
                             std::span<const std::size_t> indices, std::size_t requested) {
  if (requested != static_cast<std::size_t>(-1))
    return requested;
  std::vector<std::size_t> sorted(indices.size());
  std::iota(sorted.begin(), sorted.end(), 0);
  std::stable_sort(sorted.begin(), sorted.end(), [&](auto a, auto b) {
    return context.frames()[indices[a]].shutter_seconds <
           context.frames()[indices[b]].shutter_seconds;
  });
  return sorted[sorted.size() / 2];
}

result<hdr_merge_report> merge_prepared(const raw_sequence_context &context,
                                        std::span<const std::size_t> indices,
                                        const std::string &output, const hdr_options &options,
                                        clock_type::time_point run_start) {
  if (auto valid = validate_options(indices.size(), output, options); !valid)
    return std::unexpected(valid.error());
  if (context.exposure_policy() != raw_exposure_policy::shutter_bracket)
    return fail(error_code::invalid_argument, "HDR context",
                "requires explicit shutter-bracket context");
  std::vector<fs::path> identities;
  double shortest = std::numeric_limits<double>::infinity(), longest = 0;
  for (auto index : indices) {
    if (index >= context.frames().size())
      return fail(error_code::invalid_argument, "HDR context", "frame index outside context");
    std::error_code ec;
    auto path = fs::canonical(context.frames()[index].path, ec);
    if (ec || std::ranges::find(identities, path) != identities.end())
      return fail(error_code::invalid_argument, "HDR context", "missing or duplicate input",
                  context.frames()[index].path);
    identities.push_back(std::move(path));
    const auto shutter = context.frames()[index].shutter_seconds;
    shortest = std::min(shortest, shutter);
    longest = std::max(longest, shutter);
  }
  if (longest <= shortest * (1 + 1e-6))
    return fail(error_code::invalid_argument, "HDR preflight",
                "need distinct shutter measurements, not an identical-exposure burst");
  hdr_merge_report report;
  report.raw_context = context;
  auto &profile = report.profile;
  const auto reference = choose_reference(context, indices, options.reference_index);
  const auto size = context.frames()[indices[reference]].output_dimensions;
  if (size.width < 32 || size.height < 32 || size.width > 65536 || size.height > 65536 ||
      size.width * size.height > 200000000)
    return fail(error_code::invalid_argument, "HDR preflight",
                "requires dimensions 32..65536, <=200 MP");
  const auto pixels = size.width * size.height,
             usable = options.memory_budget_mib * mib * 4 / 5;
  std::size_t decode = 0;
  for (auto index : indices)
    decode = std::max(decode, context.frames()[index].decode_reserve_bytes + 8 * pixels);
  if (decode > usable)
    return fail(error_code::invalid_argument, "HDR memory budget",
                "single-frame measurement decode reserve exceeds budget");
  auto step = std::max<std::size_t>(
      1, (std::max(size.width, size.height) + options.proxy_long_edge - 1) /
             options.proxy_long_edge);
  auto tile = options.tile_size;
  const auto proxy_pixels = [&] {
    return ((size.width + step - 1) / step) * ((size.height + step - 1) / step);
  };
  std::size_t analysis = 0, fusion = 0, registration = 0;
  while (true) {
    const auto q = proxy_pixels();
    analysis = 192 * q + 128 * mib;
    registration = 512 * q + 128 * mib;
    fusion = 192 * q + (192 + 4 * indices.size()) * tile * tile + 128 * mib;
    if (std::max({analysis, fusion, registration}) <= usable)
      break;
    if (registration > usable && (size.width + step - 1) / step >= 64 &&
        (size.height + step - 1) / step >= 64)
      step *= 2;
    else if (fusion > usable && tile > 32)
      tile = std::max<std::size_t>(32, tile / 2);
    else
      return fail(error_code::invalid_argument, "HDR memory budget",
                  "minimum proxy/tile workspace exceeds budget");
  }
  auto fine_step = std::max<std::size_t>(1, step / 2);
  const auto fine_pixels = [&] {
    return ((size.width + fine_step - 1) / fine_step) *
           ((size.height + fine_step - 1) / fine_step);
  };
  while (128 * fine_pixels() + 128 * mib > usable)
    ++fine_step;
  registration = std::max(registration, 128 * fine_pixels() + 128 * mib);
  const auto q = proxy_pixels();
  const extent small{(size.width + step - 1) / step, (size.height + step - 1) / step};
  const extent fine_size{(size.width + fine_step - 1) / fine_step,
                         (size.height + fine_step - 1) / fine_step};
  report.dimensions = size;
  report.reference_index = reference;
  report.proxy_step = step;
  report.refinement_step = fine_step;
  report.tile_size = tile;
  report.estimated_peak_bytes = std::max({decode, analysis, fusion, registration}) * 5 / 4;
  report.scratch_bytes = indices.size() * (20 * pixels + 40 * q + 4 * fine_pixels()) +
                         40 * pixels +
                         (options.diagnostics_directory.empty() ? 0 : 16 * pixels) + 8 * mib;
  report.frames.resize(indices.size());
  std::error_code ec;
  const auto scratch_root = options.scratch_directory.empty()
                                ? fs::temp_directory_path(ec)
                                : fs::path(options.scratch_directory);
  const auto space = fs::space(scratch_root, ec);
  if (ec || space.available < report.scratch_bytes)
    return fail(error_code::io, "HDR scratch",
                "invalid directory or insufficient scratch space", scratch_root.string());
  auto pattern = (scratch_root / "photo-ops-hdr-XXXXXX").string();
  if (!::mkdtemp(pattern.data()))
    return fail(error_code::io, "HDR scratch", "cannot create private run directory",
                scratch_root.string());
  scratch_run scratch{pattern};
  {
    std::ofstream manifest(scratch.path / "context.txt");
    manifest << context.fingerprint() << '\n';
    for (auto index : indices)
      manifest << index << ' ' << context.frames()[index].input_fingerprint << '\n';
    manifest.close();
    if (!manifest)
      return fail(error_code::io, "HDR scratch", "cannot finish context manifest");
  }
  std::vector<frame> frames(indices.size());
  for (std::size_t i = 0; i < frames.size(); ++i) {
    auto &f = frames[i];
    const auto &m = context.frames()[indices[i]];
    const auto prefix = scratch.path / std::to_string(i);
    f.pixels = prefix.string() + ".measure";
    f.proxy = prefix.string() + ".proxy";
    f.fine = prefix.string() + ".fine";
    f.aligned = prefix.string() + ".aligned";
    f.gain = context.exposure_scale(indices[i]);
    f.shutter = m.shutter_seconds;
    f.levels = std::numeric_limits<double>::infinity();
    for (std::size_t c = 0; c < 4; ++c)
      f.levels = std::min(f.levels, m.white[c] - m.black[c]);
    report.frames[i].context_index = indices[i];
    report.frames[i].shutter_seconds = m.shutter_seconds;
    report.frames[i].exposure_scale = f.gain;
  }
  profile.preflight_seconds = seconds(run_start);
  image<gray> reference_proxy;
  for (std::size_t pass = 0; pass < frames.size(); ++pass) {
    const auto i = pass == 0 ? reference : pass <= reference ? pass - 1 : pass;
    const auto start = clock_type::now();
    auto decoded = decode_raw_frame(context.frames()[indices[i]].path, context, indices[i],
                                    {.sensor_support = true});
    if (!decoded)
      return std::unexpected(decoded.error());
    report.frames[i].raw_statistics = decoded->statistics;
    auto proxy = make_proxy(*decoded, step);
    auto luminance = luminance_proxy(proxy.view(), context.encoding());
    if (auto written = save<measurement>(frames[i].proxy, proxy.view(), profile); !written)
      return std::unexpected(written.error());
    proxy = {};
    auto fine = fine_proxy(decoded->image.pixels.view(), fine_step, context.encoding());
    if (auto written = save<gray>(frames[i].fine, fine.view(), profile); !written)
      return std::unexpected(written.error());
    fine = {};
    std::ofstream spool(frames[i].pixels, std::ios::binary);
    std::vector<measurement> row(size.width);
    const auto rgb = decoded->image.pixels.view();
    const auto support = decoded->sensor_support.view();
    for (std::size_t y = 0; y < size.height; ++y) {
      const auto color = rgb.row(y);
      const auto sensor = support.row(y);
      for (std::size_t x = 0; x < size.width; ++x)
        row[x] = {color[x], sensor[x]};
      spool.write(reinterpret_cast<const char *>(row.data()), row.size() * sizeof(measurement));
      profile.scratch_write_bytes += row.size() * sizeof(measurement);
    }
    spool.close();
    if (!spool)
      return fail(error_code::io, "HDR scratch", "cannot finish measurement spool",
                  frames[i].pixels.string());
    decoded->image.pixels = {};
    decoded->sensor_support = {};
    profile.decode_seconds += seconds(start);
    if (i == reference)
      reference_proxy = std::move(luminance);
    else if (!options.already_aligned) {
      const auto align_start = clock_type::now();
      auto aligned = detail::align_bracket(reference_proxy.view(), luminance.view());
      if (!aligned) {
        auto e = aligned.error();
        e.operation = "HDR registration";
        e.path = context.frames()[indices[i]].path;
        return std::unexpected(std::move(e));
      }
      auto t = *aligned;
      const double center = (step - 1) * .5;
      t.tx = step * t.tx + center - (t.a - t.b) * center;
      t.ty = step * t.ty + center - (t.a + t.b) * center;
      t.residual_px *= step;
      frames[i].transform = t;
      profile.registration_seconds += seconds(align_start);
    }
  }
  reference_proxy = {};
  if (!options.already_aligned) {
    const auto start = clock_type::now();
    auto reference_fine = patch<gray>(frames[reference].fine, fine_size,
                                      {0, 0, fine_size.width, fine_size.height}, profile);
    if (!reference_fine)
      return std::unexpected(reference_fine.error());
    for (std::size_t i = 0; i < frames.size(); ++i) {
      if (i == reference || frames[i].transform.identity)
        continue;
      auto fine = patch<gray>(frames[i].fine, fine_size,
                              {0, 0, fine_size.width, fine_size.height}, profile);
      if (!fine)
        return std::unexpected(fine.error());
      auto t = frames[i].transform;
      const double center = (fine_step - 1) * .5;
      t.tx = (t.tx - center + (t.a - t.b) * center) / fine_step;
      t.ty = (t.ty - center + (t.a + t.b) * center) / fine_step;
      auto refined = detail::refine_bracket(reference_fine->view(), fine->view(), t);
      if (!refined) {
        auto e = refined.error();
        e.operation = "HDR refinement";
        e.path = context.frames()[indices[i]].path;
        return std::unexpected(std::move(e));
      }
      t = *refined;
      t.tx = fine_step * t.tx + center - (t.a - t.b) * center;
      t.ty = fine_step * t.ty + center - (t.a + t.b) * center;
      t.dense_residual_px *= fine_step;
      frames[i].transform = t;
    }
    profile.registration_seconds += seconds(start);
  }
  for (std::size_t i = 0; i < frames.size(); ++i)
    report.frames[i].alignment = frames[i].transform;
  double color_noise_gain = 0;
  std::array<double, 3> row_energy{};
  for (std::size_t c = 0; c < 3; ++c) {
    const auto &m = context.reference().camera_to_xyz;
    const auto rgb =
        from_xyz({float(m[c]), float(m[3 + c]), float(m[6 + c])}, context.encoding());
    const double wb = context.white_balance()[c];
    row_energy[0] += rgb.r * rgb.r * wb * wb;
    row_energy[1] += rgb.g * rgb.g * wb * wb;
    row_energy[2] += rgb.b * rgb.b * wb * wb;
  }
  for (auto e : row_energy)
    color_noise_gain = std::max(color_noise_gain, std::sqrt(e));
  auto stage_start = clock_type::now();
  auto maps = analyze(frames, size, small, step, reference, color_noise_gain, profile);
  profile.analysis_seconds = seconds(stage_start);
  if (!maps)
    return std::unexpected(maps.error());
  for (auto p : maps->motion.view().storage())
    report.deghost_fraction += p.value != 0;
  report.deghost_fraction /= q;
  stage_start = clock_type::now();
  std::fstream fused(scratch.path / "fused.rgb",
                     std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
  std::array<std::fstream, 4> scalar_spools;
  for (std::size_t i = 0; i < scalar_spools.size(); ++i)
    scalar_spools[i].open(scratch.path / ("map-" + std::to_string(i)),
                          std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
  if (!fused || std::ranges::any_of(scalar_spools, [](const auto &f) { return !f; }))
    return fail(error_code::io, "HDR fusion", "cannot create fused spools");
  report.minimum = std::numeric_limits<float>::infinity();
  report.maximum = -report.minimum;
  for (std::size_t y = 0; y < size.height; y += tile)
    for (std::size_t x = 0; x < size.width; x += tile) {
      const rectangle area{x, y, std::min(tile, size.width - x),
                           std::min(tile, size.height - y)};
      auto result = fuse_tile(frames, size, area, *maps, step, reference, report);
      if (!result)
        return std::unexpected(result.error());
      std::vector<rgb_f32> rgb_row(area.width);
      std::array<std::vector<gray>, 4> scalar_rows;
      for (auto &row : scalar_rows)
        row.resize(area.width);
      for (std::size_t row = 0; row < area.height; ++row) {
        const auto input = result->view().row(row);
        for (std::size_t col = 0; col < area.width; ++col) {
          rgb_row[col] = input[col].rgb;
          scalar_rows[0][col] = input[col].confidence;
          scalar_rows[1][col] = input[col].deghost;
          scalar_rows[2][col] = input[col].clipping;
          scalar_rows[3][col] = input[col].low_snr;
        }
        fused.seekp(((y + row) * size.width + x) * sizeof(rgb_f32));
        fused.write(reinterpret_cast<const char *>(rgb_row.data()),
                    rgb_row.size() * sizeof(rgb_f32));
        for (std::size_t i = 0; i < 4; ++i) {
          scalar_spools[i].seekp(((y + row) * size.width + x) * sizeof(gray));
          scalar_spools[i].write(reinterpret_cast<const char *>(scalar_rows[i].data()),
                                 scalar_rows[i].size() * sizeof(gray));
        }
        profile.scratch_write_bytes += area.width * sizeof(fused_measurement);
        if (!fused || std::ranges::any_of(scalar_spools, [](const auto &f) { return !f; }))
          return fail(error_code::io, "HDR fusion", "cannot write fused tile");
      }
    }
  fused.flush();
  for (auto &f : scalar_spools)
    f.flush();
  if (!fused || std::ranges::any_of(scalar_spools, [](const auto &f) { return !f; }))
    return fail(error_code::io, "HDR fusion", "cannot flush fused spools");
  profile.fusion_seconds = seconds(stage_start);
  report.clipped_fraction /= pixels;
  report.low_snr_fraction /= pixels;
  for (auto &f : report.frames)
    f.contribution_fraction /= pixels;
  for (fs::directory_iterator it(scratch.path, ec), end; !ec && it != end; it.increment(ec))
    profile.scratch_peak_file_bytes += it->file_size(ec);
  if (ec)
    return fail(error_code::io, "HDR scratch profile", ec.message(), scratch.path.string());
  stage_start = clock_type::now();
  auto written = write_working_tiff_rows(
      output, size, context.encoding(),
      [&](std::size_t y, std::span<rgb_f32> row) { return spool_row(fused, y, row, profile); });
  profile.output_seconds = seconds(stage_start);
  if (!written)
    return std::unexpected(written.error());
  profile.output_bytes = fs::file_size(output, ec);
  if (ec)
    return fail(error_code::io, "HDR output", ec.message(), output);
  profile.total_seconds = seconds(run_start);
  if (!options.diagnostics_directory.empty()) {
    auto result = diagnostics(options.diagnostics_directory, report, *maps, scalar_spools);
    if (!result) {
      auto e = result.error();
      e.message =
          "complete image published at " + output + "; diagnostics failed: " + e.message;
      return std::unexpected(std::move(e));
    }
  }
  return report;
}
} // namespace

result<hdr_merge_report> merge_hdr(std::span<const std::string> inputs,
                                   const std::string &output, const hdr_options &options) {
  const auto start = clock_type::now();
  if (auto valid = validate_options(inputs.size(), output, options); !valid)
    return std::unexpected(valid.error());
  std::vector<raw_frame_metadata> metadata;
  for (const auto &path : inputs) {
    auto extension = fs::path(path).extension().string();
    std::ranges::transform(extension, extension.begin(),
                           [](unsigned char c) { return std::tolower(c); });
    if (extension != ".cr3" && extension != ".dng")
      return fail(error_code::unsupported_format, "HDR input",
                  "requires Bayer CR3/DNG measurements", path);
    auto frame = inspect_raw_frame(path, options.memory_budget_mib);
    if (!frame)
      return std::unexpected(frame.error());
    metadata.push_back(std::move(*frame));
  }
  std::vector<std::size_t> order(inputs.size());
  std::iota(order.begin(), order.end(), 0);
  std::stable_sort(order.begin(), order.end(), [&](auto a, auto b) {
    return metadata[a].shutter_seconds < metadata[b].shutter_seconds;
  });
  raw_sequence_options settings;
  settings.exposure_policy = raw_exposure_policy::shutter_bracket;
  settings.memory_budget_mib = options.memory_budget_mib;
  settings.reference_index = options.reference_index == static_cast<std::size_t>(-1)
                                 ? order[order.size() / 2]
                                 : options.reference_index;
  auto context = prepare_raw_sequence(metadata, settings);
  if (!context)
    return std::unexpected(context.error());
  std::iota(order.begin(), order.end(), 0);
  return merge_prepared(*context, order, output, options, start);
}

result<hdr_merge_report> merge_hdr(const raw_sequence_context &context,
                                   std::span<const std::size_t> frame_indices,
                                   const std::string &output, const hdr_options &options) {
  return merge_prepared(context, frame_indices, output, options, clock_type::now());
}
} // namespace photo_ops
