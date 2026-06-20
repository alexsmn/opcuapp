#include "opcua/namespace_table.h"

#include "opcua/scada/variant.h"

namespace opcua {

NamespaceTable NamespaceTable::FromVariant(const scada::Variant& value) {
  if (!value.is_array()) {
    return {};
  }
  const auto* uris = value.get_if<std::vector<scada::String>>();
  if (!uris) {
    return {};
  }
  return NamespaceTable{*uris};
}

std::optional<scada::NamespaceIndex> NamespaceTable::IndexForUri(
    std::string_view uri) const {
  for (std::size_t i = 0; i < uris_.size(); ++i) {
    if (uris_[i] == uri) {
      return static_cast<scada::NamespaceIndex>(i);
    }
  }
  return std::nullopt;
}

std::string_view NamespaceTable::UriForIndex(
    scada::NamespaceIndex index) const {
  if (index < uris_.size()) {
    return uris_[index];
  }
  return {};
}

}  // namespace opcua
