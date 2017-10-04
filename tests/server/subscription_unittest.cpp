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

  void Stop() {}
};

TEST(Subcription, Test) {
  Platform platform;
  ProxyStub proxy_stub{platform, ProxyStubConfiguration{}};

  auto subscription = BasicSubscription<TestTimer>::Create(SubscriptionContext{
      123,
      0,
      0,
      0,
      0,
      false,
      0,
      nullptr,
      nullptr,
      nullptr,
  });
}

} // namespace server
} // namespace opcua