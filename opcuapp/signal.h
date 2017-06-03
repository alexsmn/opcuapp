#pragma once

#include <functional>
#include <mutex>
#include <vector>

namespace opcua {

class SignalConnection;

class BasicSignal {
 public:
  virtual ~BasicSignal() {}

  virtual void Disconnect(const SignalConnection& connection)  = 0;
};

class SignalConnection {
 public:
  using Id = size_t;

  SignalConnection() {}
  SignalConnection(BasicSignal& signal, Id id) : signal_{&signal}, id_{id} {}

  void Disconnect() {
    if (signal_) {
      auto* signal = signal_;
      signal_ = nullptr;
      signal->Disconnect(*this);
    }
  }

  Id id() const { return id_; }

 private:
  BasicSignal* signal_ = nullptr;
  Id id_ = 0;
};

class ScopedSignalConnection {
 public:
  ScopedSignalConnection() {}
  ScopedSignalConnection(SignalConnection connection) : connection_{connection} {}
  ~ScopedSignalConnection() { Reset(); }

  ScopedSignalConnection(const ScopedSignalConnection&) = delete;
  ScopedSignalConnection& operator=(const ScopedSignalConnection&) = delete;

  ScopedSignalConnection(ScopedSignalConnection&& source) : connection_{source.connection_} {
    source.connection_ = SignalConnection();
  }

  ScopedSignalConnection& operator=(ScopedSignalConnection&& source) {
    if (&source != this) {
      auto connection = connection_;
      connection_ = source.connection_;
      source.connection_ = SignalConnection();
      connection.Disconnect();
    }
    return *this;
  }

  void Reset() {
    auto connection = std::move(connection_);
    connection_ = SignalConnection{};
    connection.Disconnect();
  }

 private:
  SignalConnection connection_;
};

template<typename Signature>
class Signal : public BasicSignal {
 public:
  using Sink = std::function<Signature>;

  template<class T>
  SignalConnection Connect(T&& sink) {
    std::lock_guard<std::mutex> lock{mutex_};
    auto p = std::find(sinks_.begin(), sinks_.end(), nullptr);
    if (p == sinks_.end()) {
      sinks_.emplace_back(std::forward<T>(sink));
      p = sinks_.end() - 1;
    } else {
      *p = std::forward<T>(sink);
    }
    SignalConnection::Id id = p - sinks_.begin();
    return SignalConnection{*this, id};
  }

  virtual void Disconnect(const SignalConnection& connection) override {
    std::lock_guard<std::mutex> lock{mutex_};
    sinks_[connection.id()] = nullptr;
  }

  template<typename... Args>
  void operator()(Args... args) const {
    for (auto& sink : *this) {
      if (sink)
        sink(args...);
    }
  }

  class Iterator {
   public:
    Iterator(const Signal& signal, size_t pos) : signal_{signal}, pos_{pos} {}

    Iterator& operator++() {
      ++pos_;
      return *this;
    }

    Sink operator*() const {
      std::lock_guard<std::mutex> lock{signal_.mutex_};
      return signal_.sinks_[pos_];
    }

    bool operator==(const Iterator& other) const { return pos_ == other.pos_; }
    bool operator!=(const Iterator& other) const { return !operator==(other); }

   private:
    const Signal& signal_;
    size_t pos_;
  };

  Iterator begin() const { return Iterator{*this, 0}; }

  Iterator end() const {
    std::lock_guard<std::mutex> lock{mutex_};
    return Iterator{*this, sinks_.size()};
  }

 private:
  mutable std::mutex mutex_;
  std::vector<Sink> sinks_;
};

} // namespace opcua
