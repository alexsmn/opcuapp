#pragma once

#include "opcua/base/any_executor.h"
#include "opcua/base/awaitable.h"
#include "opcua/base/callback_awaitable.h"

#include <boost/asio/cancel_after.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/system_error.hpp>
#include <cassert>
#include <chrono>
#include <exception>
#include <functional>
#include <list>
#include <memory>
#include <tuple>
#include <utility>

// opcuapp's copy of the superproject's `base/async_completion.h`. opcuapp is
// standalone and cannot consume that product, so the two are kept in step by
// hand -- change one, change both. The one deliberate difference is `assert`
// where the original panics through `base::Check`.

namespace opcua {
namespace base {

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
//
// A wait can be bounded without touching the gate: `WaitFor(timeout)` releases
// this one waiter when the deadline fires and leaves the gate open for the
// owner to settle later. That is the deadline shape every bounded wait wants --
// the gate belongs to the work, the deadline to the waiter -- and it used to be
// nine lines of hand-rolled `steady_timer` at each site.
class AsyncCompletion {
 public:
  explicit AsyncCompletion(AnyExecutor executor)
      : state_{std::make_shared<State>(std::move(executor))} {}

  AsyncCompletion(const AsyncCompletion&) = default;
  AsyncCompletion& operator=(const AsyncCompletion&) = default;

  // Suspends until the gate is settled; rethrows the `Fail()` error.
  //
  // Honours the awaiting handler's cancellation slot, if it has one connected:
  // a cancelled wait leaves the gate's waiter list and throws
  // `boost::system::system_error{boost::asio::error::operation_aborted}`, the
  // way any cancelled asio operation does under `use_awaitable`. The gate
  // itself is untouched.
  [[nodiscard]] Awaitable<void> Wait() const { return WaitOnState(state_); }

  // Suspends until the gate is settled or `timeout` elapses, whichever comes
  // first. Returns true when the gate settled and false when the deadline fired
  // first; rethrows the `Fail()` error when that is what settled it.
  //
  // The deadline releases *this waiter* and nothing else: the gate stays open,
  // other waiters keep waiting, and the owner still settles it later -- so an
  // owner that may lose such a race uses `TryComplete()`. What is bounded is
  // the wait, never the work behind the gate; a caller that walks away from
  // that work has to be sure it owns everything it will still touch.
  [[nodiscard]] Awaitable<bool> WaitFor(
      std::chrono::steady_clock::duration timeout) const {
    return WaitOnStateFor(state_, timeout);
  }

  void Complete() const { Finish({}); }

  void Fail(std::exception_ptr error) const {
    assert(error);
    Finish(std::move(error));
  }

  // Completes the gate unless it has completed already; returns true when this
  // call was the one that completed it.
  //
  // `Complete()` asserts on a second call, which is the right contract for a
  // gate one owner settles exactly once. It is the wrong one where two parties
  // legitimately race to settle the same gate -- a deadline against the work it
  // bounds -- and such sites were hand-guarding with
  // `if (!x.completed()) x.Complete();`, which reads as defensive noise rather
  // than as the intended semantics.
  bool TryComplete() const {
    if (state_->completed) {
      return false;
    }
    Finish({});
    return true;
  }

  [[nodiscard]] bool completed() const { return state_->completed; }

 private:
  using Handler = std::function<void(std::exception_ptr)>;

  struct State {
    explicit State(AnyExecutor executor) : executor{std::move(executor)} {}

    AnyExecutor executor;
    bool completed = false;
    std::exception_ptr error;
    // A list rather than a vector: a waiter whose wait is cancelled leaves
    // before the gate settles, and needs a handle to itself that survives its
    // neighbours joining and leaving. See `MakeWaitStart`.
    std::list<Handler> waiters;
  };

  // What a wait completes with: the error the gate settled with, and whether
  // it settled at all -- false means the wait was cancelled and the gate is
  // still open.
  //
  // Registers a waiter on `state`, and, when the awaiting handler has a
  // cancellation slot connected, arranges for cancellation to remove that
  // waiter again and complete it as cancelled. The gate settling and the slot
  // firing both run on the gate's executor, so `claimed` is a flag rather than
  // an atomic -- but either can run after the other has already taken the
  // waiter (a settle hands its waiters over before their completions are
  // posted), so whichever comes second finds nothing to do.
  static auto MakeWaitStart(std::shared_ptr<State> state) {
    return [state = std::move(state)](
               auto callback, boost::asio::cancellation_slot slot) mutable {
      auto completion = std::make_shared<std::decay_t<decltype(callback)>>(
          std::move(callback));

      if (state->completed) {
        (*completion)(state->error, true);
        return;
      }

      auto claimed = std::make_shared<bool>(false);
      auto waiter = state->waiters.insert(
          state->waiters.end(),
          [completion, claimed](std::exception_ptr error) mutable {
            if (std::exchange(*claimed, true)) {
              return;
            }
            (*completion)(std::move(error), true);
          });

      if (!slot.is_connected()) {
        return;
      }
      // Any cancellation type: leaving a wait has no side effect to make
      // partial or total cancellation unsafe.
      slot.assign([state, waiter, completion,
                   claimed](boost::asio::cancellation_type) mutable {
        if (std::exchange(*claimed, true)) {
          return;
        }
        state->waiters.erase(waiter);
        (*completion)(std::exception_ptr{}, false);
      });
    };
  }

  static Awaitable<void> WaitOnState(std::shared_ptr<State> state) {
    auto executor = state->executor;
    auto [error, settled] =
        co_await CallbackToAwaitable<std::exception_ptr, bool>(
            std::move(executor), MakeWaitStart(std::move(state)));

    if (error) {
      std::rethrow_exception(error);
    }
    if (!settled) {
      throw boost::system::system_error{boost::asio::error::operation_aborted};
    }
    co_return;
  }

  static Awaitable<bool> WaitOnStateFor(
      std::shared_ptr<State> state,
      std::chrono::steady_clock::duration timeout) {
    auto executor = state->executor;
    // `cancel_after` arms a timer on the initiation's executor -- the gate's --
    // and emits a cancellation into the slot `MakeWaitStart` assigned when it
    // fires first; when the wait completes first it cancels the timer.
    auto [error, settled] =
        co_await CallbackToAwaitable<std::exception_ptr, bool>(
            std::move(executor), MakeWaitStart(std::move(state)),
            boost::asio::cancel_after(timeout, boost::asio::use_awaitable));

    if (error) {
      std::rethrow_exception(error);
    }
    co_return settled;
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

}  // namespace base
}  // namespace opcua
