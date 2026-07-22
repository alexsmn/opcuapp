#pragma once

#include "opcua/types/basic_types.h"
#include "opcua/types/node_id.h"
#include "opcua/types/status.h"
#include "opcua/ua/ua_types.h"

#include <string>
#include <string_view>

// Reconciles opcuapp's envelope-based request/response headers with the
// generated message types, which — following the spec — embed the full
// RequestHeader/ResponseHeader as the message's first field.
//
// opcuapp historically carried the header outside the service body (the
// binary::ServiceRequestHeader envelope, the response's request-handle +
// Status), so the hand-written per-service codec wrote the header separately.
// A service whose body is a generated type instead lets the generated codec
// write header + body as one spec-conformant message; these helpers move the
// envelope fields into and out of the embedded header. They are deliberately
// free of any dependency on the binary transport so opcua/ua stays
// self-contained.
namespace opcua::ua {

// Builds the embedded RequestHeader from the envelope fields. Encoding the
// result reproduces the hand-written AppendRequestHeader byte-for-byte: the
// unused fields (timestamp, returnDiagnostics, auditEntryId, timeoutHint) are
// left at the zero values that code always wrote, and a non-empty
// `trace_parent` becomes an AdditionalParametersType {"traceparent": ...}
// ExtensionObject in additionalHeader (OPC UA Part 4 §7.33 RequestHeader,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/7.33).
RequestHeader MakeRequestHeader(const NodeId& authentication_token,
                                UInt32 request_handle,
                                std::string_view trace_parent);

// Recovers the W3C traceparent carried in a decoded RequestHeader's
// additionalHeader, or empty if absent.
std::string GetTraceParent(const RequestHeader& header);

// Builds the embedded ResponseHeader. Encoding the result reproduces the
// hand-written AppendResponseHeader byte-for-byte.
ResponseHeader MakeResponseHeader(UInt32 request_handle, Status service_result);

}  // namespace opcua::ua
