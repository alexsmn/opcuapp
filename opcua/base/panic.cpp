#include "opcua/base/panic.h"

#include <cstdio>
#include <cstdlib>

#if defined(WIN32)
#include <intrin.h>
#include <windows.h>
#endif

namespace opcua {
namespace base {

namespace {

// Borrowed from Chromium's immediate_crash.h approach: emit diagnostics first,
// then terminate with a non-continuable trap instead of relying on assert-only
// behavior in debug builds.
[[noreturn]] void ImmediateCrash() {
#if defined(WIN32)
  __fastfail(FAST_FAIL_FATAL_APP_EXIT);
#elif defined(__clang__) || defined(__GNUC__)
  __builtin_trap();
  __builtin_unreachable();
#else
  std::abort();
#endif
}

}  // namespace

[[noreturn]] void Panic(std::string_view message,
                        const std::source_location& location) {
  // Keep panic reporting on stderr. Boost.Log is too heavyweight for a panic
  // path because it may allocate, lock, or depend on partially torn down
  // logging infrastructure.
  std::fputs("Panic: ", stderr);
  std::fwrite(message.data(), 1, message.size(), stderr);
  std::fputc('\n', stderr);
  std::fprintf(stderr, "  at %s:%u:%u in %s\n", location.file_name(),
               location.line(), location.column(), location.function_name());
  std::fflush(stderr);
  ImmediateCrash();
}

}  // namespace base
}  // namespace opcua
