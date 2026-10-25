// SPDX-License-Identifier: GPL-3.0-or-later
module;
#include <expected>
#include <string>
#include <utility>
export module photo_ops.error;

export namespace photo_ops {
/// Recoverable failure categories. Contract violations are programming errors, not results.
enum class error_code {
  invalid_argument,
  unsupported_format,
  io,
  decode,
  encode,
  color,
  unavailable,
};

/// An actionable failure with the operation and, when available, the affected file.
struct error {
  error_code code;
  std::string operation;
  std::string message;
  std::string path;
};

/// A value or a recoverable failure; allocation failures may still throw std::bad_alloc.
/// Pinned libstdc++ marks expected itself nodiscard, including expected<void, E>.
/// Keep the alias: producers and monadic operations retain the type-level diagnostic.
template <class T> using result = std::expected<T, error>;

/// Construct a failure without exceptions or an implicit success value.
std::unexpected<error> fail(error_code code, std::string operation, std::string message,
                            std::string path = {}) {
  return std::unexpected(
      error{code, std::move(operation), std::move(message), std::move(path)});
}
} // namespace photo_ops
