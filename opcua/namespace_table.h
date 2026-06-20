#pragma once

#include "opcua/scada/basic_types.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace opcua::scada {
class Variant;
}  // namespace opcua::scada

namespace opcua {

// Maps OPC UA namespace indices to URIs, as read from a server's
// Server_NamespaceArray (node ns=0;i=2255). Index 0 is always the OPC UA
// standard namespace ("http://opcfoundation.org/UA/"). A client uses this to
// translate between the namespace URIs it knows and the indices a particular
// server assigns, which differ between servers (OPC UA Part 3 §8.2.4).
class NamespaceTable {
 public:
  NamespaceTable() = default;
  explicit NamespaceTable(std::vector<std::string> uris)
      : uris_{std::move(uris)} {}

  // Builds a table from a Server_NamespaceArray Value variant (a String[]).
  // Returns an empty table if the variant is not a string array.
  [[nodiscard]] static NamespaceTable FromVariant(const scada::Variant& value);

  [[nodiscard]] const std::vector<std::string>& uris() const { return uris_; }
  [[nodiscard]] bool empty() const { return uris_.empty(); }
  [[nodiscard]] std::size_t size() const { return uris_.size(); }

  // Index of `uri`, or nullopt if the server does not publish it.
  [[nodiscard]] std::optional<scada::NamespaceIndex> IndexForUri(
      std::string_view uri) const;

  // URI registered at `index`, or an empty view if the index is out of range.
  [[nodiscard]] std::string_view UriForIndex(scada::NamespaceIndex index) const;

 private:
  std::vector<std::string> uris_;
};

}  // namespace opcua
