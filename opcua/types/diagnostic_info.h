#pragma once

#include "opcua/types/basic_types.h"
#include "opcua/types/status.h"
#include "opcua/types/string.h"

#include <memory>
#include <optional>
#include <ostream>

namespace opcua {

// Built-in OPC UA DiagnosticInfo: vendor diagnostics attached to a StatusCode.
// Every field is optional, matching the encoding mask the Binary encoding
// writes; the four integer fields are indices into the enclosing response's
// ResponseHeader.stringTable rather than inline text. `inner_diagnostic_info`
// makes the structure recursive and is held behind a shared pointer so that
// DiagnosticInfo stays copyable and cheap to pass around. OPC UA Part 4 §7.12
// DiagnosticInfo,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/7.12
struct DiagnosticInfo {
  std::optional<Int32> symbolic_id;
  std::optional<Int32> namespace_uri;
  std::optional<Int32> locale;
  std::optional<Int32> localized_text;
  std::optional<String> additional_info;
  std::optional<Status> inner_status_code;
  std::shared_ptr<const DiagnosticInfo> inner_diagnostic_info;

  // True when nothing at all is set — the common case, encoded as a single
  // zero mask byte.
  bool empty() const {
    return !symbolic_id.has_value() && !namespace_uri.has_value() &&
           !locale.has_value() && !localized_text.has_value() &&
           !additional_info.has_value() && !inner_status_code.has_value() &&
           inner_diagnostic_info == nullptr;
  }

  // Compares by value, following the inner pointer rather than comparing
  // addresses.
  bool operator==(const DiagnosticInfo& other) const {
    if (symbolic_id != other.symbolic_id ||
        namespace_uri != other.namespace_uri || locale != other.locale ||
        localized_text != other.localized_text ||
        additional_info != other.additional_info ||
        inner_status_code != other.inner_status_code) {
      return false;
    }
    if (inner_diagnostic_info == other.inner_diagnostic_info)
      return true;
    if (inner_diagnostic_info == nullptr ||
        other.inner_diagnostic_info == nullptr)
      return false;
    return *inner_diagnostic_info == *other.inner_diagnostic_info;
  }
};

inline std::ostream& operator<<(std::ostream& stream,
                                const DiagnosticInfo& info) {
  stream << "{";
  if (info.symbolic_id.has_value())
    stream << "symbolic_id: " << *info.symbolic_id << ", ";
  if (info.additional_info.has_value())
    stream << "additional_info: \"" << *info.additional_info << "\", ";
  if (info.inner_status_code.has_value())
    stream << "inner_status_code: " << *info.inner_status_code;
  return stream << "}";
}

}  // namespace opcua
