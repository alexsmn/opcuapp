#pragma once

#include <memory>

// Move operator is also defined, to enable `NodeId` move.
namespace opcua {
// opcuapp/SCADA-specific wrapper holding an immutable value behind a shared
// pointer, so copies share storage while comparing by value. It is an internal
// utility, not a standard OPC UA type.
template <class T>
class SharedValue {
 public:
  template <class U>
  explicit SharedValue(U&& value)
      : value_{std::make_shared<T>(std::forward<U>(value))} {}

  const T& get() const { return *value_; }

  // Copies of the same value share one control block, so an identical pointer
  // means equal without dereferencing — the common case for a NodeId that has
  // been copied around. Distinct pointers fall back to a value compare.
  bool operator==(const SharedValue& other) const {
    return value_ == other.value_ || *value_ == *other.value_;
  }

  auto operator<=>(const SharedValue& other) const {
    using ordering = decltype(*value_ <=> *other.value_);
    if (value_ == other.value_)
      return ordering::equal;
    return *value_ <=> *other.value_;
  }

 private:
  std::shared_ptr<const T> value_;
};
}  // namespace opcua
