// SPDX-License-Identifier: GPL-3.0-or-later
#include <charconv>
#include <cmath>
#include <expected>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>
import photo_ops.image;
import photo_ops.io;
import photo_ops.diagnostics;
import photo_ops.sunlight;
import photo_ops.focus_stack;

namespace {
namespace photo = photo_ops;

struct arguments {
  std::string command;
  std::string input;
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
  parsed.input = tokens[1];
  parsed.inputs.push_back(parsed.input);
  if (parsed.command != "inspect" && parsed.command != "convert" &&
      parsed.command != "sunlight-enhance" && parsed.command != "focus-stack")
    return photo::fail(photo::error_code::invalid_argument, "parse arguments",
                       "unknown command: " + parsed.command);

  for (std::size_t index = 2; index < tokens.size(); ++index) {
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
    } else if (parsed.command == "focus-stack" && !option.starts_with('-')) {
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
      parsed.command != "focus-stack")
    return photo::fail(photo::error_code::invalid_argument, "parse arguments",
                       "--diagnostics requires sunlight-enhance or focus-stack");
  if (parsed.focus_options_specified && parsed.command != "focus-stack")
    return photo::fail(photo::error_code::invalid_argument, "parse arguments",
                       "focus-stack options require focus-stack");
  if (parsed.command == "focus-stack" && (parsed.inputs.size() < 2 || parsed.half_size))
    return photo::fail(
        photo::error_code::invalid_argument, "parse arguments",
        "focus-stack requires at least two developed frames; --half-size is RAW-only");

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
  if (options.command == "focus-stack") {
    auto stacked = photo::stack_focus(options.inputs, options.output, options.focus);
    if (!stacked)
      return std::unexpected(stacked.error());
    std::cout << "written: " << options.output
              << "\nreference_index: " << stacked->reference_index
              << "\nestimated_peak_bytes: " << stacked->estimated_peak_bytes
              << "\nmotion_fraction: " << stacked->motion_fraction
              << "\nlow_confidence_fraction: " << stacked->low_confidence_fraction << '\n';
    return {};
  }
  auto decoded = photo::read_image(options.input, options.half_size);
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
         "Focus stacking: consistently developed TIFF/JPEG -> 16-bit TIFF; RAW pending.\n"
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
