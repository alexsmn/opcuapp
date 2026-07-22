#include "opcua/ua/ua_service_header.h"

#include "opcua/ua/ua_binary_codec.h"
#include "opcua/ua/ua_encoding_ids.h"

namespace opcua::ua {
namespace {

// The parameter name the traceparent travels under, matching the hand-written
// codec. OPC UA Part 4 §7.33 RequestHeader (additionalHeader),
// https://reference.opcfoundation.org/Core/Part4/v105/docs/7.33
constexpr std::string_view kTraceParentParameterName = "traceparent";

}  // namespace

RequestHeader MakeRequestHeader(const NodeId& authentication_token,
                                UInt32 request_handle,
                                std::string_view trace_parent) {
  RequestHeader header;
  header.authentication_token = authentication_token;
  header.request_handle = request_handle;
  if (!trace_parent.empty()) {
    AdditionalParametersType parameters;
    parameters.parameters.push_back(
        {.key = QualifiedName{std::string{kTraceParentParameterName}},
         .value = Variant{String{trace_parent}}});
    header.additional_header = ToExtensionObject(parameters);
  }
  return header;
}

std::string GetTraceParent(const RequestHeader& header) {
  AdditionalParametersType parameters;
  if (!FromExtensionObject(header.additional_header, parameters))
    return {};
  for (const KeyValuePair& parameter : parameters.parameters) {
    if (parameter.key.namespace_index() == 0 &&
        parameter.key.name() == kTraceParentParameterName) {
      if (const String* text = parameter.value.get_if<String>())
        return *text;
      return {};  // A non-String "traceparent" value is dropped.
    }
  }
  return {};
}

ResponseHeader MakeResponseHeader(UInt32 request_handle,
                                  Status service_result) {
  ResponseHeader header;
  header.request_handle = request_handle;
  header.service_result = service_result;
  return header;
}

}  // namespace opcua::ua
