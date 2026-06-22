#pragma once

#include "opcua/base/time/time.h"

namespace opcua {
namespace base {

// Overrides opcua::base::Time::Now() in tests. When constructed, sets a fixed
// time point. Call Advance() to move time forward.
//
// NOTE: This uses a thread-local override. Only one instance should be active
// at a time per thread.
class ScopedMockClockOverride {
 public:
  ScopedMockClockOverride() : now_{Time::Now()} { current_ = this; }

  ~ScopedMockClockOverride() { current_ = nullptr; }

  void Advance(TimeDelta delta) { now_ += delta; }

  Time Now() const { return now_; }

  static ScopedMockClockOverride* current() { return current_; }

 private:
  Time now_;
  inline static thread_local ScopedMockClockOverride* current_ = nullptr;
};

}  // namespace base
}  // namespace opcua
