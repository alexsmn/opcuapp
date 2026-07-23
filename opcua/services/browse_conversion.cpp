#include "opcua/services/browse_conversion.h"

#include <utility>

namespace opcua {

BrowseDescription ToHandWritten(const ua::BrowseDescription& description) {
  return {
      .node_id = description.node_id,
      .direction = static_cast<BrowseDirection>(description.browse_direction),
      .reference_type_id = description.reference_type_id,
      .include_subtypes = description.include_subtypes,
      .node_class_mask = description.node_class_mask,
      .result_mask = description.result_mask,
  };
}

ua::BrowseDescription ToGenerated(const BrowseDescription& description) {
  return {
      .node_id = description.node_id,
      .browse_direction =
          static_cast<ua::BrowseDirection>(description.direction),
      .reference_type_id = description.reference_type_id,
      .include_subtypes = description.include_subtypes,
      .node_class_mask = description.node_class_mask,
      .result_mask = description.result_mask,
  };
}

ReferenceDescription ToHandWritten(const ua::ReferenceDescription& reference) {
  return {
      .reference_type_id = reference.reference_type_id,
      .forward = reference.is_forward,
      .node_id = reference.node_id.node_id(),
      .node_class = static_cast<NodeClass>(reference.node_class),
      .browse_name = reference.browse_name,
      .display_name = reference.display_name,
      .type_definition = reference.type_definition.node_id(),
  };
}

ua::ReferenceDescription ToGenerated(const ReferenceDescription& reference) {
  return {
      .reference_type_id = reference.reference_type_id,
      .is_forward = reference.forward,
      .node_id = ExpandedNodeId{reference.node_id},
      .browse_name = reference.browse_name,
      .display_name = reference.display_name,
      .node_class = static_cast<ua::NodeClass>(reference.node_class),
      .type_definition = ExpandedNodeId{reference.type_definition},
  };
}

BrowseResult ToHandWritten(const ua::BrowseResult& result) {
  BrowseResult out;
  out.status_code = result.status_code.code();
  out.continuation_point = result.continuation_point;
  out.references.reserve(result.references.size());
  for (const auto& reference : result.references)
    out.references.push_back(ToHandWritten(reference));
  return out;
}

ua::BrowseResult ToGenerated(const BrowseResult& result) {
  ua::BrowseResult out;
  out.status_code = Status{result.status_code};
  out.continuation_point = result.continuation_point;
  out.references.reserve(result.references.size());
  for (const auto& reference : result.references)
    out.references.push_back(ToGenerated(reference));
  return out;
}

namespace {

RelativePathElement ToHandWritten(const ua::RelativePathElement& element) {
  return RelativePathElement{.reference_type_id = element.reference_type_id,
                             .inverse = element.is_inverse,
                             .include_subtypes = element.include_subtypes,
                             .target_name = element.target_name};
}

ua::RelativePathElement ToGenerated(const RelativePathElement& element) {
  return ua::RelativePathElement{.reference_type_id = element.reference_type_id,
                                 .is_inverse = element.inverse,
                                 .include_subtypes = element.include_subtypes,
                                 .target_name = element.target_name};
}

BrowsePathTarget ToHandWritten(const ua::BrowsePathTarget& target) {
  return BrowsePathTarget{.target_id = target.target_id,
                          .remaining_path_index = target.remaining_path_index};
}

ua::BrowsePathTarget ToGenerated(const BrowsePathTarget& target) {
  return ua::BrowsePathTarget{
      .target_id = target.target_id,
      .remaining_path_index =
          static_cast<std::uint32_t>(target.remaining_path_index)};
}

}  // namespace

BrowsePath ToHandWritten(const ua::BrowsePath& path) {
  BrowsePath out;
  out.node_id = path.starting_node;
  out.relative_path.reserve(path.relative_path.elements.size());
  for (const auto& element : path.relative_path.elements)
    out.relative_path.push_back(ToHandWritten(element));
  return out;
}

ua::BrowsePath ToGenerated(const BrowsePath& path) {
  ua::BrowsePath out;
  out.starting_node = path.node_id;
  out.relative_path.elements.reserve(path.relative_path.size());
  for (const auto& element : path.relative_path)
    out.relative_path.elements.push_back(ToGenerated(element));
  return out;
}

BrowsePathResult ToHandWritten(const ua::BrowsePathResult& result) {
  BrowsePathResult out;
  out.status_code = result.status_code.code();
  out.targets.reserve(result.targets.size());
  for (const auto& target : result.targets)
    out.targets.push_back(ToHandWritten(target));
  return out;
}

ua::BrowsePathResult ToGenerated(const BrowsePathResult& result) {
  ua::BrowsePathResult out;
  out.status_code = Status{result.status_code};
  out.targets.reserve(result.targets.size());
  for (const auto& target : result.targets)
    out.targets.push_back(ToGenerated(target));
  return out;
}

}  // namespace opcua
