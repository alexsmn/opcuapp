#pragma once

#include "opcua/base/debug_holder.h"

#include <memory>
#include <source_location>
#include <stop_token>

namespace opcua {
namespace internal {

template <class C, class T>
struct CancelationWrapper {
  template <class... Args>
  void operator()(Args&&... args) const& {
    if (auto ref = cancelation_.lock()) {
      std::invoke(task_, std::forward<Args>(args)...);
    }
  }

  template <class... Args>
  void operator()(Args&&... args) & {
    if (auto ref = cancelation_.lock()) {
      std::invoke(task_, std::forward<Args>(args)...);
    }
  }

  template <class... Args>
  void operator()(Args&&... args) && {
    if (auto ref = cancelation_.lock()) {
      std::invoke(std::move(task_), std::forward<Args>(args)...);
    }
  }

  const std::weak_ptr<C> cancelation_;
  T task_;
  const DebugHolder<std::source_location> location_;
};

template <class C, class F, class CF>
struct CancelationFuncWrapper {
  template <class... Args>
  auto operator()(Args&&... args) const {
    if (auto ref = cancelation_.lock())
      return func_(std::forward<Args>(args)...);
    else
      return canceled_func_(std::forward<Args>(args)...);
  }

  template <class... Args>
  auto operator()(Args&&... args) {
    if (auto ref = cancelation_.lock())
      return func_(std::forward<Args>(args)...);
    else
      return canceled_func_(std::forward<Args>(args)...);
  }

  const std::weak_ptr<C> cancelation_;
  F func_;
  CF canceled_func_;
  const DebugHolder<std::source_location> location_;
};

template <class T>
struct StopTokenWrapper {
  template <class... Args>
  void operator()(Args&&... args) const {
    if (!stop_token_.stop_requested()) {
      task_(std::forward<Args>(args)...);
    }
  }

  template <class... Args>
  void operator()(Args&&... args) {
    if (!stop_token_.stop_requested()) {
      task_(std::forward<Args>(args)...);
    }
  }

  const std::stop_token stop_token_;
  T task_;
};

}  // namespace internal

template <class C, class T>
inline auto BindCancelation(
    std::weak_ptr<C> cancelation,
    T&& task,
    const std::source_location& location = std::source_location::current()) {
  return internal::CancelationWrapper<C, T>{
      std::move(cancelation), std::forward<T>(task), DebugHolder{location}};
}

template <class C, class F, class CF>
inline auto BindCancelationFunc(
    std::weak_ptr<C> cancelation,
    F&& func,
    CF&& canceled_func,
    const std::source_location& location = std::source_location::current()) {
  return internal::CancelationFuncWrapper<C, F, CF>{
      std::move(cancelation), std::forward<F>(func),
      std::forward<CF>(canceled_func), DebugHolder{location}};
}

template <class T>
inline auto BindStopToken(std::stop_token stop_token, T&& task) {
  return internal::StopTokenWrapper<T>{std::move(stop_token),
                                       std::forward<T>(task)};
}

class Cancelation;

class CancelationRef {
 public:
  explicit CancelationRef(const Cancelation& cancelation) noexcept;

  bool canceled() const { return weak_ptr_.expired(); }

  template <class T>
  static CancelationRef FromWeakPtr(const std::weak_ptr<T>& weak_ptr) {
    return CancelationRef{weak_ptr};
  }

  template <class T>
  static CancelationRef FromSharedPtr(const std::shared_ptr<T>& shared_ptr) {
    return CancelationRef{std::weak_ptr<T>{shared_ptr}};
  }

 private:
  explicit CancelationRef(const std::weak_ptr<void>& weak_ptr) noexcept
      : weak_ptr_{weak_ptr} {}

  std::weak_ptr<void> weak_ptr_;
};

class Cancelation {
 public:
  CancelationRef ref() const { return CancelationRef{*this}; }

  std::weak_ptr<void> weak_ptr() const { return shared_ptr_; }

  template <class T>
  auto Bind(T&& task) const {
    return BindCancelation(std::weak_ptr<void>{shared_ptr_},
                           std::forward<T>(task));
  }

  void Cancel() { shared_ptr_ = std::make_shared<bool>(); }

 private:
  std::shared_ptr<void> shared_ptr_ = std::make_shared<bool>();

  friend class CancelationRef;
};

// Requires `Cancelation` to be defined.
inline CancelationRef::CancelationRef(const Cancelation& cancelation) noexcept
    : weak_ptr_{cancelation.shared_ptr_} {}

template <class T>
inline auto BindCancelation(const Cancelation& cancelation, T&& task) {
  return cancelation.Bind(std::forward<T>(task));
}
}  // namespace opcua
