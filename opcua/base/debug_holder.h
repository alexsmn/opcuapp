#pragma once

namespace opcua {
#if defined(NDEBUG)

template <class T>
#if defined(_MSC_VER)
[[msvc::no_unique_address]]
#endif
class DebugHolder {
 public:
  explicit DebugHolder(const T&) {}

  T get() const { return {}; }
};

#else

template <class T>
class DebugHolder {
 public:
  explicit DebugHolder(const T& value) : value_{value} {}

  const T& get() const { return value_; }

 private:
  T value_;
};

#endif
}  // namespace opcua (vendored)
