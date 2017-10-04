#pragma once

#include <memory>
#include <opcua_timer.h>
#include <opcuapp/basic_types.h>

namespace opcua {

class Timer {
 public:
  void set_interval(UInt32 interval_ms) { interval_ms_ = interval_ms; }

  template<class WaitHandler>
  void Start(WaitHandler&& handler) {
    core_ = std::make_shared<CoreImpl<WaitHandler>>(std::forward<WaitHandler>(handler));
    core_->Start(interval_ms_);
  }

  void Stop() {
    core_ = nullptr;
  }

 private:
  class Core {
   public:
    virtual ~Core() {}
    virtual void Start(UInt32 interval_ms) = 0;
  };

  template<class WaitHandler>
  class CoreImpl : public Core {
   public:
    explicit CoreImpl(WaitHandler&& handler) : handler_{std::forward<WaitHandler>(handler)} {}

    ~CoreImpl() {
      if (impl_) {
        ::OpcUa_Timer_Delete(&impl_);
        impl_ = OpcUa_Null;
      }
    }

    void Start(UInt32 interval_ms) {
      assert(!impl_);
      Check(::OpcUa_Timer_Create(&impl_, interval_ms, &TimerCallback, nullptr, this));
    }

   private:
    static OpcUa_StatusCode OPCUA_DLLCALL TimerCallback(OpcUa_Void* pvCallbackData, OpcUa_Timer hTimer, UInt32 msecElapsed) {
      static_cast<CoreImpl*>(pvCallbackData)->handler_();
      return OpcUa_Good;
    }

    WaitHandler handler_;
    OpcUa_Timer impl_ = OpcUa_Null;
  };

  UInt32 interval_ms_;
  std::shared_ptr<Core> core_;
};

} // namespace opcua