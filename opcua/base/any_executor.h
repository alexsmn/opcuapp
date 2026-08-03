#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/execution.hpp>
#include <boost/asio/execution_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <functional>
#include <memory>
#include <source_location>

namespace opcua {
using AnyExecutor = boost::asio::any_io_executor;

using AnyExecutorFactory = std::function<AnyExecutor()>;

inline AnyExecutorFactory MakeSingleExecutorFactory(AnyExecutor executor) {
  return [executor = std::move(executor)] { return executor; };
}

template <class Ex, class Task>
inline void PostDelayedTask(
    const Ex& executor,
    std::chrono::nanoseconds delay,
    Task&& task,
    const std::source_location& location = std::source_location::current()) {
  if (delay == std::chrono::nanoseconds::zero()) {
    boost::asio::post(executor, std::forward<Task>(task));
    return;
  }

  auto timer = std::make_shared<boost::asio::steady_timer>(executor);
  timer->expires_after(delay);
  timer->async_wait([timer, captured_task = std::forward<Task>(task)](
                        boost::system::error_code ec) mutable {
    if (ec != boost::asio::error::operation_aborted) {
      std::move(captured_task)();
    }
  });
}

inline AnyExecutor MakeAnyExecutor(AnyExecutor executor) {
  return executor;
}

template <class Executor>
class AnyExecutorAdapter {
 public:
  explicit AnyExecutorAdapter(std::shared_ptr<Executor> executor)
      : state_{std::make_shared<State>(std::move(executor))} {}

  bool operator==(const AnyExecutorAdapter& other) const noexcept {
    return state_->executor == other.state_->executor;
  }

  boost::asio::execution_context& query(
      boost::asio::execution::context_t) const noexcept {
    return state_->context;
  }

  static constexpr boost::asio::execution::blocking_t::never_t query(
      boost::asio::execution::blocking_t) noexcept {
    return boost::asio::execution::blocking.never;
  }

  template <class F>
  void execute(F&& f) const {
    state_->executor->PostDelayedTask({}, MakeTask(std::forward<F>(f)));
  }

 private:
  struct State {
    explicit State(std::shared_ptr<Executor> executor)
        : executor{std::move(executor)} {}

    boost::asio::execution_context context;
    std::shared_ptr<Executor> executor;
  };

  template <class F>
  static std::function<void()> MakeTask(F&& f) {
    using Func = std::decay_t<F>;
    return
        [copyable_fun = std::make_shared<Func>(std::forward<F>(f))]() mutable {
          std::move (*copyable_fun)();
        };
  }

  std::shared_ptr<State> state_;
};

template <class Executor>
inline AnyExecutor MakeAnyExecutor(std::shared_ptr<Executor> executor) {
  return AnyExecutor{AnyExecutorAdapter<Executor>{std::move(executor)}};
}
}  // namespace opcua
