// SPDX-License-Identifier: GPL-3.0-or-later
module;
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <limits>
#include <numeric>
#include <span>
#include <string>
#include <vector>
module photo_ops.focus_stack;
import photo_ops.hdr;

namespace photo_ops {
namespace {
namespace fs = std::filesystem;
using matrix_clock = std::chrono::steady_clock;
constexpr std::size_t matrix_mib = 1024 * 1024;
constexpr std::array<const char *, 6> hdr_artifacts{"confidence.tif",     "deghost.tif",
                                                    "clipping.tif",       "low-snr.tif",
                                                    "deghost-source.tif", "report.json"};
constexpr std::array<const char *, 13> focus_artifacts{
    "source-map.tif",         "confidence.tif",   "motion.tif",      "defocus-boundary.tif",
    "boundary-source.tif",    "before.jpg",       "after.jpg",       "report.json",
    "hdr-confidence.tif",     "hdr-clipping.tif", "hdr-low-snr.tif", "hdr-deghost.tif",
    "combined-confidence.tif"};

struct matrix_directory {
  fs::path path;

  ~matrix_directory() {
    std::error_code ec;
    if (!path.empty())
      fs::remove_all(path, ec);
  }
};

double matrix_elapsed(matrix_clock::time_point start) {
  return std::chrono::duration<double>(matrix_clock::now() - start).count();
}

bool control_character(const std::string &value) {
  return std::ranges::any_of(value, [](unsigned char c) { return c < 32 || c == 127; });
}

result<void> no_existing(const fs::path &path) {
  std::error_code ec;
  const auto status = fs::symlink_status(path, ec);
  if (!ec && status.type() != fs::file_type::not_found)
    return fail(error_code::io, "focus matrix output", "output already exists", path.string());
  if (ec && ec != std::errc::no_such_file_or_directory)
    return fail(error_code::io, "focus matrix output", ec.message(), path.string());
  return {};
}

result<std::size_t> directory_bytes(const fs::path &root) {
  std::error_code ec;
  std::size_t bytes = 0;
  for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
    if (it->is_regular_file(ec))
      bytes += it->file_size(ec);
  if (ec)
    return fail(error_code::io, "focus matrix scratch", ec.message(), root.string());
  return bytes;
}

std::size_t median_shutter(std::span<const raw_frame_metadata> metadata, std::size_t offset,
                           std::size_t count) {
  std::vector<std::size_t> order(count);
  std::iota(order.begin(), order.end(), 0);
  std::stable_sort(order.begin(), order.end(), [&](auto a, auto b) {
    return metadata[offset + a].shutter_seconds < metadata[offset + b].shutter_seconds;
  });
  return order[count / 2];
}
} // namespace

result<focus_stack_report> stack_focus(const focus_exposure_matrix &matrix,
                                       const std::string &output,
                                       const focus_stack_options &options) {
  const auto start = matrix_clock::now();
  const auto groups = matrix.focus_planes.size();
  if (groups < 2 || groups > 32 || options.memory_budget_mib < 512 ||
      options.memory_budget_mib > 1024 * 1024 || options.tile_size < 32 ||
      options.tile_size > 4096 || options.proxy_long_edge < 32 ||
      options.proxy_long_edge > 2048 ||
      (options.reference_index != static_cast<std::size_t>(-1) &&
       options.reference_index >= groups))
    return fail(error_code::invalid_argument, "focus matrix preflight",
                "need 2..32 planes, budget 512..1048576 MiB, tile 32..4096, proxy 32..2048 and "
                "valid focus reference");
  const auto exposures = matrix.focus_planes.front().exposures.size();
  if (exposures < 2 || exposures > 16 || groups * exposures > 256)
    return fail(error_code::invalid_argument, "focus matrix preflight",
                "need 2..16 exposures per plane, <=256 originals");
  auto extension = fs::path(output).extension().string();
  std::ranges::transform(extension, extension.begin(),
                         [](unsigned char c) { return std::tolower(c); });
  if (extension != ".tif" && extension != ".tiff")
    return fail(error_code::unsupported_format, "focus matrix output", "requires float TIFF",
                output);
  if (auto free = no_existing(output); !free)
    return std::unexpected(free.error());
  if (!options.diagnostics_directory.empty()) {
    const fs::path root(options.diagnostics_directory);
    for (const auto *name : focus_artifacts)
      if (auto free = no_existing(root / name); !free)
        return std::unexpected(free.error());
    for (std::size_t i = 0; i < groups; ++i)
      for (const auto *name : hdr_artifacts)
        if (auto free = no_existing(root / ("hdr-plane-" + std::to_string(i)) / name); !free)
          return std::unexpected(free.error());
  }
  std::vector<std::string> names;
  std::vector<fs::path> canonical_inputs;
  std::vector<std::string> original_paths;
  std::vector<raw_frame_metadata> metadata;
  metadata.reserve(groups * exposures);
  for (const auto &plane : matrix.focus_planes) {
    if (plane.name.empty() || plane.name.size() > 128 || control_character(plane.name) ||
        plane.name.find_first_not_of(' ') == std::string::npos ||
        std::ranges::find(names, plane.name) != names.end() ||
        plane.exposures.size() != exposures)
      return fail(error_code::invalid_argument, "focus matrix preflight",
                  "require unique nonempty plane names and a rectangular exposure grid",
                  plane.name);
    names.push_back(plane.name);
    for (const auto &path : plane.exposures) {
      if (path.empty() || control_character(path))
        return fail(error_code::invalid_argument, "focus matrix input", "invalid path", path);
      auto suffix = fs::path(path).extension().string();
      std::ranges::transform(suffix, suffix.begin(),
                             [](unsigned char c) { return std::tolower(c); });
      if (suffix != ".cr3" && suffix != ".dng")
        return fail(error_code::unsupported_format, "focus matrix input",
                    "requires Bayer CR3/DNG originals", path);
      std::error_code ec;
      auto canonical = fs::canonical(path, ec);
      if (ec || std::ranges::find(canonical_inputs, canonical) != canonical_inputs.end())
        return fail(error_code::invalid_argument, "focus matrix input",
                    "missing or reused original", path);
      canonical_inputs.push_back(std::move(canonical));
      original_paths.push_back(path);
    }
  }
  for (const auto &path : original_paths) {
    auto inspected = inspect_raw_frame(path, options.memory_budget_mib);
    if (!inspected)
      return std::unexpected(inspected.error());
    metadata.push_back(std::move(*inspected));
  }
  std::vector<double> shutter_grid;
  for (std::size_t plane = 0; plane < groups; ++plane) {
    std::vector<double> shutters;
    for (std::size_t e = 0; e < exposures; ++e)
      shutters.push_back(metadata[plane * exposures + e].shutter_seconds);
    std::sort(shutters.begin(), shutters.end());
    if (plane == 0)
      shutter_grid = shutters;
    else
      for (std::size_t e = 0; e < exposures; ++e)
        if (std::abs(shutters[e] - shutter_grid[e]) >
            1e-6 * std::max(shutters[e], shutter_grid[e]))
          return fail(error_code::invalid_argument, "focus matrix exposure grid",
                      "focus planes must contain the same shutter grid",
                      matrix.focus_planes[plane].name);
  }
  const auto reference = options.reference_index == static_cast<std::size_t>(-1)
                             ? groups / 2
                             : options.reference_index;
  raw_sequence_options settings;
  settings.memory_budget_mib = options.memory_budget_mib;
  settings.exposure_policy = raw_exposure_policy::shutter_bracket;
  settings.reference_index =
      reference * exposures + median_shutter(metadata, reference * exposures, exposures);
  auto context = prepare_raw_sequence(metadata, settings);
  if (!context)
    return std::unexpected(context.error());
  const auto size = context->reference().output_dimensions;
  const auto pixels = size.width * size.height;
  const auto step = std::max<std::size_t>(
      1, (std::max(size.width, size.height) + options.proxy_long_edge - 1) /
             options.proxy_long_edge);
  const auto q = ((size.width + step - 1) / step) * ((size.height + step - 1) / step);
  const auto fine_step = std::max<std::size_t>(1, step / 2);
  const auto r =
      ((size.width + fine_step - 1) / fine_step) * ((size.height + fine_step - 1) / fine_step);
  const auto hdr_peak =
      28 * groups * pixels + exposures * (20 * pixels + 40 * q + 4 * r) + 28 * pixels;
  const auto focus_peak =
      56 * groups * pixels + 24 * pixels + groups * (56 * q + 4 * r) +
      (options.diagnostics_directory.empty() ? 0 : 16 * groups * pixels + 32 * q);
  const auto required_bytes = std::max(hdr_peak, focus_peak) + 16 * matrix_mib;
  std::error_code ec;
  const auto root = options.scratch_directory.empty() ? fs::temp_directory_path(ec)
                                                      : fs::path(options.scratch_directory);
  const auto space = fs::space(root, ec);
  if (ec || space.available < required_bytes)
    return fail(error_code::io, "focus matrix scratch",
                "invalid directory or insufficient aggregate scratch space", root.string());
  auto pattern = (root / "photo-ops-matrix-XXXXXX").string();
  if (!::mkdtemp(pattern.data()))
    return fail(error_code::io, "focus matrix scratch", "cannot create private run directory",
                root.string());
  matrix_directory scratch{pattern};
  focus_stack_report seed;
  seed.raw_context = *context;
  seed.scratch_bytes = required_bytes;
  seed.profile.matrix_preflight_seconds = matrix_elapsed(start);
  seed.hdr_planes.reserve(groups);
  std::vector<detail::hdr_plane_input> planes;
  planes.reserve(groups);
  const auto hdr_start = matrix_clock::now();
  for (std::size_t plane = 0; plane < groups; ++plane) {
    const auto directory = scratch.path / ("plane-" + std::to_string(plane));
    fs::create_directory(directory, ec);
    if (ec)
      return fail(error_code::io, "focus matrix scratch", ec.message(), directory.string());
    const auto image = (directory / "radiance.tif").string();
    const auto diagnostic = directory / "diagnostics";
    hdr_options hdr;
    hdr.memory_budget_mib = options.memory_budget_mib;
    hdr.tile_size = options.tile_size;
    hdr.proxy_long_edge = options.proxy_long_edge;
    hdr.already_aligned = options.already_aligned;
    hdr.reference_index = median_shutter(metadata, plane * exposures, exposures);
    hdr.scratch_directory = scratch.path.string();
    hdr.diagnostics_directory = diagnostic.string();
    std::vector<std::size_t> indices(exposures);
    std::iota(indices.begin(), indices.end(), plane * exposures);
    auto merged = merge_hdr(*context, indices, image, hdr);
    if (!merged) {
      auto error = merged.error();
      error.operation = "focus matrix HDR plane";
      const auto published = "complete image published at " + image + "; diagnostics failed: ";
      if (error.message.starts_with(published))
        error.message =
            "intermediate HDR diagnostics failed: " + error.message.substr(published.size());
      error.message = "plane " + matrix.focus_planes[plane].name +
                      " failed; private intermediates discarded: " + error.message;
      return std::unexpected(std::move(error));
    }
    auto retained = directory_bytes(scratch.path);
    if (!retained)
      return std::unexpected(retained.error());
    seed.profile.hdr_scratch_peak_file_bytes =
        std::max(seed.profile.hdr_scratch_peak_file_bytes,
                 merged->profile.scratch_peak_file_bytes + *retained);
    seed.estimated_peak_bytes =
        std::max(seed.estimated_peak_bytes, merged->estimated_peak_bytes);
    seed.hdr_planes.push_back({matrix.focus_planes[plane].name, std::move(*merged)});
    planes.push_back({image, (diagnostic / "confidence.tif").string(),
                      (diagnostic / "deghost.tif").string(),
                      (diagnostic / "clipping.tif").string(),
                      (diagnostic / "low-snr.tif").string(), diagnostic.string()});
  }
  seed.profile.hdr_seconds = matrix_elapsed(hdr_start);
  auto retained = directory_bytes(scratch.path);
  if (!retained)
    return std::unexpected(retained.error());
  seed.matrix_intermediate_bytes = *retained;
  seed.profile.total_seconds = matrix_elapsed(start);
  return detail::stack_hdr_planes(planes, output, options, std::move(seed));
}
} // namespace photo_ops
