#pragma once

#include "opcua/types/node_id.h"
#include "opcua/types/status.h"
#include "opcua/types/variant.h"

#include <vector>

namespace opcua {

// The payload of a successful Call. Operation-level failure is reported by the
// enclosing StatusOr, not by a field here, so a value being present already
// means the method succeeded.
//
// `input_argument_results` is deliberately absent even though the wire type
// carries it: OPC UA Part 4 §5.11.2 populates it only when the operation status
// is Bad_InvalidArgument — precisely the case where the StatusOr holds an error
// and no value, so a field here could never be reached. The wire type still
// carries it and ClientProtocolSession still decodes it, for any future
// consumer that needs the per-argument detail.
struct CallResult {
  std::vector<Variant> output_arguments;
};

}  // namespace opcua
