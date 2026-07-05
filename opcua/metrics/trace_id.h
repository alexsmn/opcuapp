#pragma once

#include <string>
#include <string_view>

namespace opcua {

using TraceId = std::string;
using TraceSpanId = std::string;

// True when `value` is a version-00 W3C traceparent
// (https://www.w3.org/TR/trace-context/#traceparent-header):
// `00-<32 lowercase hex>-<16 lowercase hex>-<2 lowercase hex>`. Client
// sessions only inject a context's trace id into the request header when it
// passes this check, so legacy UUID trace ids never reach the wire header.
inline bool IsW3CTraceParent(std::string_view value) {
  constexpr size_t kLength = 2 + 1 + 32 + 1 + 16 + 1 + 2;
  if (value.size() != kLength || value[0] != '0' || value[1] != '0' ||
      value[2] != '-' || value[35] != '-' || value[52] != '-') {
    return false;
  }
  auto is_hex = [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  };
  for (size_t i = 3; i < 35; ++i) {
    if (!is_hex(value[i]))
      return false;
  }
  for (size_t i = 36; i < 52; ++i) {
    if (!is_hex(value[i]))
      return false;
  }
  return is_hex(value[53]) && is_hex(value[54]);
}

}  // namespace opcua
