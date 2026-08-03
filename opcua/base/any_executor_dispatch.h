#pragma once

#include "opcua/base/any_executor.h"
#include "opcua/base/cancelation.h"
#include "opcua/base/debug_holder.h"

#include <boost/asio/post.hpp>
#include <functional>
#include <source_location>
#include <stop_token>

namespace opcua {
template <class T>
inline void Dispatch(
    const AnyExecutor& executor,
    T&& task,
    const std::source_location& location = std::source_location::current()) {
  boost::asio::post(executor, std::forward<T>(task));
}

template <class C, class T>
inline void Dispatch(
    const AnyExecutor& executor,
    std::weak_ptr<C> cancelation,
    T&& task,
    const std::source_location& location = std::source_location::current()) {
  Dispatch(executor,
           BindCancelation(std::move(cancelation), std::forward<T>(task)),
           location);
}

template <class T>
inline void Dispatch(
    const AnyExecutor& executor,
    const Cancelation& cancelation,
    T&& task,
    const std::source_location& location = std::source_location::current()) {
  Dispatch(executor, cancelation.Bind(std::forward<T>(task)), location);
}

namespace internal {

template <class F>
struct AnyExecutorBoundFunc {
  template <class U>
  AnyExecutorBoundFunc(AnyExecutor executor,
                       U&& func,
                       const std::source_location& location)
      : executor_{std::move(executor)},
        func_{std::forward<U>(func)},
        location_{location} {}

  template <class... Args>
  void operator()(Args&&... args) const {
    auto closure = [func = func_, args = std::make_tuple(
                                      std::forward<Args>(args)...)]() mutable {
      std::apply(std::move(func), args);
    };

    Dispatch(executor_, std::move(closure), location_.get());
  }

  const AnyExecutor executor_;

  // WARNING: The operator may be called for multiple times. The functor must
  // not be moved.
  const F func_;

  const DebugHolder<std::source_location> location_;
};

}  // namespace internal

template <class T>
inline auto BindExecutor(
    AnyExecutor executor,
    T&& task,
    const std::source_location& location = std::source_location::current()) {
  return internal::AnyExecutorBoundFunc<T>{std::move(executor),
                                           std::forward<T>(task), location};
}

template <class C, class T>
inline auto BindExecutor(
    AnyExecutor executor,
    std::weak_ptr<C> cancelation,
    T&& task,
    const std::source_location& location = std::source_location::current()) {
  return BindExecutor(
      std::move(executor),
      BindCancelation(std::move(cancelation), std::forward<T>(task)), location);
}

template <class T>
inline auto BindExecutor(
    AnyExecutor executor,
    const Cancelation& cancelation,
    T&& task,
    const std::source_location& location = std::source_location::current()) {
  return BindExecutor(std::move(executor),
                      cancelation.Bind(std::forward<T>(task)), location);
}

template <class T>
inline auto BindExecutor(
    AnyExecutor executor,
    std::stop_token stop_token,
    T&& task,
    const std::source_location& location = std::source_location::current()) {
  return BindExecutor(
      std::move(executor),
      BindStopToken(std::move(stop_token), std::forward<T>(task)), location);
}
}  // namespace opcua
