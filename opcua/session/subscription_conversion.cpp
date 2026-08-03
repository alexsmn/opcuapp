#include "opcua/session/subscription_conversion.h"

#include "opcua/events/event_filter.h"
#include "opcua/types/attribute_ids.h"
#include "opcua/types/read_value_id.h"
#include "opcua/types/standard_node_ids.h"
#include "opcua/ua/ua_binary_codec.h"
#include "opcua/ua/ua_json_codec.h"

#include <boost/json.hpp>

#include <cstdint>
#include <type_traits>

namespace opcua::subscription_conversion {
namespace {

// JSON DefaultJson encoding ids for the NotificationData subtypes
// (NodeIds.csv); the DefaultBinary ids come from BinaryEncodingId<T>.
constexpr std::uint32_t kDataChangeNotificationJsonId = 15345;
constexpr std::uint32_t kEventNotificationListJsonId = 15347;
constexpr std::uint32_t kStatusChangeNotificationJsonId = 15350;

// DefaultJson encoding id for EventFilterResult (NodeIds.csv); used to carry a
// filter_result JSON blob as a conformant ExtensionObject.
constexpr std::uint32_t kEventFilterResultJsonId = 15314;

template <class To, class From>
To CastEnum(From value) {
  return static_cast<To>(static_cast<std::underlying_type_t<From>>(value));
}

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

// Wraps a generated notification body as an ExtensionObject. `json_body`
// selects the body form: a JSON body renders inline on the websocket transport
// (the web client has no binary decoder), a binary body renders as a spec
// ExtensionObject on UA-TCP (and as UaEncoding=1 base64 over JSON).
template <class Notification>
ExtensionObject WrapNotification(const Notification& wire,
                                 std::uint32_t json_id,
                                 bool json_body) {
  if (json_body) {
    return ExtensionObject{ExpandedNodeId{NodeId{json_id}},
                           ua::EncodeJson(wire)};
  }
  return ua::ToExtensionObject(wire);
}

// Wraps a managed NotificationData alternative as its generated
// ExtensionObject.
ExtensionObject ToExtensionObject(const NotificationData& notification,
                                  bool json_body) {
  if (const auto* data_change =
          std::get_if<DataChangeNotification>(&notification)) {
    ua::DataChangeNotification wire;
    wire.monitored_items.reserve(data_change->monitored_items.size());
    for (const auto& item : data_change->monitored_items) {
      wire.monitored_items.push_back(ToUa(item));
    }
    return WrapNotification(wire, kDataChangeNotificationJsonId, json_body);
  }
  if (const auto* events = std::get_if<EventNotificationList>(&notification)) {
    ua::EventNotificationList wire;
    wire.events.reserve(events->events.size());
    for (const auto& event : events->events) {
      wire.events.push_back(ToUa(event));
    }
    return WrapNotification(wire, kEventNotificationListJsonId, json_body);
  }
  const auto& status_change = std::get<StatusChangeNotification>(notification);
  return WrapNotification(
      ua::StatusChangeNotification{.status = Status{status_change.status}},
      kStatusChangeNotificationJsonId, json_body);
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

ua::NotificationMessage ToUa(const NotificationMessage& m, bool json_body) {
  ua::NotificationMessage wire;
  wire.sequence_number = m.sequence_number;
  wire.publish_time = m.publish_time;
  wire.notification_data.reserve(m.notification_data.size());
  for (const auto& notification : m.notification_data) {
    wire.notification_data.push_back(
        ToExtensionObject(notification, json_body));
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

// --- Monitored-item filter (the ReadValueId, DataChangeFilter, and the
// EventFilter reshape). ---

// The generated ReadValueId folds the monitored-item index range and data
// encoding in; the hand-written MonitoredItemCreateRequest keeps index_range as
// a separate field.
ua::ReadValueId ToUaReadValueId(const ReadValueId& item_to_monitor,
                                const std::optional<std::string>& index_range) {
  return ua::ReadValueId{
      .node_id = item_to_monitor.node_id,
      .attribute_id = static_cast<UInt32>(item_to_monitor.attribute_id),
      .index_range = index_range.value_or(std::string{})};
}

// Builds a select-clause operand for one event field browse path
// (SimpleAttributeOperand over BaseEventType / Value, OPC UA Part 4 §7.4.4).
ua::SimpleAttributeOperand ToSelectClause(
    const std::vector<std::string>& browse_path) {
  ua::SimpleAttributeOperand operand;
  operand.type_definition_id = NodeId{id::BaseEventType};
  operand.browse_path.reserve(browse_path.size());
  for (const auto& segment : browse_path) {
    operand.browse_path.push_back(QualifiedName{segment, 0});
  }
  operand.attribute_id = static_cast<UInt32>(AttributeId::Value);
  return operand;
}

ua::ContentFilterElement ToWhereClauseElement(ua::FilterOperator op,
                                              const NodeId& node_id) {
  ua::ContentFilterElement element;
  element.filter_operator = op;
  element.filter_operands.push_back(
      ua::ToExtensionObject(ua::LiteralOperand{.value = Variant{node_id}}));
  return element;
}

// The ACKED/UNACKED bits travel as the standard
// `Equals(SimpleAttributeOperand("AckedState"), Literal(Boolean))` where clause
// (OPC UA Part 4 §7.7.3), matching the hand-written AppendEventFilter.
ua::ContentFilterElement ToAckedClause(bool acked) {
  ua::ContentFilterElement element;
  element.filter_operator = ua::FilterOperator::Equals;
  element.filter_operands.push_back(
      ua::ToExtensionObject(ToSelectClause({"AckedState"})));
  element.filter_operands.push_back(
      ua::ToExtensionObject(ua::LiteralOperand{.value = Variant{acked}}));
  return element;
}

// Reshapes the JSON-blob EventFilter (select field paths + the SCADA
// of_type/child_of/types where-clause) into a conformant ua::EventFilter.
ua::EventFilter ToUaEventFilter(const boost::json::value& json) {
  ua::EventFilter filter;
  for (const auto& browse_path : ParseEventFilterFieldPaths(json)) {
    filter.select_clauses.push_back(ToSelectClause(browse_path));
  }

  EventFilter where;
  if (json.is_object()) {
    const auto& object = json.as_object();
    const auto read_ids = [&object](const char* key, std::vector<NodeId>& out) {
      const auto* array = object.if_contains(key);
      if (array == nullptr || !array->is_array()) {
        return;
      }
      for (const auto& value : array->as_array()) {
        if (value.is_string()) {
          out.push_back(NodeId::FromString(value.as_string().c_str()));
        }
      }
    };
    read_ids("of_type", where.of_type);
    read_ids("child_of", where.child_of);
    if (const auto* types = object.if_contains("types");
        types != nullptr && types->is_number()) {
      where.types = types->to_number<unsigned>();
    }
  }

  for (const auto& node_id : where.of_type) {
    filter.where_clause.elements.push_back(
        ToWhereClauseElement(ua::FilterOperator::OfType, node_id));
  }
  for (const auto& node_id : where.child_of) {
    filter.where_clause.elements.push_back(
        ToWhereClauseElement(ua::FilterOperator::RelatedTo, node_id));
  }
  if (where.types & EventFilter::ACKED) {
    filter.where_clause.elements.push_back(ToAckedClause(true));
  }
  if (where.types & EventFilter::UNACKED) {
    filter.where_clause.elements.push_back(ToAckedClause(false));
  }
  return filter;
}

// Extracts the last browse-path segment of a SimpleAttributeOperand operand.
std::optional<std::string> OperandLeaf(const ExtensionObject& operand) {
  ua::SimpleAttributeOperand decoded;
  if (!ua::FromExtensionObject(operand, decoded) ||
      decoded.browse_path.empty()) {
    return std::nullopt;
  }
  return decoded.browse_path.back().name();
}

std::optional<NodeId> OperandNodeId(const ExtensionObject& operand) {
  ua::LiteralOperand decoded;
  if (!ua::FromExtensionObject(operand, decoded)) {
    return std::nullopt;
  }
  if (const auto* node_id = decoded.value.get_if<NodeId>()) {
    return *node_id;
  }
  return std::nullopt;
}

std::optional<bool> OperandBool(const ExtensionObject& operand) {
  ua::LiteralOperand decoded;
  if (!ua::FromExtensionObject(operand, decoded)) {
    return std::nullopt;
  }
  if (const auto* value = decoded.value.get_if<bool>()) {
    return *value;
  }
  return std::nullopt;
}

// Inverse of ToUaEventFilter: rebuilds the JSON-blob EventFilter from a
// conformant ua::EventFilter. Unrecognized operators/shapes are ignored (the
// server evaluates only these), matching the hand-written
// DecodeEventFilterBody.
boost::json::value ToJsonEventFilter(const ua::EventFilter& filter) {
  std::vector<std::vector<std::string>> field_paths;
  field_paths.reserve(filter.select_clauses.size());
  for (const auto& clause : filter.select_clauses) {
    std::vector<std::string> browse_path;
    browse_path.reserve(clause.browse_path.size());
    for (const auto& segment : clause.browse_path) {
      browse_path.push_back(segment.name());
    }
    field_paths.push_back(std::move(browse_path));
  }

  EventFilter where;
  for (const auto& element : filter.where_clause.elements) {
    if (element.filter_operator == ua::FilterOperator::OfType &&
        element.filter_operands.size() == 1) {
      if (auto node_id = OperandNodeId(element.filter_operands[0])) {
        where.of_type.push_back(std::move(*node_id));
      }
    } else if (element.filter_operator == ua::FilterOperator::RelatedTo &&
               element.filter_operands.size() == 1) {
      if (auto node_id = OperandNodeId(element.filter_operands[0])) {
        where.child_of.push_back(std::move(*node_id));
      }
    } else if (element.filter_operator == ua::FilterOperator::Equals &&
               element.filter_operands.size() == 2) {
      // Equals(SimpleAttributeOperand("AckedState"), Literal(Boolean)) in
      // either operand order maps onto the ACKED/UNACKED selection.
      for (int attribute_index = 0; attribute_index < 2; ++attribute_index) {
        const auto leaf = OperandLeaf(element.filter_operands[attribute_index]);
        const auto acked =
            OperandBool(element.filter_operands[1 - attribute_index]);
        if (leaf == "AckedState" && acked.has_value()) {
          where.types |= *acked ? EventFilter::ACKED : EventFilter::UNACKED;
          break;
        }
      }
    }
  }

  auto json = BuildEventFilter(field_paths);
  auto& object = json.as_object();
  boost::json::array of_type;
  boost::json::array child_of;
  for (const auto& node_id : where.of_type) {
    of_type.emplace_back(std::string{node_id.ToString()});
  }
  for (const auto& node_id : where.child_of) {
    child_of.emplace_back(std::string{node_id.ToString()});
  }
  object["_scada"] = "event";
  object["types"] = where.types;
  object["of_type"] = std::move(of_type);
  object["child_of"] = std::move(child_of);
  return json;
}

ExtensionObject FilterToExtensionObject(
    const std::optional<MonitoringFilter>& filter) {
  if (!filter.has_value()) {
    return ExtensionObject{};
  }
  if (const auto* data_change = std::get_if<DataChangeFilter>(&*filter)) {
    return ua::ToExtensionObject(ua::DataChangeFilter{
        .trigger = CastEnum<ua::DataChangeTrigger>(data_change->trigger),
        .deadband_type = static_cast<UInt32>(data_change->deadband_type),
        .deadband_value = data_change->deadband_value});
  }
  const auto& json = std::get<boost::json::value>(*filter);
  return ua::ToExtensionObject(ToUaEventFilter(json));
}

std::optional<MonitoringFilter> FilterFromExtensionObject(
    const ExtensionObject& extension_object) {
  ua::DataChangeFilter data_change;
  if (ua::FromExtensionObject(extension_object, data_change)) {
    return MonitoringFilter{DataChangeFilter{
        .trigger = CastEnum<DataChangeTrigger>(data_change.trigger),
        .deadband_type = static_cast<DeadbandType>(data_change.deadband_type),
        .deadband_value = data_change.deadband_value}};
  }
  ua::EventFilter event_filter;
  if (ua::FromExtensionObject(extension_object, event_filter)) {
    return MonitoringFilter{ToJsonEventFilter(event_filter)};
  }
  // The websocket transport carries the filter as an inline JSON body; match on
  // the DefaultJson encoding id.
  const auto* json =
      std::any_cast<boost::json::value>(&extension_object.value());
  if (json == nullptr) {
    return std::nullopt;
  }
  const NodeId& id = extension_object.data_type_id().node_id();
  if (!id.is_numeric() || id.namespace_index() != 0) {
    return std::nullopt;
  }
  // DataChangeFilter DefaultJson id 15296, EventFilter DefaultJson id 15295.
  if (id.numeric_id() == 15296) {
    ua::DecodeJson(*json, data_change);
    return MonitoringFilter{DataChangeFilter{
        .trigger = CastEnum<DataChangeTrigger>(data_change.trigger),
        .deadband_type = static_cast<DeadbandType>(data_change.deadband_type),
        .deadband_value = data_change.deadband_value}};
  }
  if (id.numeric_id() == 15295) {
    ua::DecodeJson(*json, event_filter);
    return MonitoringFilter{ToJsonEventFilter(event_filter)};
  }
  return std::nullopt;
}

// The managed filter_result is an opaque JSON blob (in practice always empty,
// since this server reports no per-clause filter status); carry it as an
// ExtensionObject so a populated result still round-trips.
ExtensionObject FilterResultToExtensionObject(
    const std::optional<boost::json::value>& filter_result) {
  if (!filter_result.has_value()) {
    return ExtensionObject{};
  }
  return ExtensionObject{ExpandedNodeId{NodeId{kEventFilterResultJsonId}},
                         *filter_result};
}

std::optional<boost::json::value> FilterResultFromExtensionObject(
    const ExtensionObject& extension_object) {
  if (const auto* json =
          std::any_cast<boost::json::value>(&extension_object.value())) {
    return *json;
  }
  return std::nullopt;
}

ua::MonitoringParameters ToUa(const MonitoringParameters& m) {
  return ua::MonitoringParameters{.client_handle = m.client_handle,
                                  .sampling_interval = m.sampling_interval_ms,
                                  .filter = FilterToExtensionObject(m.filter),
                                  .queue_size = m.queue_size,
                                  .discard_oldest = m.discard_oldest};
}

MonitoringParameters FromUa(const ua::MonitoringParameters& w) {
  return MonitoringParameters{.client_handle = w.client_handle,
                              .sampling_interval_ms = w.sampling_interval,
                              .filter = FilterFromExtensionObject(w.filter),
                              .queue_size = w.queue_size,
                              .discard_oldest = w.discard_oldest};
}

ua::MonitoredItemCreateRequest ToUa(const MonitoredItemCreateRequest& m) {
  return ua::MonitoredItemCreateRequest{
      .item_to_monitor = ToUaReadValueId(m.item_to_monitor, m.index_range),
      .monitoring_mode = CastEnum<ua::MonitoringMode>(m.monitoring_mode),
      .requested_parameters = ToUa(m.requested_parameters)};
}

MonitoredItemCreateRequest FromUa(const ua::MonitoredItemCreateRequest& w) {
  MonitoredItemCreateRequest managed;
  managed.item_to_monitor = {
      .node_id = w.item_to_monitor.node_id,
      .attribute_id = static_cast<AttributeId>(w.item_to_monitor.attribute_id)};
  if (!w.item_to_monitor.index_range.empty()) {
    managed.index_range = w.item_to_monitor.index_range;
  }
  managed.monitoring_mode = CastEnum<MonitoringMode>(w.monitoring_mode);
  managed.requested_parameters = FromUa(w.requested_parameters);
  return managed;
}

ua::MonitoredItemCreateResult ToUa(const MonitoredItemCreateResult& m) {
  return ua::MonitoredItemCreateResult{
      .status_code = m.status,
      .monitored_item_id = m.monitored_item_id,
      .revised_sampling_interval = m.revised_sampling_interval_ms,
      .revised_queue_size = m.revised_queue_size,
      .filter_result = FilterResultToExtensionObject(m.filter_result)};
}

MonitoredItemCreateResult FromUa(const ua::MonitoredItemCreateResult& w) {
  return MonitoredItemCreateResult{
      .status = w.status_code,
      .monitored_item_id = w.monitored_item_id,
      .revised_sampling_interval_ms = w.revised_sampling_interval,
      .revised_queue_size = w.revised_queue_size,
      .filter_result = FilterResultFromExtensionObject(w.filter_result)};
}

ua::MonitoredItemModifyRequest ToUa(const MonitoredItemModifyRequest& m) {
  return ua::MonitoredItemModifyRequest{
      .monitored_item_id = m.monitored_item_id,
      .requested_parameters = ToUa(m.requested_parameters)};
}

MonitoredItemModifyRequest FromUa(const ua::MonitoredItemModifyRequest& w) {
  return MonitoredItemModifyRequest{
      .monitored_item_id = w.monitored_item_id,
      .requested_parameters = FromUa(w.requested_parameters)};
}

ua::MonitoredItemModifyResult ToUa(const MonitoredItemModifyResult& m) {
  return ua::MonitoredItemModifyResult{
      .status_code = m.status,
      .revised_sampling_interval = m.revised_sampling_interval_ms,
      .revised_queue_size = m.revised_queue_size,
      .filter_result = FilterResultToExtensionObject(m.filter_result)};
}

MonitoredItemModifyResult FromUa(const ua::MonitoredItemModifyResult& w) {
  return MonitoredItemModifyResult{
      .status = w.status_code,
      .revised_sampling_interval_ms = w.revised_sampling_interval,
      .revised_queue_size = w.revised_queue_size,
      .filter_result = FilterResultFromExtensionObject(w.filter_result)};
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

ua::PublishResponse ToWire(const PublishResponse& managed, bool json_body) {
  ua::PublishResponse wire;
  wire.response_header.service_result = managed.status;
  wire.subscription_id = managed.subscription_id;
  wire.available_sequence_numbers = managed.available_sequence_numbers;
  wire.more_notifications = managed.more_notifications;
  wire.notification_message = ToUa(managed.notification_message, json_body);
  wire.results.reserve(managed.results.size());
  for (const auto result : managed.results) {
    wire.results.push_back(Status{result});
  }
  return wire;
}

ua::RepublishResponse ToWire(const RepublishResponse& managed, bool json_body) {
  ua::RepublishResponse wire;
  wire.response_header.service_result = managed.status;
  wire.notification_message = ToUa(managed.notification_message, json_body);
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

CreateMonitoredItemsRequest ToManaged(
    const ua::CreateMonitoredItemsRequest& wire) {
  CreateMonitoredItemsRequest managed;
  managed.subscription_id = wire.subscription_id;
  managed.timestamps_to_return =
      CastEnum<TimestampsToReturn>(wire.timestamps_to_return);
  managed.items_to_create.reserve(wire.items_to_create.size());
  for (const auto& item : wire.items_to_create) {
    managed.items_to_create.push_back(FromUa(item));
  }
  return managed;
}

ModifyMonitoredItemsRequest ToManaged(
    const ua::ModifyMonitoredItemsRequest& wire) {
  ModifyMonitoredItemsRequest managed;
  managed.subscription_id = wire.subscription_id;
  managed.timestamps_to_return =
      CastEnum<TimestampsToReturn>(wire.timestamps_to_return);
  managed.items_to_modify.reserve(wire.items_to_modify.size());
  for (const auto& item : wire.items_to_modify) {
    managed.items_to_modify.push_back(FromUa(item));
  }
  return managed;
}

ua::CreateMonitoredItemsResponse ToWire(
    const CreateMonitoredItemsResponse& managed) {
  ua::CreateMonitoredItemsResponse wire;
  wire.response_header.service_result = managed.status;
  wire.results.reserve(managed.results.size());
  for (const auto& result : managed.results) {
    wire.results.push_back(ToUa(result));
  }
  return wire;
}

ua::ModifyMonitoredItemsResponse ToWire(
    const ModifyMonitoredItemsResponse& managed) {
  ua::ModifyMonitoredItemsResponse wire;
  wire.response_header.service_result = managed.status;
  wire.results.reserve(managed.results.size());
  for (const auto& result : managed.results) {
    wire.results.push_back(ToUa(result));
  }
  return wire;
}

ua::CreateMonitoredItemsRequest ToWire(
    const CreateMonitoredItemsRequest& managed) {
  ua::CreateMonitoredItemsRequest wire;
  wire.subscription_id = managed.subscription_id;
  wire.timestamps_to_return =
      CastEnum<ua::TimestampsToReturn>(managed.timestamps_to_return);
  wire.items_to_create.reserve(managed.items_to_create.size());
  for (const auto& item : managed.items_to_create) {
    wire.items_to_create.push_back(ToUa(item));
  }
  return wire;
}

ua::ModifyMonitoredItemsRequest ToWire(
    const ModifyMonitoredItemsRequest& managed) {
  ua::ModifyMonitoredItemsRequest wire;
  wire.subscription_id = managed.subscription_id;
  wire.timestamps_to_return =
      CastEnum<ua::TimestampsToReturn>(managed.timestamps_to_return);
  wire.items_to_modify.reserve(managed.items_to_modify.size());
  for (const auto& item : managed.items_to_modify) {
    wire.items_to_modify.push_back(ToUa(item));
  }
  return wire;
}

CreateMonitoredItemsResponse ToManaged(
    const ua::CreateMonitoredItemsResponse& wire) {
  CreateMonitoredItemsResponse managed{.status =
                                           wire.response_header.service_result};
  managed.results.reserve(wire.results.size());
  for (const auto& result : wire.results) {
    managed.results.push_back(FromUa(result));
  }
  return managed;
}

ModifyMonitoredItemsResponse ToManaged(
    const ua::ModifyMonitoredItemsResponse& wire) {
  ModifyMonitoredItemsResponse managed{.status =
                                           wire.response_header.service_result};
  managed.results.reserve(wire.results.size());
  for (const auto& result : wire.results) {
    managed.results.push_back(FromUa(result));
  }
  return managed;
}

}  // namespace opcua::subscription_conversion
