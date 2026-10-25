// SPDX-License-Identifier: GPL-3.0-or-later
#include <charconv>
#include <cmath>
#include <expected>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
import photo_ops.image;
import photo_ops.io;
import photo_ops.diagnostics;
import photo_ops.sunlight;

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
  if (parsed.command != "inspect" && parsed.command != "convert" &&
      parsed.command != "sunlight-enhance")
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
    } else if (option == "--strength" && index + 1 < tokens.size()) {
      const std::string_view value = tokens[++index];
      const auto [end, status] =
          std::from_chars(value.data(), value.data() + value.size(), parsed.strength);
      if (status != std::errc{} || end != value.data() + value.size() ||
          !std::isfinite(parsed.strength) || parsed.strength < 0 || parsed.strength > 2)
        return photo::fail(photo::error_code::invalid_argument, "parse strength",
                           "strength must be a finite number in [0,2]");
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

  if (parsed.command != "sunlight-enhance" &&
      (!parsed.diagnostics.empty() || parsed.strength != 1.f))
    return photo::fail(photo::error_code::invalid_argument, "parse arguments",
                       "--diagnostics and --strength require sunlight-enhance");

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
  std::cout << "photo-ops inspect INPUT [--half-size]\n"
               "photo-ops convert INPUT -o OUTPUT [--half-size]\n"
               "photo-ops sunlight-enhance INPUT -o OUTPUT [--strength 0..2]\n"
               "                           [--half-size] [--diagnostics DIRECTORY]\n"
               "Inputs: CR3, RGB TIFF, JPEG. Outputs: 16-bit TIFF, EXR, JPEG.\n"
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
