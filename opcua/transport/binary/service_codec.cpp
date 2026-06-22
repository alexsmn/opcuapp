#include "opcua/transport/binary/service_codec.h"

#include "opcua/events/event_filter.h"
#include "opcua/transport/binary/codec_utils.h"

#include "opcua/types/localized_text.h"
#include "opcua/types/standard_node_ids.h"

namespace opcua::binary {
namespace {

// OPC UA Part 4, 7.7.3: filter operator codes. Only the subset actually
// emitted/consumed by opcua events is enumerated here.
constexpr std::uint32_t kFilterOperatorOfType = 11;
constexpr std::uint32_t kFilterOperatorRelatedTo = 15;

constexpr std::uint32_t kFindServersRequestEncodingId = 422;
constexpr std::uint32_t kFindServersResponseEncodingId = 425;
constexpr std::uint32_t kGetEndpointsRequestEncodingId = 428;
constexpr std::uint32_t kGetEndpointsResponseEncodingId = 431;
constexpr std::uint32_t kCreateSessionRequestEncodingId = 461;
constexpr std::uint32_t kCreateSessionResponseEncodingId = 464;
constexpr std::uint32_t kActivateSessionRequestEncodingId = 467;
constexpr std::uint32_t kActivateSessionResponseEncodingId = 470;
constexpr std::uint32_t kCloseSessionRequestEncodingId = 473;
constexpr std::uint32_t kCloseSessionResponseEncodingId = 476;
constexpr std::uint32_t kAddNodesRequestEncodingId = 488;
constexpr std::uint32_t kAddNodesResponseEncodingId = 491;
constexpr std::uint32_t kAddReferencesRequestEncodingId = 494;
constexpr std::uint32_t kAddReferencesResponseEncodingId = 497;
constexpr std::uint32_t kDeleteNodesRequestEncodingId = 500;
constexpr std::uint32_t kDeleteNodesResponseEncodingId = 503;
constexpr std::uint32_t kDeleteReferencesRequestEncodingId = 506;
constexpr std::uint32_t kDeleteReferencesResponseEncodingId = 509;
// OPC UA Part 6 DefaultBinary encoding ids for RegisterNodes / UnregisterNodes.
constexpr std::uint32_t kRegisterNodesRequestEncodingId = 560;
constexpr std::uint32_t kRegisterNodesResponseEncodingId = 563;
constexpr std::uint32_t kUnregisterNodesRequestEncodingId = 566;
constexpr std::uint32_t kUnregisterNodesResponseEncodingId = 569;
// OPC UA Part 6 DefaultBinary encoding id for ServiceFault.
constexpr std::uint32_t kServiceFaultEncodingId = 397;
constexpr std::uint32_t kBrowseRequestEncodingId = 525;
constexpr std::uint32_t kBrowseResponseEncodingId = 528;
constexpr std::uint32_t kBrowseNextRequestEncodingId = 533;
constexpr std::uint32_t kBrowseNextResponseEncodingId = 536;
constexpr std::uint32_t kTranslateBrowsePathsRequestEncodingId = 554;
constexpr std::uint32_t kTranslateBrowsePathsResponseEncodingId = 557;
constexpr std::uint32_t kCreateSubscriptionRequestEncodingId = 787;
constexpr std::uint32_t kCreateSubscriptionResponseEncodingId = 790;
constexpr std::uint32_t kModifySubscriptionRequestEncodingId = 793;
constexpr std::uint32_t kModifySubscriptionResponseEncodingId = 796;
constexpr std::uint32_t kSetPublishingModeRequestEncodingId = 799;
constexpr std::uint32_t kSetPublishingModeResponseEncodingId = 802;
constexpr std::uint32_t kCreateMonitoredItemsRequestEncodingId = 751;
constexpr std::uint32_t kCreateMonitoredItemsResponseEncodingId = 754;
constexpr std::uint32_t kModifyMonitoredItemsRequestEncodingId = 763;
constexpr std::uint32_t kModifyMonitoredItemsResponseEncodingId = 766;
constexpr std::uint32_t kDeleteMonitoredItemsRequestEncodingId = 781;
constexpr std::uint32_t kDeleteMonitoredItemsResponseEncodingId = 784;
constexpr std::uint32_t kDeleteSubscriptionsRequestEncodingId = 847;
constexpr std::uint32_t kDeleteSubscriptionsResponseEncodingId = 850;
constexpr std::uint32_t kSetMonitoringModeRequestEncodingId = 769;
constexpr std::uint32_t kSetMonitoringModeResponseEncodingId = 772;
constexpr std::uint32_t kNotificationMessageEncodingId = 805;
constexpr std::uint32_t kDataChangeNotificationEncodingId = 811;
constexpr std::uint32_t kEventNotificationListEncodingId = 916;
constexpr std::uint32_t kEventFieldListEncodingId = 919;
constexpr std::uint32_t kStatusChangeNotificationEncodingId = 820;
constexpr std::uint32_t kPublishRequestEncodingId = 826;
constexpr std::uint32_t kPublishResponseEncodingId = 829;
constexpr std::uint32_t kRepublishRequestEncodingId = 832;
constexpr std::uint32_t kRepublishResponseEncodingId = 835;
constexpr std::uint32_t kTransferSubscriptionsRequestEncodingId = 841;
constexpr std::uint32_t kTransferSubscriptionsResponseEncodingId = 844;
constexpr std::uint32_t kCallRequestEncodingId = 710;
constexpr std::uint32_t kCallResponseEncodingId = 713;
constexpr std::uint32_t kReadEventDetailsEncodingId = 646;
constexpr std::uint32_t kReadRawModifiedDetailsEncodingId = 649;
constexpr std::uint32_t kHistoryDataEncodingId = 658;
constexpr std::uint32_t kHistoryEventEncodingId = 661;
constexpr std::uint32_t kHistoryReadRequestEncodingId = 664;
constexpr std::uint32_t kHistoryReadResponseEncodingId = 667;
constexpr std::uint32_t kDataChangeFilterEncodingId = 724;
constexpr std::uint32_t kEventFilterEncodingId = 727;
constexpr std::uint32_t kLiteralOperandEncodingId = 597;
constexpr std::uint32_t kSimpleAttributeOperandEncodingId = 603;
constexpr std::uint32_t kHistoryEventFieldListEncodingId = 922;
constexpr std::uint32_t kReadRequestEncodingId = 629;
constexpr std::uint32_t kReadResponseEncodingId = 632;
constexpr std::uint32_t kWriteRequestEncodingId = 671;
constexpr std::uint32_t kWriteResponseEncodingId = 674;
constexpr std::uint32_t kUpdateDataDetailsEncodingId = 682;
constexpr std::uint32_t kHistoryUpdateRequestEncodingId = 700;
constexpr std::uint32_t kHistoryUpdateResponseEncodingId = 703;
constexpr std::uint32_t kAnonymousIdentityTokenEncodingId = 321;
constexpr std::uint32_t kUserNameIdentityTokenEncodingId = 324;
constexpr std::uint32_t kObjectAttributesEncodingId = 354;
constexpr std::uint32_t kVariableAttributesEncodingId = 357;

enum class WireTimestampsToReturn : std::uint32_t {
  Source = 0,
  Server = 1,
  Both = 2,
  Neither = 3,
};

constexpr std::uint32_t EncodeSpecifiedAttribute(AttributeId attribute_id) {
  return 1u << static_cast<unsigned>(attribute_id);
}

std::size_t EstimateStringSize(std::string_view value) {
  return sizeof(std::int32_t) + value.size();
}

std::size_t EstimateByteStringSize(const ByteString& value) {
  return sizeof(std::int32_t) + value.size();
}

std::size_t EstimateNodeIdSize(const NodeId& node_id) {
  if (node_id.is_null()) {
    return 1;
  }
  if (node_id.is_numeric() && node_id.namespace_index() <= 0xff &&
      node_id.numeric_id() <= 0xffff) {
    return 1 + sizeof(std::uint8_t) + sizeof(std::uint16_t);
  }
  if (node_id.is_numeric()) {
    return 1 + sizeof(std::uint16_t) + sizeof(std::uint32_t);
  }
  if (node_id.is_string()) {
    return 1 + sizeof(std::uint16_t) + EstimateStringSize(node_id.string_id());
  }
  return 1 + sizeof(std::uint16_t) +
         EstimateByteStringSize(node_id.opaque_id());
}

std::size_t EstimateExpandedNodeIdSize(const ExpandedNodeId& node_id) {
  auto size = EstimateNodeIdSize(node_id.node_id());
  if (!node_id.namespace_uri().empty()) {
    size += EstimateStringSize(node_id.namespace_uri());
  }
  if (node_id.server_index() != 0) {
    size += sizeof(std::uint32_t);
  }
  return size;
}

std::size_t EstimateQualifiedNameSize(const QualifiedName& name) {
  return sizeof(std::uint16_t) + EstimateStringSize(name.name());
}

std::size_t EstimateVariantSize(const Variant& value);

std::size_t EstimateDataValueSize(const DataValue& value) {
  std::size_t size = sizeof(std::uint8_t);
  if (!value.value.is_null()) {
    size += EstimateVariantSize(value.value);
  }
  if (!IsGood(value.status_code)) {
    size += sizeof(std::uint32_t);
  }
  if (!value.source_timestamp.is_null()) {
    size += sizeof(std::int64_t);
  }
  if (!value.server_timestamp.is_null()) {
    size += sizeof(std::int64_t);
  }
  return size;
}

template <class T>
std::size_t EstimateFixedArrayVariantSize(const std::vector<T>& values,
                                          std::size_t element_size) {
  return sizeof(std::uint8_t) + sizeof(std::int32_t) +
         values.size() * element_size;
}

std::size_t EstimateVariantSize(const Variant& value) {
  if (value.is_null()) {
    return sizeof(std::uint8_t);
  }

  if (value.is_array()) {
    switch (value.type()) {
      case Variant::EMPTY:
        return EstimateFixedArrayVariantSize(
            value.get<std::vector<std::monostate>>(), 0);
      case Variant::BOOL:
        return EstimateFixedArrayVariantSize(value.get<std::vector<bool>>(), 1);
      case Variant::INT8:
        return EstimateFixedArrayVariantSize(value.get<std::vector<Int8>>(), 1);
      case Variant::UINT8:
        return EstimateFixedArrayVariantSize(value.get<std::vector<UInt8>>(),
                                             1);
      case Variant::INT16:
        return EstimateFixedArrayVariantSize(value.get<std::vector<Int16>>(),
                                             sizeof(std::uint16_t));
      case Variant::UINT16:
        return EstimateFixedArrayVariantSize(value.get<std::vector<UInt16>>(),
                                             sizeof(std::uint16_t));
      case Variant::INT32:
        return EstimateFixedArrayVariantSize(value.get<std::vector<Int32>>(),
                                             sizeof(std::uint32_t));
      case Variant::UINT32:
        return EstimateFixedArrayVariantSize(value.get<std::vector<UInt32>>(),
                                             sizeof(std::uint32_t));
      case Variant::INT64:
        return EstimateFixedArrayVariantSize(value.get<std::vector<Int64>>(),
                                             sizeof(std::int64_t));
      case Variant::UINT64:
        return EstimateFixedArrayVariantSize(value.get<std::vector<UInt64>>(),
                                             sizeof(std::uint64_t));
      case Variant::DOUBLE:
        return EstimateFixedArrayVariantSize(value.get<std::vector<Double>>(),
                                             sizeof(double));
      case Variant::BYTE_STRING: {
        std::size_t size = sizeof(std::uint8_t) + sizeof(std::int32_t);
        for (const auto& item : value.get<std::vector<ByteString>>()) {
          size += EstimateByteStringSize(item);
        }
        return size;
      }
      case Variant::STRING: {
        std::size_t size = sizeof(std::uint8_t) + sizeof(std::int32_t);
        for (const auto& item : value.get<std::vector<String>>()) {
          size += EstimateStringSize(item);
        }
        return size;
      }
      case Variant::QUALIFIED_NAME: {
        std::size_t size = sizeof(std::uint8_t) + sizeof(std::int32_t);
        for (const auto& item : value.get<std::vector<QualifiedName>>()) {
          size += EstimateQualifiedNameSize(item);
        }
        return size;
      }
      case Variant::NODE_ID: {
        std::size_t size = sizeof(std::uint8_t) + sizeof(std::int32_t);
        for (const auto& item : value.get<std::vector<NodeId>>()) {
          size += EstimateNodeIdSize(item);
        }
        return size;
      }
      case Variant::EXPANDED_NODE_ID: {
        std::size_t size = sizeof(std::uint8_t) + sizeof(std::int32_t);
        for (const auto& item : value.get<std::vector<ExpandedNodeId>>()) {
          size += EstimateExpandedNodeIdSize(item);
        }
        return size;
      }
      case Variant::LOCALIZED_TEXT:
      case Variant::EXTENSION_OBJECT:
      case Variant::DATE_TIME:
      case Variant::COUNT:
        return sizeof(std::uint8_t) + sizeof(std::int32_t);
    }
  }

  switch (value.type()) {
    case Variant::BOOL:
    case Variant::INT8:
    case Variant::UINT8:
      return sizeof(std::uint8_t) + 1;
    case Variant::INT16:
    case Variant::UINT16:
      return sizeof(std::uint8_t) + sizeof(std::uint16_t);
    case Variant::INT32:
    case Variant::UINT32:
      return sizeof(std::uint8_t) + sizeof(std::uint32_t);
    case Variant::INT64:
    case Variant::UINT64:
    case Variant::DOUBLE:
    case Variant::DATE_TIME:
      return sizeof(std::uint8_t) + sizeof(std::uint64_t);
    case Variant::BYTE_STRING:
      return sizeof(std::uint8_t) +
             EstimateByteStringSize(value.get<ByteString>());
    case Variant::STRING:
      return sizeof(std::uint8_t) + EstimateStringSize(value.get<String>());
    case Variant::QUALIFIED_NAME:
      return sizeof(std::uint8_t) +
             EstimateQualifiedNameSize(value.get<QualifiedName>());
    case Variant::NODE_ID:
      return sizeof(std::uint8_t) + EstimateNodeIdSize(value.get<NodeId>());
    case Variant::EXPANDED_NODE_ID:
      return sizeof(std::uint8_t) +
             EstimateExpandedNodeIdSize(value.get<ExpandedNodeId>());
    case Variant::LOCALIZED_TEXT:
    case Variant::EXTENSION_OBJECT:
    case Variant::EMPTY:
    case Variant::COUNT:
      return sizeof(std::uint8_t);
  }
  return sizeof(std::uint8_t);
}

std::size_t EstimateReferenceDescriptionSize(
    const ReferenceDescription& reference) {
  return EstimateNodeIdSize(reference.reference_type_id) + sizeof(bool) +
         EstimateExpandedNodeIdSize(ExpandedNodeId{reference.node_id}) +
         EstimateQualifiedNameSize(QualifiedName{}) + sizeof(std::uint8_t) +
         sizeof(std::uint32_t) + EstimateExpandedNodeIdSize(ExpandedNodeId{});
}

std::size_t EstimateReadRequestPayloadSize(const ReadRequest& request) {
  std::size_t size = 96;
  for (const auto& input : request.inputs) {
    size += EstimateNodeIdSize(input.node_id) + sizeof(std::uint32_t) +
            EstimateStringSize({}) + EstimateQualifiedNameSize({});
  }
  return size;
}

std::size_t EstimateBrowseRequestPayloadSize(const BrowseRequest& request) {
  std::size_t size = 112;
  for (const auto& input : request.inputs) {
    size += EstimateNodeIdSize(input.node_id) + sizeof(std::uint32_t) +
            EstimateNodeIdSize(input.reference_type_id) + sizeof(bool) +
            2 * sizeof(std::uint32_t);
  }
  return size;
}

std::size_t EstimateReadResponsePayloadSize(const ReadResponse& response) {
  std::size_t size = 64;
  for (const auto& result : response.results) {
    size += EstimateDataValueSize(result);
  }
  return size;
}

std::size_t EstimateBrowseResponsePayloadSize(
    const std::vector<BrowseResult>& results) {
  std::size_t size = 64;
  for (const auto& result : results) {
    size += sizeof(std::uint32_t) +
            EstimateByteStringSize(result.continuation_point) +
            sizeof(std::int32_t);
    for (const auto& reference : result.references) {
      size += EstimateReferenceDescriptionSize(reference);
    }
  }
  return size;
}

void AppendRequestHeader(Encoder& encoder, const ServiceRequestHeader& header) {
  encoder.Encode(header.authentication_token);
  encoder.Encode(std::int64_t{0});
  encoder.Encode(header.request_handle);
  encoder.Encode(std::uint32_t{0});
  encoder.Encode(std::string_view{""});
  encoder.Encode(std::uint32_t{0});
  encoder.Encode(NodeId{});
  encoder.Encode(std::uint8_t{0x00});
}

void AppendResponseHeader(Encoder& encoder,
                          std::uint32_t request_handle,
                          Status status) {
  encoder.Encode(std::int64_t{0});
  encoder.Encode(request_handle);
  encoder.Encode(status.full_code());
  encoder.Encode(std::uint8_t{0});
  encoder.Encode(std::int32_t{0});
  encoder.Encode(NodeId{});
  encoder.Encode(std::uint8_t{0x00});
}

void AppendStringArray(Encoder& encoder,
                       const std::vector<std::string>& values) {
  encoder.Encode(static_cast<std::int32_t>(values.size()));
  for (const auto& value : values)
    encoder.Encode(value);
}

// True when an array element count is larger than the bytes left to decode.
// Every encoded element occupies at least one byte, so a larger count is
// malformed; rejecting it bounds the resize/reserve against a decode bomb.
// OPC UA Part 6 §5.1.2 Decoding Errors,
// https://reference.opcfoundation.org/Core/Part6/v105/docs/5.1.2
bool ArrayCountExceedsRemaining(const Decoder& decoder, std::int32_t count) {
  return count < 0 ||
         static_cast<std::size_t>(count) > decoder.remaining().size();
}

bool ReadStringArray(Decoder& decoder, std::vector<std::string>& values) {
  std::int32_t count = 0;
  if (!decoder.Decode(count) || count < -1)
    return false;
  values.clear();
  if (count < 0)
    return true;
  if (ArrayCountExceedsRemaining(decoder, count))
    return false;
  values.reserve(static_cast<std::size_t>(count));
  for (std::int32_t i = 0; i < count; ++i) {
    std::string value;
    if (!decoder.Decode(value))
      return false;
    values.push_back(std::move(value));
  }
  return true;
}

void AppendApplicationDescription(Encoder& encoder,
                                  const ApplicationDescription& description) {
  encoder.Encode(description.application_uri);
  encoder.Encode(description.product_uri);
  encoder.Encode(description.application_name);
  encoder.Encode(static_cast<std::uint32_t>(description.application_type));
  encoder.Encode(description.gateway_server_uri);
  encoder.Encode(description.discovery_profile_uri);
  AppendStringArray(encoder, description.discovery_urls);
}

bool ReadApplicationDescription(Decoder& decoder,
                                ApplicationDescription& description) {
  std::uint32_t application_type = 0;
  if (!decoder.Decode(description.application_uri) ||
      !decoder.Decode(description.product_uri) ||
      !decoder.Decode(description.application_name) ||
      !decoder.Decode(application_type) ||
      !decoder.Decode(description.gateway_server_uri) ||
      !decoder.Decode(description.discovery_profile_uri) ||
      !ReadStringArray(decoder, description.discovery_urls)) {
    return false;
  }
  description.application_type = static_cast<ApplicationType>(application_type);
  return description.application_type == ApplicationType::Server ||
         description.application_type == ApplicationType::Client ||
         description.application_type == ApplicationType::ClientAndServer ||
         description.application_type == ApplicationType::DiscoveryServer;
}

void AppendUserTokenPolicy(Encoder& encoder, const UserTokenPolicy& policy) {
  encoder.Encode(policy.policy_id);
  encoder.Encode(static_cast<std::uint32_t>(policy.token_type));
  encoder.Encode(policy.issued_token_type);
  encoder.Encode(policy.issuer_endpoint_url);
  encoder.Encode(policy.security_policy_uri);
}

bool ReadUserTokenPolicy(Decoder& decoder, UserTokenPolicy& policy) {
  std::uint32_t token_type = 0;
  if (!decoder.Decode(policy.policy_id) || !decoder.Decode(token_type) ||
      !decoder.Decode(policy.issued_token_type) ||
      !decoder.Decode(policy.issuer_endpoint_url) ||
      !decoder.Decode(policy.security_policy_uri)) {
    return false;
  }
  policy.token_type = static_cast<UserTokenType>(token_type);
  return policy.token_type == UserTokenType::Anonymous ||
         policy.token_type == UserTokenType::UserName ||
         policy.token_type == UserTokenType::Certificate ||
         policy.token_type == UserTokenType::IssuedToken;
}

void AppendEndpointDescription(Encoder& encoder,
                               const EndpointDescription& endpoint) {
  encoder.Encode(endpoint.endpoint_url);
  AppendApplicationDescription(encoder, endpoint.server);
  encoder.Encode(endpoint.server_certificate);
  encoder.Encode(static_cast<std::uint32_t>(endpoint.security_mode));
  encoder.Encode(endpoint.security_policy_uri);
  encoder.Encode(
      static_cast<std::int32_t>(endpoint.user_identity_tokens.size()));
  for (const auto& policy : endpoint.user_identity_tokens)
    AppendUserTokenPolicy(encoder, policy);
  encoder.Encode(endpoint.transport_profile_uri);
  encoder.Encode(endpoint.security_level);
}

bool ReadEndpointDescription(Decoder& decoder, EndpointDescription& endpoint) {
  std::uint32_t security_mode = 0;
  std::int32_t token_count = 0;
  if (!decoder.Decode(endpoint.endpoint_url) ||
      !ReadApplicationDescription(decoder, endpoint.server) ||
      !decoder.Decode(endpoint.server_certificate) ||
      !decoder.Decode(security_mode) ||
      !decoder.Decode(endpoint.security_policy_uri) ||
      !decoder.Decode(token_count) || token_count < -1) {
    return false;
  }
  endpoint.security_mode = static_cast<MessageSecurityMode>(security_mode);
  if (endpoint.security_mode != MessageSecurityMode::Invalid &&
      endpoint.security_mode != MessageSecurityMode::None &&
      endpoint.security_mode != MessageSecurityMode::Sign &&
      endpoint.security_mode != MessageSecurityMode::SignAndEncrypt) {
    return false;
  }
  endpoint.user_identity_tokens.clear();
  if (token_count >= 0) {
    endpoint.user_identity_tokens.resize(static_cast<std::size_t>(token_count));
    for (auto& policy : endpoint.user_identity_tokens) {
      if (!ReadUserTokenPolicy(decoder, policy))
        return false;
    }
  }
  return decoder.Decode(endpoint.transport_profile_uri) &&
         decoder.Decode(endpoint.security_level);
}

std::uint32_t EncodeStatusCode(StatusCode status_code) {
  return static_cast<std::uint32_t>(status_code) << 16;
}

void AppendDataValue(Encoder& encoder, const DataValue& value) {
  std::uint8_t mask = 0;
  if (!value.value.is_null()) {
    mask |= 0x01;
  }
  if (!IsGood(value.status_code)) {
    mask |= 0x02;
  }
  if (!value.source_timestamp.is_null()) {
    mask |= 0x04;
  }
  if (!value.server_timestamp.is_null()) {
    mask |= 0x08;
  }

  encoder.Encode(mask);
  if ((mask & 0x01) != 0) {
    encoder.Encode(value.value);
  }
  if ((mask & 0x02) != 0) {
    encoder.Encode(EncodeStatusCode(value.status_code));
  }
  if ((mask & 0x04) != 0) {
    encoder.Encode(value.source_timestamp);
  }
  if ((mask & 0x08) != 0) {
    encoder.Encode(value.server_timestamp);
  }
}

void AppendReferenceDescription(Encoder& encoder,
                                const ReferenceDescription& reference) {
  encoder.Encode(reference.reference_type_id);
  encoder.Encode(reference.forward);
  encoder.Encode(ExpandedNodeId{reference.node_id});
  encoder.Encode(reference.browse_name);
  encoder.Encode(reference.display_name);
  // OPC UA Part 4 §7.29 ReferenceDescription: BrowseName, DisplayName,
  // NodeClass, and TypeDefinition of the target node. Empty/default values are
  // sent when the BrowseDescription.resultMask did not request a field.
  // https://reference.opcfoundation.org/Core/Part4/v105/docs/7.29
  encoder.Encode(static_cast<std::uint32_t>(reference.node_class));
  encoder.Encode(ExpandedNodeId{reference.type_definition});
}

void AppendRelativePathElement(Encoder& encoder,
                               const RelativePathElement& element) {
  encoder.Encode(element.reference_type_id);
  encoder.Encode(element.inverse);
  encoder.Encode(element.include_subtypes);
  encoder.Encode(element.target_name);
}

void AppendBrowsePath(Encoder& encoder, const BrowsePath& path) {
  encoder.Encode(path.node_id);
  encoder.Encode(static_cast<std::int32_t>(path.relative_path.size()));
  for (const auto& element : path.relative_path) {
    AppendRelativePathElement(encoder, element);
  }
}

void AppendBrowsePathTarget(Encoder& encoder, const BrowsePathTarget& target) {
  encoder.Encode(target.target_id);
  encoder.Encode(static_cast<std::uint32_t>(target.remaining_path_index));
}

void AppendBrowsePathResult(Encoder& encoder, const BrowsePathResult& result) {
  encoder.Encode(EncodeStatusCode(result.status_code));
  encoder.Encode(static_cast<std::int32_t>(result.targets.size()));
  for (const auto& target : result.targets) {
    AppendBrowsePathTarget(encoder, target);
  }
}

void AppendHistoryData(Encoder& encoder, const HistoryReadRawResult& result) {
  std::vector<char> body;
  body.reserve(sizeof(std::int32_t) + result.values.size() * 32);
  Encoder body_encoder{body};
  body_encoder.Encode(static_cast<std::int32_t>(result.values.size()));
  for (const auto& value : result.values) {
    AppendDataValue(body_encoder, value);
  }
  encoder.Encode(EncodedExtensionObject{
      .type_id = kHistoryDataEncodingId,
      .body = std::move(body),
  });
}

void AppendLiteralOperand(Encoder& encoder, const Variant& value) {
  std::vector<char> body;
  body.reserve(EstimateVariantSize(value));
  Encoder body_encoder{body};
  body_encoder.Encode(value);
  encoder.Encode(EncodedExtensionObject{
      .type_id = kLiteralOperandEncodingId,
      .body = std::move(body),
  });
}

void AppendSimpleAttributeOperand(Encoder& encoder,
                                  std::span<const std::string> browse_path) {
  std::vector<char> body;
  body.reserve(16 + browse_path.size() * 24);
  Encoder body_encoder{body};
  body_encoder.Encode(NodeId{id::BaseEventType});
  body_encoder.Encode(static_cast<std::int32_t>(browse_path.size()));
  for (const auto& segment : browse_path) {
    body_encoder.Encode(QualifiedName{segment, 0});
  }
  body_encoder.Encode(static_cast<std::uint32_t>(AttributeId::Value));
  body_encoder.Encode(std::string_view{""});
  encoder.Encode(EncodedExtensionObject{
      .type_id = kSimpleAttributeOperandEncodingId,
      .body = std::move(body),
  });
}

void AppendEventFilter(Encoder& encoder,
                       std::span<const std::vector<std::string>> field_paths,
                       const EventFilter& filter) {
  std::vector<char> body;
  body.reserve(32 + field_paths.size() * 48 +
               (filter.of_type.size() + filter.child_of.size()) * 32);
  Encoder body_encoder{body};
  body_encoder.Encode(static_cast<std::int32_t>(field_paths.size()));
  for (const auto& field_path : field_paths) {
    AppendSimpleAttributeOperand(body_encoder, field_path);
  }

  body_encoder.Encode(static_cast<std::int32_t>(filter.of_type.size() +
                                                filter.child_of.size()));
  for (const auto& of_type : filter.of_type) {
    body_encoder.Encode(kFilterOperatorOfType);
    body_encoder.Encode(std::int32_t{1});
    AppendLiteralOperand(body_encoder, Variant{of_type});
  }
  for (const auto& child_of : filter.child_of) {
    body_encoder.Encode(kFilterOperatorRelatedTo);
    body_encoder.Encode(std::int32_t{1});
    AppendLiteralOperand(body_encoder, Variant{child_of});
  }

  encoder.Encode(EncodedExtensionObject{
      .type_id = kEventFilterEncodingId,
      .body = std::move(body),
  });
}

void AppendHistoryEvent(Encoder& encoder,
                        const HistoryReadEventsResponse& response,
                        std::span<const std::vector<std::string>> field_paths) {
  const auto paths =
      NormalizeEventFieldPaths(std::vector<std::vector<std::string>>(
          field_paths.begin(), field_paths.end()));
  std::vector<char> body;
  body.reserve(16 + response.result.events.size() * paths.size() * 24);
  Encoder body_encoder{body};
  body_encoder.Encode(static_cast<std::int32_t>(response.result.events.size()));
  for (const auto& event : response.result.events) {
    const auto event_fields = ProjectEventFields(paths, std::any{event});
    body_encoder.Encode(static_cast<std::int32_t>(event_fields.size()));
    for (const auto& field : event_fields) {
      body_encoder.Encode(field);
    }
  }
  encoder.Encode(EncodedExtensionObject{
      .type_id = kHistoryEventEncodingId,
      .body = std::move(body),
  });
}

void AppendDataChangeFilter(Encoder& encoder, const DataChangeFilter& filter) {
  std::vector<char> body;
  body.reserve(3 * sizeof(std::uint32_t));
  Encoder body_encoder{body};
  body_encoder.Encode(static_cast<std::uint32_t>(filter.trigger));
  body_encoder.Encode(static_cast<std::uint32_t>(filter.deadband_type));
  body_encoder.Encode(filter.deadband_value);
  encoder.Encode(EncodedExtensionObject{
      .type_id = kDataChangeFilterEncodingId,
      .body = std::move(body),
  });
}

void AppendNullExtensionObject(Encoder& encoder) {
  encoder.Encode(NodeId{});
  encoder.Encode(std::uint8_t{0x00});
}

void AppendMonitoringFilter(Encoder& encoder,
                            const std::optional<MonitoringFilter>& filter) {
  if (!filter.has_value()) {
    AppendNullExtensionObject(encoder);
    return;
  }

  std::visit(
      [&encoder](const auto& typed_filter) {
        using T = std::decay_t<decltype(typed_filter)>;
        if constexpr (std::is_same_v<T, DataChangeFilter>) {
          AppendDataChangeFilter(encoder, typed_filter);
        } else {
          AppendEventFilter(encoder, ParseEventFilterFieldPaths(typed_filter),
                            EventFilter{});
        }
      },
      *filter);
}

void AppendMonitoredItemCreateRequest(
    Encoder& encoder,
    const MonitoredItemCreateRequest& request) {
  encoder.Encode(request.item_to_monitor.node_id);
  encoder.Encode(
      static_cast<std::uint32_t>(request.item_to_monitor.attribute_id));
  encoder.Encode(request.index_range.value_or(String{}));
  encoder.Encode(QualifiedName{});
  encoder.Encode(static_cast<std::uint32_t>(request.monitoring_mode));
  encoder.Encode(request.requested_parameters.client_handle);
  encoder.Encode(request.requested_parameters.sampling_interval_ms);
  AppendMonitoringFilter(encoder, request.requested_parameters.filter);
  encoder.Encode(request.requested_parameters.queue_size);
  encoder.Encode(request.requested_parameters.discard_oldest);
}

void AppendMonitoredItemCreateResult(Encoder& encoder,
                                     const MonitoredItemCreateResult& result) {
  encoder.Encode(result.status.full_code());
  encoder.Encode(result.monitored_item_id);
  encoder.Encode(result.revised_sampling_interval_ms);
  encoder.Encode(result.revised_queue_size);
  AppendNullExtensionObject(encoder);
}

void AppendMonitoredItemModifyResult(Encoder& encoder,
                                     const MonitoredItemModifyResult& result) {
  encoder.Encode(result.status.full_code());
  encoder.Encode(result.revised_sampling_interval_ms);
  encoder.Encode(result.revised_queue_size);
  AppendNullExtensionObject(encoder);
}

void AppendNotificationData(Encoder& encoder, const NotificationData& data);

void AppendNotificationMessage(Encoder& encoder,
                               const NotificationMessage& message) {
  encoder.Encode(message.sequence_number);
  encoder.Encode(message.publish_time);
  encoder.Encode(static_cast<std::int32_t>(message.notification_data.size()));
  for (const auto& data : message.notification_data) {
    AppendNotificationData(encoder, data);
  }
}

void AppendNotificationData(Encoder& encoder, const NotificationData& data) {
  std::visit(
      [&](const auto& typed) {
        using T = std::decay_t<decltype(typed)>;
        std::vector<char> body;
        if constexpr (std::is_same_v<T, DataChangeNotification>) {
          body.reserve(sizeof(std::int32_t) +
                       typed.monitored_items.size() * 32);
        } else if constexpr (std::is_same_v<T, EventNotificationList>) {
          body.reserve(sizeof(std::int32_t) + typed.events.size() * 64);
        } else if constexpr (std::is_same_v<T, StatusChangeNotification>) {
          body.reserve(sizeof(std::uint32_t));
        }
        Encoder body_encoder{body};
        std::uint32_t type_id = 0;
        if constexpr (std::is_same_v<T, DataChangeNotification>) {
          type_id = kDataChangeNotificationEncodingId;
          body_encoder.Encode(
              static_cast<std::int32_t>(typed.monitored_items.size()));
          for (const auto& item : typed.monitored_items) {
            body_encoder.Encode(item.client_handle);
            AppendDataValue(body_encoder, item.value);
          }
        } else if constexpr (std::is_same_v<T, EventNotificationList>) {
          type_id = kEventNotificationListEncodingId;
          body_encoder.Encode(static_cast<std::int32_t>(typed.events.size()));
          for (const auto& event : typed.events) {
            std::vector<char> event_body;
            event_body.reserve(2 * sizeof(std::uint32_t) +
                               event.event_fields.size() * 24);
            Encoder event_encoder{event_body};
            event_encoder.Encode(event.client_handle);
            event_encoder.Encode(
                static_cast<std::int32_t>(event.event_fields.size()));
            for (const auto& field : event.event_fields) {
              event_encoder.Encode(field);
            }
            body_encoder.Encode(EncodedExtensionObject{
                .type_id = kEventFieldListEncodingId, .body = event_body});
          }
        } else if constexpr (std::is_same_v<T, StatusChangeNotification>) {
          type_id = kStatusChangeNotificationEncodingId;
          body_encoder.Encode(EncodeStatusCode(typed.status));
        }
        encoder.Encode(
            EncodedExtensionObject{.type_id = type_id, .body = body});
      },
      data);
}

std::optional<EncodedExtensionObject> EncodeNodeAttributesExtension(
    NodeClass node_class,
    const NodeAttributes& attributes) {
  std::vector<char> body;
  body.reserve(96);
  Encoder encoder{body};
  std::uint32_t specified_attributes = 0;
  if (!attributes.display_name.empty()) {
    specified_attributes |= EncodeSpecifiedAttribute(AttributeId::DisplayName);
  }
  if (!attributes.value.has_value() && attributes.data_type.is_null() &&
      node_class == NodeClass::Variable) {
    specified_attributes |= 0;
  }

  switch (node_class) {
    case NodeClass::Object: {
      encoder.Encode(specified_attributes);
      encoder.Encode(attributes.display_name);
      encoder.Encode(LocalizedText{});
      encoder.Encode(std::uint32_t{0});
      encoder.Encode(std::uint32_t{0});
      encoder.Encode(std::uint8_t{0});
      return EncodedExtensionObject{.type_id = kObjectAttributesEncodingId,
                                    .body = body};
    }

    case NodeClass::Variable: {
      if (attributes.value.has_value()) {
        specified_attributes |= EncodeSpecifiedAttribute(AttributeId::Value);
      }
      if (!attributes.data_type.is_null()) {
        specified_attributes |= EncodeSpecifiedAttribute(AttributeId::DataType);
      }
      encoder.Encode(specified_attributes);
      encoder.Encode(attributes.display_name);
      encoder.Encode(LocalizedText{});
      encoder.Encode(std::uint32_t{0});
      encoder.Encode(std::uint32_t{0});
      encoder.Encode(attributes.value.value_or(Variant{}));
      encoder.Encode(attributes.data_type);
      encoder.Encode(std::int32_t{-1});
      encoder.Encode(std::int32_t{-1});
      encoder.Encode(std::uint8_t{0});
      encoder.Encode(std::uint8_t{0});
      encoder.Encode(0.0);
      encoder.Encode(false);
      return EncodedExtensionObject{.type_id = kVariableAttributesEncodingId,
                                    .body = body};
    }

    default:
      return std::nullopt;
  }
}

bool DecodeNodeAttributesObject(Decoder& decoder, NodeAttributes& attributes) {
  std::uint32_t specified_attributes = 0;
  LocalizedText display_name;
  LocalizedText ignored_description;
  std::uint32_t ignored_write_mask = 0;
  std::uint32_t ignored_user_write_mask = 0;
  std::uint8_t ignored_event_notifier = 0;
  if (!decoder.Decode(specified_attributes) || !decoder.Decode(display_name) ||
      !decoder.Decode(ignored_description) ||
      !decoder.Decode(ignored_write_mask) ||
      !decoder.Decode(ignored_user_write_mask) ||
      !decoder.Decode(ignored_event_notifier) || !decoder.consumed()) {
    return false;
  }

  if ((specified_attributes &
       EncodeSpecifiedAttribute(AttributeId::DisplayName)) != 0) {
    attributes.set_display_name(std::move(display_name));
  }
  return true;
}

bool DecodeNodeAttributesVariable(Decoder& decoder,
                                  NodeAttributes& attributes) {
  std::uint32_t specified_attributes = 0;
  LocalizedText display_name;
  LocalizedText ignored_description;
  std::uint32_t ignored_write_mask = 0;
  std::uint32_t ignored_user_write_mask = 0;
  Variant value;
  NodeId data_type;
  std::int32_t ignored_value_rank = 0;
  std::int32_t ignored_array_dimensions = 0;
  std::uint8_t ignored_access_level = 0;
  std::uint8_t ignored_user_access_level = 0;
  double ignored_minimum_sampling_interval = 0;
  bool ignored_historizing = false;
  if (!decoder.Decode(specified_attributes) || !decoder.Decode(display_name) ||
      !decoder.Decode(ignored_description) ||
      !decoder.Decode(ignored_write_mask) ||
      !decoder.Decode(ignored_user_write_mask) || !decoder.Decode(value) ||
      !decoder.Decode(data_type) || !decoder.Decode(ignored_value_rank) ||
      !decoder.Decode(ignored_array_dimensions) ||
      !decoder.Decode(ignored_access_level) ||
      !decoder.Decode(ignored_user_access_level) ||
      !decoder.Decode(ignored_minimum_sampling_interval) ||
      !decoder.Decode(ignored_historizing) || !decoder.consumed()) {
    return false;
  }

  if ((specified_attributes &
       EncodeSpecifiedAttribute(AttributeId::DisplayName)) != 0) {
    attributes.set_display_name(std::move(display_name));
  }
  if ((specified_attributes & EncodeSpecifiedAttribute(AttributeId::Value)) !=
      0) {
    attributes.set_value(std::move(value));
  }
  if ((specified_attributes &
       EncodeSpecifiedAttribute(AttributeId::DataType)) != 0) {
    attributes.set_data_type(std::move(data_type));
  }
  return true;
}

bool DecodeNodeAttributesExtension(Decoder& decoder,
                                   NodeClass node_class,
                                   NodeAttributes& attributes) {
  DecodedExtensionObject extension;
  if (!decoder.Decode(extension) || extension.encoding != 0x01) {
    return false;
  }

  Decoder body_decoder{extension.body};
  switch (node_class) {
    case NodeClass::Object:
      return extension.type_id == kObjectAttributesEncodingId &&
             DecodeNodeAttributesObject(body_decoder, attributes);
    case NodeClass::Variable:
      return extension.type_id == kVariableAttributesEncodingId &&
             DecodeNodeAttributesVariable(body_decoder, attributes);
    default:
      return false;
  }
}

bool SkipStringArray(Decoder& decoder) {
  std::int32_t count = 0;
  if (!decoder.Decode(count)) {
    return false;
  }
  if (count < 0) {
    return true;
  }
  for (std::int32_t i = 0; i < count; ++i) {
    std::string ignored;
    if (!decoder.Decode(ignored)) {
      return false;
    }
  }
  return true;
}

bool SkipString(Decoder& decoder) {
  std::int32_t length = 0;
  if (!decoder.Decode(length)) {
    return false;
  }
  if (length < 0) {
    return true;
  }
  return decoder.Skip(static_cast<std::size_t>(length));
}

bool DecodeStringEmpty(Decoder& decoder, bool& empty) {
  std::int32_t length = 0;
  if (!decoder.Decode(length)) {
    return false;
  }
  if (length < 0) {
    empty = true;
    return true;
  }
  empty = length == 0;
  return decoder.Skip(static_cast<std::size_t>(length));
}

bool SkipQualifiedName(Decoder& decoder) {
  std::uint16_t ignored_namespace_index = 0;
  return decoder.Decode(ignored_namespace_index) && SkipString(decoder);
}

bool SkipLocalizedText(Decoder& decoder) {
  std::uint8_t mask = 0;
  if (!decoder.Decode(mask)) {
    return false;
  }
  if ((mask & 0x01) != 0) {
    std::string locale;
    if (!decoder.Decode(locale)) {
      return false;
    }
  }
  if ((mask & 0x02) != 0) {
    std::string text;
    if (!decoder.Decode(text)) {
      return false;
    }
  }
  return true;
}

bool SkipApplicationDescriptionFields(Decoder& decoder) {
  std::string ignored;
  std::int32_t application_type = 0;
  return decoder.Decode(ignored) && decoder.Decode(ignored) &&
         SkipLocalizedText(decoder) && decoder.Decode(application_type) &&
         decoder.Decode(ignored) && decoder.Decode(ignored) &&
         SkipStringArray(decoder);
}

bool SkipSignatureData(Decoder& decoder) {
  std::string algorithm;
  ByteString signature;
  return decoder.Decode(algorithm) && decoder.Decode(signature);
}

bool SkipSignedSoftwareCertificates(Decoder& decoder) {
  std::int32_t count = 0;
  if (!decoder.Decode(count)) {
    return false;
  }
  if (count < 0) {
    return true;
  }
  for (std::int32_t i = 0; i < count; ++i) {
    ByteString certificate_data;
    ByteString signature;
    if (!decoder.Decode(certificate_data) || !decoder.Decode(signature)) {
      return false;
    }
  }
  return true;
}

bool ReadRequestHeader(Decoder& decoder, ServiceRequestHeader& header) {
  std::int64_t ignored_timestamp = 0;
  if (!decoder.Decode(header.authentication_token) ||
      !decoder.Decode(ignored_timestamp) ||
      !decoder.Decode(header.request_handle)) {
    return false;
  }

  std::uint32_t ignored_u32 = 0;
  if (!decoder.Decode(ignored_u32) || !SkipString(decoder) ||
      !decoder.Decode(ignored_u32)) {
    return false;
  }

  DecodedExtensionObject additional;
  return decoder.Decode(additional);
}

std::optional<Variant> DecodeDataValue(Decoder& decoder) {
  std::uint8_t mask = 0;
  if (!decoder.Decode(mask) || (mask & 0xf0) != 0) {
    return std::nullopt;
  }

  Variant value;
  if ((mask & 0x01) != 0) {
    if (!decoder.Decode(value)) {
      return std::nullopt;
    }
  }

  if ((mask & 0x02) != 0) {
    std::uint32_t ignored_status = 0;
    if (!decoder.Decode(ignored_status)) {
      return std::nullopt;
    }
  }
  if ((mask & 0x04) != 0) {
    std::int64_t ignored_source_timestamp = 0;
    if (!decoder.Decode(ignored_source_timestamp)) {
      return std::nullopt;
    }
  }
  if ((mask & 0x08) != 0) {
    std::int64_t ignored_server_timestamp = 0;
    if (!decoder.Decode(ignored_server_timestamp)) {
      return std::nullopt;
    }
  }

  return value;
}

bool DecodeWriteValue(Decoder& decoder, WriteValue& value) {
  std::uint32_t attribute_id = 0;
  bool index_range_empty = false;
  if (!decoder.Decode(value.node_id) || !decoder.Decode(attribute_id) ||
      !DecodeStringEmpty(decoder, index_range_empty)) {
    return false;
  }

  auto decoded = DecodeDataValue(decoder);
  if (!decoded.has_value()) {
    return false;
  }

  value.attribute_id = static_cast<AttributeId>(attribute_id);
  value.value = std::move(*decoded);
  return index_range_empty;
}

bool DecodeReadValueId(Decoder& decoder, ReadValueId& value_id) {
  std::uint32_t attribute_id = 0;
  bool index_range_empty = false;
  if (!decoder.Decode(value_id.node_id) || !decoder.Decode(attribute_id) ||
      !DecodeStringEmpty(decoder, index_range_empty) ||
      !SkipQualifiedName(decoder)) {
    return false;
  }
  value_id.attribute_id = static_cast<AttributeId>(attribute_id);
  return index_range_empty;
}

bool DecodeBrowseDescription(Decoder& decoder, BrowseDescription& description) {
  std::uint32_t browse_direction = 0;
  std::uint32_t node_class_mask = 0;
  std::uint32_t result_mask = 0;
  bool include_subtypes = false;
  if (!decoder.Decode(description.node_id) ||
      !decoder.Decode(browse_direction) ||
      !decoder.Decode(description.reference_type_id) ||
      !decoder.Decode(include_subtypes) || !decoder.Decode(node_class_mask) ||
      !decoder.Decode(result_mask)) {
    return false;
  }
  description.direction = static_cast<BrowseDirection>(browse_direction);
  description.include_subtypes = include_subtypes;
  description.node_class_mask = node_class_mask;
  description.result_mask = result_mask;
  return description.direction == BrowseDirection::Forward ||
         description.direction == BrowseDirection::Inverse ||
         description.direction == BrowseDirection::Both;
}

bool DecodeRelativePathElement(Decoder& decoder, RelativePathElement& element) {
  return decoder.Decode(element.reference_type_id) &&
         decoder.Decode(element.inverse) &&
         decoder.Decode(element.include_subtypes) &&
         decoder.Decode(element.target_name);
}

bool DecodeBrowsePath(Decoder& decoder, BrowsePath& path) {
  std::int32_t count = 0;
  if (!decoder.Decode(path.node_id) || !decoder.Decode(count) || count < 0) {
    return false;
  }

  if (ArrayCountExceedsRemaining(decoder, count))
    return false;
  path.relative_path.resize(static_cast<std::size_t>(count));
  for (auto& element : path.relative_path) {
    if (!DecodeRelativePathElement(decoder, element)) {
      return false;
    }
  }
  return true;
}

bool DecodeDeleteNodesItem(Decoder& decoder, DeleteNodesItem& item) {
  return decoder.Decode(item.node_id) &&
         decoder.Decode(item.delete_target_references);
}

bool DecodeAddNodesItem(Decoder& decoder, AddNodesItem& item) {
  ExpandedNodeId parent_id;
  NodeId ignored_reference_type_id;
  ExpandedNodeId requested_new_node_id;
  QualifiedName browse_name;
  std::uint32_t node_class = 0;
  ExpandedNodeId type_definition;
  if (!decoder.Decode(parent_id) ||
      !decoder.Decode(ignored_reference_type_id) ||
      !decoder.Decode(requested_new_node_id) || !decoder.Decode(browse_name) ||
      !decoder.Decode(node_class)) {
    return false;
  }

  item.parent_id = parent_id.node_id();
  item.requested_id = requested_new_node_id.node_id();
  item.node_class = static_cast<NodeClass>(node_class);
  item.attributes.set_browse_name(std::move(browse_name));
  if (!DecodeNodeAttributesExtension(decoder, item.node_class,
                                     item.attributes) ||
      !decoder.Decode(type_definition)) {
    return false;
  }
  item.type_definition_id = type_definition.node_id();
  return true;
}

bool DecodeDeleteReferencesItem(Decoder& decoder, DeleteReferencesItem& item) {
  return decoder.Decode(item.source_node_id) &&
         decoder.Decode(item.reference_type_id) &&
         decoder.Decode(item.forward) && decoder.Decode(item.target_node_id) &&
         decoder.Decode(item.delete_bidirectional);
}

bool DecodeAddReferencesItem(Decoder& decoder, AddReferencesItem& item) {
  std::uint32_t target_node_class = 0;
  if (!decoder.Decode(item.source_node_id) ||
      !decoder.Decode(item.reference_type_id) ||
      !decoder.Decode(item.forward) ||
      !decoder.Decode(item.target_server_uri) ||
      !decoder.Decode(item.target_node_id) ||
      !decoder.Decode(target_node_class)) {
    return false;
  }
  item.target_node_class = static_cast<NodeClass>(target_node_class);
  return true;
}

// -- Response decode helpers (client-side inverses of Append*/Encode*) -------

struct DecodedResponseHeader {
  std::uint32_t request_handle = 0;
  Status service_result{StatusCode::Good};
};

bool ReadResponseHeader(Decoder& decoder, DecodedResponseHeader& header) {
  std::int64_t ignored_timestamp = 0;
  std::uint32_t status_word = 0;
  std::uint8_t ignored_diagnostics_mask = 0;
  std::int32_t ignored_string_table_count = 0;
  if (!decoder.Decode(ignored_timestamp) ||
      !decoder.Decode(header.request_handle) || !decoder.Decode(status_word) ||
      !decoder.Decode(ignored_diagnostics_mask) ||
      !decoder.Decode(ignored_string_table_count)) {
    return false;
  }
  header.service_result = Status::FromFullCode(status_word);

  // Additional header is an ExtensionObject; skip it.
  DecodedExtensionObject ignored_additional;
  return decoder.Decode(ignored_additional);
}

// After every response payload, the encoder writes a trailing
// `std::int32_t{-1}` to signal "no diagnostic info array". Skip it.
bool SkipTrailingDiagnosticInfo(Decoder& decoder) {
  std::int32_t sentinel = 0;
  if (!decoder.Decode(sentinel)) {
    return false;
  }
  return sentinel == -1 || sentinel == 0;
}

bool ReadDataValue(Decoder& decoder, DataValue& value) {
  std::uint8_t mask = 0;
  if (!decoder.Decode(mask)) {
    return false;
  }
  if ((mask & 0x01) != 0) {
    if (!decoder.Decode(value.value)) {
      return false;
    }
  }
  if ((mask & 0x02) != 0) {
    std::uint32_t status_word = 0;
    if (!decoder.Decode(status_word)) {
      return false;
    }
    value.status_code = static_cast<StatusCode>(status_word >> 16);
  }
  if ((mask & 0x04) != 0) {
    if (!decoder.Decode(value.source_timestamp)) {
      return false;
    }
  }
  if ((mask & 0x08) != 0) {
    if (!decoder.Decode(value.server_timestamp)) {
      return false;
    }
  }
  return true;
}

bool ReadReferenceDescription(Decoder& decoder,
                              ReferenceDescription& reference) {
  ExpandedNodeId expanded_node_id;
  std::uint32_t node_class = 0;
  ExpandedNodeId type_definition;
  if (!decoder.Decode(reference.reference_type_id) ||
      !decoder.Decode(reference.forward) || !decoder.Decode(expanded_node_id) ||
      !decoder.Decode(reference.browse_name) ||
      !decoder.Decode(reference.display_name) || !decoder.Decode(node_class) ||
      !decoder.Decode(type_definition)) {
    return false;
  }
  reference.node_id = expanded_node_id.node_id();
  reference.node_class = static_cast<NodeClass>(node_class);
  reference.type_definition = type_definition.node_id();
  return true;
}

bool ReadBrowseResult(Decoder& decoder, BrowseResult& result) {
  std::uint32_t status_word = 0;
  std::int32_t reference_count = 0;
  if (!decoder.Decode(status_word) ||
      !decoder.Decode(result.continuation_point) ||
      !decoder.Decode(reference_count)) {
    return false;
  }
  result.status_code = static_cast<StatusCode>(status_word >> 16);
  if (reference_count < 0) {
    reference_count = 0;
  }
  result.references.resize(static_cast<std::size_t>(reference_count));
  for (auto& reference : result.references) {
    if (!ReadReferenceDescription(decoder, reference)) {
      return false;
    }
  }
  return true;
}

bool ReadBrowsePathTarget(Decoder& decoder, BrowsePathTarget& target) {
  std::uint32_t remaining = 0;
  if (!decoder.Decode(target.target_id) || !decoder.Decode(remaining)) {
    return false;
  }
  target.remaining_path_index = remaining;
  return true;
}

bool ReadBrowsePathResult(Decoder& decoder, BrowsePathResult& result) {
  std::uint32_t status_word = 0;
  std::int32_t target_count = 0;
  if (!decoder.Decode(status_word) || !decoder.Decode(target_count)) {
    return false;
  }
  result.status_code = static_cast<StatusCode>(status_word >> 16);
  if (target_count < 0) {
    target_count = 0;
  }
  result.targets.resize(static_cast<std::size_t>(target_count));
  for (auto& target : result.targets) {
    if (!ReadBrowsePathTarget(decoder, target)) {
      return false;
    }
  }
  return true;
}

bool ReadMonitoredItemCreateResult(Decoder& decoder,
                                   MonitoredItemCreateResult& result) {
  std::uint32_t status_word = 0;
  DecodedExtensionObject ignored_filter;
  if (!decoder.Decode(status_word) ||
      !decoder.Decode(result.monitored_item_id) ||
      !decoder.Decode(result.revised_sampling_interval_ms) ||
      !decoder.Decode(result.revised_queue_size) ||
      !decoder.Decode(ignored_filter)) {
    return false;
  }
  result.status = Status::FromFullCode(status_word);
  return true;
}

bool ReadMonitoredItemModifyResult(Decoder& decoder,
                                   MonitoredItemModifyResult& result) {
  std::uint32_t status_word = 0;
  DecodedExtensionObject ignored_filter;
  if (!decoder.Decode(status_word) ||
      !decoder.Decode(result.revised_sampling_interval_ms) ||
      !decoder.Decode(result.revised_queue_size) ||
      !decoder.Decode(ignored_filter)) {
    return false;
  }
  result.status = Status::FromFullCode(status_word);
  return true;
}

bool ReadNotificationData(Decoder& decoder, NotificationData& data) {
  DecodedExtensionObject ext;
  if (!decoder.Decode(ext) || ext.encoding != 0x01) {
    return false;
  }
  Decoder body{ext.body};
  if (ext.type_id == kDataChangeNotificationEncodingId) {
    DataChangeNotification change;
    std::int32_t item_count = 0;
    if (!body.Decode(item_count)) {
      return false;
    }
    if (item_count < 0) {
      item_count = 0;
    }
    change.monitored_items.resize(static_cast<std::size_t>(item_count));
    for (auto& item : change.monitored_items) {
      if (!body.Decode(item.client_handle) ||
          !ReadDataValue(body, item.value)) {
        return false;
      }
    }
    data = std::move(change);
    return true;
  }
  if (ext.type_id == kEventNotificationListEncodingId) {
    EventNotificationList list;
    std::int32_t event_count = 0;
    if (!body.Decode(event_count)) {
      return false;
    }
    if (event_count < 0) {
      event_count = 0;
    }
    list.events.resize(static_cast<std::size_t>(event_count));
    for (auto& event : list.events) {
      DecodedExtensionObject event_ext;
      if (!body.Decode(event_ext) ||
          event_ext.type_id != kEventFieldListEncodingId ||
          event_ext.encoding != 0x01) {
        return false;
      }
      Decoder event_body{event_ext.body};
      std::int32_t field_count = 0;
      if (!event_body.Decode(event.client_handle) ||
          !event_body.Decode(field_count)) {
        return false;
      }
      if (field_count < 0) {
        field_count = 0;
      }
      event.event_fields.resize(static_cast<std::size_t>(field_count));
      for (auto& field : event.event_fields) {
        if (!event_body.Decode(field)) {
          return false;
        }
      }
    }
    data = std::move(list);
    return true;
  }
  if (ext.type_id == kStatusChangeNotificationEncodingId) {
    StatusChangeNotification change;
    std::uint32_t status_word = 0;
    if (!body.Decode(status_word)) {
      return false;
    }
    change.status = static_cast<StatusCode>(status_word >> 16);
    data = std::move(change);
    return true;
  }
  return false;
}

bool ReadNotificationMessage(Decoder& decoder, NotificationMessage& message) {
  std::int32_t data_count = 0;
  if (!decoder.Decode(message.sequence_number) ||
      !decoder.Decode(message.publish_time) || !decoder.Decode(data_count)) {
    return false;
  }
  if (data_count < 0) {
    data_count = 0;
  }
  message.notification_data.resize(static_cast<std::size_t>(data_count));
  for (auto& data : message.notification_data) {
    if (!ReadNotificationData(decoder, data)) {
      return false;
    }
  }
  return true;
}

// -- Decode*Response helpers -------------------------------------------------

// Each returns a populated DecodedResponse on success. They operate
// on the payload inside the ExtensionObject wrapper (the caller strips the
// wrapper via ReadMessage).

std::optional<DecodedResponse> DecodeFindServersResponse(
    std::span<const char> body) {
  Decoder decoder{body};
  DecodedResponseHeader header;
  std::int32_t count = 0;
  if (!ReadResponseHeader(decoder, header) || !decoder.Decode(count) ||
      count < -1) {
    return std::nullopt;
  }

  FindServersResponse response{.status = header.service_result};
  if (count >= 0) {
    response.servers.resize(static_cast<std::size_t>(count));
    for (auto& server : response.servers) {
      if (!ReadApplicationDescription(decoder, server))
        return std::nullopt;
    }
  }
  return DecodedResponse{.request_handle = header.request_handle,
                         .body = std::move(response)};
}

std::optional<DecodedResponse> DecodeGetEndpointsResponse(
    std::span<const char> body) {
  Decoder decoder{body};
  DecodedResponseHeader header;
  std::int32_t count = 0;
  if (!ReadResponseHeader(decoder, header) || !decoder.Decode(count) ||
      count < -1) {
    return std::nullopt;
  }

  GetEndpointsResponse response{.status = header.service_result};
  if (count >= 0) {
    response.endpoints.resize(static_cast<std::size_t>(count));
    for (auto& endpoint : response.endpoints) {
      if (!ReadEndpointDescription(decoder, endpoint))
        return std::nullopt;
    }
  }
  return DecodedResponse{.request_handle = header.request_handle,
                         .body = std::move(response)};
}

std::optional<DecodedResponse> DecodeCreateSessionResponse(
    std::span<const char> body) {
  Decoder decoder{body};
  DecodedResponseHeader header;
  if (!ReadResponseHeader(decoder, header)) {
    return std::nullopt;
  }

  CreateSessionResponse response{.status = header.service_result};
  std::int32_t ignored_endpoints = 0;
  std::int32_t ignored_policies = 0;
  std::string ignored_server_signature_algorithm;
  ByteString ignored_server_signature;
  std::uint32_t ignored_max_request_size = 0;
  double revised_timeout_ms = 0;
  if (!decoder.Decode(response.session_id) ||
      !decoder.Decode(response.authentication_token) ||
      !decoder.Decode(revised_timeout_ms) ||
      !decoder.Decode(response.server_nonce) ||
      !decoder.Decode(response.server_certificate) ||
      !decoder.Decode(ignored_endpoints) || !decoder.Decode(ignored_policies) ||
      !decoder.Decode(ignored_server_signature_algorithm) ||
      !decoder.Decode(ignored_server_signature) ||
      !decoder.Decode(ignored_max_request_size)) {
    return std::nullopt;
  }
  response.revised_timeout =
      base::TimeDelta::FromMillisecondsD(revised_timeout_ms);
  return DecodedResponse{.request_handle = header.request_handle,
                         .body = std::move(response)};
}

std::optional<DecodedResponse> DecodeActivateSessionResponse(
    std::span<const char> body) {
  Decoder decoder{body};
  DecodedResponseHeader header;
  ByteString ignored_nonce;
  std::int32_t ignored_results = 0;
  std::int32_t ignored_diagnostic_infos = 0;
  if (!ReadResponseHeader(decoder, header) || !decoder.Decode(ignored_nonce) ||
      !decoder.Decode(ignored_results) ||
      !decoder.Decode(ignored_diagnostic_infos)) {
    return std::nullopt;
  }
  return DecodedResponse{
      .request_handle = header.request_handle,
      .body = ActivateSessionResponse{.status = header.service_result}};
}

std::optional<DecodedResponse> DecodeCloseSessionResponse(
    std::span<const char> body) {
  Decoder decoder{body};
  DecodedResponseHeader header;
  if (!ReadResponseHeader(decoder, header)) {
    return std::nullopt;
  }
  return DecodedResponse{
      .request_handle = header.request_handle,
      .body = CloseSessionResponse{.status = header.service_result}};
}

template <typename Response>
std::optional<DecodedResponse> DecodeStatusCodeArrayResponse(
    std::span<const char> body) {
  Decoder decoder{body};
  DecodedResponseHeader header;
  std::int32_t count = 0;
  if (!ReadResponseHeader(decoder, header) || !decoder.Decode(count)) {
    return std::nullopt;
  }
  Response response{.status = header.service_result};
  if (count < 0) {
    count = 0;
  }
  response.results.resize(static_cast<std::size_t>(count));
  for (auto& status : response.results) {
    std::uint32_t status_word = 0;
    if (!decoder.Decode(status_word)) {
      return std::nullopt;
    }
    status = static_cast<StatusCode>(status_word >> 16);
  }
  if (!SkipTrailingDiagnosticInfo(decoder)) {
    return std::nullopt;
  }
  return DecodedResponse{.request_handle = header.request_handle,
                         .body = std::move(response)};
}

std::optional<DecodedResponse> DecodeRegisterNodesResponse(
    std::span<const char> body) {
  Decoder decoder{body};
  DecodedResponseHeader header;
  std::int32_t count = 0;
  if (!ReadResponseHeader(decoder, header) || !decoder.Decode(count)) {
    return std::nullopt;
  }
  RegisterNodesResponse response{.status = header.service_result};
  if (count < 0)
    count = 0;
  response.registered_node_ids.resize(static_cast<std::size_t>(count));
  for (auto& node_id : response.registered_node_ids) {
    if (!decoder.Decode(node_id))
      return std::nullopt;
  }
  return DecodedResponse{.request_handle = header.request_handle,
                         .body = std::move(response)};
}

std::optional<DecodedResponse> DecodeUnregisterNodesResponse(
    std::span<const char> body) {
  Decoder decoder{body};
  DecodedResponseHeader header;
  if (!ReadResponseHeader(decoder, header)) {
    return std::nullopt;
  }
  return DecodedResponse{
      .request_handle = header.request_handle,
      .body = UnregisterNodesResponse{.status = header.service_result}};
}

// Client-side inverse of the HistoryUpdate response encoder: one
// HistoryUpdateResult (statusCode + per-value operationResults) followed by the
// result-level and response-level diagnosticInfo arrays.
std::optional<DecodedResponse> DecodeHistoryUpdateResponse(
    std::span<const char> body) {
  Decoder decoder{body};
  DecodedResponseHeader header;
  std::int32_t results_count = 0;
  std::uint32_t result_status_word = 0;
  std::int32_t operation_count = 0;
  if (!ReadResponseHeader(decoder, header) || !decoder.Decode(results_count) ||
      results_count != 1 || !decoder.Decode(result_status_word) ||
      !decoder.Decode(operation_count)) {
    return std::nullopt;
  }

  HistoryUpdateResponse response;
  response.result.status = header.service_result;
  if (operation_count < 0) {
    operation_count = 0;
  }
  response.result.operation_results.resize(
      static_cast<std::size_t>(operation_count));
  for (auto& status : response.result.operation_results) {
    std::uint32_t status_word = 0;
    if (!decoder.Decode(status_word)) {
      return std::nullopt;
    }
    status = static_cast<StatusCode>(status_word >> 16);
  }
  // Result-level then response-level diagnosticInfo arrays.
  if (!SkipTrailingDiagnosticInfo(decoder) ||
      !SkipTrailingDiagnosticInfo(decoder)) {
    return std::nullopt;
  }
  return DecodedResponse{.request_handle = header.request_handle,
                         .body = std::move(response)};
}

// Client-side inverse of the HistoryReadRaw response encoder: one HistoryResult
// (statusCode, continuationPoint, HistoryData ExtensionObject) followed by the
// response-level diagnosticInfo array. The response shares one encoding id for
// raw and event reads; the inner HistoryData (id 658) vs HistoryEvent (id 661)
// ExtensionObject distinguishes them, so a non-HistoryData payload (event-read
// response — not yet decoded client-side) is rejected here.
std::optional<DecodedResponse> DecodeHistoryReadResponse(
    std::span<const char> body) {
  Decoder decoder{body};
  DecodedResponseHeader header;
  std::int32_t results_count = 0;
  std::uint32_t result_status_word = 0;
  ByteString continuation_point;
  DecodedExtensionObject history_payload;
  if (!ReadResponseHeader(decoder, header) || !decoder.Decode(results_count) ||
      results_count != 1 || !decoder.Decode(result_status_word) ||
      !decoder.Decode(continuation_point) || !decoder.Decode(history_payload) ||
      !SkipTrailingDiagnosticInfo(decoder)) {
    return std::nullopt;
  }

  if (history_payload.type_id == kHistoryDataEncodingId) {
    HistoryReadRawResponse response;
    response.result.status = header.service_result;
    response.result.continuation_point = std::move(continuation_point);

    Decoder values_decoder{history_payload.body};
    std::int32_t values_count = 0;
    if (!values_decoder.Decode(values_count) ||
        ArrayCountExceedsRemaining(values_decoder, values_count)) {
      return std::nullopt;
    }
    response.result.values.resize(static_cast<std::size_t>(values_count));
    for (auto& value : response.result.values) {
      if (!ReadDataValue(values_decoder, value)) {
        return std::nullopt;
      }
    }
    return DecodedResponse{.request_handle = header.request_handle,
                           .body = std::move(response)};
  }

  if (history_payload.type_id == kHistoryEventEncodingId) {
    // The HistoryEvent body is an array of EventFieldList (one list of select-
    // clause field values per event). Reconstruct each Event from the default
    // BaseEventType field order. The continuation point is ignored for events
    // since the result carries no continuation field.
    HistoryReadEventsResponse response;
    response.result.status = header.service_result;

    Decoder events_decoder{history_payload.body};
    std::int32_t events_count = 0;
    if (!events_decoder.Decode(events_count) ||
        ArrayCountExceedsRemaining(events_decoder, events_count)) {
      return std::nullopt;
    }
    const auto& field_paths = DefaultEventFieldPaths();
    response.result.events.reserve(static_cast<std::size_t>(events_count));
    for (std::int32_t i = 0; i < events_count; ++i) {
      std::int32_t field_count = 0;
      if (!events_decoder.Decode(field_count) ||
          ArrayCountExceedsRemaining(events_decoder, field_count)) {
        return std::nullopt;
      }
      std::vector<Variant> fields(static_cast<std::size_t>(field_count));
      for (auto& field : fields) {
        if (!events_decoder.Decode(field)) {
          return std::nullopt;
        }
      }
      response.result.events.push_back(
          ReconstructEventFromFields(field_paths, fields));
    }
    return DecodedResponse{.request_handle = header.request_handle,
                           .body = std::move(response)};
  }

  return std::nullopt;
}

std::optional<DecodedResponse> DecodeServiceFault(std::span<const char> body) {
  Decoder decoder{body};
  DecodedResponseHeader header;
  if (!ReadResponseHeader(decoder, header)) {
    return std::nullopt;
  }
  return DecodedResponse{.request_handle = header.request_handle,
                         .body = ServiceFault{.status = header.service_result}};
}

std::optional<DecodedResponse> DecodeAddNodesResponse(
    std::span<const char> body) {
  Decoder decoder{body};
  DecodedResponseHeader header;
  std::int32_t count = 0;
  if (!ReadResponseHeader(decoder, header) || !decoder.Decode(count)) {
    return std::nullopt;
  }

  AddNodesResponse response{.status = header.service_result};
  if (count < 0) {
    count = 0;
  }
  response.results.resize(static_cast<std::size_t>(count));
  for (auto& result : response.results) {
    std::uint32_t status_word = 0;
    if (!decoder.Decode(status_word) || !decoder.Decode(result.added_node_id)) {
      return std::nullopt;
    }
    result.status_code = Status::FromFullCode(status_word).code();
  }
  if (!SkipTrailingDiagnosticInfo(decoder)) {
    return std::nullopt;
  }
  return DecodedResponse{.request_handle = header.request_handle,
                         .body = std::move(response)};
}

std::optional<DecodedResponse> DecodeCreateSubscriptionResponse(
    std::span<const char> body) {
  Decoder decoder{body};
  DecodedResponseHeader header;
  CreateSubscriptionResponse response;
  if (!ReadResponseHeader(decoder, header) ||
      !decoder.Decode(response.subscription_id) ||
      !decoder.Decode(response.revised_publishing_interval_ms) ||
      !decoder.Decode(response.revised_lifetime_count) ||
      !decoder.Decode(response.revised_max_keep_alive_count)) {
    return std::nullopt;
  }
  response.status = header.service_result;
  return DecodedResponse{.request_handle = header.request_handle,
                         .body = std::move(response)};
}

std::optional<DecodedResponse> DecodeModifySubscriptionResponse(
    std::span<const char> body) {
  Decoder decoder{body};
  DecodedResponseHeader header;
  ModifySubscriptionResponse response;
  if (!ReadResponseHeader(decoder, header) ||
      !decoder.Decode(response.revised_publishing_interval_ms) ||
      !decoder.Decode(response.revised_lifetime_count) ||
      !decoder.Decode(response.revised_max_keep_alive_count)) {
    return std::nullopt;
  }
  response.status = header.service_result;
  return DecodedResponse{.request_handle = header.request_handle,
                         .body = std::move(response)};
}

std::optional<DecodedResponse> DecodeCreateMonitoredItemsResponse(
    std::span<const char> body) {
  Decoder decoder{body};
  DecodedResponseHeader header;
  std::int32_t count = 0;
  if (!ReadResponseHeader(decoder, header) || !decoder.Decode(count)) {
    return std::nullopt;
  }
  CreateMonitoredItemsResponse response{.status = header.service_result};
  if (count < 0) {
    count = 0;
  }
  response.results.resize(static_cast<std::size_t>(count));
  for (auto& result : response.results) {
    if (!ReadMonitoredItemCreateResult(decoder, result)) {
      return std::nullopt;
    }
  }
  if (!SkipTrailingDiagnosticInfo(decoder)) {
    return std::nullopt;
  }
  return DecodedResponse{.request_handle = header.request_handle,
                         .body = std::move(response)};
}

std::optional<DecodedResponse> DecodeModifyMonitoredItemsResponse(
    std::span<const char> body) {
  Decoder decoder{body};
  DecodedResponseHeader header;
  std::int32_t count = 0;
  if (!ReadResponseHeader(decoder, header) || !decoder.Decode(count)) {
    return std::nullopt;
  }
  ModifyMonitoredItemsResponse response{.status = header.service_result};
  if (count < 0) {
    count = 0;
  }
  response.results.resize(static_cast<std::size_t>(count));
  for (auto& result : response.results) {
    if (!ReadMonitoredItemModifyResult(decoder, result)) {
      return std::nullopt;
    }
  }
  if (!SkipTrailingDiagnosticInfo(decoder)) {
    return std::nullopt;
  }
  return DecodedResponse{.request_handle = header.request_handle,
                         .body = std::move(response)};
}

std::optional<DecodedResponse> DecodePublishResponse(
    std::span<const char> body) {
  Decoder decoder{body};
  DecodedResponseHeader header;
  PublishResponse response;
  std::int32_t sequence_number_count = 0;
  if (!ReadResponseHeader(decoder, header) ||
      !decoder.Decode(response.subscription_id) ||
      !decoder.Decode(sequence_number_count)) {
    return std::nullopt;
  }
  if (sequence_number_count < 0) {
    sequence_number_count = 0;
  }
  response.available_sequence_numbers.resize(
      static_cast<std::size_t>(sequence_number_count));
  for (auto& sequence_number : response.available_sequence_numbers) {
    if (!decoder.Decode(sequence_number)) {
      return std::nullopt;
    }
  }
  if (!decoder.Decode(response.more_notifications) ||
      !ReadNotificationMessage(decoder, response.notification_message)) {
    return std::nullopt;
  }
  std::int32_t result_count = 0;
  if (!decoder.Decode(result_count)) {
    return std::nullopt;
  }
  if (result_count < 0) {
    result_count = 0;
  }
  response.results.resize(static_cast<std::size_t>(result_count));
  for (auto& status : response.results) {
    std::uint32_t status_word = 0;
    if (!decoder.Decode(status_word)) {
      return std::nullopt;
    }
    status = static_cast<StatusCode>(status_word >> 16);
  }
  if (!SkipTrailingDiagnosticInfo(decoder)) {
    return std::nullopt;
  }
  response.status = header.service_result;
  return DecodedResponse{.request_handle = header.request_handle,
                         .body = std::move(response)};
}

std::optional<DecodedResponse> DecodeRepublishResponse(
    std::span<const char> body) {
  Decoder decoder{body};
  DecodedResponseHeader header;
  RepublishResponse response;
  if (!ReadResponseHeader(decoder, header) ||
      !ReadNotificationMessage(decoder, response.notification_message)) {
    return std::nullopt;
  }
  response.status = header.service_result;
  return DecodedResponse{.request_handle = header.request_handle,
                         .body = std::move(response)};
}

std::optional<DecodedResponse> DecodeReadResponse(std::span<const char> body) {
  Decoder decoder{body};
  DecodedResponseHeader header;
  std::int32_t count = 0;
  if (!ReadResponseHeader(decoder, header) || !decoder.Decode(count)) {
    return std::nullopt;
  }
  ReadResponse response{.status = header.service_result};
  if (count < 0) {
    count = 0;
  }
  response.results.resize(static_cast<std::size_t>(count));
  for (auto& value : response.results) {
    if (!ReadDataValue(decoder, value)) {
      return std::nullopt;
    }
  }
  if (!SkipTrailingDiagnosticInfo(decoder)) {
    return std::nullopt;
  }
  return DecodedResponse{.request_handle = header.request_handle,
                         .body = std::move(response)};
}

std::optional<DecodedResponse> DecodeWriteResponse(std::span<const char> body) {
  if (auto decoded = DecodeStatusCodeArrayResponse<WriteResponse>(body)) {
    return decoded;
  }
  return std::nullopt;
}

std::optional<DecodedResponse> DecodeBrowseResponseImpl(
    std::span<const char> body) {
  Decoder decoder{body};
  DecodedResponseHeader header;
  std::int32_t count = 0;
  if (!ReadResponseHeader(decoder, header) || !decoder.Decode(count)) {
    return std::nullopt;
  }
  BrowseResponse response{.status = header.service_result};
  if (count < 0) {
    count = 0;
  }
  response.results.resize(static_cast<std::size_t>(count));
  for (auto& result : response.results) {
    if (!ReadBrowseResult(decoder, result)) {
      return std::nullopt;
    }
  }
  if (!SkipTrailingDiagnosticInfo(decoder)) {
    return std::nullopt;
  }
  return DecodedResponse{.request_handle = header.request_handle,
                         .body = std::move(response)};
}

std::optional<DecodedResponse> DecodeBrowseNextResponseImpl(
    std::span<const char> body) {
  Decoder decoder{body};
  DecodedResponseHeader header;
  std::int32_t count = 0;
  if (!ReadResponseHeader(decoder, header) || !decoder.Decode(count)) {
    return std::nullopt;
  }
  BrowseNextResponse response{.status = header.service_result};
  if (count < 0) {
    count = 0;
  }
  response.results.resize(static_cast<std::size_t>(count));
  for (auto& result : response.results) {
    if (!ReadBrowseResult(decoder, result)) {
      return std::nullopt;
    }
  }
  if (!SkipTrailingDiagnosticInfo(decoder)) {
    return std::nullopt;
  }
  return DecodedResponse{.request_handle = header.request_handle,
                         .body = std::move(response)};
}

std::optional<DecodedResponse> DecodeTranslateBrowsePathsResponse(
    std::span<const char> body) {
  Decoder decoder{body};
  DecodedResponseHeader header;
  std::int32_t count = 0;
  if (!ReadResponseHeader(decoder, header) || !decoder.Decode(count)) {
    return std::nullopt;
  }
  TranslateBrowsePathsResponse response{.status = header.service_result};
  if (count < 0) {
    count = 0;
  }
  response.results.resize(static_cast<std::size_t>(count));
  for (auto& result : response.results) {
    if (!ReadBrowsePathResult(decoder, result)) {
      return std::nullopt;
    }
  }
  if (!SkipTrailingDiagnosticInfo(decoder)) {
    return std::nullopt;
  }
  return DecodedResponse{.request_handle = header.request_handle,
                         .body = std::move(response)};
}

std::optional<DecodedResponse> DecodeCallResponse(std::span<const char> body) {
  Decoder decoder{body};
  DecodedResponseHeader header;
  std::int32_t count = 0;
  if (!ReadResponseHeader(decoder, header) || !decoder.Decode(count)) {
    return std::nullopt;
  }
  CallResponse response;
  response.status = header.service_result;
  if (count < 0) {
    count = 0;
  }
  response.results.resize(static_cast<std::size_t>(count));
  for (auto& result : response.results) {
    std::uint32_t status_word = 0;
    std::int32_t input_argument_count = 0;
    if (!decoder.Decode(status_word) || !decoder.Decode(input_argument_count)) {
      return std::nullopt;
    }
    result.status = Status::FromFullCode(status_word);
    if (input_argument_count < 0) {
      input_argument_count = 0;
    }
    result.input_argument_results.resize(
        static_cast<std::size_t>(input_argument_count));
    for (auto& arg_status : result.input_argument_results) {
      std::uint32_t arg_status_word = 0;
      if (!decoder.Decode(arg_status_word)) {
        return std::nullopt;
      }
      arg_status = static_cast<StatusCode>(arg_status_word >> 16);
    }
    std::int32_t input_argument_diag_count = 0;
    std::int32_t output_count = 0;
    if (!decoder.Decode(input_argument_diag_count) ||
        !decoder.Decode(output_count)) {
      return std::nullopt;
    }
    if (output_count < 0) {
      output_count = 0;
    }
    result.output_arguments.resize(static_cast<std::size_t>(output_count));
    for (auto& output : result.output_arguments) {
      if (!decoder.Decode(output)) {
        return std::nullopt;
      }
    }
  }
  if (!SkipTrailingDiagnosticInfo(decoder)) {
    return std::nullopt;
  }
  return DecodedResponse{.request_handle = header.request_handle,
                         .body = std::move(response)};
}

std::optional<DecodedRequest> DecodeCreateSessionRequest(
    std::span<const char> body) {
  Decoder decoder{body};
  ServiceRequestHeader header;
  if (!ReadRequestHeader(decoder, header) ||
      !SkipApplicationDescriptionFields(decoder)) {
    return std::nullopt;
  }

  std::string ignored;
  ByteString ignored_bytes;
  double requested_timeout_ms = 0;
  std::uint32_t ignored_max_response_size = 0;
  if (!decoder.Decode(ignored) || !decoder.Decode(ignored) ||
      !decoder.Decode(ignored) || !decoder.Decode(ignored_bytes) ||
      !decoder.Decode(ignored_bytes) || !decoder.Decode(requested_timeout_ms) ||
      !decoder.Decode(ignored_max_response_size) || !decoder.consumed()) {
    return std::nullopt;
  }

  return DecodedRequest{
      .header = header,
      .body = CreateSessionRequest{.requested_timeout =
                                       base::TimeDelta::FromMillisecondsD(
                                           requested_timeout_ms)},
  };
}

std::optional<DecodedRequest> DecodeFindServersRequest(
    std::span<const char> body) {
  Decoder decoder{body};
  ServiceRequestHeader header;
  FindServersRequest request;
  if (!ReadRequestHeader(decoder, header) ||
      !decoder.Decode(request.endpoint_url) ||
      !ReadStringArray(decoder, request.locale_ids) ||
      !ReadStringArray(decoder, request.server_uris) || !decoder.consumed()) {
    return std::nullopt;
  }

  return DecodedRequest{.header = header, .body = std::move(request)};
}

std::optional<DecodedRequest> DecodeGetEndpointsRequest(
    std::span<const char> body) {
  Decoder decoder{body};
  ServiceRequestHeader header;
  GetEndpointsRequest request;
  if (!ReadRequestHeader(decoder, header) ||
      !decoder.Decode(request.endpoint_url) ||
      !ReadStringArray(decoder, request.locale_ids) ||
      !ReadStringArray(decoder, request.profile_uris) || !decoder.consumed()) {
    return std::nullopt;
  }

  return DecodedRequest{.header = header, .body = std::move(request)};
}

std::optional<DecodedRequest> DecodeActivateSessionRequest(
    std::span<const char> body) {
  Decoder decoder{body};
  ServiceRequestHeader header;
  std::string client_signature_algorithm;
  ByteString client_signature;
  if (!ReadRequestHeader(decoder, header) ||
      !header.authentication_token.is_numeric() ||
      // clientSignature (SignatureData): signature over
      // (serverCertificate || serverNonce). Captured so the server can verify
      // it (OPC UA Part 4 §5.6.3); empty under SecurityPolicy=None.
      !decoder.Decode(client_signature_algorithm) ||
      !decoder.Decode(client_signature) ||
      !SkipSignedSoftwareCertificates(decoder) || !SkipStringArray(decoder)) {
    return std::nullopt;
  }

  DecodedExtensionObject user_identity;
  if (!decoder.Decode(user_identity) || user_identity.encoding != 0x01 ||
      !SkipSignatureData(decoder) || !decoder.consumed()) {
    return std::nullopt;
  }

  ActivateSessionRequest request{
      .authentication_token = header.authentication_token,
      .client_signature_algorithm = std::move(client_signature_algorithm),
      .client_signature = std::move(client_signature),
  };

  Decoder body_decoder{user_identity.body};
  std::string ignored_policy_id;
  if (!body_decoder.Decode(ignored_policy_id)) {
    return std::nullopt;
  }
  if (user_identity.type_id == kAnonymousIdentityTokenEncodingId) {
    request.allow_anonymous = true;
  } else if (user_identity.type_id == kUserNameIdentityTokenEncodingId) {
    std::string user_name;
    ByteString password;
    std::string encryption_algorithm;
    if (!body_decoder.Decode(user_name) || !body_decoder.Decode(password) ||
        !body_decoder.Decode(encryption_algorithm) ||
        !body_decoder.consumed()) {
      return std::nullopt;
    }
    request.user_name = ToLocalizedText(user_name);
    if (encryption_algorithm.empty()) {
      // Channel-protected (or insecure) token: the password is in the clear.
      request.password =
          ToLocalizedText(std::string{password.begin(), password.end()});
    } else {
      // Encrypted token: the manager decrypts it with the server private key.
      request.encrypted_password = std::move(password);
      request.password_encryption_algorithm = std::move(encryption_algorithm);
    }
  } else {
    return std::nullopt;
  }

  return DecodedRequest{
      .header = header,
      .body = std::move(request),
  };
}

std::optional<DecodedRequest> DecodeCloseSessionRequest(
    std::span<const char> body) {
  Decoder decoder{body};
  ServiceRequestHeader header;
  std::uint8_t delete_subscriptions = 0;
  if (!ReadRequestHeader(decoder, header) ||
      !decoder.Decode(delete_subscriptions) || !decoder.consumed()) {
    return std::nullopt;
  }

  return DecodedRequest{
      .header = header,
      .body =
          CloseSessionRequest{
              .authentication_token = header.authentication_token,
          },
  };
}

std::optional<DecodedRequest> DecodeReadRequest(std::span<const char> body) {
  Decoder decoder{body};
  ServiceRequestHeader header;
  double max_age = 0;
  std::uint32_t timestamps_to_return = 0;
  std::int32_t count = 0;
  if (!ReadRequestHeader(decoder, header) || !decoder.Decode(max_age) ||
      !decoder.Decode(timestamps_to_return) || !decoder.Decode(count) ||
      count < 0) {
    return std::nullopt;
  }
  if (max_age < 0) {
    return std::nullopt;
  }

  ReadRequest request;
  // Range-validated by the service handler so an out-of-range value yields a
  // service-level Bad_TimestampsToReturnInvalid rather than dropping the
  // connection (OPC UA Part 4 §7.40).
  request.timestamps_to_return = timestamps_to_return;
  if (ArrayCountExceedsRemaining(decoder, count))
    return std::nullopt;
  request.inputs.resize(static_cast<std::size_t>(count));
  for (auto& input : request.inputs) {
    if (!DecodeReadValueId(decoder, input)) {
      return std::nullopt;
    }
  }
  if (!decoder.consumed()) {
    return std::nullopt;
  }

  return DecodedRequest{
      .header = header,
      .body = std::move(request),
  };
}

std::optional<DecodedRequest> DecodeWriteRequest(std::span<const char> body) {
  Decoder decoder{body};
  ServiceRequestHeader header;
  std::int32_t count = 0;
  if (!ReadRequestHeader(decoder, header) || !decoder.Decode(count) ||
      count < 0) {
    return std::nullopt;
  }

  WriteRequest request;
  if (ArrayCountExceedsRemaining(decoder, count))
    return std::nullopt;
  request.inputs.resize(static_cast<std::size_t>(count));
  for (auto& input : request.inputs) {
    if (!DecodeWriteValue(decoder, input)) {
      return std::nullopt;
    }
  }
  if (!decoder.consumed()) {
    return std::nullopt;
  }

  return DecodedRequest{
      .header = header,
      .body = std::move(request),
  };
}

std::optional<DecodedRequest> DecodeBrowseRequest(std::span<const char> body) {
  Decoder decoder{body};
  ServiceRequestHeader header;
  NodeId view_id;
  std::int64_t ignored_view_timestamp = 0;
  std::uint32_t ignored_view_version = 0;
  std::uint32_t requested_max_references_per_node = 0;
  std::int32_t count = 0;
  if (!ReadRequestHeader(decoder, header) || !decoder.Decode(view_id) ||
      !decoder.Decode(ignored_view_timestamp) ||
      !decoder.Decode(ignored_view_version) ||
      !decoder.Decode(requested_max_references_per_node) ||
      !decoder.Decode(count) || count < 0) {
    return std::nullopt;
  }

  BrowseRequest request;
  request.requested_max_references_per_node = requested_max_references_per_node;
  request.view_id = std::move(view_id);
  if (ArrayCountExceedsRemaining(decoder, count))
    return std::nullopt;
  request.inputs.resize(static_cast<std::size_t>(count));
  for (auto& input : request.inputs) {
    if (!DecodeBrowseDescription(decoder, input)) {
      return std::nullopt;
    }
  }
  if (!decoder.consumed()) {
    return std::nullopt;
  }

  return DecodedRequest{
      .header = header,
      .body = std::move(request),
  };
}

std::optional<DecodedRequest> DecodeCallRequest(std::span<const char> body) {
  Decoder decoder{body};
  ServiceRequestHeader header;
  std::int32_t count = 0;
  if (!ReadRequestHeader(decoder, header) || !decoder.Decode(count) ||
      count < 0) {
    return std::nullopt;
  }

  CallRequest request;
  if (ArrayCountExceedsRemaining(decoder, count))
    return std::nullopt;
  request.methods.resize(static_cast<std::size_t>(count));
  for (auto& method : request.methods) {
    std::int32_t argument_count = 0;
    if (!decoder.Decode(method.object_id) ||
        !decoder.Decode(method.method_id) || !decoder.Decode(argument_count) ||
        argument_count < 0) {
      return std::nullopt;
    }

    if (ArrayCountExceedsRemaining(decoder, argument_count))
      return std::nullopt;
    method.arguments.reserve(static_cast<std::size_t>(argument_count));
    for (std::int32_t i = 0; i < argument_count; ++i) {
      Variant argument;
      if (!decoder.Decode(argument)) {
        return std::nullopt;
      }
      method.arguments.push_back(std::move(argument));
    }
  }
  if (!decoder.consumed()) {
    return std::nullopt;
  }

  return DecodedRequest{
      .header = header,
      .body = std::move(request),
  };
}

bool DecodeLiteralOperandNodeId(const DecodedExtensionObject& operand,
                                NodeId& node_id) {
  if (operand.type_id != kLiteralOperandEncodingId ||
      operand.encoding != 0x01) {
    return false;
  }

  Decoder decoder{operand.body};
  Variant value;
  if (!decoder.Decode(value) || !decoder.consumed()) {
    return false;
  }

  auto* typed_node_id = value.get_if<NodeId>();
  if (!typed_node_id) {
    return false;
  }
  node_id = *typed_node_id;
  return true;
}

bool DecodeEventFilterBody(Decoder& filter_decoder,
                           EventFilter& filter,
                           std::vector<std::vector<std::string>>& field_paths) {
  std::int32_t select_clause_count = 0;
  if (!filter_decoder.Decode(select_clause_count) || select_clause_count < 0) {
    return false;
  }

  field_paths.clear();
  if (ArrayCountExceedsRemaining(filter_decoder, select_clause_count))
    return false;
  field_paths.reserve(static_cast<std::size_t>(select_clause_count));
  for (std::int32_t i = 0; i < select_clause_count; ++i) {
    DecodedExtensionObject operand;
    if (!filter_decoder.Decode(operand) ||
        operand.type_id != kSimpleAttributeOperandEncodingId ||
        operand.encoding != 0x01) {
      return false;
    }

    Decoder operand_decoder{operand.body};
    NodeId ignored_type_definition_id;
    std::int32_t browse_path_count = 0;
    std::uint32_t ignored_attribute_id = 0;
    std::string ignored_index_range;
    if (!operand_decoder.Decode(ignored_type_definition_id) ||
        !operand_decoder.Decode(browse_path_count) || browse_path_count < 0) {
      return false;
    }

    std::vector<std::string> browse_path;
    if (ArrayCountExceedsRemaining(operand_decoder, browse_path_count))
      return false;
    browse_path.reserve(static_cast<std::size_t>(browse_path_count));
    for (std::int32_t path_index = 0; path_index < browse_path_count;
         ++path_index) {
      QualifiedName segment;
      if (!operand_decoder.Decode(segment)) {
        return false;
      }
      browse_path.emplace_back(segment.name());
    }

    if (!operand_decoder.Decode(ignored_attribute_id) ||
        !operand_decoder.Decode(ignored_index_range) ||
        !operand_decoder.consumed() || !ignored_index_range.empty()) {
      return false;
    }
    field_paths.push_back(std::move(browse_path));
  }

  std::int32_t where_clause_count = 0;
  if (!filter_decoder.Decode(where_clause_count) || where_clause_count < 0) {
    return false;
  }

  filter = {};
  for (std::int32_t i = 0; i < where_clause_count; ++i) {
    std::uint32_t filter_operator = 0;
    std::int32_t operand_count = 0;
    if (!filter_decoder.Decode(filter_operator) ||
        !filter_decoder.Decode(operand_count) || operand_count != 1) {
      return false;
    }

    DecodedExtensionObject operand;
    NodeId node_id;
    if (!filter_decoder.Decode(operand) ||
        !DecodeLiteralOperandNodeId(operand, node_id)) {
      return false;
    }

    if (filter_operator == kFilterOperatorOfType) {
      filter.of_type.push_back(std::move(node_id));
    } else if (filter_operator == kFilterOperatorRelatedTo) {
      filter.child_of.push_back(std::move(node_id));
    } else {
      return false;
    }
  }

  return filter_decoder.consumed();
}

bool DecodeEventFilter(Decoder& decoder,
                       EventFilter& filter,
                       std::vector<std::vector<std::string>>& field_paths) {
  DecodedExtensionObject encoded_filter;
  if (!decoder.Decode(encoded_filter)) {
    return false;
  }
  if (encoded_filter.type_id != kEventFilterEncodingId ||
      encoded_filter.encoding != 0x01) {
    return false;
  }

  Decoder filter_decoder{encoded_filter.body};
  return DecodeEventFilterBody(filter_decoder, filter, field_paths);
}

bool DecodeMonitoringFilter(const DecodedExtensionObject& encoded_filter,
                            std::optional<MonitoringFilter>& filter) {
  if (encoded_filter.type_id == 0 && encoded_filter.encoding == 0x00 &&
      encoded_filter.body.empty()) {
    filter.reset();
    return true;
  }
  if (encoded_filter.encoding != 0x01) {
    return false;
  }

  Decoder decoder{encoded_filter.body};
  if (encoded_filter.type_id == kDataChangeFilterEncodingId) {
    DataChangeFilter data_change_filter;
    std::uint32_t trigger = 0;
    std::uint32_t deadband_type = 0;
    if (!decoder.Decode(trigger) || !decoder.Decode(deadband_type) ||
        !decoder.Decode(data_change_filter.deadband_value) ||
        !decoder.consumed()) {
      return false;
    }
    data_change_filter.trigger = static_cast<DataChangeTrigger>(trigger);
    data_change_filter.deadband_type = static_cast<DeadbandType>(deadband_type);
    if (data_change_filter.trigger != DataChangeTrigger::Status &&
        data_change_filter.trigger != DataChangeTrigger::StatusValue &&
        data_change_filter.trigger != DataChangeTrigger::StatusValueTimestamp) {
      return false;
    }
    if (data_change_filter.deadband_type != DeadbandType::None &&
        data_change_filter.deadband_type != DeadbandType::Absolute &&
        data_change_filter.deadband_type != DeadbandType::Percent) {
      return false;
    }
    filter = std::move(data_change_filter);
    return true;
  }

  if (encoded_filter.type_id == kEventFilterEncodingId) {
    EventFilter ignored_filter;
    std::vector<std::vector<std::string>> field_paths;
    if (!DecodeEventFilterBody(decoder, ignored_filter, field_paths)) {
      return false;
    }
    filter = MonitoringFilter{BuildEventFilter(field_paths)};
    return true;
  }

  return false;
}

std::optional<DecodedRequest> DecodeHistoryReadRawRequest(
    std::span<const char> body) {
  Decoder decoder{body};
  ServiceRequestHeader header;
  DecodedExtensionObject details;
  std::uint32_t timestamps_to_return = 0;
  bool release_continuation_points = false;
  std::int32_t count = 0;
  if (!ReadRequestHeader(decoder, header) || !decoder.Decode(details) ||
      !decoder.Decode(timestamps_to_return) ||
      !decoder.Decode(release_continuation_points) || !decoder.Decode(count) ||
      count != 1) {
    return std::nullopt;
  }

  const auto timestamps = static_cast<TimestampsToReturn>(timestamps_to_return);
  if (timestamps != TimestampsToReturn::Source &&
      timestamps != TimestampsToReturn::Server &&
      timestamps != TimestampsToReturn::Both &&
      timestamps != TimestampsToReturn::Neither) {
    return std::nullopt;
  }

  if (details.type_id != kReadRawModifiedDetailsEncodingId ||
      details.encoding != 0x01) {
    return std::nullopt;
  }

  Decoder details_decoder{details.body};
  bool is_read_modified = false;
  DateTime from;
  DateTime to;
  std::uint32_t max_count = 0;
  bool return_bounds = false;
  if (!details_decoder.Decode(is_read_modified) ||
      !details_decoder.Decode(from) || !details_decoder.Decode(to) ||
      !details_decoder.Decode(max_count) ||
      !details_decoder.Decode(return_bounds) || !details_decoder.consumed() ||
      is_read_modified || return_bounds) {
    return std::nullopt;
  }

  HistoryReadRawRequest request;
  if (!decoder.Decode(request.details.node_id)) {
    return std::nullopt;
  }
  std::string index_range;
  QualifiedName data_encoding;
  if (!decoder.Decode(index_range) || !decoder.Decode(data_encoding) ||
      !decoder.Decode(request.details.continuation_point) ||
      !decoder.consumed() || !index_range.empty() ||
      !data_encoding.name().empty() || data_encoding.namespace_index() != 0) {
    return std::nullopt;
  }

  request.details.from = from;
  request.details.to = to;
  request.details.max_count = max_count;
  request.details.release_continuation_point = release_continuation_points;

  return DecodedRequest{
      .header = header,
      .body = std::move(request),
  };
}

std::optional<DecodedRequest> DecodeHistoryReadEventsRequest(
    std::span<const char> body) {
  Decoder decoder{body};
  ServiceRequestHeader header;
  DecodedExtensionObject details;
  std::uint32_t timestamps_to_return = 0;
  bool release_continuation_points = false;
  std::int32_t count = 0;
  if (!ReadRequestHeader(decoder, header) || !decoder.Decode(details) ||
      !decoder.Decode(timestamps_to_return) ||
      !decoder.Decode(release_continuation_points) || !decoder.Decode(count) ||
      count != 1) {
    return std::nullopt;
  }

  const auto timestamps = static_cast<TimestampsToReturn>(timestamps_to_return);
  if (timestamps != TimestampsToReturn::Source &&
      timestamps != TimestampsToReturn::Server &&
      timestamps != TimestampsToReturn::Both &&
      timestamps != TimestampsToReturn::Neither) {
    return std::nullopt;
  }

  if (details.type_id != kReadEventDetailsEncodingId ||
      details.encoding != 0x01 || release_continuation_points) {
    return std::nullopt;
  }

  Decoder details_decoder{details.body};
  std::uint32_t ignored_num_values_per_node = 0;
  HistoryReadEventsRequest request;
  std::vector<std::vector<std::string>> field_paths;
  if (!details_decoder.Decode(ignored_num_values_per_node) ||
      !details_decoder.Decode(request.details.from) ||
      !details_decoder.Decode(request.details.to) ||
      !DecodeEventFilter(details_decoder, request.details.filter,
                         field_paths) ||
      !details_decoder.consumed()) {
    return std::nullopt;
  }

  if (!decoder.Decode(request.details.node_id)) {
    return std::nullopt;
  }
  std::string index_range;
  QualifiedName data_encoding;
  ByteString continuation_point;
  if (!decoder.Decode(index_range) || !decoder.Decode(data_encoding) ||
      !decoder.Decode(continuation_point) || !decoder.consumed() ||
      !index_range.empty() || !data_encoding.name().empty() ||
      data_encoding.namespace_index() != 0 || !continuation_point.empty()) {
    return std::nullopt;
  }

  field_paths = NormalizeEventFieldPaths(std::move(field_paths));

  return DecodedRequest{
      .header = header,
      .body = std::move(request),
      .history_event_field_paths = std::move(field_paths),
  };
}

// OPC UA Part 4 §5.10.5 HistoryUpdate / Part 11 §6.8 UpdateDataDetails. The
// server supports a single UpdateDataDetails element per request.
std::optional<DecodedRequest> DecodeHistoryUpdateRequest(
    std::span<const char> body) {
  Decoder decoder{body};
  ServiceRequestHeader header;
  std::int32_t count = 0;
  DecodedExtensionObject details;
  if (!ReadRequestHeader(decoder, header) || !decoder.Decode(count) ||
      count != 1 || !decoder.Decode(details) || !decoder.consumed()) {
    return std::nullopt;
  }

  if (details.type_id != kUpdateDataDetailsEncodingId ||
      details.encoding != 0x01) {
    return std::nullopt;
  }

  Decoder details_decoder{details.body};
  HistoryUpdateRequest request;
  std::int32_t perform_update_type = 0;
  std::int32_t values_count = 0;
  if (!details_decoder.Decode(request.details.node_id) ||
      !details_decoder.Decode(perform_update_type) ||
      !details_decoder.Decode(values_count) || values_count < 0 ||
      perform_update_type < 1 || perform_update_type > 4) {
    return std::nullopt;
  }
  request.details.perform_insert_replace =
      static_cast<PerformUpdateType>(perform_update_type);

  if (ArrayCountExceedsRemaining(details_decoder, values_count)) {
    return std::nullopt;
  }
  request.details.values.resize(static_cast<std::size_t>(values_count));
  for (auto& value : request.details.values) {
    if (!ReadDataValue(details_decoder, value)) {
      return std::nullopt;
    }
  }
  if (!details_decoder.consumed()) {
    return std::nullopt;
  }

  return DecodedRequest{
      .header = header,
      .body = std::move(request),
  };
}

std::optional<DecodedRequest> DecodeTranslateBrowsePathsRequest(
    std::span<const char> body) {
  Decoder decoder{body};
  ServiceRequestHeader header;
  std::int32_t count = 0;
  if (!ReadRequestHeader(decoder, header) || !decoder.Decode(count) ||
      count < 0) {
    return std::nullopt;
  }

  TranslateBrowsePathsRequest request;
  if (ArrayCountExceedsRemaining(decoder, count))
    return std::nullopt;
  request.inputs.resize(static_cast<std::size_t>(count));
  for (auto& input : request.inputs) {
    if (!DecodeBrowsePath(decoder, input)) {
      return std::nullopt;
    }
  }
  if (!decoder.consumed()) {
    return std::nullopt;
  }

  return DecodedRequest{
      .header = header,
      .body = std::move(request),
  };
}

std::optional<DecodedRequest> DecodeBrowseNextRequest(
    std::span<const char> body) {
  Decoder decoder{body};
  ServiceRequestHeader header;
  bool release_continuation_points = false;
  std::int32_t count = 0;
  if (!ReadRequestHeader(decoder, header) ||
      !decoder.Decode(release_continuation_points) || !decoder.Decode(count) ||
      count < 0) {
    return std::nullopt;
  }

  BrowseNextRequest request;
  request.release_continuation_points = release_continuation_points;
  if (ArrayCountExceedsRemaining(decoder, count))
    return std::nullopt;
  request.continuation_points.resize(static_cast<std::size_t>(count));
  for (auto& continuation_point : request.continuation_points) {
    if (!decoder.Decode(continuation_point)) {
      return std::nullopt;
    }
  }
  if (!decoder.consumed()) {
    return std::nullopt;
  }

  return DecodedRequest{
      .header = header,
      .body = std::move(request),
  };
}

std::optional<DecodedRequest> DecodeCreateSubscriptionRequest(
    std::span<const char> body) {
  Decoder decoder{body};
  ServiceRequestHeader header;
  CreateSubscriptionRequest request;
  if (!ReadRequestHeader(decoder, header) ||
      !decoder.Decode(request.parameters.publishing_interval_ms) ||
      !decoder.Decode(request.parameters.lifetime_count) ||
      !decoder.Decode(request.parameters.max_keep_alive_count) ||
      !decoder.Decode(request.parameters.max_notifications_per_publish) ||
      !decoder.Decode(request.parameters.publishing_enabled) ||
      !decoder.Decode(request.parameters.priority) || !decoder.consumed()) {
    return std::nullopt;
  }
  return DecodedRequest{
      .header = header,
      .body = std::move(request),
  };
}

std::optional<DecodedRequest> DecodeDeleteSubscriptionsRequest(
    std::span<const char> body) {
  Decoder decoder{body};
  ServiceRequestHeader header;
  std::int32_t count = 0;
  if (!ReadRequestHeader(decoder, header) || !decoder.Decode(count) ||
      count < 0) {
    return std::nullopt;
  }
  DeleteSubscriptionsRequest request;
  if (ArrayCountExceedsRemaining(decoder, count))
    return std::nullopt;
  request.subscription_ids.resize(static_cast<std::size_t>(count));
  for (auto& subscription_id : request.subscription_ids) {
    if (!decoder.Decode(subscription_id)) {
      return std::nullopt;
    }
  }
  if (!decoder.consumed()) {
    return std::nullopt;
  }
  return DecodedRequest{
      .header = header,
      .body = std::move(request),
  };
}

std::optional<DecodedRequest> DecodeModifySubscriptionRequest(
    std::span<const char> body) {
  Decoder decoder{body};
  ServiceRequestHeader header;
  ModifySubscriptionRequest request;
  if (!ReadRequestHeader(decoder, header) ||
      !decoder.Decode(request.subscription_id) ||
      !decoder.Decode(request.parameters.publishing_interval_ms) ||
      !decoder.Decode(request.parameters.lifetime_count) ||
      !decoder.Decode(request.parameters.max_keep_alive_count) ||
      !decoder.Decode(request.parameters.max_notifications_per_publish) ||
      !decoder.Decode(request.parameters.priority) || !decoder.consumed()) {
    return std::nullopt;
  }
  return DecodedRequest{
      .header = header,
      .body = std::move(request),
  };
}

std::optional<DecodedRequest> DecodeSetPublishingModeRequest(
    std::span<const char> body) {
  Decoder decoder{body};
  ServiceRequestHeader header;
  SetPublishingModeRequest request;
  std::int32_t count = 0;
  if (!ReadRequestHeader(decoder, header) ||
      !decoder.Decode(request.publishing_enabled) || !decoder.Decode(count) ||
      count < 0) {
    return std::nullopt;
  }

  if (ArrayCountExceedsRemaining(decoder, count))
    return std::nullopt;
  request.subscription_ids.resize(static_cast<std::size_t>(count));
  for (auto& subscription_id : request.subscription_ids) {
    if (!decoder.Decode(subscription_id)) {
      return std::nullopt;
    }
  }
  if (!decoder.consumed()) {
    return std::nullopt;
  }
  return DecodedRequest{
      .header = header,
      .body = std::move(request),
  };
}

std::optional<DecodedRequest> DecodeCreateMonitoredItemsRequest(
    std::span<const char> body) {
  Decoder decoder{body};
  ServiceRequestHeader header;
  CreateMonitoredItemsRequest request;
  std::uint32_t timestamps_to_return = 0;
  std::int32_t count = 0;
  if (!ReadRequestHeader(decoder, header) ||
      !decoder.Decode(request.subscription_id) ||
      !decoder.Decode(timestamps_to_return) || !decoder.Decode(count) ||
      count < 0) {
    return std::nullopt;
  }

  request.timestamps_to_return =
      static_cast<TimestampsToReturn>(timestamps_to_return);
  if (request.timestamps_to_return != TimestampsToReturn::Source &&
      request.timestamps_to_return != TimestampsToReturn::Server &&
      request.timestamps_to_return != TimestampsToReturn::Both &&
      request.timestamps_to_return != TimestampsToReturn::Neither) {
    return std::nullopt;
  }

  if (ArrayCountExceedsRemaining(decoder, count))
    return std::nullopt;
  request.items_to_create.resize(static_cast<std::size_t>(count));
  for (auto& item : request.items_to_create) {
    std::uint32_t attribute_id = 0;
    std::string index_range;
    QualifiedName ignored_data_encoding;
    std::uint32_t monitoring_mode = 0;
    DecodedExtensionObject filter;
    if (!decoder.Decode(item.item_to_monitor.node_id) ||
        !decoder.Decode(attribute_id) || !decoder.Decode(index_range) ||
        !decoder.Decode(ignored_data_encoding) ||
        !decoder.Decode(monitoring_mode) ||
        !decoder.Decode(item.requested_parameters.client_handle) ||
        !decoder.Decode(item.requested_parameters.sampling_interval_ms) ||
        !decoder.Decode(filter) ||
        !decoder.Decode(item.requested_parameters.queue_size) ||
        !decoder.Decode(item.requested_parameters.discard_oldest)) {
      return std::nullopt;
    }
    item.item_to_monitor.attribute_id = static_cast<AttributeId>(attribute_id);
    if (!index_range.empty()) {
      item.index_range = index_range;
    }
    item.monitoring_mode = static_cast<MonitoringMode>(monitoring_mode);
    if (item.monitoring_mode != MonitoringMode::Disabled &&
        item.monitoring_mode != MonitoringMode::Sampling &&
        item.monitoring_mode != MonitoringMode::Reporting) {
      return std::nullopt;
    }
    if (!DecodeMonitoringFilter(filter, item.requested_parameters.filter)) {
      return std::nullopt;
    }
  }
  if (!decoder.consumed()) {
    return std::nullopt;
  }
  return DecodedRequest{
      .header = header,
      .body = std::move(request),
  };
}

std::optional<DecodedRequest> DecodeModifyMonitoredItemsRequest(
    std::span<const char> body) {
  Decoder decoder{body};
  ServiceRequestHeader header;
  ModifyMonitoredItemsRequest request;
  std::uint32_t timestamps_to_return = 0;
  std::int32_t count = 0;
  if (!ReadRequestHeader(decoder, header) ||
      !decoder.Decode(request.subscription_id) ||
      !decoder.Decode(timestamps_to_return) || !decoder.Decode(count) ||
      count < 0) {
    return std::nullopt;
  }

  request.timestamps_to_return =
      static_cast<TimestampsToReturn>(timestamps_to_return);
  if (request.timestamps_to_return != TimestampsToReturn::Source &&
      request.timestamps_to_return != TimestampsToReturn::Server &&
      request.timestamps_to_return != TimestampsToReturn::Both &&
      request.timestamps_to_return != TimestampsToReturn::Neither) {
    return std::nullopt;
  }

  if (ArrayCountExceedsRemaining(decoder, count))
    return std::nullopt;
  request.items_to_modify.resize(static_cast<std::size_t>(count));
  for (auto& item : request.items_to_modify) {
    DecodedExtensionObject filter;
    if (!decoder.Decode(item.monitored_item_id) ||
        !decoder.Decode(item.requested_parameters.client_handle) ||
        !decoder.Decode(item.requested_parameters.sampling_interval_ms) ||
        !decoder.Decode(filter) ||
        !decoder.Decode(item.requested_parameters.queue_size) ||
        !decoder.Decode(item.requested_parameters.discard_oldest)) {
      return std::nullopt;
    }
    if (!DecodeMonitoringFilter(filter, item.requested_parameters.filter)) {
      return std::nullopt;
    }
  }
  if (!decoder.consumed()) {
    return std::nullopt;
  }
  return DecodedRequest{
      .header = header,
      .body = std::move(request),
  };
}

std::optional<DecodedRequest> DecodePublishRequest(std::span<const char> body) {
  Decoder decoder{body};
  ServiceRequestHeader header;
  std::int32_t count = 0;
  if (!ReadRequestHeader(decoder, header) || !decoder.Decode(count) ||
      count < 0) {
    return std::nullopt;
  }

  PublishRequest request;
  if (ArrayCountExceedsRemaining(decoder, count))
    return std::nullopt;
  request.subscription_acknowledgements.resize(static_cast<std::size_t>(count));
  for (auto& acknowledgement : request.subscription_acknowledgements) {
    if (!decoder.Decode(acknowledgement.subscription_id) ||
        !decoder.Decode(acknowledgement.sequence_number)) {
      return std::nullopt;
    }
  }
  if (!decoder.consumed()) {
    return std::nullopt;
  }
  return DecodedRequest{
      .header = header,
      .body = std::move(request),
  };
}

std::optional<DecodedRequest> DecodeTransferSubscriptionsRequest(
    std::span<const char> body) {
  Decoder decoder{body};
  ServiceRequestHeader header;
  TransferSubscriptionsRequest request;
  std::int32_t count = 0;
  if (!ReadRequestHeader(decoder, header) || !decoder.Decode(count) ||
      count < 0) {
    return std::nullopt;
  }

  if (ArrayCountExceedsRemaining(decoder, count))
    return std::nullopt;
  request.subscription_ids.resize(static_cast<std::size_t>(count));
  for (auto& subscription_id : request.subscription_ids) {
    if (!decoder.Decode(subscription_id)) {
      return std::nullopt;
    }
  }
  if (!decoder.Decode(request.send_initial_values) || !decoder.consumed()) {
    return std::nullopt;
  }

  return DecodedRequest{
      .header = header,
      .body = std::move(request),
  };
}

std::optional<DecodedRequest> DecodeDeleteMonitoredItemsRequest(
    std::span<const char> body) {
  Decoder decoder{body};
  ServiceRequestHeader header;
  DeleteMonitoredItemsRequest request;
  std::int32_t count = 0;
  if (!ReadRequestHeader(decoder, header) ||
      !decoder.Decode(request.subscription_id) || !decoder.Decode(count) ||
      count < 0) {
    return std::nullopt;
  }
  if (ArrayCountExceedsRemaining(decoder, count))
    return std::nullopt;
  request.monitored_item_ids.resize(static_cast<std::size_t>(count));
  for (auto& monitored_item_id : request.monitored_item_ids) {
    if (!decoder.Decode(monitored_item_id)) {
      return std::nullopt;
    }
  }
  if (!decoder.consumed()) {
    return std::nullopt;
  }
  return DecodedRequest{
      .header = header,
      .body = std::move(request),
  };
}

std::optional<DecodedRequest> DecodeSetMonitoringModeRequest(
    std::span<const char> body) {
  Decoder decoder{body};
  ServiceRequestHeader header;
  SetMonitoringModeRequest request;
  std::uint32_t monitoring_mode = 0;
  std::int32_t count = 0;
  if (!ReadRequestHeader(decoder, header) ||
      !decoder.Decode(request.subscription_id) ||
      !decoder.Decode(monitoring_mode) || !decoder.Decode(count) || count < 0) {
    return std::nullopt;
  }
  request.monitoring_mode = static_cast<MonitoringMode>(monitoring_mode);
  if (request.monitoring_mode != MonitoringMode::Disabled &&
      request.monitoring_mode != MonitoringMode::Sampling &&
      request.monitoring_mode != MonitoringMode::Reporting) {
    return std::nullopt;
  }
  if (ArrayCountExceedsRemaining(decoder, count))
    return std::nullopt;
  request.monitored_item_ids.resize(static_cast<std::size_t>(count));
  for (auto& monitored_item_id : request.monitored_item_ids) {
    if (!decoder.Decode(monitored_item_id)) {
      return std::nullopt;
    }
  }
  if (!decoder.consumed()) {
    return std::nullopt;
  }
  return DecodedRequest{
      .header = header,
      .body = std::move(request),
  };
}

std::optional<DecodedRequest> DecodeRepublishRequest(
    std::span<const char> body) {
  Decoder decoder{body};
  ServiceRequestHeader header;
  RepublishRequest request;
  if (!ReadRequestHeader(decoder, header) ||
      !decoder.Decode(request.subscription_id) ||
      !decoder.Decode(request.retransmit_sequence_number) ||
      !decoder.consumed()) {
    return std::nullopt;
  }
  return DecodedRequest{
      .header = header,
      .body = std::move(request),
  };
}

std::optional<DecodedRequest> DecodeDeleteNodesRequest(
    std::span<const char> body) {
  Decoder decoder{body};
  ServiceRequestHeader header;
  std::int32_t count = 0;
  if (!ReadRequestHeader(decoder, header) || !decoder.Decode(count) ||
      count < 0) {
    return std::nullopt;
  }

  DeleteNodesRequest request;
  if (ArrayCountExceedsRemaining(decoder, count))
    return std::nullopt;
  request.items.resize(static_cast<std::size_t>(count));
  for (auto& item : request.items) {
    if (!DecodeDeleteNodesItem(decoder, item)) {
      return std::nullopt;
    }
  }
  if (!decoder.consumed()) {
    return std::nullopt;
  }

  return DecodedRequest{
      .header = header,
      .body = std::move(request),
  };
}

std::optional<DecodedRequest> DecodeAddNodesRequest(
    std::span<const char> body) {
  Decoder decoder{body};
  ServiceRequestHeader header;
  std::int32_t count = 0;
  if (!ReadRequestHeader(decoder, header) || !decoder.Decode(count) ||
      count < 0) {
    return std::nullopt;
  }

  AddNodesRequest request;
  if (ArrayCountExceedsRemaining(decoder, count))
    return std::nullopt;
  request.items.resize(static_cast<std::size_t>(count));
  for (auto& item : request.items) {
    if (!DecodeAddNodesItem(decoder, item)) {
      return std::nullopt;
    }
  }
  if (!decoder.consumed()) {
    return std::nullopt;
  }

  return DecodedRequest{
      .header = header,
      .body = std::move(request),
  };
}

std::optional<DecodedRequest> DecodeDeleteReferencesRequest(
    std::span<const char> body) {
  Decoder decoder{body};
  ServiceRequestHeader header;
  std::int32_t count = 0;
  if (!ReadRequestHeader(decoder, header) || !decoder.Decode(count) ||
      count < 0) {
    return std::nullopt;
  }

  DeleteReferencesRequest request;
  if (ArrayCountExceedsRemaining(decoder, count))
    return std::nullopt;
  request.items.resize(static_cast<std::size_t>(count));
  for (auto& item : request.items) {
    if (!DecodeDeleteReferencesItem(decoder, item)) {
      return std::nullopt;
    }
  }
  if (!decoder.consumed()) {
    return std::nullopt;
  }

  return DecodedRequest{
      .header = header,
      .body = std::move(request),
  };
}

std::optional<DecodedRequest> DecodeAddReferencesRequest(
    std::span<const char> body) {
  Decoder decoder{body};
  ServiceRequestHeader header;
  std::int32_t count = 0;
  if (!ReadRequestHeader(decoder, header) || !decoder.Decode(count) ||
      count < 0) {
    return std::nullopt;
  }

  AddReferencesRequest request;
  if (ArrayCountExceedsRemaining(decoder, count))
    return std::nullopt;
  request.items.resize(static_cast<std::size_t>(count));
  for (auto& item : request.items) {
    if (!DecodeAddReferencesItem(decoder, item)) {
      return std::nullopt;
    }
  }
  if (!decoder.consumed()) {
    return std::nullopt;
  }

  return DecodedRequest{
      .header = header,
      .body = std::move(request),
  };
}

}  // namespace

std::optional<std::vector<char>> EncodeServiceRequest(
    const ServiceRequestHeader& header,
    const RequestBody& request) {
  return std::visit(
      [&](const auto& typed_request) -> std::optional<std::vector<char>> {
        std::vector<char> payload;
        std::vector<char> body;
        using T = std::decay_t<decltype(typed_request)>;
        if constexpr (std::is_same_v<T, ReadRequest>) {
          payload.reserve(EstimateReadRequestPayloadSize(typed_request));
        } else if constexpr (std::is_same_v<T, BrowseRequest>) {
          payload.reserve(EstimateBrowseRequestPayloadSize(typed_request));
        }
        Encoder payload_encoder{payload};
        Encoder body_encoder{body};

        if constexpr (std::is_same_v<T, FindServersRequest>) {
          AppendRequestHeader(payload_encoder, header);
          payload_encoder.Encode(typed_request.endpoint_url);
          AppendStringArray(payload_encoder, typed_request.locale_ids);
          AppendStringArray(payload_encoder, typed_request.server_uris);
          AppendMessage(body_encoder, kFindServersRequestEncodingId, payload);
        } else if constexpr (std::is_same_v<T, GetEndpointsRequest>) {
          AppendRequestHeader(payload_encoder, header);
          payload_encoder.Encode(typed_request.endpoint_url);
          AppendStringArray(payload_encoder, typed_request.locale_ids);
          AppendStringArray(payload_encoder, typed_request.profile_uris);
          AppendMessage(body_encoder, kGetEndpointsRequestEncodingId, payload);
        } else if constexpr (std::is_same_v<T, CreateSessionRequest>) {
          AppendRequestHeader(payload_encoder, header);
          payload_encoder.Encode(std::string_view{""});
          payload_encoder.Encode(std::string_view{""});
          payload_encoder.Encode(std::uint8_t{0});
          payload_encoder.Encode(std::int32_t{0});
          payload_encoder.Encode(std::string_view{""});
          payload_encoder.Encode(std::string_view{""});
          payload_encoder.Encode(std::int32_t{-1});
          payload_encoder.Encode(std::string_view{""});
          payload_encoder.Encode(std::string_view{"opc.tcp://localhost:4840"});
          payload_encoder.Encode(std::string_view{"binary-session"});
          // clientNonce, clientCertificate. Populated for a secured session so
          // the server can verify the ActivateSession signature.
          payload_encoder.Encode(typed_request.client_nonce);
          payload_encoder.Encode(typed_request.client_certificate);
          payload_encoder.Encode(
              typed_request.requested_timeout.InMillisecondsF());
          payload_encoder.Encode(std::uint32_t{0});
          AppendMessage(body_encoder, kCreateSessionRequestEncodingId, payload);
        } else if constexpr (std::is_same_v<T, ActivateSessionRequest>) {
          AppendRequestHeader(payload_encoder, header);
          // clientSignature (SignatureData): algorithm + signature over
          // (serverCertificate || serverNonce). Empty under None.
          payload_encoder.Encode(typed_request.client_signature_algorithm);
          payload_encoder.Encode(typed_request.client_signature);
          payload_encoder.Encode(std::int32_t{-1});
          payload_encoder.Encode(std::int32_t{-1});
          if (typed_request.allow_anonymous) {
            std::vector<char> identity;
            Encoder identity_encoder{identity};
            identity_encoder.Encode(std::string_view{""});
            payload_encoder.Encode(EncodedExtensionObject{
                .type_id = kAnonymousIdentityTokenEncodingId,
                .body = identity});
          } else {
            std::vector<char> identity;
            Encoder identity_encoder{identity};
            identity_encoder.Encode(std::string_view{""});
            identity_encoder.Encode(typed_request.user_name.has_value()
                                        ? ToString(*typed_request.user_name)
                                        : std::string{});
            const auto password = typed_request.password.has_value()
                                      ? ToString(*typed_request.password)
                                      : std::string{};
            identity_encoder.Encode(
                ByteString{password.begin(), password.end()});
            identity_encoder.Encode(std::string_view{""});
            payload_encoder.Encode(EncodedExtensionObject{
                .type_id = kUserNameIdentityTokenEncodingId, .body = identity});
          }
          payload_encoder.Encode(std::string_view{""});
          payload_encoder.Encode(ByteString{});
          AppendMessage(body_encoder, kActivateSessionRequestEncodingId,
                        payload);
        } else if constexpr (std::is_same_v<T, CloseSessionRequest>) {
          AppendRequestHeader(payload_encoder, header);
          payload_encoder.Encode(std::uint8_t{1});
          AppendMessage(body_encoder, kCloseSessionRequestEncodingId, payload);
        } else if constexpr (std::is_same_v<T, CreateSubscriptionRequest>) {
          AppendRequestHeader(payload_encoder, header);
          payload_encoder.Encode(
              typed_request.parameters.publishing_interval_ms);
          payload_encoder.Encode(typed_request.parameters.lifetime_count);
          payload_encoder.Encode(typed_request.parameters.max_keep_alive_count);
          payload_encoder.Encode(
              typed_request.parameters.max_notifications_per_publish);
          payload_encoder.Encode(typed_request.parameters.publishing_enabled);
          payload_encoder.Encode(typed_request.parameters.priority);
          AppendMessage(body_encoder, kCreateSubscriptionRequestEncodingId,
                        payload);
        } else if constexpr (std::is_same_v<T, ModifySubscriptionRequest>) {
          AppendRequestHeader(payload_encoder, header);
          payload_encoder.Encode(typed_request.subscription_id);
          payload_encoder.Encode(
              typed_request.parameters.publishing_interval_ms);
          payload_encoder.Encode(typed_request.parameters.lifetime_count);
          payload_encoder.Encode(typed_request.parameters.max_keep_alive_count);
          payload_encoder.Encode(
              typed_request.parameters.max_notifications_per_publish);
          payload_encoder.Encode(typed_request.parameters.priority);
          AppendMessage(body_encoder, kModifySubscriptionRequestEncodingId,
                        payload);
        } else if constexpr (std::is_same_v<T, SetPublishingModeRequest>) {
          AppendRequestHeader(payload_encoder, header);
          payload_encoder.Encode(typed_request.publishing_enabled);
          payload_encoder.Encode(
              static_cast<std::int32_t>(typed_request.subscription_ids.size()));
          for (const auto subscription_id : typed_request.subscription_ids) {
            payload_encoder.Encode(subscription_id);
          }
          AppendMessage(body_encoder, kSetPublishingModeRequestEncodingId,
                        payload);
        } else if constexpr (std::is_same_v<T, DeleteSubscriptionsRequest>) {
          AppendRequestHeader(payload_encoder, header);
          payload_encoder.Encode(
              static_cast<std::int32_t>(typed_request.subscription_ids.size()));
          for (const auto subscription_id : typed_request.subscription_ids) {
            payload_encoder.Encode(subscription_id);
          }
          AppendMessage(body_encoder, kDeleteSubscriptionsRequestEncodingId,
                        payload);
        } else if constexpr (std::is_same_v<T, CreateMonitoredItemsRequest>) {
          AppendRequestHeader(payload_encoder, header);
          payload_encoder.Encode(typed_request.subscription_id);
          payload_encoder.Encode(
              static_cast<std::uint32_t>(typed_request.timestamps_to_return));
          payload_encoder.Encode(
              static_cast<std::int32_t>(typed_request.items_to_create.size()));
          for (const auto& item : typed_request.items_to_create) {
            AppendMonitoredItemCreateRequest(payload_encoder, item);
          }
          AppendMessage(body_encoder, kCreateMonitoredItemsRequestEncodingId,
                        payload);
        } else if constexpr (std::is_same_v<T, ModifyMonitoredItemsRequest>) {
          AppendRequestHeader(payload_encoder, header);
          payload_encoder.Encode(typed_request.subscription_id);
          payload_encoder.Encode(
              static_cast<std::uint32_t>(typed_request.timestamps_to_return));
          payload_encoder.Encode(
              static_cast<std::int32_t>(typed_request.items_to_modify.size()));
          for (const auto& item : typed_request.items_to_modify) {
            payload_encoder.Encode(item.monitored_item_id);
            payload_encoder.Encode(item.requested_parameters.client_handle);
            payload_encoder.Encode(
                item.requested_parameters.sampling_interval_ms);
            AppendMonitoringFilter(payload_encoder,
                                   item.requested_parameters.filter);
            payload_encoder.Encode(item.requested_parameters.queue_size);
            payload_encoder.Encode(item.requested_parameters.discard_oldest);
          }
          AppendMessage(body_encoder, kModifyMonitoredItemsRequestEncodingId,
                        payload);
        } else if constexpr (std::is_same_v<T, PublishRequest>) {
          AppendRequestHeader(payload_encoder, header);
          payload_encoder.Encode(static_cast<std::int32_t>(
              typed_request.subscription_acknowledgements.size()));
          for (const auto& acknowledgement :
               typed_request.subscription_acknowledgements) {
            payload_encoder.Encode(acknowledgement.subscription_id);
            payload_encoder.Encode(acknowledgement.sequence_number);
          }
          AppendMessage(body_encoder, kPublishRequestEncodingId, payload);
        } else if constexpr (std::is_same_v<T, RepublishRequest>) {
          AppendRequestHeader(payload_encoder, header);
          payload_encoder.Encode(typed_request.subscription_id);
          payload_encoder.Encode(typed_request.retransmit_sequence_number);
          AppendMessage(body_encoder, kRepublishRequestEncodingId, payload);
        } else if constexpr (std::is_same_v<T, TransferSubscriptionsRequest>) {
          AppendRequestHeader(payload_encoder, header);
          payload_encoder.Encode(
              static_cast<std::int32_t>(typed_request.subscription_ids.size()));
          for (const auto subscription_id : typed_request.subscription_ids) {
            payload_encoder.Encode(subscription_id);
          }
          payload_encoder.Encode(typed_request.send_initial_values);
          AppendMessage(body_encoder, kTransferSubscriptionsRequestEncodingId,
                        payload);
        } else if constexpr (std::is_same_v<T, DeleteMonitoredItemsRequest>) {
          AppendRequestHeader(payload_encoder, header);
          payload_encoder.Encode(typed_request.subscription_id);
          payload_encoder.Encode(static_cast<std::int32_t>(
              typed_request.monitored_item_ids.size()));
          for (const auto monitored_item_id :
               typed_request.monitored_item_ids) {
            payload_encoder.Encode(monitored_item_id);
          }
          AppendMessage(body_encoder, kDeleteMonitoredItemsRequestEncodingId,
                        payload);
        } else if constexpr (std::is_same_v<T, SetMonitoringModeRequest>) {
          AppendRequestHeader(payload_encoder, header);
          payload_encoder.Encode(typed_request.subscription_id);
          payload_encoder.Encode(
              static_cast<std::uint32_t>(typed_request.monitoring_mode));
          payload_encoder.Encode(static_cast<std::int32_t>(
              typed_request.monitored_item_ids.size()));
          for (const auto monitored_item_id :
               typed_request.monitored_item_ids) {
            payload_encoder.Encode(monitored_item_id);
          }
          AppendMessage(body_encoder, kSetMonitoringModeRequestEncodingId,
                        payload);
        } else if constexpr (std::is_same_v<T, ReadRequest>) {
          AppendRequestHeader(payload_encoder, header);
          payload_encoder.Encode(0.0);
          payload_encoder.Encode(
              static_cast<std::uint32_t>(WireTimestampsToReturn::Both));
          payload_encoder.Encode(
              static_cast<std::int32_t>(typed_request.inputs.size()));
          for (const auto& input : typed_request.inputs) {
            payload_encoder.Encode(input.node_id);
            payload_encoder.Encode(
                static_cast<std::uint32_t>(input.attribute_id));
            payload_encoder.Encode(std::string_view{""});
            payload_encoder.Encode(std::uint16_t{0});
            payload_encoder.Encode(std::string_view{""});
          }
          AppendMessage(body_encoder, kReadRequestEncodingId, payload);
        } else if constexpr (std::is_same_v<T, WriteRequest>) {
          AppendRequestHeader(payload_encoder, header);
          payload_encoder.Encode(
              static_cast<std::int32_t>(typed_request.inputs.size()));
          for (const auto& input : typed_request.inputs) {
            payload_encoder.Encode(input.node_id);
            payload_encoder.Encode(
                static_cast<std::uint32_t>(input.attribute_id));
            payload_encoder.Encode(std::string_view{""});
            payload_encoder.Encode(std::uint8_t{0x01});
            payload_encoder.Encode(input.value);
          }
          AppendMessage(body_encoder, kWriteRequestEncodingId, payload);
        } else if constexpr (std::is_same_v<T, BrowseRequest>) {
          AppendRequestHeader(payload_encoder, header);
          payload_encoder.Encode(typed_request.view_id);
          payload_encoder.Encode(std::int64_t{0});
          payload_encoder.Encode(std::uint32_t{0});
          payload_encoder.Encode(static_cast<std::uint32_t>(
              typed_request.requested_max_references_per_node));
          payload_encoder.Encode(
              static_cast<std::int32_t>(typed_request.inputs.size()));
          for (const auto& input : typed_request.inputs) {
            payload_encoder.Encode(input.node_id);
            payload_encoder.Encode(static_cast<std::uint32_t>(input.direction));
            payload_encoder.Encode(input.reference_type_id);
            payload_encoder.Encode(input.include_subtypes);
            payload_encoder.Encode(input.node_class_mask);
            payload_encoder.Encode(input.result_mask);
          }
          AppendMessage(body_encoder, kBrowseRequestEncodingId, payload);
        } else if constexpr (std::is_same_v<T, BrowseNextRequest>) {
          AppendRequestHeader(payload_encoder, header);
          payload_encoder.Encode(typed_request.release_continuation_points);
          payload_encoder.Encode(static_cast<std::int32_t>(
              typed_request.continuation_points.size()));
          for (const auto& continuation_point :
               typed_request.continuation_points) {
            payload_encoder.Encode(continuation_point);
          }
          AppendMessage(body_encoder, kBrowseNextRequestEncodingId, payload);
        } else if constexpr (std::is_same_v<T, TranslateBrowsePathsRequest>) {
          AppendRequestHeader(payload_encoder, header);
          payload_encoder.Encode(
              static_cast<std::int32_t>(typed_request.inputs.size()));
          for (const auto& input : typed_request.inputs) {
            AppendBrowsePath(payload_encoder, input);
          }
          AppendMessage(body_encoder, kTranslateBrowsePathsRequestEncodingId,
                        payload);
        } else if constexpr (std::is_same_v<T, CallRequest>) {
          AppendRequestHeader(payload_encoder, header);
          payload_encoder.Encode(
              static_cast<std::int32_t>(typed_request.methods.size()));
          for (const auto& method : typed_request.methods) {
            payload_encoder.Encode(method.object_id);
            payload_encoder.Encode(method.method_id);
            payload_encoder.Encode(
                static_cast<std::int32_t>(method.arguments.size()));
            for (const auto& argument : method.arguments) {
              payload_encoder.Encode(argument);
            }
          }
          AppendMessage(body_encoder, kCallRequestEncodingId, payload);
        } else if constexpr (std::is_same_v<T, HistoryReadRawRequest>) {
          AppendRequestHeader(payload_encoder, header);

          std::vector<char> details;
          Encoder details_encoder{details};
          details_encoder.Encode(false);
          details_encoder.Encode(typed_request.details.from);
          details_encoder.Encode(typed_request.details.to);
          details_encoder.Encode(
              static_cast<std::uint32_t>(typed_request.details.max_count));
          details_encoder.Encode(false);

          payload_encoder.Encode(EncodedExtensionObject{
              .type_id = kReadRawModifiedDetailsEncodingId,
              .body = std::move(details),
          });
          payload_encoder.Encode(
              static_cast<std::uint32_t>(WireTimestampsToReturn::Both));
          payload_encoder.Encode(
              typed_request.details.release_continuation_point);
          payload_encoder.Encode(std::int32_t{1});
          payload_encoder.Encode(typed_request.details.node_id);
          payload_encoder.Encode(std::string_view{""});
          payload_encoder.Encode(QualifiedName{});
          payload_encoder.Encode(typed_request.details.continuation_point);
          AppendMessage(body_encoder, kHistoryReadRequestEncodingId, payload);
        } else if constexpr (std::is_same_v<T, HistoryReadEventsRequest>) {
          AppendRequestHeader(payload_encoder, header);

          std::vector<char> details;
          Encoder details_encoder{details};
          details_encoder.Encode(std::uint32_t{0});
          details_encoder.Encode(typed_request.details.from);
          details_encoder.Encode(typed_request.details.to);
          const auto& field_paths = DefaultEventFieldPaths();
          AppendEventFilter(details_encoder, field_paths,
                            typed_request.details.filter);

          payload_encoder.Encode(EncodedExtensionObject{
              .type_id = kReadEventDetailsEncodingId,
              .body = std::move(details),
          });
          payload_encoder.Encode(
              static_cast<std::uint32_t>(WireTimestampsToReturn::Both));
          payload_encoder.Encode(false);
          payload_encoder.Encode(std::int32_t{1});
          payload_encoder.Encode(typed_request.details.node_id);
          payload_encoder.Encode(std::string_view{""});
          payload_encoder.Encode(QualifiedName{});
          payload_encoder.Encode(ByteString{});
          AppendMessage(body_encoder, kHistoryReadRequestEncodingId, payload);
        } else if constexpr (std::is_same_v<T, HistoryUpdateRequest>) {
          AppendRequestHeader(payload_encoder, header);

          std::vector<char> details;
          Encoder details_encoder{details};
          details_encoder.Encode(typed_request.details.node_id);
          details_encoder.Encode(static_cast<std::int32_t>(
              typed_request.details.perform_insert_replace));
          details_encoder.Encode(
              static_cast<std::int32_t>(typed_request.details.values.size()));
          for (const auto& value : typed_request.details.values) {
            AppendDataValue(details_encoder, value);
          }

          payload_encoder.Encode(std::int32_t{1});
          payload_encoder.Encode(EncodedExtensionObject{
              .type_id = kUpdateDataDetailsEncodingId,
              .body = std::move(details),
          });
          AppendMessage(body_encoder, kHistoryUpdateRequestEncodingId, payload);
        } else if constexpr (std::is_same_v<T, AddNodesRequest>) {
          AppendRequestHeader(payload_encoder, header);
          payload_encoder.Encode(
              static_cast<std::int32_t>(typed_request.items.size()));
          for (const auto& item : typed_request.items) {
            const auto encoded_attributes =
                EncodeNodeAttributesExtension(item.node_class, item.attributes);
            if (!encoded_attributes.has_value()) {
              return std::nullopt;
            }
            payload_encoder.Encode(ExpandedNodeId{item.parent_id});
            payload_encoder.Encode(NodeId{});
            payload_encoder.Encode(ExpandedNodeId{item.requested_id});
            payload_encoder.Encode(item.attributes.browse_name);
            payload_encoder.Encode(static_cast<std::uint32_t>(item.node_class));
            payload_encoder.Encode(*encoded_attributes);
            payload_encoder.Encode(ExpandedNodeId{item.type_definition_id});
          }
          AppendMessage(body_encoder, kAddNodesRequestEncodingId, payload);
        } else if constexpr (std::is_same_v<T, DeleteNodesRequest>) {
          AppendRequestHeader(payload_encoder, header);
          payload_encoder.Encode(
              static_cast<std::int32_t>(typed_request.items.size()));
          for (const auto& item : typed_request.items) {
            payload_encoder.Encode(item.node_id);
            payload_encoder.Encode(item.delete_target_references);
          }
          AppendMessage(body_encoder, kDeleteNodesRequestEncodingId, payload);
        } else if constexpr (std::is_same_v<T, DeleteReferencesRequest>) {
          AppendRequestHeader(payload_encoder, header);
          payload_encoder.Encode(
              static_cast<std::int32_t>(typed_request.items.size()));
          for (const auto& item : typed_request.items) {
            payload_encoder.Encode(item.source_node_id);
            payload_encoder.Encode(item.reference_type_id);
            payload_encoder.Encode(item.forward);
            payload_encoder.Encode(item.target_node_id);
            payload_encoder.Encode(item.delete_bidirectional);
          }
          AppendMessage(body_encoder, kDeleteReferencesRequestEncodingId,
                        payload);
        } else if constexpr (std::is_same_v<T, AddReferencesRequest>) {
          AppendRequestHeader(payload_encoder, header);
          payload_encoder.Encode(
              static_cast<std::int32_t>(typed_request.items.size()));
          for (const auto& item : typed_request.items) {
            payload_encoder.Encode(item.source_node_id);
            payload_encoder.Encode(item.reference_type_id);
            payload_encoder.Encode(item.forward);
            payload_encoder.Encode(item.target_server_uri);
            payload_encoder.Encode(item.target_node_id);
            payload_encoder.Encode(
                static_cast<std::uint32_t>(item.target_node_class));
          }
          AppendMessage(body_encoder, kAddReferencesRequestEncodingId, payload);
        } else if constexpr (std::is_same_v<T, RegisterNodesRequest>) {
          AppendRequestHeader(payload_encoder, header);
          payload_encoder.Encode(static_cast<std::int32_t>(
              typed_request.nodes_to_register.size()));
          for (const auto& node_id : typed_request.nodes_to_register)
            payload_encoder.Encode(node_id);
          AppendMessage(body_encoder, kRegisterNodesRequestEncodingId, payload);
        } else if constexpr (std::is_same_v<T, UnregisterNodesRequest>) {
          AppendRequestHeader(payload_encoder, header);
          payload_encoder.Encode(static_cast<std::int32_t>(
              typed_request.nodes_to_unregister.size()));
          for (const auto& node_id : typed_request.nodes_to_unregister)
            payload_encoder.Encode(node_id);
          AppendMessage(body_encoder, kUnregisterNodesRequestEncodingId,
                        payload);
        } else {
          return std::nullopt;
        }

        return body;
      },
      request);
}

std::optional<DecodedRequest> DecodeRegisterNodesRequest(
    std::span<const char> body) {
  Decoder decoder{body};
  ServiceRequestHeader header;
  std::int32_t count = 0;
  if (!ReadRequestHeader(decoder, header) || !decoder.Decode(count) ||
      count < 0) {
    return std::nullopt;
  }
  RegisterNodesRequest request;
  if (ArrayCountExceedsRemaining(decoder, count))
    return std::nullopt;
  request.nodes_to_register.resize(static_cast<std::size_t>(count));
  for (auto& node_id : request.nodes_to_register) {
    if (!decoder.Decode(node_id))
      return std::nullopt;
  }
  if (!decoder.consumed())
    return std::nullopt;
  return DecodedRequest{.header = header, .body = std::move(request)};
}

std::optional<DecodedRequest> DecodeUnregisterNodesRequest(
    std::span<const char> body) {
  Decoder decoder{body};
  ServiceRequestHeader header;
  std::int32_t count = 0;
  if (!ReadRequestHeader(decoder, header) || !decoder.Decode(count) ||
      count < 0) {
    return std::nullopt;
  }
  UnregisterNodesRequest request;
  if (ArrayCountExceedsRemaining(decoder, count))
    return std::nullopt;
  request.nodes_to_unregister.resize(static_cast<std::size_t>(count));
  for (auto& node_id : request.nodes_to_unregister) {
    if (!decoder.Decode(node_id))
      return std::nullopt;
  }
  if (!decoder.consumed())
    return std::nullopt;
  return DecodedRequest{.header = header, .body = std::move(request)};
}

std::optional<DecodedRequest> DecodeServiceRequest(
    const std::vector<char>& payload) {
  Decoder decoder{payload};
  const auto message = ReadMessage(decoder);
  if (!message.has_value()) {
    return std::nullopt;
  }

  switch (message->first) {
    case kFindServersRequestEncodingId:
      return DecodeFindServersRequest(message->second);
    case kGetEndpointsRequestEncodingId:
      return DecodeGetEndpointsRequest(message->second);
    case kCreateSessionRequestEncodingId:
      return DecodeCreateSessionRequest(message->second);
    case kActivateSessionRequestEncodingId:
      return DecodeActivateSessionRequest(message->second);
    case kCloseSessionRequestEncodingId:
      return DecodeCloseSessionRequest(message->second);
    case kCreateSubscriptionRequestEncodingId:
      return DecodeCreateSubscriptionRequest(message->second);
    case kModifySubscriptionRequestEncodingId:
      return DecodeModifySubscriptionRequest(message->second);
    case kSetPublishingModeRequestEncodingId:
      return DecodeSetPublishingModeRequest(message->second);
    case kDeleteSubscriptionsRequestEncodingId:
      return DecodeDeleteSubscriptionsRequest(message->second);
    case kCreateMonitoredItemsRequestEncodingId:
      return DecodeCreateMonitoredItemsRequest(message->second);
    case kModifyMonitoredItemsRequestEncodingId:
      return DecodeModifyMonitoredItemsRequest(message->second);
    case kPublishRequestEncodingId:
      return DecodePublishRequest(message->second);
    case kRepublishRequestEncodingId:
      return DecodeRepublishRequest(message->second);
    case kTransferSubscriptionsRequestEncodingId:
      return DecodeTransferSubscriptionsRequest(message->second);
    case kDeleteMonitoredItemsRequestEncodingId:
      return DecodeDeleteMonitoredItemsRequest(message->second);
    case kSetMonitoringModeRequestEncodingId:
      return DecodeSetMonitoringModeRequest(message->second);
    case kBrowseRequestEncodingId:
      return DecodeBrowseRequest(message->second);
    case kBrowseNextRequestEncodingId:
      return DecodeBrowseNextRequest(message->second);
    case kTranslateBrowsePathsRequestEncodingId:
      return DecodeTranslateBrowsePathsRequest(message->second);
    case kCallRequestEncodingId:
      return DecodeCallRequest(message->second);
    case kHistoryReadRequestEncodingId:
      if (const auto decoded = DecodeHistoryReadRawRequest(message->second)) {
        return decoded;
      }
      return DecodeHistoryReadEventsRequest(message->second);
    case kHistoryUpdateRequestEncodingId:
      return DecodeHistoryUpdateRequest(message->second);
    case kReadRequestEncodingId:
      return DecodeReadRequest(message->second);
    case kWriteRequestEncodingId:
      return DecodeWriteRequest(message->second);
    case kAddNodesRequestEncodingId:
      return DecodeAddNodesRequest(message->second);
    case kDeleteNodesRequestEncodingId:
      return DecodeDeleteNodesRequest(message->second);
    case kDeleteReferencesRequestEncodingId:
      return DecodeDeleteReferencesRequest(message->second);
    case kAddReferencesRequestEncodingId:
      return DecodeAddReferencesRequest(message->second);
    case kRegisterNodesRequestEncodingId:
      return DecodeRegisterNodesRequest(message->second);
    case kUnregisterNodesRequestEncodingId:
      return DecodeUnregisterNodesRequest(message->second);
    default:
      return std::nullopt;
  }
}

std::optional<std::uint32_t> DecodeRequestHandle(
    const std::vector<char>& payload) {
  Decoder decoder{payload};
  const auto message = ReadMessage(decoder);
  if (!message.has_value())
    return std::nullopt;
  Decoder body{message->second};
  ServiceRequestHeader header;
  if (!ReadRequestHeader(body, header))
    return std::nullopt;
  return header.request_handle;
}

std::optional<DecodedResponse> DecodeServiceResponse(
    const std::vector<char>& payload) {
  Decoder decoder{payload};
  const auto message = ReadMessage(decoder);
  if (!message.has_value()) {
    return std::nullopt;
  }

  switch (message->first) {
    case kFindServersResponseEncodingId:
      return DecodeFindServersResponse(message->second);
    case kGetEndpointsResponseEncodingId:
      return DecodeGetEndpointsResponse(message->second);
    case kCreateSessionResponseEncodingId:
      return DecodeCreateSessionResponse(message->second);
    case kActivateSessionResponseEncodingId:
      return DecodeActivateSessionResponse(message->second);
    case kCloseSessionResponseEncodingId:
      return DecodeCloseSessionResponse(message->second);
    case kCreateSubscriptionResponseEncodingId:
      return DecodeCreateSubscriptionResponse(message->second);
    case kModifySubscriptionResponseEncodingId:
      return DecodeModifySubscriptionResponse(message->second);
    case kSetPublishingModeResponseEncodingId:
      return DecodeStatusCodeArrayResponse<SetPublishingModeResponse>(
          message->second);
    case kDeleteSubscriptionsResponseEncodingId:
      return DecodeStatusCodeArrayResponse<DeleteSubscriptionsResponse>(
          message->second);
    case kCreateMonitoredItemsResponseEncodingId:
      return DecodeCreateMonitoredItemsResponse(message->second);
    case kModifyMonitoredItemsResponseEncodingId:
      return DecodeModifyMonitoredItemsResponse(message->second);
    case kDeleteMonitoredItemsResponseEncodingId:
      return DecodeStatusCodeArrayResponse<DeleteMonitoredItemsResponse>(
          message->second);
    case kSetMonitoringModeResponseEncodingId:
      return DecodeStatusCodeArrayResponse<SetMonitoringModeResponse>(
          message->second);
    case kPublishResponseEncodingId:
      return DecodePublishResponse(message->second);
    case kRepublishResponseEncodingId:
      return DecodeRepublishResponse(message->second);
    case kTransferSubscriptionsResponseEncodingId:
      return DecodeStatusCodeArrayResponse<TransferSubscriptionsResponse>(
          message->second);
    case kReadResponseEncodingId:
      return DecodeReadResponse(message->second);
    case kWriteResponseEncodingId:
      return DecodeWriteResponse(message->second);
    case kHistoryUpdateResponseEncodingId:
      return DecodeHistoryUpdateResponse(message->second);
    case kHistoryReadResponseEncodingId:
      return DecodeHistoryReadResponse(message->second);
    case kBrowseResponseEncodingId:
      return DecodeBrowseResponseImpl(message->second);
    case kBrowseNextResponseEncodingId:
      return DecodeBrowseNextResponseImpl(message->second);
    case kTranslateBrowsePathsResponseEncodingId:
      return DecodeTranslateBrowsePathsResponse(message->second);
    case kCallResponseEncodingId:
      return DecodeCallResponse(message->second);
    case kAddNodesResponseEncodingId:
      return DecodeAddNodesResponse(message->second);
    case kDeleteNodesResponseEncodingId:
      return DecodeStatusCodeArrayResponse<DeleteNodesResponse>(
          message->second);
    case kDeleteReferencesResponseEncodingId:
      return DecodeStatusCodeArrayResponse<DeleteReferencesResponse>(
          message->second);
    case kAddReferencesResponseEncodingId:
      return DecodeStatusCodeArrayResponse<AddReferencesResponse>(
          message->second);
    case kRegisterNodesResponseEncodingId:
      return DecodeRegisterNodesResponse(message->second);
    case kUnregisterNodesResponseEncodingId:
      return DecodeUnregisterNodesResponse(message->second);
    case kServiceFaultEncodingId:
      return DecodeServiceFault(message->second);
    default:
      return std::nullopt;
  }
}

std::optional<std::vector<char>> EncodeServiceResponse(
    std::uint32_t request_handle,
    const ResponseBody& response) {
  return std::visit(
      [&](const auto& typed_response) -> std::optional<std::vector<char>> {
        std::vector<char> payload;
        std::vector<char> body;
        using T = std::decay_t<decltype(typed_response)>;
        if constexpr (std::is_same_v<T, ReadResponse>) {
          payload.reserve(EstimateReadResponsePayloadSize(typed_response));
        } else if constexpr (std::is_same_v<T, BrowseResponse> ||
                             std::is_same_v<T, BrowseNextResponse>) {
          payload.reserve(
              EstimateBrowseResponsePayloadSize(typed_response.results));
        }
        Encoder payload_encoder{payload};
        Encoder body_encoder{body};

        if constexpr (std::is_same_v<T, FindServersResponse>) {
          AppendResponseHeader(payload_encoder, request_handle,
                               typed_response.status);
          payload_encoder.Encode(
              static_cast<std::int32_t>(typed_response.servers.size()));
          for (const auto& server : typed_response.servers)
            AppendApplicationDescription(payload_encoder, server);
          AppendMessage(body_encoder, kFindServersResponseEncodingId, payload);
        } else if constexpr (std::is_same_v<T, GetEndpointsResponse>) {
          AppendResponseHeader(payload_encoder, request_handle,
                               typed_response.status);
          payload_encoder.Encode(
              static_cast<std::int32_t>(typed_response.endpoints.size()));
          for (const auto& endpoint : typed_response.endpoints)
            AppendEndpointDescription(payload_encoder, endpoint);
          AppendMessage(body_encoder, kGetEndpointsResponseEncodingId, payload);
        } else if constexpr (std::is_same_v<T, CreateSessionResponse>) {
          AppendResponseHeader(payload_encoder, request_handle,
                               typed_response.status);
          payload_encoder.Encode(typed_response.session_id);
          payload_encoder.Encode(typed_response.authentication_token);
          payload_encoder.Encode(
              typed_response.revised_timeout.InMillisecondsF());
          payload_encoder.Encode(typed_response.server_nonce);
          // serverCertificate: empty for the in-repo server today, but encoded
          // from the struct so the field round-trips (the client validates it).
          payload_encoder.Encode(typed_response.server_certificate);
          payload_encoder.Encode(std::int32_t{-1});
          payload_encoder.Encode(std::int32_t{-1});
          payload_encoder.Encode(std::string_view{""});
          payload_encoder.Encode(ByteString{});
          payload_encoder.Encode(std::uint32_t{0});
          AppendMessage(body_encoder, kCreateSessionResponseEncodingId,
                        payload);
        } else if constexpr (std::is_same_v<T, ActivateSessionResponse>) {
          AppendResponseHeader(payload_encoder, request_handle,
                               typed_response.status);
          payload_encoder.Encode(ByteString{});
          payload_encoder.Encode(std::int32_t{-1});
          payload_encoder.Encode(std::int32_t{-1});
          AppendMessage(body_encoder, kActivateSessionResponseEncodingId,
                        payload);
        } else if constexpr (std::is_same_v<T, CloseSessionResponse>) {
          AppendResponseHeader(payload_encoder, request_handle,
                               typed_response.status);
          AppendMessage(body_encoder, kCloseSessionResponseEncodingId, payload);
        } else if constexpr (std::is_same_v<T, CreateSubscriptionResponse>) {
          AppendResponseHeader(payload_encoder, request_handle,
                               typed_response.status);
          payload_encoder.Encode(typed_response.subscription_id);
          payload_encoder.Encode(typed_response.revised_publishing_interval_ms);
          payload_encoder.Encode(typed_response.revised_lifetime_count);
          payload_encoder.Encode(typed_response.revised_max_keep_alive_count);
          AppendMessage(body_encoder, kCreateSubscriptionResponseEncodingId,
                        payload);
        } else if constexpr (std::is_same_v<T, ModifySubscriptionResponse>) {
          AppendResponseHeader(payload_encoder, request_handle,
                               typed_response.status);
          payload_encoder.Encode(typed_response.revised_publishing_interval_ms);
          payload_encoder.Encode(typed_response.revised_lifetime_count);
          payload_encoder.Encode(typed_response.revised_max_keep_alive_count);
          AppendMessage(body_encoder, kModifySubscriptionResponseEncodingId,
                        payload);
        } else if constexpr (std::is_same_v<T, SetPublishingModeResponse>) {
          AppendResponseHeader(payload_encoder, request_handle,
                               typed_response.status);
          payload_encoder.Encode(
              static_cast<std::int32_t>(typed_response.results.size()));
          for (const auto result : typed_response.results) {
            payload_encoder.Encode(EncodeStatusCode(result));
          }
          payload_encoder.Encode(std::int32_t{-1});
          AppendMessage(body_encoder, kSetPublishingModeResponseEncodingId,
                        payload);
        } else if constexpr (std::is_same_v<T, DeleteSubscriptionsResponse>) {
          AppendResponseHeader(payload_encoder, request_handle,
                               typed_response.status);
          payload_encoder.Encode(
              static_cast<std::int32_t>(typed_response.results.size()));
          for (const auto result : typed_response.results) {
            payload_encoder.Encode(EncodeStatusCode(result));
          }
          payload_encoder.Encode(std::int32_t{-1});
          AppendMessage(body_encoder, kDeleteSubscriptionsResponseEncodingId,
                        payload);
        } else if constexpr (std::is_same_v<T, CreateMonitoredItemsResponse>) {
          AppendResponseHeader(payload_encoder, request_handle,
                               typed_response.status);
          payload_encoder.Encode(
              static_cast<std::int32_t>(typed_response.results.size()));
          for (const auto& result : typed_response.results) {
            AppendMonitoredItemCreateResult(payload_encoder, result);
          }
          payload_encoder.Encode(std::int32_t{-1});
          AppendMessage(body_encoder, kCreateMonitoredItemsResponseEncodingId,
                        payload);
        } else if constexpr (std::is_same_v<T, ModifyMonitoredItemsResponse>) {
          AppendResponseHeader(payload_encoder, request_handle,
                               typed_response.status);
          payload_encoder.Encode(
              static_cast<std::int32_t>(typed_response.results.size()));
          for (const auto& result : typed_response.results) {
            AppendMonitoredItemModifyResult(payload_encoder, result);
          }
          payload_encoder.Encode(std::int32_t{-1});
          AppendMessage(body_encoder, kModifyMonitoredItemsResponseEncodingId,
                        payload);
        } else if constexpr (std::is_same_v<T, PublishResponse>) {
          AppendResponseHeader(payload_encoder, request_handle,
                               typed_response.status);
          payload_encoder.Encode(typed_response.subscription_id);
          payload_encoder.Encode(static_cast<std::int32_t>(
              typed_response.available_sequence_numbers.size()));
          for (const auto sequence_number :
               typed_response.available_sequence_numbers) {
            payload_encoder.Encode(sequence_number);
          }
          payload_encoder.Encode(typed_response.more_notifications);
          AppendNotificationMessage(payload_encoder,
                                    typed_response.notification_message);
          payload_encoder.Encode(
              static_cast<std::int32_t>(typed_response.results.size()));
          for (const auto result : typed_response.results) {
            payload_encoder.Encode(EncodeStatusCode(result));
          }
          payload_encoder.Encode(std::int32_t{-1});
          AppendMessage(body_encoder, kPublishResponseEncodingId, payload);
        } else if constexpr (std::is_same_v<T, RepublishResponse>) {
          AppendResponseHeader(payload_encoder, request_handle,
                               typed_response.status);
          AppendNotificationMessage(payload_encoder,
                                    typed_response.notification_message);
          AppendMessage(body_encoder, kRepublishResponseEncodingId, payload);
        } else if constexpr (std::is_same_v<T, TransferSubscriptionsResponse>) {
          AppendResponseHeader(payload_encoder, request_handle,
                               typed_response.status);
          payload_encoder.Encode(
              static_cast<std::int32_t>(typed_response.results.size()));
          for (const auto result : typed_response.results) {
            payload_encoder.Encode(EncodeStatusCode(result));
          }
          payload_encoder.Encode(std::int32_t{-1});
          AppendMessage(body_encoder, kTransferSubscriptionsResponseEncodingId,
                        payload);
        } else if constexpr (std::is_same_v<T, DeleteMonitoredItemsResponse>) {
          AppendResponseHeader(payload_encoder, request_handle,
                               typed_response.status);
          payload_encoder.Encode(
              static_cast<std::int32_t>(typed_response.results.size()));
          for (const auto result : typed_response.results) {
            payload_encoder.Encode(EncodeStatusCode(result));
          }
          payload_encoder.Encode(std::int32_t{-1});
          AppendMessage(body_encoder, kDeleteMonitoredItemsResponseEncodingId,
                        payload);
        } else if constexpr (std::is_same_v<T, SetMonitoringModeResponse>) {
          AppendResponseHeader(payload_encoder, request_handle,
                               typed_response.status);
          payload_encoder.Encode(
              static_cast<std::int32_t>(typed_response.results.size()));
          for (const auto result : typed_response.results) {
            payload_encoder.Encode(EncodeStatusCode(result));
          }
          payload_encoder.Encode(std::int32_t{-1});
          AppendMessage(body_encoder, kSetMonitoringModeResponseEncodingId,
                        payload);
        } else if constexpr (std::is_same_v<T, ReadResponse>) {
          AppendResponseHeader(payload_encoder, request_handle,
                               typed_response.status);
          payload_encoder.Encode(
              static_cast<std::int32_t>(typed_response.results.size()));
          for (const auto& result : typed_response.results) {
            AppendDataValue(payload_encoder, result);
          }
          payload_encoder.Encode(std::int32_t{-1});
          AppendMessage(body_encoder, kReadResponseEncodingId, payload);
        } else if constexpr (std::is_same_v<T, WriteResponse>) {
          AppendResponseHeader(payload_encoder, request_handle,
                               typed_response.status);
          payload_encoder.Encode(
              static_cast<std::int32_t>(typed_response.results.size()));
          for (const auto& result : typed_response.results) {
            payload_encoder.Encode(EncodeStatusCode(result));
          }
          payload_encoder.Encode(std::int32_t{-1});
          AppendMessage(body_encoder, kWriteResponseEncodingId, payload);
        } else if constexpr (std::is_same_v<T, BrowseResponse>) {
          AppendResponseHeader(payload_encoder, request_handle,
                               typed_response.status);
          payload_encoder.Encode(
              static_cast<std::int32_t>(typed_response.results.size()));
          for (const auto& result : typed_response.results) {
            payload_encoder.Encode(EncodeStatusCode(result.status_code));
            payload_encoder.Encode(result.continuation_point);
            payload_encoder.Encode(
                static_cast<std::int32_t>(result.references.size()));
            for (const auto& reference : result.references) {
              AppendReferenceDescription(payload_encoder, reference);
            }
          }
          payload_encoder.Encode(std::int32_t{-1});
          AppendMessage(body_encoder, kBrowseResponseEncodingId, payload);
        } else if constexpr (std::is_same_v<T, BrowseNextResponse>) {
          AppendResponseHeader(payload_encoder, request_handle,
                               typed_response.status);
          payload_encoder.Encode(
              static_cast<std::int32_t>(typed_response.results.size()));
          for (const auto& result : typed_response.results) {
            payload_encoder.Encode(EncodeStatusCode(result.status_code));
            payload_encoder.Encode(result.continuation_point);
            payload_encoder.Encode(
                static_cast<std::int32_t>(result.references.size()));
            for (const auto& reference : result.references) {
              AppendReferenceDescription(payload_encoder, reference);
            }
          }
          payload_encoder.Encode(std::int32_t{-1});
          AppendMessage(body_encoder, kBrowseNextResponseEncodingId, payload);
        } else if constexpr (std::is_same_v<T, TranslateBrowsePathsResponse>) {
          AppendResponseHeader(payload_encoder, request_handle,
                               typed_response.status);
          payload_encoder.Encode(
              static_cast<std::int32_t>(typed_response.results.size()));
          for (const auto& result : typed_response.results) {
            AppendBrowsePathResult(payload_encoder, result);
          }
          payload_encoder.Encode(std::int32_t{-1});
          AppendMessage(body_encoder, kTranslateBrowsePathsResponseEncodingId,
                        payload);
        } else if constexpr (std::is_same_v<T, CallResponse>) {
          AppendResponseHeader(payload_encoder, request_handle,
                               typed_response.status);
          payload_encoder.Encode(
              static_cast<std::int32_t>(typed_response.results.size()));
          for (const auto& result : typed_response.results) {
            payload_encoder.Encode(result.status.full_code());
            payload_encoder.Encode(static_cast<std::int32_t>(
                result.input_argument_results.size()));
            for (const auto& input_result : result.input_argument_results) {
              payload_encoder.Encode(EncodeStatusCode(input_result));
            }
            payload_encoder.Encode(std::int32_t{-1});
            payload_encoder.Encode(
                static_cast<std::int32_t>(result.output_arguments.size()));
            for (const auto& output_argument : result.output_arguments) {
              payload_encoder.Encode(output_argument);
            }
          }
          payload_encoder.Encode(std::int32_t{-1});
          AppendMessage(body_encoder, kCallResponseEncodingId, payload);
        } else if constexpr (std::is_same_v<T, HistoryReadRawResponse>) {
          AppendResponseHeader(payload_encoder, request_handle,
                               typed_response.result.status);
          payload_encoder.Encode(std::int32_t{1});
          payload_encoder.Encode(typed_response.result.status.full_code());
          payload_encoder.Encode(typed_response.result.continuation_point);
          AppendHistoryData(payload_encoder, typed_response.result);
          payload_encoder.Encode(std::int32_t{-1});
          AppendMessage(body_encoder, kHistoryReadResponseEncodingId, payload);
        } else if constexpr (std::is_same_v<T, HistoryUpdateResponse>) {
          // HistoryUpdateResponse: results[] of HistoryUpdateResult
          // (statusCode, operationResults[], diagnosticInfos[]) plus the
          // response-level diagnosticInfos[].
          AppendResponseHeader(payload_encoder, request_handle,
                               typed_response.result.status);
          payload_encoder.Encode(std::int32_t{1});
          payload_encoder.Encode(typed_response.result.status.full_code());
          payload_encoder.Encode(static_cast<std::int32_t>(
              typed_response.result.operation_results.size()));
          for (const auto& operation_result :
               typed_response.result.operation_results) {
            payload_encoder.Encode(EncodeStatusCode(operation_result));
          }
          payload_encoder.Encode(std::int32_t{-1});
          payload_encoder.Encode(std::int32_t{-1});
          AppendMessage(body_encoder, kHistoryUpdateResponseEncodingId,
                        payload);
        } else if constexpr (std::is_same_v<T, AddNodesResponse>) {
          AppendResponseHeader(payload_encoder, request_handle,
                               typed_response.status);
          payload_encoder.Encode(
              static_cast<std::int32_t>(typed_response.results.size()));
          for (const auto& result : typed_response.results) {
            payload_encoder.Encode(EncodeStatusCode(result.status_code));
            payload_encoder.Encode(result.added_node_id);
          }
          payload_encoder.Encode(std::int32_t{-1});
          AppendMessage(body_encoder, kAddNodesResponseEncodingId, payload);
        } else if constexpr (std::is_same_v<T, DeleteNodesResponse>) {
          AppendResponseHeader(payload_encoder, request_handle,
                               typed_response.status);
          payload_encoder.Encode(
              static_cast<std::int32_t>(typed_response.results.size()));
          for (const auto& result : typed_response.results) {
            payload_encoder.Encode(EncodeStatusCode(result));
          }
          payload_encoder.Encode(std::int32_t{-1});
          AppendMessage(body_encoder, kDeleteNodesResponseEncodingId, payload);
        } else if constexpr (std::is_same_v<T, DeleteReferencesResponse>) {
          AppendResponseHeader(payload_encoder, request_handle,
                               typed_response.status);
          payload_encoder.Encode(
              static_cast<std::int32_t>(typed_response.results.size()));
          for (const auto& result : typed_response.results) {
            payload_encoder.Encode(EncodeStatusCode(result));
          }
          payload_encoder.Encode(std::int32_t{-1});
          AppendMessage(body_encoder, kDeleteReferencesResponseEncodingId,
                        payload);
        } else if constexpr (std::is_same_v<T, AddReferencesResponse>) {
          AppendResponseHeader(payload_encoder, request_handle,
                               typed_response.status);
          payload_encoder.Encode(
              static_cast<std::int32_t>(typed_response.results.size()));
          for (const auto& result : typed_response.results) {
            payload_encoder.Encode(EncodeStatusCode(result));
          }
          payload_encoder.Encode(std::int32_t{-1});
          AppendMessage(body_encoder, kAddReferencesResponseEncodingId,
                        payload);
        } else if constexpr (std::is_same_v<T, RegisterNodesResponse>) {
          AppendResponseHeader(payload_encoder, request_handle,
                               typed_response.status);
          payload_encoder.Encode(static_cast<std::int32_t>(
              typed_response.registered_node_ids.size()));
          for (const auto& node_id : typed_response.registered_node_ids)
            payload_encoder.Encode(node_id);
          AppendMessage(body_encoder, kRegisterNodesResponseEncodingId,
                        payload);
        } else if constexpr (std::is_same_v<T, UnregisterNodesResponse>) {
          AppendResponseHeader(payload_encoder, request_handle,
                               typed_response.status);
          AppendMessage(body_encoder, kUnregisterNodesResponseEncodingId,
                        payload);
        } else if constexpr (std::is_same_v<T, ServiceFault>) {
          // OPC UA Part 4 §7.34: a ServiceFault carries only the
          // ResponseHeader, whose serviceResult holds the failure status.
          AppendResponseHeader(payload_encoder, request_handle,
                               typed_response.status);
          AppendMessage(body_encoder, kServiceFaultEncodingId, payload);
        } else {
          return std::nullopt;
        }

        return body;
      },
      response);
}

std::optional<std::vector<char>> EncodeHistoryReadEventsResponse(
    std::uint32_t request_handle,
    const HistoryReadEventsResponse& response,
    std::span<const std::vector<std::string>> field_paths) {
  std::vector<char> payload;
  std::vector<char> body;
  Encoder payload_encoder{payload};
  Encoder body_encoder{body};

  AppendResponseHeader(payload_encoder, request_handle, response.result.status);
  payload_encoder.Encode(std::int32_t{1});
  payload_encoder.Encode(response.result.status.full_code());
  payload_encoder.Encode(ByteString{});
  AppendHistoryEvent(payload_encoder, response, field_paths);
  payload_encoder.Encode(std::int32_t{-1});
  AppendMessage(body_encoder, kHistoryReadResponseEncodingId, payload);
  return body;
}

}  // namespace opcua::binary
