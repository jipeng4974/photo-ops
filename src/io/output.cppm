// SPDX-License-Identifier: GPL-3.0-or-later
module;
#include <string>
export module photo_ops.output;
export import photo_ops.error;

export namespace photo_ops {
/// A same-directory temporary file, published only after the encoder closes successfully.
/// Linux local filesystems: atomic visibility, no overwrite (including dangling symlinks).
/// Not a multi-file transaction or a guarantee of directory durability after power loss.
class atomic_output {
public:
  [[nodiscard]] static result<atomic_output> create(const std::string &destination);
  atomic_output(const atomic_output &) = delete;
  atomic_output &operator=(const atomic_output &) = delete;
  atomic_output(atomic_output &&other) noexcept;
  atomic_output &operator=(atomic_output &&) = delete;
  ~atomic_output();

  /// Borrow the reserved path for an encoder; close/check the encoder before commit().
  [[nodiscard]] const std::string &temporary_path() const noexcept;
  /// Sync file data and publish using renameat2(RENAME_NOREPLACE); errors retain the target.
  [[nodiscard]] result<void> commit();

private:
  atomic_output(std::string temporary, std::string destination, int descriptor) noexcept;
  std::string temporary_;
  std::string destination_;
  int descriptor_ = -1;
};
} // namespace photo_ops
