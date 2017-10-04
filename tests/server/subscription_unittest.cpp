#include <gtest/gtest.h>

#include <opcuapp/platform.h>
#include <opcuapp/proxy_stub.h>
#include <opcuapp/server/subscription.h>

namespace opcua {
namespace server {

class TestTimer {
 public:
  void set_interval(UInt32 interval_ms) {}

  template<class WaitHandler>
  void Start(WaitHandler&& handler) {}
};

TEST(Subcription, Test) {
  Platform platform;
  ProxyStub proxy_stub{platform, ProxyStubConfiguration{}};

  BasicSubscription<TestTimer> subscription{SubscriptionContext{
      123,
      nullptr,
      nullptr,
      0,
      0,
      0,
      0,
      false,
      0,
  }};
}

} // namespace server
} // namespace opcua