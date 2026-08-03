#pragma once

#include <string>

namespace opcua {

// Result of parsing an "opc.tcp://" endpoint URL into the host/port a
// transport::TransportString needs.
struct ParsedEndpointUrl {
  std::string host;
  int port = 4840;  // OPC UA default opc.tcp port.
  bool valid = false;
};

// Parses "opc.tcp://host[:port][/path]". Any "/path" suffix is ignored (the
// path identifies a server endpoint name, tracked separately, not the TCP
// target). Returns {valid=false} for any non-opc.tcp or malformed input.
[[nodiscard]] ParsedEndpointUrl ParseOpcTcpUrl(const std::string& url);

}  // namespace opcua
