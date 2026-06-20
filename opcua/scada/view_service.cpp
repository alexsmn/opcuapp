#include "opcua/scada/view_service.h"

#include "opcua/base/debug_util.h"


namespace opcua::scada {

std::ostream& operator<<(std::ostream& stream, BrowseDirection v) {
  std::string_view name;

  switch (v) {
    case BrowseDirection::Forward:
      name = "Forward";
      break;
    case BrowseDirection::Inverse:
      name = "Inverse";
      break;
    case BrowseDirection::Both:
      name = "Both";
      break;
    default:
      assert(false);
      break;
  }

  return stream << name;
}

std::ostream& operator<<(std::ostream& stream, const BrowseDescription& v) {
  return stream << "{node_id: " << v.node_id << ", direction: " << v.direction
                << ", reference_type_id: " << v.reference_type_id
                << ", include_subtypes: " << v.include_subtypes << "}";
}

std::ostream& operator<<(std::ostream& stream, const ReferenceDescription& v) {
  return stream << "{reference_type_id: " << v.reference_type_id << ", "
                << "forward: " << v.forward << ", "
                << "node_id: " << v.node_id << "}";
}

std::ostream& operator<<(std::ostream& stream, const BrowseResult& v) {
  using ::operator<<;
  return stream << "{status_code: " << v.status_code
                << ", references: " << v.references << "}";
}

std::ostream& operator<<(std::ostream& stream, const RelativePathElement& v) {
  return stream << "{reference_type_id: " << v.reference_type_id
                << ", inverse: " << v.inverse << ", "
                << ", include_subtypes: " << v.include_subtypes << ", "
                << ", target_name: " << v.target_name << "}";
}

std::ostream& operator<<(std::ostream& stream, const BrowsePath& v) {
  using ::operator<<;
  return stream << "{node_id: " << v.node_id
                << ", relative_path: " << v.relative_path << "}";
}

std::ostream& operator<<(std::ostream& stream, const BrowsePathTarget& v) {
  return stream << "{target_id: " << v.target_id
                << ", remaining_path_index: " << v.remaining_path_index << "}";
}

std::ostream& operator<<(std::ostream& stream, const BrowsePathResult& v) {
  using ::operator<<;
  return stream << "{status_code: " << v.status_code
                << ", targets: " << v.targets << "}";
}

}  // namespace opcua::scada
