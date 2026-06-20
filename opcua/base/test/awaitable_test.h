#pragma once

#include "opcua/base/any_executor.h"

#include "opcua/base/awaitable.h"
#include "opcua/base/test/test_executor.h"

#include <chrono>
#include <exception>
#include <optional>
#include <thread>
#include <type_traits>

namespace opcua {
using namespace std::chrono_literals;

inline void Drain(TestExecutor& executor) {
  while (executor.HasReadyTasks()) {
    executor.Poll();
  }
}

template <class T>
struct AwaitableResult {
  std::optional<T> value;
  std::exception_ptr error;
  bool done = false;
};

template <>
struct AwaitableResult<void> {
  std::exception_ptr error;
  bool done = false;
};

template <class T>
std::shared_ptr<AwaitableResult<T>> StartAwaitable(
    TestExecutor executor,
    Awaitable<T> awaitable) {
  auto result = std::make_shared<AwaitableResult<T>>();
  CoSpawn(executor,
          [result, awaitable = std::move(awaitable)]() mutable
              -> Awaitable<void> {
            try {
              if constexpr (std::is_void_v<T>) {
                co_await std::move(awaitable);
              } else {
                result->value.emplace(co_await std::move(awaitable));
              }
            } catch (...) {
              result->error = std::current_exception();
            }
            result->done = true;
          });
  return result;
}

template <class T>
T WaitResult(TestExecutor executor,
             std::shared_ptr<AwaitableResult<T>> result) {
  while (!result->done) {
    Drain(executor);
    std::this_thread::yield();
  }
  if (result->error) {
    std::rethrow_exception(result->error);
  }
  return std::move(*result->value);
}

inline void WaitResult(TestExecutor executor,
                       std::shared_ptr<AwaitableResult<void>> result) {
  while (!result->done) {
    Drain(executor);
    std::this_thread::yield();
  }
  if (result->error) {
    std::rethrow_exception(result->error);
  }
}

template <class T>
T WaitAwaitable(TestExecutor executor, Awaitable<T> awaitable) {
  return WaitResult(executor, StartAwaitable(executor, std::move(awaitable)));
}

inline void WaitAwaitable(TestExecutor executor,
                          Awaitable<void> awaitable) {
  WaitResult(executor, StartAwaitable(executor, std::move(awaitable)));
}
}  // namespace opcua (vendored)
