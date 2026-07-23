#include "opcua/session/subscription_conversion.h"

namespace opcua::subscription_conversion {

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

}  // namespace opcua::subscription_conversion
