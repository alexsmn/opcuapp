#pragma once

#include <opcuapp/node_id.h>
#include <cassert>

namespace opcua {

inline bool IsValid(const OpcUa_DataChangeNotification& notification) {
  assert(notification.NoOfMonitoredItems != 0);
  if (notification.NoOfMonitoredItems == 0)
    return false;

  return true;
}

inline bool IsValid(const OpcUa_ExtensionObject& extension_object) {
  assert(extension_object.Encoding ==
         OpcUa_ExtensionObjectEncoding_EncodeableObject);
  if (extension_object.Encoding !=
      OpcUa_ExtensionObjectEncoding_EncodeableObject)
    return false;

  if (extension_object.TypeId.NodeId ==
      OpcUaId_DataChangeNotification_Encoding_DefaultBinary) {
    if (!IsValid(*static_cast<const OpcUa_DataChangeNotification*>(
            extension_object.Body.EncodeableObject.Object)))
      return false;

  } else {
    assert(false);
    return false;
  }

  return true;
}

inline bool IsValid(const OpcUa_NotificationMessage& message) {
  Span<const OpcUa_ExtensionObject> notifications{
      message.NotificationData,
      static_cast<size_t>(message.NoOfNotificationData)};
  for (auto& notification : notifications) {
    if (!IsValid(notification))
      return false;
  }

  return true;
}

}  // namespace opcua
