#pragma once

// Standalone re-implementation of Chromium's base::TimeTicks: a monotonic
// clock counting microseconds since an arbitrary origin.

#include "opcua/base/time_base.h"

namespace opcua {
namespace base {

class TimeTicks : public time_internal::TimeBase<TimeTicks> {
 public:
  constexpr TimeTicks() : TimeBase(0) {}

  static TimeTicks Now();

  static constexpr TimeTicks FromInternalValue(int64_t us) {
    return TimeTicks(us);
  }

 private:
  friend class time_internal::TimeBase<TimeTicks>;

  constexpr explicit TimeTicks(int64_t us) : TimeBase(us) {}
};

std::ostream& operator<<(std::ostream& os, TimeTicks time_ticks);

}  // namespace base
}  // namespace opcua
