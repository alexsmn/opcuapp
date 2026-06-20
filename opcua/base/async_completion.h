#pragma once

#include "opcua/base/any_executor.h"
#include "opcua/base/awaitable.h"
#include "opcua/base/callback_awaitable.h"

#include <cassert>
#include <exception>
#include <functional>
#include <memory>
#include <vector>

namespace opcua::base {

// Executor-affine one-shot async completion gate.
//
// `AsyncCompletion` lets many coroutine waiters suspend until an owner calls
// `Complete()` or `Fail(error)`. Waiters that arrive after completion observe
// the stored result immediately, and failures are rethrown from `Wait()`.
//
// Copies share the same one-shot completion state. This lets coroutine
// launchers keep the gate alive independently of the object that created it
// without forcing every call site to add a separate `std::shared_ptr` wrapper.
//
// Example:
//   opcua::base::AsyncCompletion ready{executor};
//   CoSpawn(executor, [&]() -> Awaitable<void> {
//     co_await InitializeAsync();
//     ready.Complete();
//   });
//   co_await ready.Wait();
class AsyncCompletion {
 public:
  explicit AsyncCompletion(AnyExecutor executor)
      : state_{std::make_shared<State>(std::move(executor))} {}

  AsyncCompletion(const AsyncCompletion&) = default;
  AsyncCompletion& operator=(const AsyncCompletion&) = default;

  [[nodiscard]] Awaitable<void> Wait() const {
    return WaitOnState(state_);
  }

  void Complete() const {
    Finish({});
  }

  void Fail(std::exception_ptr error) const {
    assert(error);
    Finish(std::move(error));
  }

  [[nodiscard]] bool completed() const { return state_->completed; }

 private:
  using Handler = std::function<void(std::exception_ptr)>;

  struct State {
    explicit State(AnyExecutor executor) : executor{std::move(executor)} {}

    AnyExecutor executor;
    bool completed = false;
    std::exception_ptr error;
    std::vector<Handler> waiters;
  };

  static Awaitable<void> WaitOnState(std::shared_ptr<State> state) {
    auto executor = state->executor;
    auto [error] = co_await CallbackToAwaitable<std::exception_ptr>(
        std::move(executor), [state = std::move(state)](auto callback) mutable {
          auto completion =
              std::make_shared<std::decay_t<decltype(callback)>>(
                  std::move(callback));

          if (state->completed) {
            (*completion)(state->error);
            return;
          }

          state->waiters.emplace_back(
              [completion](std::exception_ptr error) mutable {
                (*completion)(std::move(error));
              });
        });

    if (error) {
      std::rethrow_exception(error);
    }
    co_return;
  }

  void Finish(std::exception_ptr error) const {
    assert(!state_->completed);
    if (state_->completed) {
      return;
    }

    state_->completed = true;
    state_->error = std::move(error);

    auto completion_error = state_->error;
    auto waiters = std::move(state_->waiters);
    for (auto& waiter : waiters) {
      waiter(completion_error);
    }
  }

  std::shared_ptr<State> state_;
};

}  // namespace opcua::base
