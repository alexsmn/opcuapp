#pragma once

#include "opcua/base/any_executor.h"
#include "opcua/base/auto_reset.h"
#include "opcua/base/common_types.h"

#include <algorithm>
#include <functional>
#include <mutex>
#include <source_location>
#include <thread>
#include <vector>

namespace opcua {
class TestExecutor {
 public:
  using Task = std::function<void()>;

  explicit TestExecutor(bool instant = false)
      : state_{std::make_shared<State>(instant)} {}

  bool operator==(const TestExecutor& other) const noexcept {
    return state_ == other.state_;
  }

  bool operator!=(const TestExecutor& other) const noexcept {
    return !(*this == other);
  }

  boost::asio::execution_context& query(
      boost::asio::execution::context_t) const noexcept {
    return state_->context;
  }

  boost::asio::execution_context& context() const noexcept {
    return state_->context;
  }

  static constexpr boost::asio::execution::blocking_t::never_t query(
      boost::asio::execution::blocking_t) noexcept {
    return boost::asio::execution::blocking.never;
  }

  template <class F>
  void execute(F&& f) const {
    PostDelayedTask({}, MakeTask(std::forward<F>(f)));
  }

  void on_work_started() const noexcept {}
  void on_work_finished() const noexcept {}

  template <class F, class Allocator>
  void dispatch(F&& f, const Allocator&) const {
    execute(std::forward<F>(f));
  }

  template <class F, class Allocator>
  void post(F&& f, const Allocator&) const {
    execute(std::forward<F>(f));
  }

  template <class F, class Allocator>
  void defer(F&& f, const Allocator&) const {
    execute(std::forward<F>(f));
  }

  void PostTask(Task task,
                const std::source_location& location =
                    std::source_location::current()) const {
    PostDelayedTask({}, std::move(task), location);
  }

  /*~TestExecutor() {
    // It's important to run all the remaining pending tasks. E.g.
    // HistoricalDb will only close on the posted task.
    for (;;) {
      auto run_tasks = PopRunTasks(SteadyDuration());
      if (run_tasks.empty()) {
        break;
      }
      for (auto& task : run_tasks) {
        task();
      }
    }
  }*/

  bool is_current_executor() const {
    return std::ranges::find(current_executor_stack_, state_.get()) !=
           current_executor_stack_.end();
  }

  void PostDelayedTask(SteadyDuration delay,
                       Task task,
                       const std::source_location& location =
                           std::source_location::current()) const {
    if (state_->instant) {
      ScopedCurrentExecutor current{state_.get()};
      task();
    } else {
      // Queue zero-delay work as well. Running it inline makes this executor
      // reentrant across foreign threads, which breaks Asio/coroutine adapter
      // paths that expect posted continuations to run later when polled.
      std::lock_guard lock{state_->mutex};
      state_->pending_tasks.emplace_back(delay, std::move(task), location);
    }
  }

  size_t GetTaskCount() const {
    std::lock_guard lock{state_->mutex};
    return state_->pending_tasks.size();
  }

  bool HasReadyTasks() const {
    std::lock_guard lock{state_->mutex};
    return std::ranges::any_of(
        state_->pending_tasks,
        [](const PendingTask& task) { return task.delay <= SteadyDuration{}; });
  }

  void Poll() { Advance({}); }

  void Advance(SteadyDuration delta) {
    ScopedCurrentExecutor current{state_.get()};

    auto run_tasks = PopRunTasks(delta);

    for (auto& task : run_tasks) {
      task();
    }
  }

 private:
  struct PendingTask {
    SteadyDuration delay;
    Task task;
    std::source_location location;
  };

  struct State {
    explicit State(bool instant) : instant{instant} {}

    const bool instant;
    boost::asio::execution_context context;
    mutable std::mutex mutex;
    std::vector<PendingTask> pending_tasks;
  };

  class ScopedCurrentExecutor {
   public:
    explicit ScopedCurrentExecutor(const State* state) : state_{state} {
      current_executor_stack_.push_back(state_);
    }

    ~ScopedCurrentExecutor() { current_executor_stack_.pop_back(); }

   private:
    const State* state_;
  };

  std::vector<Task> PopRunTasks(SteadyDuration delta) {
    std::lock_guard lock{state_->mutex};

    // Move run tasks with |task.delay <= delta| to the end of queue.
    auto p = std::stable_partition(
        state_->pending_tasks.begin(), state_->pending_tasks.end(),
        [delta](const PendingTask& p) { return p.delay > delta; });

    // Sort run tasks.
    std::stable_sort(p, state_->pending_tasks.end(),
                     [](const PendingTask& a, const PendingTask& b) {
                       return a.delay < b.delay;
                     });

    // Collect run tasks
    std::vector<Task> run_tasks;
    run_tasks.reserve(state_->pending_tasks.end() - p);
    for (auto i = p; i != state_->pending_tasks.end(); ++i)
      run_tasks.emplace_back(std::move(i->task));

    // Remove run tasks.
    state_->pending_tasks.erase(p, state_->pending_tasks.end());

    for (auto& t : state_->pending_tasks) {
      t.delay -= delta;
    }

    return run_tasks;
  }

  template <class F>
  static Task MakeTask(F&& f) {
    using Func = std::decay_t<F>;
    return
        [copyable_fun = std::make_shared<Func>(std::forward<F>(f))]() mutable {
          std::move (*copyable_fun)();
        };
  }

  std::shared_ptr<State> state_;
  inline static thread_local std::vector<const State*> current_executor_stack_;
};
}  // namespace opcua
