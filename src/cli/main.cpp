// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <json/json.h>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>
import photo_ops.image;
import photo_ops.io;
import photo_ops.diagnostics;
import photo_ops.sunlight;
import photo_ops.focus_stack;
import photo_ops.hdr;

namespace {
namespace photo = photo_ops;

struct arguments {
  std::string command;
  std::string input;
  std::string matrix_manifest;
  std::string output;
  bool half_size = false;
  std::string diagnostics;
  float strength = 1.f;
  photo::output_options encoding;
  std::vector<std::string> inputs;
  photo::focus_stack_options focus;
  bool strength_specified = false, focus_options_specified = false;
};

photo::result<photo::output_options> output_encoding(const std::string &path) {
  const auto extension = std::filesystem::path(path).extension().string();
  if (extension == ".tif" || extension == ".tiff")
    return photo::output_options{};
  if (extension == ".exr")
    return photo::output_options{photo::output_format::exr32};
  if (extension == ".jpg" || extension == ".jpeg")
    return photo::output_options{photo::output_format::jpeg};

  return photo::fail(photo::error_code::invalid_argument, "parse output",
                     "output must end in .tif, .tiff, .exr, .jpg or .jpeg", path);
}

photo::result<arguments> parse_arguments(std::span<char *> tokens) {
  if (tokens.size() < 2)
    return photo::fail(photo::error_code::invalid_argument, "parse arguments",
                       "expected inspect/convert and input; see --help");

  arguments parsed{};
  parsed.command = tokens[0];
  std::size_t first_option = 2;
  if (std::string_view(tokens[1]) == "--matrix") {
    if (parsed.command != "focus-stack")
      return photo::fail(photo::error_code::invalid_argument, "parse arguments",
                         "--matrix requires focus-stack");
    if (tokens.size() < 3 || std::string_view(tokens[2]).starts_with('-'))
      return photo::fail(photo::error_code::invalid_argument, "parse arguments",
                         "--matrix requires a JSON manifest path");
    parsed.matrix_manifest = tokens[2];
    first_option = 3;
  } else {
    parsed.input = tokens[1];
    parsed.inputs.push_back(parsed.input);
  }
  if (parsed.command != "inspect" && parsed.command != "convert" &&
      parsed.command != "sunlight-enhance" && parsed.command != "focus-stack" &&
      parsed.command != "hdr")
    return photo::fail(photo::error_code::invalid_argument, "parse arguments",
                       "unknown command: " + parsed.command);

  for (std::size_t index = first_option; index < tokens.size(); ++index) {
    const std::string_view option = tokens[index];
    if (option == "--half-size") {
      parsed.half_size = true;
    } else if (option == "-o" && index + 1 < tokens.size() && parsed.output.empty()) {
      parsed.output = tokens[++index];
    } else if (option == "--diagnostics" && index + 1 < tokens.size()) {
      parsed.diagnostics = tokens[++index];
      parsed.focus.diagnostics_directory = parsed.diagnostics;
    } else if (option == "--strength" && index + 1 < tokens.size()) {
      parsed.strength_specified = true;
      const std::string_view value = tokens[++index];
      const auto [end, status] =
          std::from_chars(value.data(), value.data() + value.size(), parsed.strength);
      if (status != std::errc{} || end != value.data() + value.size() ||
          !std::isfinite(parsed.strength) || parsed.strength < 0 || parsed.strength > 2)
        return photo::fail(photo::error_code::invalid_argument, "parse strength",
                           "strength must be a finite number in [0,2]");
    } else if (option == "--aligned") {
      parsed.focus.already_aligned = true;
      parsed.focus_options_specified = true;
    } else if (option == "--scratch-directory" && index + 1 < tokens.size()) {
      parsed.focus.scratch_directory = tokens[++index];
      parsed.focus_options_specified = true;
    } else if ((option == "--memory-budget-mib" || option == "--tile-size" ||
                option == "--proxy-long-edge" || option == "--reference") &&
               index + 1 < tokens.size()) {
      std::size_t value = 0;
      const std::string_view text = tokens[++index];
      const auto [end, status] = std::from_chars(text.data(), text.data() + text.size(), value);
      if (status != std::errc{} || end != text.data() + text.size())
        return photo::fail(photo::error_code::invalid_argument, "parse arguments",
                           "expected a nonnegative integer for " + std::string(option));
      if (option == "--memory-budget-mib")
        parsed.focus.memory_budget_mib = value;
      if (option == "--tile-size")
        parsed.focus.tile_size = value;
      if (option == "--proxy-long-edge")
        parsed.focus.proxy_long_edge = value;
      if (option == "--reference")
        parsed.focus.reference_index = value;
      parsed.focus_options_specified = true;
    } else if ((parsed.command == "focus-stack" || parsed.command == "hdr") &&
               !option.starts_with('-')) {
      if (!parsed.matrix_manifest.empty())
        return photo::fail(photo::error_code::invalid_argument, "parse arguments",
                           "matrix manifest cannot be mixed with positional frames");
      parsed.inputs.emplace_back(option);
    } else {
      return photo::fail(photo::error_code::invalid_argument, "parse arguments",
                         "invalid argument: " + std::string(option));
    }
  }

  if (parsed.command != "inspect" && parsed.output.empty())
    return photo::fail(photo::error_code::invalid_argument, "parse arguments",
                       "processing requires -o OUTPUT");
  if (parsed.command == "inspect" && !parsed.output.empty())
    return photo::fail(photo::error_code::invalid_argument, "parse arguments",
                       "inspect does not take -o");

  if (parsed.strength_specified && parsed.command != "sunlight-enhance")
    return photo::fail(photo::error_code::invalid_argument, "parse arguments",
                       "--strength requires sunlight-enhance");
  if (!parsed.diagnostics.empty() && parsed.command != "sunlight-enhance" &&
      parsed.command != "focus-stack" && parsed.command != "hdr")
    return photo::fail(photo::error_code::invalid_argument, "parse arguments",
                       "--diagnostics requires sunlight-enhance, focus-stack or hdr");
  if (parsed.focus_options_specified && parsed.command != "focus-stack" &&
      parsed.command != "hdr")
    return photo::fail(photo::error_code::invalid_argument, "parse arguments",
                       "bracket options require focus-stack or hdr");
  if ((parsed.command == "focus-stack" || parsed.command == "hdr") &&
      parsed.matrix_manifest.empty() && (parsed.inputs.size() < 2 || parsed.half_size))
    return photo::fail(photo::error_code::invalid_argument, "parse arguments",
                       "bracket requires at least two frames; --half-size is single-RAW-only");

  if (!parsed.matrix_manifest.empty() && parsed.half_size)
    return photo::fail(photo::error_code::invalid_argument, "parse arguments",
                       "matrix output retains full resolution; --half-size is single-RAW-only");

  if (!parsed.output.empty()) {
    auto encoding = output_encoding(parsed.output);
    if (!encoding)
      return std::unexpected(encoding.error());
    parsed.encoding = *encoding;

    std::error_code status;
    const bool exists = std::filesystem::exists(parsed.output, status);
    if (status || exists)
      return photo::fail(photo::error_code::io, "validate output",
                         status ? status.message() : "output already exists", parsed.output);
  }
  return parsed;
}

photo::result<photo::focus_exposure_matrix> read_matrix_manifest(const std::string &path) {
  const auto error = [&](const std::string &message) {
    return photo::fail(photo::error_code::invalid_argument, "matrix manifest", message, path);
  };
  std::error_code ec;
  const auto bytes = std::filesystem::file_size(path, ec);
  if (ec || !bytes || bytes > 1024 * 1024)
    return error("requires a nonempty JSON file <=1 MiB");
  std::ifstream stream(path, std::ios::binary);
  std::string text(bytes, '\0');
  if (!stream.read(text.data(), text.size()) || stream.peek() != std::char_traits<char>::eof())
    return error("cannot read stable complete manifest");
  Json::CharReaderBuilder builder;
  builder["allowComments"] = false;
  builder["collectComments"] = false;
  builder["allowTrailingCommas"] = false;
  builder["strictRoot"] = true;
  builder["failIfExtra"] = true;
  builder["rejectDupKeys"] = true;
  builder["allowDroppedNullPlaceholders"] = false;
  builder["allowNumericKeys"] = false;
  builder["allowSpecialFloats"] = false;
  builder["stackLimit"] = 64;
  Json::Value root;
  std::string errors;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  try {
    if (!reader->parse(text.data(), text.data() + text.size(), &root, &errors))
      return error(errors);
  } catch (const Json::Exception &failure) {
    return error(failure.what());
  }
  if (!root.isObject() || !root.isMember("focus_planes") || !root["focus_planes"].isArray())
    return error("root must contain focus_planes array");
  for (const auto &key : root.getMemberNames())
    if (key != "focus_planes" && key != "version")
      return error("unknown root field: " + key);
  if (root.isMember("version") &&
      ((root["version"].type() != Json::intValue &&
        root["version"].type() != Json::uintValue) ||
       (!root["version"].isUInt64() || root["version"].asUInt64() != 1)))
    return error("unsupported matrix schema version");
  if (root["focus_planes"].size() < 2 || root["focus_planes"].size() > 32)
    return error("requires 2..32 explicit focus planes");
  photo::focus_exposure_matrix matrix;
  const auto directory =
      std::filesystem::canonical(std::filesystem::path(path), ec).parent_path();
  if (ec)
    return error("cannot resolve manifest directory");
  for (const auto &plane : root["focus_planes"]) {
    if (!plane.isObject() || !plane.isMember("name") || !plane["name"].isString() ||
        !plane.isMember("exposures") || !plane["exposures"].isArray())
      return error("each plane needs name and exposures");
    for (const auto &key : plane.getMemberNames())
      if (key != "name" && key != "exposures")
        return error("unknown plane field: " + key);
    photo::focus_exposure_plane group{plane["name"].asString(), {}};
    if (plane["exposures"].size() < 2 || plane["exposures"].size() > 16)
      return error("each plane needs 2..16 exposure paths");
    for (const auto &input : plane["exposures"]) {
      if (!input.isString() || input.asString().empty() || input.asString().size() > 4096)
        return error("exposure entries must be nonempty paths");
      const auto value = input.asString();
      if (std::ranges::any_of(value, [](unsigned char c) { return c < 32 || c == 127; }))
        return error("exposure paths cannot contain control characters");
      std::filesystem::path source(value);
      if (source.is_relative())
        source = directory / source;
      group.exposures.push_back(source.lexically_normal().string());
    }
    matrix.focus_planes.push_back(std::move(group));
  }
  return matrix;
}

void print_statistics(const photo::decoded_image &decoded) {
  const auto stats = photo::statistics(decoded.pixels.view());
  std::cout << "size: " << decoded.pixels.size().width << 'x' << decoded.pixels.size().height
            << "\nworking: float32 linear sRGB/D65\ncolor: " << decoded.color_note
            << "\nmean_luminance: " << stats.mean_luminance
            << "\nchannel_range: " << stats.min_channel << " .. " << stats.max_channel
            << "\nnon_finite_pixels: " << stats.non_finite_pixels
            << "\noutside_unit_pixels: " << stats.outside_unit_pixels << '\n';
}

photo::result<void> run(const arguments &options) {
  if (options.command == "hdr") {
    photo::hdr_options settings;
    settings.memory_budget_mib = options.focus.memory_budget_mib;
    settings.tile_size = options.focus.tile_size;
    settings.proxy_long_edge = options.focus.proxy_long_edge;
    settings.reference_index = options.focus.reference_index;
    settings.scratch_directory = options.focus.scratch_directory;
    settings.diagnostics_directory = options.diagnostics;
    settings.already_aligned = options.focus.already_aligned;
    auto merged = photo::merge_hdr(options.inputs, options.output, settings);
    if (!merged)
      return std::unexpected(merged.error());
    std::cout << "written: " << options.output << "\nworking: scene-linear Rec.2020/D65"
              << "\nreference_index: " << merged->reference_index
              << "\nraw_context: " << merged->raw_context->fingerprint()
              << "\nestimated_peak_bytes: " << merged->estimated_peak_bytes
              << "\ndeghost_fraction: " << merged->deghost_fraction
              << "\nclipped_fraction: " << merged->clipped_fraction
              << "\nlow_snr_fraction: " << merged->low_snr_fraction
              << "\nchannel_range: " << merged->minimum << " .. " << merged->maximum << '\n';
    return {};
  }
  if (options.command == "focus-stack") {
    photo::result<photo::focus_stack_report> stacked;
    if (options.matrix_manifest.empty())
      stacked = photo::stack_focus(options.inputs, options.output, options.focus);
    else {
      auto matrix = read_matrix_manifest(options.matrix_manifest);
      if (!matrix)
        return std::unexpected(matrix.error());
      stacked = photo::stack_focus(*matrix, options.output, options.focus);
    }
    if (!stacked)
      return std::unexpected(stacked.error());
    std::cout << "written: " << options.output
              << "\nreference_index: " << stacked->reference_index
              << "\nestimated_peak_bytes: " << stacked->estimated_peak_bytes
              << "\nmotion_fraction: " << stacked->motion_fraction
              << "\ndefocus_boundary_fraction: " << stacked->defocus_boundary_fraction
              << "\nlow_confidence_fraction: " << stacked->low_confidence_fraction << '\n';
    std::cout << "working_space: "
              << (stacked->raw_context ? "scene-linear Rec.2020/D65"
                                       : "rendered linear sRGB/D65")
              << "\noutput_encoding: "
              << (stacked->raw_context ? "float32 linear ICC TIFF (signed/HDR retained)"
                                       : "16-bit sRGB ICC TIFF")
              << '\n';
    if (stacked->raw_context)
      std::cout << "raw_context: " << stacked->raw_context->fingerprint() << '\n';
    if (!stacked->hdr_planes.empty())
      std::cout << "hdr_focus_planes: " << stacked->hdr_planes.size()
                << "\nhdr_seconds: " << stacked->profile.hdr_seconds
                << "\nworking_range: " << stacked->working_minimum << " .. "
                << stacked->working_maximum << '\n';
    return {};
  }
  auto decoded = photo::read_image(options.input, options.half_size);
  if (!decoded && options.command == "inspect") {
    auto extension = std::filesystem::path(options.input).extension().string();
    std::ranges::transform(extension, extension.begin(),
                           [](unsigned char c) { return std::tolower(c); });
    if (extension == ".tif" || extension == ".tiff") {
      auto working = photo::read_working_tiff(options.input);
      if (working) {
        float lo = std::numeric_limits<float>::infinity(), hi = -lo;
        for (std::size_t y = 0; y < working->pixels.size().height; ++y)
          for (auto p : working->pixels.view().row(y)) {
            lo = std::min({lo, p.r, p.g, p.b});
            hi = std::max({hi, p.r, p.g, p.b});
          }
        std::cout << "size: " << working->pixels.size().width << 'x'
                  << working->pixels.size().height << "\nworking: float32 linear "
                  << (working->encoding.primaries == photo::rgb_primaries::rec2020 ? "Rec.2020"
                                                                                   : "sRGB")
                  << "/D65\nnormalization: " << working->encoding.normalization
                  << "\nchannel_range: " << lo << " .. " << hi << '\n';
        return {};
      }
      auto scalar = photo::read_scalar_tiff(options.input);
      if (scalar) {
        float lo = std::numeric_limits<float>::infinity(), hi = -lo;
        for (std::size_t y = 0; y < scalar->pixels.size().height; ++y)
          for (auto p : scalar->pixels.view().row(y)) {
            lo = std::min(lo, p.value);
            hi = std::max(hi, p.value);
          }
        std::cout << "size: " << scalar->pixels.size().width << 'x'
                  << scalar->pixels.size().height << "\nmeasurement: " << scalar->meaning
                  << "\nscalar_range: " << lo << " .. " << hi << '\n';
        return {};
      }
    }
  }
  if (!decoded)
    return std::unexpected(decoded.error());
  print_statistics(*decoded);

  if (options.command == "sunlight-enhance") {
    auto enhanced = photo::enhance_sunlight(decoded->pixels.view(), {options.strength});
    if (!enhanced)
      return std::unexpected(enhanced.error());
    std::cout << "sunlight_applied: " << enhanced->report.applied
              << "\nconfidence: " << enhanced->report.confidence
              << "\nselected_area_fraction: " << enhanced->report.selected_area_fraction
              << '\n';

    if (!options.diagnostics.empty()) {
      auto diagnostics = photo::write_sunlight_diagnostics(options.diagnostics,
                                                           decoded->pixels.view(), *enhanced);
      if (!diagnostics)
        return diagnostics;
    }
    return photo::write_image(options.output, enhanced->pixels.view(), options.encoding);
  }

  if (!options.output.empty()) {
    auto written = photo::write_image(options.output, decoded->pixels.view(), options.encoding);
    if (!written)
      return written;
    std::cout << "written: " << options.output << '\n';
  }
  return {};
}

void print_help() {
  std::cout
      << "photo-ops inspect INPUT [--half-size]\n"
         "photo-ops convert INPUT -o OUTPUT [--half-size]\n"
         "photo-ops sunlight-enhance INPUT -o OUTPUT [--strength 0..2]\n"
         "                           [--half-size] [--diagnostics DIRECTORY]\n"
         "photo-ops focus-stack FRAME1 FRAME2 ... -o stacked.tif\n"
         "                      [--aligned] [--reference INDEX] [--diagnostics DIRECTORY]\n"
         "                      [--memory-budget-mib 2048] [--scratch-directory DIRECTORY]\n"
         "                      [--tile-size 256] [--proxy-long-edge 1024]\n"
         "photo-ops focus-stack --matrix capture.json -o stacked-hdr.tif\n"
         "                      [same focus-stack options; --reference selects a focus plane]\n"
         "photo-ops hdr EXPOSURE1 EXPOSURE2 ... -o radiance.tif\n"
         "              [--aligned] [--reference INDEX] [--diagnostics DIRECTORY]\n"
         "              [--memory-budget-mib 2048] [--scratch-directory DIRECTORY]\n"
         "              [--tile-size 256] [--proxy-long-edge 1024]\n"
         "HDR: shutter-only RAW AEB -> signed float32 linear Rec.2020 TIFF, no tone map.\n"
         "Focus stacking: compatible CR3/Bayer DNG -> float32 linear Rec.2020 ICC TIFF.\n"
         "               Consistently developed TIFF/JPEG -> 16-bit sRGB TIFF.\n"
         "Other inputs: CR3, RGB TIFF, JPEG. Outputs: 16-bit TIFF, EXR, JPEG.\n"
         "EXR enabled: "
      << (photo::exr_available() ? "yes" : "no") << '\n';
}
} // namespace

int main(int argc, char **argv) {
  try {
    if (argc == 1 || (argc == 2 && std::string_view(argv[1]) == "--help")) {
      print_help();
      return 0;
    }

    const auto status =
        parse_arguments({argv + 1, static_cast<std::size_t>(argc - 1)}).and_then(run);
    if (!status) {
      const auto &failure = status.error();
      std::cerr << "photo-ops: " << failure.operation << ": " << failure.message;
      if (!failure.path.empty())
        std::cerr << " [" << failure.path << ']';
      std::cerr << '\n';
      return 1;
    }
    return 0;
  } catch (const std::exception &exception) {
    // Last-resort boundary for allocation failures and unexpected third-party exceptions.
    std::cerr << "photo-ops: unexpected failure: " << exception.what() << '\n';
    return 1;
  }
}
