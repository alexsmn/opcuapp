#include <gtest/gtest.h>
#include <opcuapp/data_value.h>
#include <opcuapp/platform.h>
#include <opcuapp/proxy_stub.h>
#include <opcuapp/expanded_node_id.h>

namespace opcua {

TEST(ExtensionObject, Copy) {
  Platform platform;
  ProxyStub proxy_stub{platform, ProxyStubConfiguration{}};

  const int kClientHandle = 123;
  const Double kValue = 456;

  MonitoredItemNotification notification;
  notification.ClientHandle = kClientHandle;
  const auto timestamp = DateTime::UtcNow();
  DataValue{OpcUa_Good, kValue, timestamp, timestamp}.release(notification.Value);

  auto extension_object = ExtensionObject::Encode(std::move(notification));
  {
    EXPECT_EQ(OpcUa_ExtensionObjectEncoding_EncodeableObject, extension_object.encoding());
    EXPECT_EQ(OpcUaId_MonitoredItemNotification_Encoding_DefaultBinary, extension_object.type_id());
    auto* moved_notification = static_cast<OpcUa_MonitoredItemNotification*>(extension_object.object());
    EXPECT_EQ(kClientHandle, moved_notification->ClientHandle);
    EXPECT_EQ(kValue, moved_notification->Value.Value);
  }

  auto extension_object_copy = extension_object;
  {
    EXPECT_EQ(OpcUa_ExtensionObjectEncoding_EncodeableObject, extension_object_copy.encoding());
    EXPECT_EQ(OpcUaId_MonitoredItemNotification_Encoding_DefaultBinary, extension_object_copy.type_id());
    auto* moved_notification = static_cast<OpcUa_MonitoredItemNotification*>(extension_object_copy.object());
    EXPECT_EQ(kClientHandle, moved_notification->ClientHandle);
    EXPECT_EQ(kValue, moved_notification->Value.Value);
  }
}

} // namespace opcua