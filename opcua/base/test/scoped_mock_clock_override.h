#pragma once

#include "opcua/types/date_time.h"

namespace opcua {
namespace base {

// Overrides opcua::DateTime::Now() in tests. When constructed, sets a fixed
// time point. Call Advance() to move time forward.
//
// NOTE: This uses a thread-local override. Only one instance should be active
// at a time per thread.
class ScopedMockClockOverride {
 public:
  ScopedMockClockOverride() : now_{DateTime::Now()} { current_ = this; }

  ~ScopedMockClockOverride() { current_ = nullptr; }

  void Advance(Duration delta) { now_ += delta; }

  DateTime Now() const { return now_; }

  static ScopedMockClockOverride* current() { return current_; }

 private:
  DateTime now_;
  inline static thread_local ScopedMockClockOverride* current_ = nullptr;
};

}  // namespace base
}  // namespace opcua
