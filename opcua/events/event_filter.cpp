#include "opcua/events/event_filter.h"

#include "opcua/base/debug_util.h"
#include "opcua/base/no_destructor.h"
#include "opcua/base/struct_writer.h"

#include <algorithm>
#include <string_view>

namespace opcua {

std::ostream& operator<<(std::ostream& stream,
                         const EventFilter& event_filter) {
  constexpr std::string_view kTypeBitStrings[] = {"ACKED", "UNACKED"};

  StructWriter{stream}
      .AddBitMaskField("types", event_filter.types, kTypeBitStrings)
      .AddField("of_type", event_filter.of_type)
      .AddField("child_of", event_filter.child_of);

  return stream;
}

const std::vector<std::vector<std::string>>& DefaultEventFieldPaths() {
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

std::vector<std::vector<std::string>> NormalizeEventFieldPaths(
    std::vector<std::vector<std::string>> field_paths) {
  if (!field_paths.empty()) {
    return field_paths;
  }
  const auto& defaults = DefaultEventFieldPaths();
  return std::vector<std::vector<std::string>>(defaults.begin(),
                                               defaults.end());
}

std::vector<std::vector<std::string>> ParseEventFilterFieldPaths(
    const boost::json::value& raw_filter) {
  constexpr std::string_view kEventFilterBody = "Body";
  constexpr std::string_view kSelectClauses = "SelectClauses";
  constexpr std::string_view kBrowsePath = "BrowsePath";
  constexpr std::string_view kName = "Name";

  if (!raw_filter.is_object()) {
    return NormalizeEventFieldPaths({});
  }

  const auto* current = &raw_filter.as_object();
  if (const auto* body_field = current->if_contains(kEventFilterBody);
      body_field != nullptr && body_field->is_object()) {
    current = &body_field->as_object();
  }

  const auto* clauses_value = current->if_contains(kSelectClauses);
  if (!clauses_value || !clauses_value->is_array()) {
    return NormalizeEventFieldPaths({});
  }

  std::vector<std::vector<std::string>> result;
  for (const auto& clause_value : clauses_value->as_array()) {
    if (!clause_value.is_object()) {
      continue;
    }
    const auto& clause = clause_value.as_object();
    const auto* browse_path_value = clause.if_contains(kBrowsePath);
    if (!browse_path_value || !browse_path_value->is_array()) {
      continue;
    }

    std::vector<std::string> path;
    for (const auto& segment_value : browse_path_value->as_array()) {
      if (!segment_value.is_object()) {
        continue;
      }
      const auto& segment = segment_value.as_object();
      const auto* name_value = segment.if_contains(kName);
      if (!name_value || !name_value->is_string()) {
        continue;
      }
      path.emplace_back(name_value->as_string().c_str());
    }
    if (!path.empty()) {
      result.push_back(std::move(path));
    }
  }

  return NormalizeEventFieldPaths(std::move(result));
}

boost::json::value BuildEventFilter(
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

std::vector<Variant> ProjectEventFields(
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

}  // namespace opcua
