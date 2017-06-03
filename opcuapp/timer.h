#pragma once

#include <functional>
#include <opcua_timer.h>

#include "opcuapp/types.h"

namespace opcua {

class Timer {
 public:
  using Callback = std::function<void()>;

  Timer() {}
  explicit Timer(Callback callback) : callback_{std::move(callback)} {}

  ~Timer() { Stop(); }

  void SetCallback(Callback callback) {
    callback_ = std::move(callback);
  }

  void Start(UInt32 interval_ms) {
    OpcUa_Timer_Create(&impl_, interval_ms, &TimerCallback, nullptr, this);
  }

  void Stop() {
    OpcUa_Timer_Delete(&impl_);
  }

 private:
  static OpcUa_StatusCode OPCUA_DLLCALL TimerCallback(OpcUa_Void* pvCallbackData, OpcUa_Timer hTimer, UInt32 msecElapsed) {
    static_cast<Timer*>(pvCallbackData)->callback_();
    return OpcUa_Good;
  }

  OpcUa_Timer impl_ = OpcUa_Null;
  Callback callback_;
};

} // namespace opcua