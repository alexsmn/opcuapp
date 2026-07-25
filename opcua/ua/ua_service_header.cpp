#include "opcua/ua/ua_service_header.h"

#include "opcua/ua/ua_binary_codec.h"
#include "opcua/ua/ua_encoding_ids.h"

#include <utility>

namespace opcua::ua {
namespace {

// The parameter name the traceparent travels under, matching the hand-written
// codec. OPC UA Part 4 §7.33 RequestHeader (additionalHeader),
// https://reference.opcfoundation.org/Core/Part4/v105/docs/7.33
constexpr std::string_view kTraceParentParameterName = "traceparent";

}  // namespace

AdditionalParametersType GetAdditionalParameters(const RequestHeader& header) {
  AdditionalParametersType parameters;
  if (!FromExtensionObject(header.additional_header, parameters))
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
  parameters.parameters.push_back({.key = QualifiedName{std::string{key}},
                                   .value = std::move(value)});
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
  if (!trace_parent.empty()) {
    SetAdditionalParameter(header, kTraceParentParameterName,
                           Variant{String{trace_parent}});
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

std::string GetTraceParent(const RequestHeader& header) {
  const AdditionalParametersType parameters = GetAdditionalParameters(header);
  const Variant* value =
      FindAdditionalParameter(parameters, kTraceParentParameterName);
  if (value == nullptr)
    return {};
  if (const String* text = value->get_if<String>())
    return *text;
  return {};  // A non-String "traceparent" value is dropped.
}

ResponseHeader MakeResponseHeader(UInt32 request_handle,
                                  Status service_result) {
  ResponseHeader header;
  header.request_handle = request_handle;
  header.service_result = service_result;
  return header;
}

}  // namespace opcua::ua
