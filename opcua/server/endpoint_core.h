#pragma once

#include "opcua/base/no_destructor.h"
#include "opcua/events/event.h"
#include "opcua/events/event_util.h"
#include "opcua/services/attribute_types.h"
#include "opcua/services/service_context.h"

#include <boost/json/value.hpp>

#include <optional>
#include <span>
#include <string_view>

namespace opcua {

inline ServiceContext MakeServiceContext(const NodeId& user_id,
                                         ServiceContext base_context = {}) {
  return base_context.with_user_id(user_id);
}

inline DataValue NormalizeReadResult(DataValue result) {
  constexpr unsigned kBadNodeIdUnknownFullCode = 0x80340000u;
  if (result.status_code == StatusCode::Bad_WrongNodeId) {
    result.status_code = Status::FromFullCode(kBadNodeIdUnknownFullCode).code();
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

// Rebuilds an `opcua::Event` (wrapped in std::any) from event fields that were
// projected by `ProjectEventFields` using the same `field_paths`. This is the
// inverse of `ProjectEventFields`: each field is assigned back onto the Event
// by matching the trailing browse-path segment name. Fields whose name is not
// one of the recognized BaseEventType fields are ignored. Used to reconstruct
// the std::any an in-process `EventHandler` expects after the event crossed the
// standard-typed notification boundary as an `EventFieldList`.
inline std::any BuildEventFromFields(
    const std::vector<std::vector<std::string>>& field_paths,
    std::span<const Variant> event_fields) {
  Event event;
  const std::size_t count = std::min(field_paths.size(), event_fields.size());
  for (std::size_t i = 0; i < count; ++i) {
    if (field_paths[i].empty())
      continue;
    const auto& field_name = field_paths[i].back();
    const auto& field = event_fields[i];
    if (field_name == "EventId") {
      field.get(event.event_id);
    } else if (field_name == "EventType") {
      field.get(event.event_type_id);
    } else if (field_name == "SourceNode") {
      field.get(event.node_id);
    } else if (field_name == "Time") {
      field.get(event.time);
    } else if (field_name == "Message") {
      field.get(event.message);
    } else if (field_name == "Severity") {
      field.get(event.severity);
    }
    // "SourceName" is derived from node_id during projection and has no
    // dedicated Event field; nothing to assign back.
  }
  return std::any{std::move(event)};
}

}  // namespace opcua
