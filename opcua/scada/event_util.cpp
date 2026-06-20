#include "opcua/scada/event_util.h"

#include "opcua/scada/event.h"

namespace opcua::scada {

opcua::scada::Event AssembleSystemEvent(std::span<const opcua::scada::Variant> fields) {
  opcua::scada::Event event;
  fields[0].get(event.event_id);
  fields[1].get(event.event_type_id);
  fields[2].get(event.time);
  fields[3].get(event.change_mask);
  fields[4].get(event.severity);
  fields[5].get(event.node_id);
  fields[6].get(event.user_id);
  event.value = fields[7];
  event.qualifier = opcua::scada::Qualifier{fields[8].get_or<unsigned>(0)};
  fields[9].get(event.message);
  fields[10].get(event.acked);
  fields[11].get(event.acknowledged_time);
  fields[12].get(event.acknowledged_user_id);
  return event;
}

opcua::scada::ModelChangeEvent AssembleModelChangeEvent(
    std::span<const opcua::scada::Variant> fields) {
  opcua::scada::ModelChangeEvent event;
  fields[1].get(event.node_id);
  fields[2].get(event.type_definition_id);
  fields[3].get(event.verb);
  return event;
}

opcua::scada::SemanticChangeEvent AssembleSemanticChangeEvent(
    std::span<const opcua::scada::Variant> fields) {
  opcua::scada::SemanticChangeEvent event;
  fields[1].get(event.node_id);
  return event;
}

std::any AssembleEvent(std::span<const opcua::scada::Variant> fields) {
  assert(!fields.empty());
  if (fields.empty())
    return {};

  auto event_type_id = fields[0].get_or<opcua::scada::NodeId>({});
  if (event_type_id == opcua::scada::id::SystemEventType) {
    return AssembleSystemEvent(fields);
  } else if (event_type_id == opcua::scada::id::GeneralModelChangeEventType) {
    return AssembleModelChangeEvent(fields);
  } else if (event_type_id == opcua::scada::id::SemanticChangeEventType) {
    return AssembleSemanticChangeEvent(fields);
  } else {
    assert(false);
    return {};
  }
}

std::vector<opcua::scada::Variant> DisassembleEvent(const opcua::scada::Event& event) {
  return {
      event.event_id,
      event.event_type_id,
      event.time,
      event.change_mask,
      event.severity,
      event.node_id,
      event.user_id,
      event.value,
      event.qualifier.raw(),
      event.message,
      event.acked,
      event.acknowledged_time,
      event.acknowledged_user_id,
  };
}

std::vector<opcua::scada::Variant> DisassembleEvent(
    const opcua::scada::ModelChangeEvent& event) {
  return {
      opcua::scada::NodeId{event.event_type_id},
      event.node_id,
      event.type_definition_id,
      event.verb,
  };
}

std::vector<opcua::scada::Variant> DisassembleEvent(
    const opcua::scada::SemanticChangeEvent& event) {
  return {
      opcua::scada::NodeId{event.event_type_id},
      event.node_id,
  };
}

std::vector<opcua::scada::Variant> DisassembleEvent(const std::any& event) {
  assert(event.has_value());
  if (auto* system_event = std::any_cast<opcua::scada::Event>(&event)) {
    return DisassembleEvent(*system_event);
  } else if (auto* model_change_event =
                 std::any_cast<opcua::scada::ModelChangeEvent>(&event)) {
    return DisassembleEvent(*model_change_event);
  } else if (auto* semantic_change_event =
                 std::any_cast<opcua::scada::SemanticChangeEvent>(&event)) {
    return DisassembleEvent(*semantic_change_event);
  } else {
    assert(false);
    return {};
  }
}

}  // namespace opcua::scada
