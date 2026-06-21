#pragma once

#include "opcua/scada/variant.h"

#include <any>
#include <span>

namespace opcua {

std::any AssembleEvent(std::span<const opcua::Variant> fields);
std::vector<opcua::Variant> DisassembleEvent(const std::any& event);

}  // namespace opcua (vendored)
