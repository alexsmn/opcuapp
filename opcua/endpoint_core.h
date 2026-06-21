#pragma once

#include "opcua/base/no_destructor.h"
#include "opcua/scada/attribute_service.h"
#include "opcua/scada/event.h"
#include "opcua/scada/event_util.h"
#include "opcua/scada/legacy_monitored_item_adapter.h"
#include "opcua/scada/monitored_item.h"
#include "opcua/scada/monitored_item_service.h"
#include "opcua/scada/service_context.h"

#include <boost/json/value.hpp>

#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace opcua {


inline ServiceContext MakeServiceContext(const NodeId& user_id,
                                         ServiceContext base_context = {}) {
  return base_context.with_user_id(user_id);
}

inline DataValue NormalizeReadResult(DataValue result) {
  constexpr unsigned kBadNodeIdUnknownFullCode = 0x80340000u;
  if (result.status_code == StatusCode::Bad_WrongNodeId) {
    result.status_code =
        Status::FromFullCode(kBadNodeIdUnknownFullCode).code();
  }
  return result;
}

inline std::vector<DataValue> NormalizeReadResults(
    std::vector<DataValue> results) {
  for (auto& result : results)
    result = NormalizeReadResult(std::move(result));
  return results;
}

inline bool IsAttributeEventNotifier(AttributeId attribute_id) {
  return attribute_id == AttributeId::EventNotifier;
}

inline bool IsSupportedMonitoredAttribute(AttributeId attribute_id) {
  return attribute_id == AttributeId::Value ||
         IsAttributeEventNotifier(attribute_id);
}

inline StatusCode TranslateCreateMonitoredItemFailure(
    const ReadValueId& item_to_monitor) {
  if (!IsSupportedMonitoredAttribute(item_to_monitor.attribute_id)) {
    return StatusCode::Bad_WrongAttributeId;
  }
  return StatusCode::Bad_WrongNodeId;
}

struct CreateMonitoredItemResult {
  std::shared_ptr<scada::MonitoredItem> monitored_item;
  StatusCode status = StatusCode::Bad;
};

inline CreateMonitoredItemResult CreateMonitoredItem(
    scada::LegacyMonitoredItemAdapter& monitored_item_adapter,
    const ReadValueId& item_to_monitor,
    const MonitoringParameters& parameters) {
  if (!IsSupportedMonitoredAttribute(item_to_monitor.attribute_id)) {
    return {.status = StatusCode::Bad_WrongAttributeId};
  }
  auto monitored_item =
      monitored_item_adapter.CreateMonitoredItem(item_to_monitor, parameters);
  const auto status =
      monitored_item ? StatusCode::Good
                     : TranslateCreateMonitoredItemFailure(item_to_monitor);
  return {.monitored_item = std::move(monitored_item), .status = status};
}

template <class DataChangeCallback, class EventCallback>
inline scada::MonitoredItemHandler MakeMonitoredItemHandler(
    const ReadValueId& item_to_monitor,
    DataChangeCallback&& data_change_callback,
    EventCallback&& event_callback) {
  if (IsAttributeEventNotifier(item_to_monitor.attribute_id)) {
    return scada::MonitoredItemHandler{
        scada::EventHandler(std::forward<EventCallback>(event_callback))};
  }
  return scada::MonitoredItemHandler{scada::DataChangeHandler(
      std::forward<DataChangeCallback>(data_change_callback))};
}

template <class DataChangeCallback, class EventCallback>
inline void SubscribeMonitoredItemNotifications(
    const ReadValueId& item_to_monitor,
    const std::shared_ptr<scada::MonitoredItem>& monitored_item,
    DataChangeCallback&& data_change_callback,
    EventCallback&& event_callback) {
  if (!monitored_item)
    return;

  monitored_item->Subscribe(MakeMonitoredItemHandler(
      item_to_monitor, std::forward<DataChangeCallback>(data_change_callback),
      std::forward<EventCallback>(event_callback)));
}

inline bool DispatchDataChangeNotification(
    const ReadValueId& item_to_monitor,
    const std::optional<scada::MonitoredItemHandler>& handler,
    const DataValue& data_value) {
  if (IsAttributeEventNotifier(item_to_monitor.attribute_id) || !handler)
    return false;

  const auto* data_change_handler =
      std::get_if<scada::DataChangeHandler>(&*handler);
  if (!data_change_handler)
    return false;

  (*data_change_handler)(data_value);
  return true;
}

inline bool DispatchEventNotification(
    const ReadValueId& item_to_monitor,
    const std::optional<scada::MonitoredItemHandler>& handler,
    const Status& status,
    const std::any& event) {
  if (!IsAttributeEventNotifier(item_to_monitor.attribute_id) || !handler)
    return false;

  const auto* event_handler = std::get_if<scada::EventHandler>(&*handler);
  if (!event_handler)
    return false;

  (*event_handler)(status, event);
  return true;
}

inline const std::vector<std::vector<std::string>>& DefaultEventFieldPaths() {
  static const base::NoDestructor<std::vector<std::vector<std::string>>>
      kFields(std::vector<std::vector<std::string>>{{"EventId"},
                                                    {"EventType"},
                                                    {"SourceNode"},
                                                    {"SourceName"},
                                                    {"Time"},
                                                    {"Message"},
                                                    {"Severity"}});
  return *kFields;
}

inline std::vector<std::vector<std::string>> NormalizeEventFieldPaths(
    std::vector<std::vector<std::string>> field_paths) {
  if (!field_paths.empty())
    return field_paths;
  const auto& defaults = DefaultEventFieldPaths();
  return std::vector<std::vector<std::string>>(defaults.begin(),
                                               defaults.end());
}

inline std::vector<std::vector<std::string>> ParseEventFilterFieldPaths(
    const boost::json::value& raw_filter) {
  constexpr std::string_view kEventFilterBody = "Body";
  constexpr std::string_view kSelectClauses = "SelectClauses";
  constexpr std::string_view kBrowsePath = "BrowsePath";
  constexpr std::string_view kName = "Name";

  if (!raw_filter.is_object()) {
    const auto& defaults = DefaultEventFieldPaths();
    return std::vector<std::vector<std::string>>(defaults.begin(),
                                                 defaults.end());
  }

  const auto* current = &raw_filter.as_object();
  if (const auto* body_field = current->if_contains(kEventFilterBody);
      body_field != nullptr && body_field->is_object()) {
    current = &body_field->as_object();
  }

  const auto* clauses_value = current->if_contains(kSelectClauses);
  if (!clauses_value || !clauses_value->is_array()) {
    const auto& defaults = DefaultEventFieldPaths();
    return std::vector<std::vector<std::string>>(defaults.begin(),
                                                 defaults.end());
  }

  std::vector<std::vector<std::string>> result;
  for (const auto& clause_value : clauses_value->as_array()) {
    if (!clause_value.is_object())
      continue;
    const auto& clause = clause_value.as_object();
    const auto* browse_path_value = clause.if_contains(kBrowsePath);
    if (!browse_path_value || !browse_path_value->is_array())
      continue;

    std::vector<std::string> path;
    for (const auto& segment_value : browse_path_value->as_array()) {
      if (!segment_value.is_object())
        continue;
      const auto& segment = segment_value.as_object();
      const auto* name_value = segment.if_contains(kName);
      if (!name_value || !name_value->is_string())
        continue;
      path.emplace_back(name_value->as_string().c_str());
    }
    if (!path.empty())
      result.push_back(std::move(path));
  }

  return NormalizeEventFieldPaths(std::move(result));
}

inline boost::json::value BuildEventFilter(
    std::span<const std::vector<std::string>> field_paths) {
  boost::json::array select_clauses;
  const auto normalized_field_paths =
      NormalizeEventFieldPaths(std::vector<std::vector<std::string>>(
          field_paths.begin(), field_paths.end()));
  for (const auto& field_path : normalized_field_paths) {
    boost::json::array browse_path;
    for (const auto& segment : field_path) {
      browse_path.emplace_back(boost::json::object{{"Name", segment}});
    }
    select_clauses.emplace_back(
        boost::json::object{{"BrowsePath", std::move(browse_path)}});
  }

  return boost::json::object{
      {"Type", "EventFilter"},
      {"Body",
       boost::json::object{{"SelectClauses", std::move(select_clauses)}}},
  };
}

inline std::vector<Variant> ProjectEventFields(
    const std::vector<std::vector<std::string>>& field_paths,
    const std::any& event) {
  // OPC UA Part 4 requires EventNotificationList.eventFields to follow the
  // exact selectClauses order for the MonitoredItem's EventFilter.
  const auto* source_event = std::any_cast<Event>(&event);
  std::vector<Variant> result;
  result.reserve(field_paths.size());

  for (const auto& field_path : field_paths) {
    if (!source_event || field_path.empty()) {
      result.emplace_back(Variant{});
      continue;
    }

    const auto& field_name = field_path.back();
    if (field_name == "EventId") {
      result.emplace_back(source_event->event_id);
    } else if (field_name == "EventType") {
      result.emplace_back(source_event->event_type_id);
    } else if (field_name == "SourceNode") {
      result.emplace_back(source_event->node_id);
    } else if (field_name == "SourceName") {
      result.emplace_back(source_event->node_id.is_null()
                              ? std::string{}
                              : source_event->node_id.ToString());
    } else if (field_name == "Time") {
      result.emplace_back(source_event->time);
    } else if (field_name == "Message") {
      result.emplace_back(source_event->message);
    } else if (field_name == "Severity") {
      result.emplace_back(source_event->severity);
    } else {
      result.emplace_back(Variant{});
    }
  }

  return result;
}

inline bool DispatchEventFieldNotification(
    const ReadValueId& item_to_monitor,
    const std::optional<scada::MonitoredItemHandler>& handler,
    std::span<const Variant> event_fields) {
  if (event_fields.empty())
    return false;
  return DispatchEventNotification(item_to_monitor, handler,
                                   StatusCode::Good,
                                   AssembleEvent(event_fields));
}

}  // namespace opcua
