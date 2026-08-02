#pragma once

#include "opcua/types/basic_types.h"
#include "opcua/types/node_id.h"
#include "opcua/types/status.h"
#include "opcua/types/variant.h"
#include "opcua/ua/ua_types.h"

#include <optional>
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
//
// Trace context rides here under two keys at once, and that duplication is
// deliberate — see "Trace context" below.
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

// --- Trace context ----------------------------------------------------------
//
// Two keys carry the same trace context, and both are written on every
// outbound request:
//
//   "traceparent"  -> String, the W3C form
//   (https://www.w3.org/TR/trace-context/) "SpanContext"  ->
//   SpanContextDataType, the OPC UA form (OPC UA Part 26
//                     §5.6.4,
//                     https://reference.opcfoundation.org/Core/Part26/v105/docs/5.6.4)
//
// Part 26 §5.6.4 prescribes exactly this carrier — the SpanContextDataType
// "transported with the AdditionalParametersType in the AdditionalHeader field
// of the RequestHeader" under the key "SpanContext" — so a conformant peer
// finds what it expects. The W3C entry stays because it is the only one of the
// two that can express the sampled flag: SpanContextDataType is
// {Guid TraceId; UInt64 SpanId} (Part 26 §5.6.2 Table 11) with no trace-flags
// field, and dropping it would silently disable downstream sampling decisions.
// Carrying both is free: additionalHeader is a list, and Part 4 §7.33 requires
// a peer to ignore keys it does not understand.
//
// The mapping between the two forms is *not* specified by Part 26 — the
// specification says only that a TraceId is a Guid and never states a byte
// order. These functions therefore fix a convention, chosen so that it is
// lossless, reversible, and legible in a packet capture:
//
//   TraceId — the 32 hex digits of the W3C trace id are the Guid's canonical
//     8-4-4-4-12 text form (OPC UA Part 6 §5.1.3 Guid,
//     https://reference.opcfoundation.org/Core/Part6/v105/docs/5.1.3). Note a
//     Guid is not an opaque 16-byte array: Part 3 §8.14 gives it four fields
//     whose binary encoding is field-by-field (Part 6 §5.2.2.6), so a raw-byte
//     reading would silently transpose the first eight bytes.
//   SpanId — the 16 hex digits of the W3C span id read as a big-endian UInt64.

// The SpanContextDataType equivalent of a W3C traceparent string, or nullopt
// when `trace_parent` is not a valid version-00 traceparent or carries ids
// Part 26 rejects (a null TraceId, or the invalid SpanId 0 — Table 11).
std::optional<SpanContextDataType> TraceParentToSpanContext(
    std::string_view trace_parent);

// The W3C traceparent equivalent of a SpanContextDataType, or empty when the
// structure carries ids that cannot be represented (null TraceId or zero
// SpanId). The sampled flag is set: Part 26 has no notion of sampling, so a
// peer that sent a SpanContext had no way to express one, and defaulting to
// "not sampled" would make a ParentBased sampler discard every trace such a
// peer starts.
std::string SpanContextToTraceParent(const SpanContextDataType& span_context);

// --- Request/response headers ----------------------------------------------

// Fills the envelope fields (authentication token, request handle, trace
// context) into a header that may already carry extension parameters, and
// preserves them. This is the form the encode path uses: a service body builds
// its own extensions into `request_header` before handing the message to the
// transport, which then stamps the envelope on top without discarding them.
//
// The unused fields (timestamp, returnDiagnostics, auditEntryId, timeoutHint)
// are left at the zero values the hand-written AppendRequestHeader always
// wrote, so an extension-free header still encodes byte-for-byte identically.
// A non-empty `trace_parent` is always written under the W3C key, verbatim;
// one that parses as a traceparent is additionally written under Part 26's.
void ApplyRequestEnvelope(RequestHeader& header,
                          const NodeId& authentication_token,
                          UInt32 request_handle,
                          std::string_view trace_parent);

// `ApplyRequestEnvelope` onto a fresh header.
RequestHeader MakeRequestHeader(const NodeId& authentication_token,
                                UInt32 request_handle,
                                std::string_view trace_parent);

// Recovers the trace context carried in a decoded RequestHeader's
// additionalHeader as a W3C traceparent, or empty if absent.
//
// A well-formed "traceparent" entry wins, because it is the only one of the
// two carrying the sampled flag. Failing that, Part 26's "SpanContext" is
// honoured through `SpanContextToTraceParent`. Failing both, whatever the
// "traceparent" entry held is returned unexamined — this layer treats that
// field as opaque, so a non-W3C correlation id still round-trips.
//
// Callers therefore never see which form arrived: the traceparent string stays
// the single internal representation of trace context.
std::string GetTraceParent(const RequestHeader& header);

// Builds the embedded ResponseHeader. Encoding the result reproduces the
// hand-written AppendResponseHeader byte-for-byte.
ResponseHeader MakeResponseHeader(UInt32 request_handle, Status service_result);

}  // namespace opcua::ua
