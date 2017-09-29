#pragma once

#include <functional>
#include <opcua_timer.h>
#include <opcuapp/types.h>

namespace opcua {

class Timer {
 public:
  using Callback = std::function<void()>;

  Timer() {}
  explicit Timer(Callback callback) : callback_{std::move(callback)} {}

  void set_callback(Callback callback) { callback_ = std::move(callback); }

  void Start(UInt32 interval_ms) {
    std::make_shared<Core>(callback_)->Start(interval_ms);
  }

  void Stop() {
    core_ = nullptr;
  }

 private:
  class Core : public std::enable_shared_from_this<Core> {
   public:
    explicit Core(Callback callback) : callback_{std::move(callback)} {}

    ~Core() {
      if (impl_) {
        ::OpcUa_Timer_Delete(&impl_);
        impl_ = OpcUa_Null;
      }
    }

    void Start(UInt32 interval_ms) {
      assert(!impl_);
      reference_ = shared_from_this();
      Check(::OpcUa_Timer_Create(&impl_, interval_ms, &TimerCallback, nullptr, this));
    }

   private:
    static OpcUa_StatusCode OPCUA_DLLCALL TimerCallback(OpcUa_Void* pvCallbackData, OpcUa_Timer hTimer, UInt32 msecElapsed) {
      static_cast<Core*>(pvCallbackData)->callback_();
      return OpcUa_Good;
    }

    const Callback callback_;

    std::shared_ptr<Core> reference_;
    OpcUa_Timer impl_ = OpcUa_Null;
  };

  Callback callback_;
  std::shared_ptr<Core> core_;
};

} // namespace opcua