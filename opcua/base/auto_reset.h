#pragma once

#include <utility>

namespace opcua {
namespace base {

template <typename T>
class AutoReset {
 public:
  AutoReset(T* target, T new_value)
      : target_(target), old_value_(std::move(*target)) {
    *target_ = std::move(new_value);
  }

  ~AutoReset() { *target_ = std::move(old_value_); }

  AutoReset(const AutoReset&) = delete;
  AutoReset& operator=(const AutoReset&) = delete;

 private:
  T* target_;
  T old_value_;
};

}  // namespace base
}  // namespace opcua (vendored)
