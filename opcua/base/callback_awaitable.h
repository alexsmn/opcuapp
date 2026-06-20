#pragma once

#include "opcua/base/any_executor.h"

#include "opcua/base/any_executor_dispatch.h"

#include <boost/asio/async_result.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <functional>
#include <memory>
#include <tuple>

namespace opcua {
template <class... Args, class CompletionToken = boost::asio::use_awaitable_t<>,
          class Start>
inline auto CallbackToAwaitable(AnyExecutor executor,
                                Start&& start,
                                CompletionToken&& token = {}) {
  auto initiate = [executor = std::move(executor),
                   start = std::forward<Start>(start)]<
                      typename Handler>(Handler&& handler) mutable {
    auto completion =
        std::make_shared<std::decay_t<Handler>>(std::forward<Handler>(handler));

    std::invoke(
        std::move(start),
        BindExecutor(executor,
                     [completion](Args... args) mutable {
                       (*completion)(
                           std::make_tuple(std::move(args)...));
                     }));
  };

  return boost::asio::async_initiate<
      CompletionToken, void(std::tuple<std::decay_t<Args>...>)>(initiate,
                                                                token);
}
}  // namespace opcua (vendored)
