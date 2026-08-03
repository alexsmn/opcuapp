// Compiles the generated OPC UA type headers into the library — without a
// translation unit that includes them, a schema bump that produces
// non-compiling C++ would only be caught by whoever first used the new types.
//
// The assertions below pin the generated output against facts the hand-written
// codec already depends on. They are cheap, and they are the reason a wrong
// encoding id cannot reach the wire the way it did before code generation:
// every one of these was previously a hand-typed constant in service_codec.cpp.

#include "opcua/ua/ua_types.h"
#include "opcua/ua/ua_encoding_ids.h"
#include "opcua/ua/ua_status_codes.h"

#include "opcua/types/status.h"

namespace opcua::ua {
namespace {

// Message ids are the ids of the *_Encoding_DefaultBinary DataTypeEncoding
// Nodes, not of the DataType Nodes — for a service the two differ by two, and
// getting it wrong makes conforming clients report an unsupported service
// (OPC UA Part 6 §5.2.2.15,
// https://reference.opcfoundation.org/Core/Part6/v105/docs/5.2.2.15).
static_assert(binary_encoding_id::kReadRequest == 631);
static_assert(binary_encoding_id::kReadResponse == 634);
static_assert(binary_encoding_id::kBrowseRequest == 527);
static_assert(binary_encoding_id::kBrowseResponse == 530);
static_assert(binary_encoding_id::kCreateSessionRequest == 461);
static_assert(binary_encoding_id::kPublishResponse == 829);
static_assert(binary_encoding_id::kServiceFault == 397);

// The JSON encoding has its own, unrelated ids.
static_assert(json_encoding_id::kReadRequest == 15257);

// StatusCodes are full 32-bit values here, matching what goes on the wire and
// what opcua::Status stores.
static_assert(status_code::kGood == 0);
static_assert(status_code::kBadNodeIdUnknown ==
              Status{StatusCode::Bad_NodeIdUnknown}.full_code());
static_assert(status_code::kBadUserAccessDenied ==
              Status{StatusCode::Bad_UserAccessDenied}.full_code());
static_assert(status_code::kBadLicenseExpired ==
              Status{StatusCode::Bad_LicenseExpired}.full_code());

// Field-level shape checks on a service the hand-written codec also encodes.
// These catch a schema that silently reorders or drops a field: the members
// exist, and the ones the old hand-written types dropped (MaxAge, IndexRange,
// DataEncoding) are present.
static_assert(sizeof(ReadRequest) > 0);
static_assert(std::is_same_v<decltype(ReadRequest::max_age), Double>);
static_assert(std::is_same_v<decltype(ReadRequest::nodes_to_read),
                             std::vector<ReadValueId>>);
static_assert(std::is_same_v<decltype(ReadValueId::index_range), String>);
static_assert(
    std::is_same_v<decltype(ReadValueId::data_encoding), QualifiedName>);
static_assert(
    std::is_same_v<decltype(ReadResponse::results), std::vector<DataValue>>);

}  // namespace
}  // namespace opcua::ua
