#pragma once

namespace opcua::detail {

template <typename T>
class ObjectWrapper {
 public:
  ObjectWrapper() { Initialize(value_); }
  ~ObjectWrapper() { Clear(value_); }

  ObjectWrapper(const ObjectWrapper&) = delete;
  ObjectWrapper& operator=(const ObjectWrapper&) = delete;

  ObjectWrapper(ObjectWrapper&& source) {
    value_ = source.value_;
    Initialize(source.value_);
  }

  ObjectWrapper& operator=(ObjectWrapper&& source) {
    if (&value_ != &source.value_) {
      value_ = source.value_;
      Initialize(source.value_);
    }
    return *this;
  }

  T& get() { return value_; }
  const T& get() const { return value_; }

 private:
  T value_;
};

}  // namespace opcua::detail
