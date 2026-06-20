#pragma once

#include "opcua/base/any_executor.h"
#include "opcua/base/awaitable.h"
#include "opcua/base/callback_awaitable.h"
#include "opcua/scada/status.h"
#include "opcua/scada/status_or.h"

#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace opcua::scada {

template <class Callback, class T>
void CompleteStatusOrCallback(Callback&& callback, StatusOr<T>&& result) {
  if (result.ok()) {
    std::forward<Callback>(callback)(Status{StatusCode::Good},
                                     std::move(*result));
  } else {
    std::forward<Callback>(callback)(result.status(), {});
  }
}

template <class T, class Start>
[[nodiscard]] Awaitable<T> AwaitCallbackValue(AnyExecutor executor,
                                              Start&& start) {
  auto [value] = co_await CallbackToAwaitable<T>(
      std::move(executor), std::forward<Start>(start));
  co_return std::move(value);
}

template <class... Args, class Start>
[[nodiscard]] Awaitable<std::tuple<std::decay_t<Args>...>> AwaitCallbackTuple(
    AnyExecutor executor,
    Start&& start) {
  co_return co_await CallbackToAwaitable<Args...>(
      std::move(executor), std::forward<Start>(start));
}

template <class Start>
[[nodiscard]] Awaitable<Status> AwaitStatusCallback(AnyExecutor executor,
                                                    Start&& start) {
  co_return co_await AwaitCallbackValue<Status>(std::move(executor),
                                                std::forward<Start>(start));
}

template <class Start>
[[nodiscard]] Awaitable<StatusOr<std::vector<StatusCode>>>
AwaitStatusCodesCallback(AnyExecutor executor, Start&& start) {
  auto [status, value] =
      co_await AwaitCallbackTuple<Status, std::vector<StatusCode>>(
      std::move(executor), std::forward<Start>(start));
  co_return status ? StatusOr<std::vector<StatusCode>>{std::move(value)}
                   : StatusOr<std::vector<StatusCode>>{std::move(status)};
}

template <class T, class Start>
[[nodiscard]] Awaitable<StatusOr<T>> AwaitStatusOrCallback(AnyExecutor executor,
                                                           Start&& start) {
  auto [status, value] = co_await AwaitCallbackTuple<Status, T>(
      std::move(executor), std::forward<Start>(start));
  co_return status ? StatusOr<T>{std::move(value)}
                   : StatusOr<T>{std::move(status)};
}

}  // namespace opcua::scada
