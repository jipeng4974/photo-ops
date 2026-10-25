// SPDX-License-Identifier: GPL-3.0-or-later
module;
#include <algorithm>
#include <cmath>
#include <expected>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
module photo_ops.diagnostics;
import photo_ops.image;
import photo_ops.io;
import photo_ops.output;
import photo_ops.sunlight;
import photo_ops.color;

namespace photo_ops {
namespace {
float preview_gain(image_view<const linear_rgb> source) {
  std::vector<float> samples;
  for (std::size_t y = 0; y < source.size().height; y += 32)
    for (std::size_t x = 0; x < source.size().width; x += 32)
      samples.push_back(std::max(luminance(source.at(x, y)), 1e-6f));
  std::ranges::sort(samples);
  const auto upper = samples[static_cast<std::size_t>(0.95f * (samples.size() - 1))];
  return std::clamp(0.5f / upper, 1.f, 16.f);
}

rectangle detail_region(image_view<const gray> mask) {
  const auto size = mask.size();
  std::size_t left = size.width, top = size.height, right = 0, bottom = 0;
  for (std::size_t y = 0; y < size.height; ++y)
    for (std::size_t x = 0; x < size.width; ++x)
      if (mask.at(x, y).value > .5f) {
        left = std::min(left, x);
        top = std::min(top, y);
        right = std::max(right, x);
        bottom = std::max(bottom, y);
      }
  if (left == size.width)
    return {};
  const auto mx = (right - left + 1) / 8 + 1, my = (bottom - top + 1) / 8 + 1;
  const auto x0 = left > mx ? left - mx : 0, y0 = top > my ? top - my : 0;
  const auto x1 = std::min(size.width, right + mx + 1);
  const auto y1 = std::min(size.height, bottom + my + 1);
  return {x0, y0, x1 - x0, y1 - y0};
}

void fill_preview(image_view<const linear_rgb> source, image_view<linear_rgb> destination,
                  float gain) {
  for (std::size_t y = 0; y < source.size().height; ++y) {
    auto row = destination.row(y);
    for (std::size_t x = 0; x < source.size().width; ++x) {
      const auto pixel = source.at(x, y);
      row[x] = {pixel.r * gain, pixel.g * gain, pixel.b * gain};
    }
  }
}
} // namespace

result<void> write_sunlight_diagnostics(const std::string &directory,
                                        image_view<const linear_rgb> source,
                                        const sunlight_result &enhanced) {
  namespace fs = std::filesystem;
  const fs::path root(directory);
  std::error_code status;
  fs::create_directories(root, status);
  if (status)
    return fail(error_code::io, "create diagnostics", status.message(), directory);

  for (const auto *name : {"before.jpg", "after.jpg", "mask.tif", "mask.jpg",
                           "detail-before.jpg", "detail-after.jpg", "report.json"}) {
    const auto path = root / name;
    const bool exists = fs::exists(path, status);
    if (status || exists)
      return fail(error_code::io, "create diagnostics",
                  status ? status.message() : "diagnostic output already exists",
                  path.string());
  }

  // A shared exposure lift is for inspection only; it never enters the photographic output.
  const float gain = preview_gain(source);
  const auto detail = detail_region(enhanced.mask.view());
  image<linear_rgb> preview(source.size());
  fill_preview(source, preview.view(), gain);
  if (auto written =
          write_image((root / "before.jpg").string(), preview.view(), {output_format::jpeg});
      !written)
    return written;
  if (detail.width)
    if (auto written = write_image((root / "detail-before.jpg").string(),
                                   preview.view().subview(detail), {output_format::jpeg});
        !written)
      return written;
  fill_preview(enhanced.pixels.view(), preview.view(), gain);
  if (auto written =
          write_image((root / "after.jpg").string(), preview.view(), {output_format::jpeg});
      !written)
    return written;

  if (detail.width)
    if (auto written = write_image((root / "detail-after.jpg").string(),
                                   preview.view().subview(detail), {output_format::jpeg});
        !written)
      return written;

  // Reuse the explicit preview allocation to render a viewable, color-managed scalar mask.
  for (std::size_t y = 0; y < source.size().height; ++y)
    for (std::size_t x = 0; x < source.size().width; ++x) {
      const float weight = enhanced.mask.view().at(x, y).value;
      preview.view().at(x, y) = {weight, weight, weight};
    }
  if (auto written = write_image((root / "mask.tif").string(), preview.view()); !written)
    return written;

  if (auto written =
          write_image((root / "mask.jpg").string(), preview.view(), {output_format::jpeg});
      !written)
    return written;

  const auto &report = enhanced.report;
  auto transaction = atomic_output::create((root / "report.json").string());
  if (!transaction)
    return std::unexpected(transaction.error());
  std::ofstream file(transaction->temporary_path());
  file << "{\n  \"algorithm\": \"sunlight-classical-v2\",\n"
       << "  \"applied\": " << (report.applied ? "true" : "false") << ",\n  \"decision\": \""
       << report.decision << "\""
       << ",\n  \"selected_relative_chroma\": " << report.selected_relative_chroma
       << ",\n  \"mean_relative_chroma_gain\": " << report.mean_relative_chroma_gain
       << ",\n  \"mean_luminance_change_ev\": " << report.mean_luminance_change_ev
       << ",\n  \"changed_pixel_fraction\": " << report.changed_pixel_fraction
       << ",\n  \"rejected_neutral_regions\": " << report.rejected_neutral_regions
       << ",\n  \"candidate_regions\": " << report.candidate_regions
       << ",\n  \"selected_area_fraction\": " << report.selected_area_fraction
       << ",\n  \"heuristic_confidence\": " << report.confidence
       << ",\n  \"luminance_contrast_ev\": " << report.luminance_contrast_ev
       << ",\n  \"chroma_deficit\": " << report.chroma_deficit
       << ",\n  \"maximum_chroma_gain\": " << report.maximum_chroma_gain
       << ",\n  \"preview_exposure_ev\": " << std::log2(gain)
       << ",\n  \"working_space\": \"float32 linear-sRGB/D65\"\n}\n";
  file.close();
  if (!file)
    return fail(error_code::io, "write diagnostics", "cannot finalize JSON",
                (root / "report.json").string());
  return transaction->commit();
}
} // namespace photo_ops
