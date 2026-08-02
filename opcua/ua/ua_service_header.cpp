#include "opcua/ua/ua_service_header.h"

#include "opcua/metrics/trace_id.h"
#include "opcua/types/guid.h"
#include "opcua/ua/ua_binary_codec.h"
#include "opcua/ua/ua_encoding_ids.h"
#include "opcua/ua/ua_extension_object_any.h"

#include <utility>

namespace opcua::ua {
namespace {

// The parameter name the traceparent travels under, matching the hand-written
// codec. OPC UA Part 4 §7.33 RequestHeader (additionalHeader),
// https://reference.opcfoundation.org/Core/Part4/v105/docs/7.33
constexpr std::string_view kTraceParentParameterName = "traceparent";

// The parameter name Part 26 assigns to the SpanContextDataType. OPC UA
// Part 26 §5.6.4 Passing SpanStructure from a Client,
// https://reference.opcfoundation.org/Core/Part26/v105/docs/5.6.4
constexpr std::string_view kSpanContextParameterName = "SpanContext";

// Offsets of the two id fields inside a version-00 traceparent
// (`00-<32 hex trace id>-<16 hex span id>-<2 hex flags>`).
constexpr std::size_t kTraceIdOffset = 3;
constexpr std::size_t kTraceIdLength = 32;
constexpr std::size_t kSpanIdOffset = 36;
constexpr std::size_t kSpanIdLength = 16;

constexpr char kHexDigits[] = "0123456789abcdef";

// Rewrites 32 hex digits as the canonical 8-4-4-4-12 Guid text form. The
// hyphen positions are the whole conversion — see the header for why the
// canonical text form, and not a raw byte copy, is the convention.
std::string ToGuidText(std::string_view trace_id_hex) {
  std::string text;
  text.reserve(36);
  for (std::size_t i = 0; i < trace_id_hex.size(); ++i) {
    if (i == 8 || i == 12 || i == 16 || i == 20)
      text += '-';
    text += trace_id_hex[i];
  }
  return text;
}

// The inverse of `ToGuidText`: the canonical text form without its hyphens,
// lower-cased, which is the form a traceparent carries.
std::string ToTraceIdHex(const Guid& trace_id) {
  const String text = trace_id.ToString();
  std::string hex;
  hex.reserve(kTraceIdLength);
  for (char c : text) {
    if (c == '-')
      continue;
    hex += (c >= 'A' && c <= 'F') ? static_cast<char>(c - 'A' + 'a') : c;
  }
  return hex;
}

// Parses 16 hex digits as a big-endian UInt64.
UInt64 ParseSpanId(std::string_view span_id_hex) {
  UInt64 value = 0;
  for (char c : span_id_hex) {
    const UInt64 digit = (c >= '0' && c <= '9')
                             ? static_cast<UInt64>(c - '0')
                             : static_cast<UInt64>(c - 'a' + 10);
    value = (value << 4) | digit;
  }
  return value;
}

// Formats a UInt64 as 16 lower-case hex digits, zero padded.
std::string FormatSpanId(UInt64 span_id) {
  std::string hex(kSpanIdLength, '0');
  for (std::size_t i = 0; i < kSpanIdLength; ++i) {
    hex[kSpanIdLength - 1 - i] = kHexDigits[(span_id >> (4 * i)) & 0x0f];
  }
  return hex;
}

}  // namespace

std::optional<SpanContextDataType> TraceParentToSpanContext(
    std::string_view trace_parent) {
  if (!IsW3CTraceParent(trace_parent))
    return std::nullopt;

  const std::optional<Guid> trace_id = Guid::FromString(
      ToGuidText(trace_parent.substr(kTraceIdOffset, kTraceIdLength)));
  if (!trace_id.has_value() || trace_id->is_null())
    return std::nullopt;

  // "0 is an invalid value for a SpanId" (Part 26 §5.6.2 Table 11). A
  // traceparent cannot legally carry an all-zero span id either, so this only
  // fires on input that already failed W3C validation elsewhere.
  const UInt64 span_id =
      ParseSpanId(trace_parent.substr(kSpanIdOffset, kSpanIdLength));
  if (span_id == 0)
    return std::nullopt;

  return SpanContextDataType{.trace_id = *trace_id, .span_id = span_id};
}

std::string SpanContextToTraceParent(const SpanContextDataType& span_context) {
  if (span_context.trace_id.is_null() || span_context.span_id == 0)
    return {};

  std::string trace_parent;
  trace_parent.reserve(55);
  trace_parent += "00-";
  trace_parent += ToTraceIdHex(span_context.trace_id);
  trace_parent += '-';
  trace_parent += FormatSpanId(span_context.span_id);
  // Sampled. See the header for why Part 26's silence on sampling resolves
  // this way rather than to "00".
  trace_parent += "-01";
  return trace_parent;
}

AdditionalParametersType GetAdditionalParameters(const RequestHeader& header) {
  AdditionalParametersType parameters;
  // Any-encoding: this slot is read on the binary transport and on UA-JSON,
  // and a binary-only decode reports "absent" for every JSON body rather than
  // failing. See ua_extension_object_any.h.
  if (!FromAnyExtensionObject(header.additional_header, parameters))
    return {};
  return parameters;
}

void SetAdditionalParameters(RequestHeader& header,
                             const AdditionalParametersType& parameters) {
  if (parameters.parameters.empty()) {
    header.additional_header = {};
    return;
  }
  header.additional_header = ToExtensionObject(parameters);
}

void SetAdditionalParameter(RequestHeader& header,
                            std::string_view key,
                            Variant value) {
  AdditionalParametersType parameters = GetAdditionalParameters(header);
  for (KeyValuePair& parameter : parameters.parameters) {
    if (parameter.key.namespace_index() == 0 && parameter.key.name() == key) {
      parameter.value = std::move(value);
      SetAdditionalParameters(header, parameters);
      return;
    }
  }
  parameters.parameters.push_back(
      {.key = QualifiedName{std::string{key}}, .value = std::move(value)});
  SetAdditionalParameters(header, parameters);
}

const Variant* FindAdditionalParameter(
    const AdditionalParametersType& parameters,
    std::string_view key) {
  for (const KeyValuePair& parameter : parameters.parameters) {
    if (parameter.key.namespace_index() == 0 && parameter.key.name() == key)
      return &parameter.value;
  }
  return nullptr;
}

void ApplyRequestEnvelope(RequestHeader& header,
                          const NodeId& authentication_token,
                          UInt32 request_handle,
                          std::string_view trace_parent) {
  header.authentication_token = authentication_token;
  header.request_handle = request_handle;
  if (trace_parent.empty())
    return;

  // The W3C entry carries whatever the caller supplied, verbatim, exactly as it
  // did before Part 26 was carried here — the field is opaque to this layer.
  SetAdditionalParameter(header, kTraceParentParameterName,
                         Variant{String{trace_parent}});

  // The Part 26 entry rides alongside whenever the value is a traceparent this
  // stack can convert. A value that is not (a legacy opaque trace id, say) has
  // no SpanContextDataType representation at all, so only the W3C key goes out
  // — the header stays valid, it just says nothing a Part 26 peer can read.
  if (const std::optional<SpanContextDataType> span_context =
          TraceParentToSpanContext(trace_parent)) {
    SetAdditionalParameter(header, kSpanContextParameterName,
                           Variant{ToExtensionObject(*span_context)});
  }
}

RequestHeader MakeRequestHeader(const NodeId& authentication_token,
                                UInt32 request_handle,
                                std::string_view trace_parent) {
  RequestHeader header;
  ApplyRequestEnvelope(header, authentication_token, request_handle,
                       trace_parent);
  return header;
}

namespace {

// The Part 26 "SpanContext" entry as a traceparent, or empty when absent or
// unusable.
std::string GetSpanContextTraceParent(
    const AdditionalParametersType& parameters) {
  const Variant* value =
      FindAdditionalParameter(parameters, kSpanContextParameterName);
  if (value == nullptr)
    return {};
  const ExtensionObject* body = value->get_if<ExtensionObject>();
  if (body == nullptr)
    return {};  // A non-Structure "SpanContext" value is dropped.
  SpanContextDataType span_context;
  if (!FromAnyExtensionObject(*body, span_context))
    return {};
  return SpanContextToTraceParent(span_context);
}

}  // namespace

std::string GetTraceParent(const RequestHeader& header) {
  const AdditionalParametersType parameters = GetAdditionalParameters(header);

  const Variant* value =
      FindAdditionalParameter(parameters, kTraceParentParameterName);
  const String* text = value ? value->get_if<String>() : nullptr;

  // A well-formed W3C entry wins outright: it is the only form of the two that
  // carries the sampled flag, so when a peer sent both it is strictly richer.
  if (text != nullptr && IsW3CTraceParent(*text))
    return *text;

  // Otherwise Part 26's entry gets its chance — a peer may have sent only that
  // one, or have paired a value this stack cannot parse with a usable
  // SpanContext.
  if (std::string from_span_context = GetSpanContextTraceParent(parameters);
      !from_span_context.empty()) {
    return from_span_context;
  }

  // Nothing conformant on either key. Hand back whatever the "traceparent"
  // entry held anyway: this field is opaque to this layer, callers have always
  // been free to put a non-W3C correlation id in it, and dropping it here
  // would lose correlation the peer clearly intended.
  return text != nullptr ? *text : std::string{};
}

ResponseHeader MakeResponseHeader(UInt32 request_handle,
                                  Status service_result) {
  ResponseHeader header;
  header.request_handle = request_handle;
  header.service_result = service_result;
  return header;
}

}  // namespace opcua::ua
