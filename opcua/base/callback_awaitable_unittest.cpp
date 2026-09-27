#include "opcua/base/callback_awaitable.h"

#include "opcua/base/test/awaitable_test.h"
#include "opcua/base/test/test_executor.h"

#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <gtest/gtest.h>

// Mirrors the superproject's `base/test/callback_awaitable_unittest.cpp` for
// the copy opcuapp carries.

namespace opcua {
namespace {

TEST(CallbackToAwaitable, CompletesWithCallbackValues) {
  TestExecutor executor;

  EXPECT_EQ(WaitAwaitable(executor,
                          [executor]() -> Awaitable<int> {
                            auto [value] = co_await CallbackToAwaitable<int>(
                                executor,
                                [](auto callback) mutable { callback(42); });
                            co_return value;
                          }()),
            42);
}

// A `start` that takes the handler's cancellation slot as well as the callback
// is handed it, and owns what cancellation means: here it completes with -1.
TEST(CallbackToAwaitable, HandsTheHandlersCancellationSlotToAStartThatTakesIt) {
  TestExecutor executor;
  boost::asio::cancellation_signal cancel;

  // Named, not invoked as a temporary: the coroutine is lazy and only runs in
  // Drain() below, and its frame reads `executor` and `cancel` through the
  // closure. A temporary closure is destroyed at the end of the statement that
  // starts the coroutine, which left every capture dangling -- a crash under
  // GCC 14 Release, silently passing under Clang.
  auto body = [executor, &cancel]() -> Awaitable<int> {
    auto [value] = co_await CallbackToAwaitable<int>(
        executor,
        [](auto callback, boost::asio::cancellation_slot slot) mutable {
          EXPECT_TRUE(slot.is_connected());
          slot.assign([callback](boost::asio::cancellation_type) mutable {
            callback(-1);
          });
        },
        boost::asio::bind_cancellation_slot(cancel.slot(),
                                            boost::asio::use_awaitable));
    co_return value;
  };
  auto result = StartAwaitable(executor, body());

  Drain(executor);
  EXPECT_FALSE(result->done) << "nothing has cancelled the wait yet";

  cancel.emit(boost::asio::cancellation_type::terminal);

  EXPECT_EQ(WaitResult(executor, result), -1);
}

TEST(CallbackToAwaitable, ADisconnectedSlotIsReportedAsSuch) {
  TestExecutor executor;

  EXPECT_EQ(
      WaitAwaitable(executor,
                    [executor]() -> Awaitable<bool> {
                      auto [connected] = co_await CallbackToAwaitable<bool>(
                          executor,
                          [](auto callback,
                             boost::asio::cancellation_slot slot) mutable {
                            callback(slot.is_connected());
                          });
                      co_return connected;
                    }()),
      false);
}

}  // namespace
}  // namespace opcua
