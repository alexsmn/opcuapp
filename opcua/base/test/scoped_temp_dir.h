#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace opcua::test {

// A temp directory owned by one test fixture, removed when the fixture is.
//
// Prefer this to naming a directory by hand. The shape it replaces looked
// salted and was not: the trust-store fixture named its directory after
// `reinterpret_cast<std::uintptr_t>(this)`, the fixture's own address. That
// separates fixtures alive at the same moment inside one process, and does
// nothing at all about a second process -- which is the case that matters,
// since `ctest -j` and two checkouts running tests at once are both exactly
// that. It also ignored what `create_directories` returned, and an
// already-existing directory is reported through that return rather than by a
// failure, so a collision would have been silent: two fixtures writing
// certificates into one trust store, surfacing as an unexplained verification
// failure somewhere else.
//
// The PID separates concurrent processes, and *checking what the create call
// returned* separates fixtures within one, by walking the suffix until it wins
// a directory it actually created. Reading the result is the whole defence --
// `create_directory` and `create_directories` behave identically on an
// existing directory, so the choice of function is not what makes it safe.
//
// Declare it before any member that opens a file inside it: members are
// destroyed in reverse declaration order, and removing a tree out from under an
// open handle fails on Windows and silently leaves it behind.
//
// The tree carries this class four times over -- here, in
// `scada-server-framework/test/`, in `client/test/` and in `designer/test/`.
// Every product must build on its own (ADR 0011) and opcuapp consumes only
// `net`, so it cannot borrow any of the others. Change one, change them all.
class ScopedTempDir {
 public:
  explicit ScopedTempDir(std::string_view prefix = "opcua_test") {
    const std::filesystem::path base = std::filesystem::temp_directory_path();
    const std::string stem =
        std::string{prefix} + "_" + std::to_string(CurrentPid()) + "_";
    for (int attempt = 0; attempt < 1000; ++attempt) {
      std::filesystem::path candidate = base / (stem + std::to_string(attempt));
      std::error_code ec;
      if (std::filesystem::create_directory(candidate, ec)) {
        path_ = std::move(candidate);
        return;
      }
    }
    throw std::runtime_error{"Could not create a unique temp directory under " +
                             base.string()};
  }

  ScopedTempDir(const ScopedTempDir&) = delete;
  ScopedTempDir& operator=(const ScopedTempDir&) = delete;

  ~ScopedTempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  const std::filesystem::path& path() const { return path_; }

 private:
  static int CurrentPid() {
#ifdef _WIN32
    return ::_getpid();
#else
    return static_cast<int>(::getpid());
#endif
  }

  std::filesystem::path path_;
};

}  // namespace opcua::test
