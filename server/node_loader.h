#pragma once

#include "opcua/server/node_state.h"
#include "opcua/string_table.h"

#include <istream>
#include <vector>

namespace opcua {
namespace server {

std::vector<NodeState> LoadPredefinedNodes(const StringTable& namespace_uris, std::istream& stream);

} // namespace server
} // namespace opcua
