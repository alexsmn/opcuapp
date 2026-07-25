#pragma once

#include "opcua/types/basic_types.h"
#include "opcua/types/node_id.h"
#include "opcua/types/status.h"
#include "opcua/types/variant.h"
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

// --- The additionalHeader extension slot -----------------------------------
//
// RequestHeader.additionalHeader is the spec's per-request extension point —
// "reserved for future use", to be ignored by applications that do not
// understand it (OPC UA Part 4 §7.33 RequestHeader,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/7.33). opcuapp
// carries every extension in it as one standard AdditionalParametersType
// key/value list, so there is a single mechanism rather than one per feature.
// The W3C traceparent is the only extension that rides here today.
//
// Everything read out of the slot is treated as untrusted and optional. An
// unknown extension type, a wrong value type, a truncated array — all yield
// "absent" rather than a decode failure, because a request must never be
// rejected over an extension its sender was free to omit.

// Reads the parameter list out of a decoded header, or an empty list when the
// slot is empty or holds an extension this stack does not understand.
AdditionalParametersType GetAdditionalParameters(const RequestHeader& header);

// Stores a parameter list into the slot. An empty list clears it, so a request
// that carries no extension encodes exactly as it did before the slot was
// used — an important property for the byte-for-byte header goldens and for
// peers that predate a given extension.
void SetAdditionalParameters(RequestHeader& header,
                             const AdditionalParametersType& parameters);

// Adds `value` under `key` (namespace 0), replacing any existing entry with
// that key and leaving the other entries — and their order — alone. Rewriting
// the slot drops an extension this stack cannot decode, which is only ever the
// case for a header it did not build itself.
void SetAdditionalParameter(RequestHeader& header,
                            std::string_view key,
                            Variant value);

// The value stored under `key`, or nullptr when absent.
const Variant* FindAdditionalParameter(
    const AdditionalParametersType& parameters,
    std::string_view key);

// --- Request/response headers ----------------------------------------------

// Fills the envelope fields (authentication token, request handle, W3C
// traceparent) into a header that may already carry extension parameters, and
// preserves them. This is the form the encode path uses: a service body builds
// its own extensions into `request_header` before handing the message to the
// transport, which then stamps the envelope on top without discarding them.
//
// The unused fields (timestamp, returnDiagnostics, auditEntryId, timeoutHint)
// are left at the zero values the hand-written AppendRequestHeader always
// wrote, so an extension-free header still encodes byte-for-byte identically.
void ApplyRequestEnvelope(RequestHeader& header,
                          const NodeId& authentication_token,
                          UInt32 request_handle,
                          std::string_view trace_parent);

// `ApplyRequestEnvelope` onto a fresh header.
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
