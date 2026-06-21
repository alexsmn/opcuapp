#pragma once

#include "opcua/scada/variant.h"

#include <any>
#include <span>

namespace opcua {

// Converts between an event object and the flat list of selected event-field
// Variants carried in an EventFieldList notification. `AssembleEvent` builds an
// event from its fields; `DisassembleEvent` projects an event back onto the
// field list. OPC UA Part 4 §7.25 NotificationData,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/7.25
std::any AssembleEvent(std::span<const opcua::Variant> fields);
std::vector<opcua::Variant> DisassembleEvent(const std::any& event);

}  // namespace opcua (vendored)
