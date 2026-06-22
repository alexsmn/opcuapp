#pragma once

#include "opcua/types/data_value.h"
#include "opcua/types/standard_node_ids.h"

#include <string>

namespace opcua {

// Convenience severity levels mapped onto the BaseEventType Severity field,
// which ranges 1..1000 (higher is more urgent). OPC UA Part 5 §6.4.2
// BaseEventType, https://reference.opcfoundation.org/Core/Part5/v105/docs/6.4.2
//
// sys event severities
enum EventSeverity : unsigned {
  kSeverityMin = 0,        // silent
  kSeverityVerbose = 20,   // verbose
  kSeverityNormal = 50,    // normal
  kSeverityWarning = 60,   // warning
  kSeverityCritical = 80,  // critical
  kSeverityMax = 100       // max
};

// Server-assigned unique identifier of an event instance (the BaseEventType
// EventId field). Cannot be zero. OPC UA Part 5 §6.4.2 BaseEventType,
// https://reference.opcfoundation.org/Core/Part5/v105/docs/6.4.2
using EventId = opcua::UInt64;

// An event instance carrying the standard BaseEventType fields (EventId,
// EventType, SourceNode, Time, ReceiveTime, Severity, Message, etc.) plus
// SCADA-specific acknowledgement and quality state. OPC UA Part 5 §6.4.2
// BaseEventType, https://reference.opcfoundation.org/Core/Part5/v105/docs/6.4.2
// TODO: Introduce an event ID and remove the ack ID.
struct Event {
 public:
  enum ChangeFlags {
    EVT_VAL = 0x0001,     // value changed
    EVT_QUAL = 0x0002,    // quality changed
    EVT_LIM = 0x0004,     // limit changed
    EVT_USER = 0x0008,    // user event
    EVT_SUBS = 0x0010,    // subsystem event
    EVT_MAN = 0x0020,     // manual input
    EVT_LOCK = 0x0040,    // locked
    EVT_CTRL = 0x0080,    // control
    EVT_BACKUP = 0x0100,  // locked
  };

  [[nodiscard]] bool is_valid() const;

  bool operator==(const Event&) const = default;

  NodeId event_type_id = opcua::id::SystemEventType;
  // `event_id` is zero until it's processed by server. And never can become
  // zero after that.
  EventId event_id = 0;
  // `time` cannot be null.
  DateTime time;
  // `receive_time` is assigned by server. It's null until the event is
  // processed by server.
  DateTime receive_time;
  opcua::UInt32 change_mask = 0;
  opcua::UInt32 severity = kSeverityNormal;
  // `node_id` can be null. TODO: Describe when it's null.
  // TODO: Rename to `source_node_id`.
  NodeId node_id;
  // `user_id` can be null.
  NodeId user_id;
  // `value` can be null.
  Variant value;
  Qualifier qualifier;
  // TODO: Require non-empty message.
  opcua::LocalizedText message;
  bool acked = false;
  // `acknowledged_time` must be non-null if `acked` is true.
  DateTime acknowledged_time;
  // `acknowledged_user_id` can be null if `acked` is true.
  NodeId acknowledged_user_id;
};

// Reports additions, deletions or modifications to the address space (the
// GeneralModelChangeEventType verb mask / Changes fields). OPC UA Part 5 §6.4
// Base EventTypes (GeneralModelChangeEventType); subsection not verified,
// parent section cited,
// https://reference.opcfoundation.org/Core/Part5/v105/docs/6.4
struct ModelChangeEvent {
  enum Verb : uint8_t {
    NodeAdded = 1 << 0,
    NodeDeleted = 1 << 1,
    ReferenceAdded = 1 << 2,
    ReferenceDeleted = 1 << 3,
    DataTypeChanged = 1 << 4,
  };

  ModelChangeEvent& set_verb(uint8_t verb) {
    this->verb = verb;
    return *this;
  }

  bool operator==(const ModelChangeEvent&) const = default;

  NodeId node_id;

  // |type_definition_id| is only set for |NodeAdded| event.
  NodeId type_definition_id;

  uint8_t verb = 0;

  static const NumericId event_type_id = id::GeneralModelChangeEventType;
};

// Reports that the semantics of a Variable's value have changed (the
// SemanticChangeEventType Changes field). OPC UA Part 5 §6.4 Base EventTypes
// (SemanticChangeEventType); subsection not verified, parent section cited,
// https://reference.opcfoundation.org/Core/Part5/v105/docs/6.4
struct SemanticChangeEvent {
  bool operator==(const SemanticChangeEvent&) const = default;

  NodeId node_id;

  static const NumericId event_type_id = id::SemanticChangeEventType;
};

inline bool Event::is_valid() const {
  if (event_id == 0 || time.is_null() || receive_time.is_null()) {
    return false;
  }

  if (acked) {
    if (acknowledged_time.is_null()) {
      return false;
    }
  } else {
    if (!acknowledged_time.is_null() || !acknowledged_user_id.is_null()) {
      return false;
    }
  }

  return true;
}

std::ostream& operator<<(std::ostream& stream, const Event& event);
std::ostream& operator<<(std::ostream& stream, const ModelChangeEvent& e);
std::ostream& operator<<(std::ostream& stream, const SemanticChangeEvent& e);

}  // namespace opcua
