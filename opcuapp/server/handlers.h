#pragma once

#include <functional>

namespace opcua {
namespace server {

using ReadCallback = std::function<void(OpcUa_ReadResponse& response)>;
using ReadHandler = std::function<void(OpcUa_ReadRequest& request, const ReadCallback& callback)>;

using BrowseCallback = std::function<void(OpcUa_BrowseResponse& response)>;
using BrowseHandler = std::function<void(OpcUa_BrowseRequest& request, const BrowseCallback& callback)>;

} // namespace server
} // namespace opcua
