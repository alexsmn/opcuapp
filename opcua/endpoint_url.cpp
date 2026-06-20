#include "opcua/endpoint_url.h"

#include <cctype>
#include <cstddef>

namespace opcua {
namespace {

constexpr std::size_t kOpcTcpPrefixLen = 10;  // "opc.tcp://"

}  // namespace

ParsedEndpointUrl ParseOpcTcpUrl(const std::string& url) {
  ParsedEndpointUrl result;
  if (url.size() <= kOpcTcpPrefixLen ||
      url.compare(0, kOpcTcpPrefixLen, "opc.tcp://") != 0) {
    return result;
  }
  const auto authority_start = kOpcTcpPrefixLen;
  const auto path_pos = url.find('/', authority_start);
  const std::string authority =
      url.substr(authority_start, path_pos == std::string::npos
                                      ? std::string::npos
                                      : path_pos - authority_start);
  const auto colon_pos = authority.rfind(':');
  if (colon_pos == std::string::npos) {
    result.host = authority;
    result.valid = !result.host.empty();
    return result;
  }
  result.host = authority.substr(0, colon_pos);
  const auto port_str = authority.substr(colon_pos + 1);
  if (result.host.empty() || port_str.empty()) {
    return result;
  }
  int port = 0;
  for (char c : port_str) {
    if (!std::isdigit(static_cast<unsigned char>(c))) {
      return result;
    }
    port = port * 10 + (c - '0');
  }
  result.port = port;
  result.valid = true;
  return result;
}

}  // namespace opcua
