#pragma once

#include "opcua/scada/variant.h"

#include <any>
#include <span>

namespace opcua::scada {

std::any AssembleEvent(std::span<const opcua::scada::Variant> fields);
std::vector<opcua::scada::Variant> DisassembleEvent(const std::any& event);

}  // namespace opcua::scada
