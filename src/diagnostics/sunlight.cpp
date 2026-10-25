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

  for (const auto *name : {"before.jpg", "after.jpg", "mask.tif", "report.json"}) {
    const auto path = root / name;
    const bool exists = fs::exists(path, status);
    if (status || exists)
      return fail(error_code::io, "create diagnostics",
                  status ? status.message() : "diagnostic output already exists",
                  path.string());
  }

  // A shared exposure lift is for inspection only; it never enters the photographic output.
  const float gain = preview_gain(source);
  image<linear_rgb> preview(source.size());
  fill_preview(source, preview.view(), gain);
  if (auto written =
          write_image((root / "before.jpg").string(), preview.view(), {output_format::jpeg});
      !written)
    return written;
  fill_preview(enhanced.pixels.view(), preview.view(), gain);
  if (auto written =
          write_image((root / "after.jpg").string(), preview.view(), {output_format::jpeg});
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

  const auto &report = enhanced.report;
  auto transaction = atomic_output::create((root / "report.json").string());
  if (!transaction)
    return std::unexpected(transaction.error());
  std::ofstream file(transaction->temporary_path());
  file << "{\n  \"algorithm\": \"sunlight-classical-v1\",\n"
       << "  \"applied\": " << (report.applied ? "true" : "false")
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
