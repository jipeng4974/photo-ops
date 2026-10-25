// SPDX-License-Identifier: GPL-3.0-or-later
module;
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <linux/fs.h>
#include <string>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>
module photo_ops.output;

namespace photo_ops {
atomic_output::atomic_output(std::string temporary, std::string destination,
                             int descriptor) noexcept
    : temporary_(std::move(temporary)), destination_(std::move(destination)),
      descriptor_(descriptor) {
}

atomic_output::atomic_output(atomic_output &&other) noexcept
    : temporary_(std::exchange(other.temporary_, {})),
      destination_(std::move(other.destination_)),
      descriptor_(std::exchange(other.descriptor_, -1)) {
}

atomic_output::~atomic_output() {
  if (descriptor_ >= 0)
    ::close(descriptor_);
  if (!temporary_.empty())
    ::unlink(temporary_.c_str());
}

result<atomic_output> atomic_output::create(const std::string &destination) {
  // Allocate names before acquiring a resource. mkstemp reserves the name exclusively.
  std::string target = destination;
  std::string temporary = destination + ".tmp-XXXXXX";
  const int descriptor = ::mkstemp(temporary.data());
  if (descriptor < 0)
    return fail(error_code::io, "create atomic output", std::strerror(errno), destination);
  return atomic_output(std::move(temporary), std::move(target), descriptor);
}

const std::string &atomic_output::temporary_path() const noexcept {
  return temporary_;
}

result<void> atomic_output::commit() {
  if (descriptor_ < 0)
    return fail(error_code::invalid_argument, "publish output", "output is already closed",
                destination_);
  if (::fsync(descriptor_) != 0)
    return fail(error_code::io, "sync output", std::strerror(errno), destination_);
  const int descriptor = std::exchange(descriptor_, -1);
  if (::close(descriptor) != 0)
    return fail(error_code::io, "close output", std::strerror(errno), destination_);
  // No exists()+rename() race and no fallback that can clobber another writer's output.
  if (::syscall(SYS_renameat2, AT_FDCWD, temporary_.c_str(), AT_FDCWD, destination_.c_str(),
                RENAME_NOREPLACE) != 0)
    return fail(error_code::io, "publish output", std::strerror(errno), destination_);
  temporary_.clear();
  return {};
}
} // namespace photo_ops
