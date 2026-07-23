#include "opcua/session/subscription_conversion.h"

#include "opcua/ua/ua_binary_codec.h"
#include "opcua/ua/ua_json_codec.h"

#include <boost/json.hpp>

#include <cstdint>

namespace opcua::subscription_conversion {
namespace {

// JSON DefaultJson encoding ids for the NotificationData subtypes
// (NodeIds.csv); the DefaultBinary ids come from BinaryEncodingId<T>.
constexpr std::uint32_t kDataChangeNotificationJsonId = 15345;
constexpr std::uint32_t kEventNotificationListJsonId = 15347;
constexpr std::uint32_t kStatusChangeNotificationJsonId = 15350;

ua::MonitoredItemNotification ToUa(const MonitoredItemNotification& m) {
  return ua::MonitoredItemNotification{.client_handle = m.client_handle,
                                       .value = m.value};
}

MonitoredItemNotification FromUa(const ua::MonitoredItemNotification& w) {
  return MonitoredItemNotification{.client_handle = w.client_handle,
                                   .value = w.value};
}

ua::EventFieldList ToUa(const EventFieldList& m) {
  return ua::EventFieldList{.client_handle = m.client_handle,
                            .event_fields = m.event_fields};
}

EventFieldList FromUa(const ua::EventFieldList& w) {
  return EventFieldList{.client_handle = w.client_handle,
                        .event_fields = w.event_fields};
}

// Wraps a managed NotificationData alternative as its generated
// ExtensionObject.
ExtensionObject ToExtensionObject(const NotificationData& notification) {
  if (const auto* data_change =
          std::get_if<DataChangeNotification>(&notification)) {
    ua::DataChangeNotification wire;
    wire.monitored_items.reserve(data_change->monitored_items.size());
    for (const auto& item : data_change->monitored_items) {
      wire.monitored_items.push_back(ToUa(item));
    }
    return ua::ToExtensionObject(wire);
  }
  if (const auto* events = std::get_if<EventNotificationList>(&notification)) {
    ua::EventNotificationList wire;
    wire.events.reserve(events->events.size());
    for (const auto& event : events->events) {
      wire.events.push_back(ToUa(event));
    }
    return ua::ToExtensionObject(wire);
  }
  const auto& status_change = std::get<StatusChangeNotification>(notification);
  return ua::ToExtensionObject(
      ua::StatusChangeNotification{.status = Status{status_change.status}});
}

// Inverse: decodes a generated NotificationData ExtensionObject (binary or
// inline JSON body) into a managed NotificationData. Returns nullopt for an
// unrecognized extension type so the caller drops it.
std::optional<NotificationData> FromExtensionObject(
    const ExtensionObject& extension_object) {
  ua::DataChangeNotification data_change;
  if (ua::FromExtensionObject(extension_object, data_change)) {
    DataChangeNotification managed;
    managed.monitored_items.reserve(data_change.monitored_items.size());
    for (const auto& item : data_change.monitored_items) {
      managed.monitored_items.push_back(FromUa(item));
    }
    return managed;
  }
  ua::EventNotificationList events;
  if (ua::FromExtensionObject(extension_object, events)) {
    EventNotificationList managed;
    managed.events.reserve(events.events.size());
    for (const auto& event : events.events) {
      managed.events.push_back(FromUa(event));
    }
    return managed;
  }
  ua::StatusChangeNotification status_change;
  if (ua::FromExtensionObject(extension_object, status_change)) {
    return StatusChangeNotification{.status = status_change.status.code()};
  }

  // Inline JSON body (the websocket transport): match the DefaultJson id.
  const auto* json =
      std::any_cast<boost::json::value>(&extension_object.value());
  if (json == nullptr) {
    return std::nullopt;
  }
  const NodeId& id = extension_object.data_type_id().node_id();
  if (!id.is_numeric() || id.namespace_index() != 0) {
    return std::nullopt;
  }
  if (id.numeric_id() == kDataChangeNotificationJsonId) {
    ua::DecodeJson(*json, data_change);
    DataChangeNotification managed;
    managed.monitored_items.reserve(data_change.monitored_items.size());
    for (const auto& item : data_change.monitored_items) {
      managed.monitored_items.push_back(FromUa(item));
    }
    return managed;
  }
  if (id.numeric_id() == kEventNotificationListJsonId) {
    ua::DecodeJson(*json, events);
    EventNotificationList managed;
    managed.events.reserve(events.events.size());
    for (const auto& event : events.events) {
      managed.events.push_back(FromUa(event));
    }
    return managed;
  }
  if (id.numeric_id() == kStatusChangeNotificationJsonId) {
    ua::DecodeJson(*json, status_change);
    return StatusChangeNotification{.status = status_change.status.code()};
  }
  return std::nullopt;
}

ua::NotificationMessage ToUa(const NotificationMessage& m) {
  ua::NotificationMessage wire;
  wire.sequence_number = m.sequence_number;
  wire.publish_time = m.publish_time;
  wire.notification_data.reserve(m.notification_data.size());
  for (const auto& notification : m.notification_data) {
    wire.notification_data.push_back(ToExtensionObject(notification));
  }
  return wire;
}

NotificationMessage FromUa(const ua::NotificationMessage& w) {
  NotificationMessage managed;
  managed.sequence_number = w.sequence_number;
  managed.publish_time = w.publish_time;
  managed.notification_data.reserve(w.notification_data.size());
  for (const auto& extension_object : w.notification_data) {
    if (auto notification = FromExtensionObject(extension_object)) {
      managed.notification_data.push_back(std::move(*notification));
    }
  }
  return managed;
}

}  // namespace

CreateSubscriptionRequest ToManaged(const ua::CreateSubscriptionRequest& wire) {
  return CreateSubscriptionRequest{
      .parameters = {
          .publishing_interval_ms = wire.requested_publishing_interval,
          .lifetime_count = wire.requested_lifetime_count,
          .max_keep_alive_count = wire.requested_max_keep_alive_count,
          .max_notifications_per_publish = wire.max_notifications_per_publish,
          .publishing_enabled = wire.publishing_enabled,
          .priority = wire.priority,
      }};
}

ModifySubscriptionRequest ToManaged(const ua::ModifySubscriptionRequest& wire) {
  return ModifySubscriptionRequest{
      .subscription_id = wire.subscription_id,
      .parameters = {
          .publishing_interval_ms = wire.requested_publishing_interval,
          .lifetime_count = wire.requested_lifetime_count,
          .max_keep_alive_count = wire.requested_max_keep_alive_count,
          .max_notifications_per_publish = wire.max_notifications_per_publish,
          // ModifySubscription carries no publishingEnabled on the wire; the
          // subscription keeps its current publishing state.
          .priority = wire.priority,
      }};
}

ua::CreateSubscriptionResponse ToWire(
    const CreateSubscriptionResponse& managed) {
  ua::CreateSubscriptionResponse wire;
  wire.response_header.service_result = managed.status;
  wire.subscription_id = managed.subscription_id;
  wire.revised_publishing_interval = managed.revised_publishing_interval_ms;
  wire.revised_lifetime_count = managed.revised_lifetime_count;
  wire.revised_max_keep_alive_count = managed.revised_max_keep_alive_count;
  return wire;
}

ua::ModifySubscriptionResponse ToWire(
    const ModifySubscriptionResponse& managed) {
  ua::ModifySubscriptionResponse wire;
  wire.response_header.service_result = managed.status;
  wire.revised_publishing_interval = managed.revised_publishing_interval_ms;
  wire.revised_lifetime_count = managed.revised_lifetime_count;
  wire.revised_max_keep_alive_count = managed.revised_max_keep_alive_count;
  return wire;
}

ua::CreateSubscriptionRequest ToWire(const CreateSubscriptionRequest& managed) {
  ua::CreateSubscriptionRequest wire;
  wire.requested_publishing_interval =
      managed.parameters.publishing_interval_ms;
  wire.requested_lifetime_count = managed.parameters.lifetime_count;
  wire.requested_max_keep_alive_count = managed.parameters.max_keep_alive_count;
  wire.max_notifications_per_publish =
      managed.parameters.max_notifications_per_publish;
  wire.publishing_enabled = managed.parameters.publishing_enabled;
  wire.priority = managed.parameters.priority;
  return wire;
}

ua::ModifySubscriptionRequest ToWire(const ModifySubscriptionRequest& managed) {
  ua::ModifySubscriptionRequest wire;
  wire.subscription_id = managed.subscription_id;
  wire.requested_publishing_interval =
      managed.parameters.publishing_interval_ms;
  wire.requested_lifetime_count = managed.parameters.lifetime_count;
  wire.requested_max_keep_alive_count = managed.parameters.max_keep_alive_count;
  wire.max_notifications_per_publish =
      managed.parameters.max_notifications_per_publish;
  wire.priority = managed.parameters.priority;
  return wire;
}

CreateSubscriptionResponse ToManaged(
    const ua::CreateSubscriptionResponse& wire) {
  return CreateSubscriptionResponse{
      .status = wire.response_header.service_result,
      .subscription_id = wire.subscription_id,
      .revised_publishing_interval_ms = wire.revised_publishing_interval,
      .revised_lifetime_count = wire.revised_lifetime_count,
      .revised_max_keep_alive_count = wire.revised_max_keep_alive_count,
  };
}

ModifySubscriptionResponse ToManaged(
    const ua::ModifySubscriptionResponse& wire) {
  return ModifySubscriptionResponse{
      .status = wire.response_header.service_result,
      .revised_publishing_interval_ms = wire.revised_publishing_interval,
      .revised_lifetime_count = wire.revised_lifetime_count,
      .revised_max_keep_alive_count = wire.revised_max_keep_alive_count,
  };
}

PublishRequest ToManaged(const ua::PublishRequest& wire) {
  PublishRequest managed;
  managed.subscription_acknowledgements.reserve(
      wire.subscription_acknowledgements.size());
  for (const auto& ack : wire.subscription_acknowledgements) {
    managed.subscription_acknowledgements.push_back(
        {.subscription_id = ack.subscription_id,
         .sequence_number = ack.sequence_number});
  }
  return managed;
}

RepublishRequest ToManaged(const ua::RepublishRequest& wire) {
  return RepublishRequest{
      .subscription_id = wire.subscription_id,
      .retransmit_sequence_number = wire.retransmit_sequence_number,
  };
}

ua::PublishResponse ToWire(const PublishResponse& managed) {
  ua::PublishResponse wire;
  wire.response_header.service_result = managed.status;
  wire.subscription_id = managed.subscription_id;
  wire.available_sequence_numbers = managed.available_sequence_numbers;
  wire.more_notifications = managed.more_notifications;
  wire.notification_message = ToUa(managed.notification_message);
  wire.results.reserve(managed.results.size());
  for (const auto result : managed.results) {
    wire.results.push_back(Status{result});
  }
  return wire;
}

ua::RepublishResponse ToWire(const RepublishResponse& managed) {
  ua::RepublishResponse wire;
  wire.response_header.service_result = managed.status;
  wire.notification_message = ToUa(managed.notification_message);
  return wire;
}

ua::PublishRequest ToWire(const PublishRequest& managed) {
  ua::PublishRequest wire;
  wire.subscription_acknowledgements.reserve(
      managed.subscription_acknowledgements.size());
  for (const auto& ack : managed.subscription_acknowledgements) {
    wire.subscription_acknowledgements.push_back(
        {.subscription_id = ack.subscription_id,
         .sequence_number = ack.sequence_number});
  }
  return wire;
}

ua::RepublishRequest ToWire(const RepublishRequest& managed) {
  return ua::RepublishRequest{
      .subscription_id = managed.subscription_id,
      .retransmit_sequence_number = managed.retransmit_sequence_number,
  };
}

PublishResponse ToManaged(const ua::PublishResponse& wire) {
  PublishResponse managed;
  managed.status = wire.response_header.service_result;
  managed.subscription_id = wire.subscription_id;
  managed.results.reserve(wire.results.size());
  for (const auto& result : wire.results) {
    managed.results.push_back(result.code());
  }
  managed.more_notifications = wire.more_notifications;
  managed.notification_message = FromUa(wire.notification_message);
  managed.available_sequence_numbers = wire.available_sequence_numbers;
  return managed;
}

RepublishResponse ToManaged(const ua::RepublishResponse& wire) {
  return RepublishResponse{
      .status = wire.response_header.service_result,
      .notification_message = FromUa(wire.notification_message),
  };
}

}  // namespace opcua::subscription_conversion
