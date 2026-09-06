#pragma once

#include "opcua/base/any_executor.h"

#include "opcua/base/any_executor_dispatch.h"

#include <boost/asio/associated_cancellation_slot.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <functional>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>

// opcuapp's copy of the superproject's `base/callback_awaitable.h`. opcuapp is
// standalone and cannot consume that product, so the two are kept in step by
// hand -- change one, change both.

namespace opcua {
namespace internal {

// The initiation `CallbackToAwaitable` hands to `async_initiate`.
//
// A class rather than a lambda so that it can carry an `executor_type`: a
// completion-token adapter that needs an executor of its own reads it from the
// initiation -- `boost::asio::cancel_after` builds its deadline timer on
// `Initiation::get_executor()` (boost/asio/impl/cancel_after.hpp) -- and a
// lambda has none to offer, so the timer would land on the system executor and
// fire the cancellation off the executor everything here is affine to.
template <class Start, class... Args>
struct CallbackInitiation {
  using executor_type = AnyExecutor;

  executor_type get_executor() const noexcept { return executor; }

  template <class Handler>
  void operator()(Handler&& handler) {
    // Read the slot off the handler before it is wrapped below: the association
    // belongs to the caller's token -- `bind_cancellation_slot`,
    // `cancel_after`, or the awaiting coroutine's own cancellation state -- and
    // the shared_ptr hides it.
    boost::asio::cancellation_slot slot =
        boost::asio::get_associated_cancellation_slot(handler);

    auto completion =
        std::make_shared<std::decay_t<Handler>>(std::forward<Handler>(handler));

    auto callback = BindExecutor(executor, [completion](Args... args) mutable {
      (*completion)(std::make_tuple(std::move(args)...));
    });

    // A `start` that also takes the slot owns what cancellation means for the
    // operation it begins: this adapter cannot fabricate `Args...` for a
    // cancelled completion, so it never completes the handler itself. A `start`
    // that takes only the callback ignores cancellation, which is what every
    // caller did before the slot was passed at all.
    if constexpr (std::is_invocable_v<Start&&, decltype(callback),
                                      boost::asio::cancellation_slot>) {
      std::invoke(std::move(start), std::move(callback), std::move(slot));
    } else {
      std::invoke(std::move(start), std::move(callback));
    }
  }

  AnyExecutor executor;
  Start start;
};

}  // namespace internal

// Adapts a callback-taking `start` into an asio asynchronous operation with
// completion signature `void(std::tuple<Args...>)`, resuming the awaiter on
// `executor` however the callback was invoked.
//
// `start` is invoked once with the callback to call when the operation is
// done, and optionally with the handler's `boost::asio::cancellation_slot` as a
// second argument. A `start` taking the slot is responsible for honouring it:
// assign a handler that completes the callback with whatever a cancelled
// outcome looks like for that operation, and check `slot.is_connected()`
// first, since most awaiters have none. `base::AsyncCompletion::WaitFor` is the
// worked example.
template <class... Args,
          class CompletionToken = boost::asio::use_awaitable_t<>,
          class Start>
inline auto CallbackToAwaitable(AnyExecutor executor,
                                Start&& start,
                                CompletionToken&& token = {}) {
  return boost::asio::async_initiate<CompletionToken,
                                     void(std::tuple<std::decay_t<Args>...>)>(
      internal::CallbackInitiation<std::decay_t<Start>, Args...>{
          std::move(executor), std::forward<Start>(start)},
      token);
}
}  // namespace opcua
