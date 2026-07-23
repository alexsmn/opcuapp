#include "opcua/transport/binary/service_codec.h"

#include "opcua/events/event_filter.h"
#include "opcua/session/discovery_conversion.h"
#include "opcua/session/session_conversion.h"
#include "opcua/session/subscription_conversion.h"
#include "opcua/transport/binary/codec_utils.h"

#include "opcua/types/localized_text.h"
#include "opcua/types/standard_node_ids.h"
#include "opcua/ua/ua_binary_codec.h"
#include "opcua/ua/ua_service_header.h"

#include <boost/json.hpp>

namespace opcua::binary {
namespace {

// OPC UA Part 4, 7.7.3: filter operator codes. Only the subset actually
// emitted/consumed by opcua events is enumerated here.
// ContentFilter operators (OPC UA Part 4 §7.7.3 ContentFilter,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/7.7.3).
constexpr std::uint32_t kFilterOperatorEquals = 0;
constexpr std::uint32_t kFilterOperatorOfType = 11;
// Nonstandard use: carries the SCADA `child_of` hierarchy filter (a documented
// internal extension; standard RelatedTo takes 6 operands).
constexpr std::uint32_t kFilterOperatorRelatedTo = 15;

// Message TypeIds are the ns-0 ids of the *_Encoding_DefaultBinary
// DataTypeEncoding nodes (OPC UA Part 6 §5.2.2.15 / Part 3 Annex,
// https://files.opcfoundation.org/schemas/UA/1.04/Opc.Ua.NodeIds.csv) — NOT
// the DataType node ids, which are 2 lower for services. A wrong id here makes
// third-party clients' requests decode as "unsupported service" and makes our
// responses undecodable for them.
constexpr std::uint32_t kFindServersRequestEncodingId = 422;
constexpr std::uint32_t kFindServersResponseEncodingId = 425;
constexpr std::uint32_t kGetEndpointsRequestEncodingId = 428;
constexpr std::uint32_t kGetEndpointsResponseEncodingId = 431;
constexpr std::uint32_t kRegisterServerRequestEncodingId = 437;
constexpr std::uint32_t kRegisterServerResponseEncodingId = 440;
// OPC UA Part 4 §5.4.6 RegisterServer2 + its MdnsDiscoveryConfiguration
// parameter (Part 4 §7.8); DefaultBinary encoding node ids from the OPC UA
// NodeIds registry (https://reference.opcfoundation.org/Core/Part6/v105/docs/).
constexpr std::uint32_t kRegisterServer2RequestEncodingId = 12211;
constexpr std::uint32_t kRegisterServer2ResponseEncodingId = 12212;
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
constexpr std::uint32_t kBrowseRequestEncodingId = 527;
constexpr std::uint32_t kBrowseResponseEncodingId = 530;
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
constexpr std::uint32_t kPublishRequestEncodingId = 826;
constexpr std::uint32_t kPublishResponseEncodingId = 829;
constexpr std::uint32_t kRepublishRequestEncodingId = 832;
constexpr std::uint32_t kRepublishResponseEncodingId = 835;
constexpr std::uint32_t kTransferSubscriptionsRequestEncodingId = 841;
constexpr std::uint32_t kTransferSubscriptionsResponseEncodingId = 844;
constexpr std::uint32_t kCallRequestEncodingId = 712;
constexpr std::uint32_t kCallResponseEncodingId = 715;
constexpr std::uint32_t kReadEventDetailsEncodingId = 646;
constexpr std::uint32_t kReadRawModifiedDetailsEncodingId = 649;
constexpr std::uint32_t kHistoryDataEncodingId = 658;
constexpr std::uint32_t kHistoryEventEncodingId = 661;
constexpr std::uint32_t kHistoryReadRequestEncodingId = 664;
constexpr std::uint32_t kHistoryReadResponseEncodingId = 667;
constexpr std::uint32_t kEventFilterEncodingId = 727;
constexpr std::uint32_t kLiteralOperandEncodingId = 597;
constexpr std::uint32_t kSimpleAttributeOperandEncodingId = 603;
constexpr std::uint32_t kReadRequestEncodingId = 631;
constexpr std::uint32_t kReadResponseEncodingId = 634;
constexpr std::uint32_t kWriteRequestEncodingId = 673;
constexpr std::uint32_t kWriteResponseEncodingId = 676;
constexpr std::uint32_t kUpdateDataDetailsEncodingId = 682;
// OPC UA Part 6 / NodeIds: UpdateEventDetails default binary encoding.
constexpr std::uint32_t kUpdateEventDetailsEncodingId = 685;
constexpr std::uint32_t kHistoryUpdateRequestEncodingId = 700;
constexpr std::uint32_t kHistoryUpdateResponseEncodingId = 703;

enum class WireTimestampsToReturn : std::uint32_t {
  Source = 0,
  Server = 1,
  Both = 2,
  Neither = 3,
};

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

// Data1 + Data2 + Data3 + Data4 (OPC UA Part 6 §5.2.2.6 Guid).
constexpr std::size_t kGuidWireSize = 16;

std::size_t EstimateVariantSize(const Variant& value) {
  // Tested before is_null(): an array of null elements also reports
  // type() == EMPTY, and its element count still has to be accounted for.
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
      case Variant::FLOAT:
        return EstimateFixedArrayVariantSize(value.get<std::vector<Float>>(),
                                             sizeof(float));
      case Variant::DATE_TIME:
        return EstimateFixedArrayVariantSize(value.get<std::vector<DateTime>>(),
                                             sizeof(std::int64_t));
      case Variant::GUID:
        return EstimateFixedArrayVariantSize(value.get<std::vector<Guid>>(),
                                             kGuidWireSize);
      case Variant::STATUS_CODE:
        return EstimateFixedArrayVariantSize(value.get<std::vector<Status>>(),
                                             sizeof(std::uint32_t));
      case Variant::XML_ELEMENT: {
        std::size_t size = sizeof(std::uint8_t) + sizeof(std::int32_t);
        for (const auto& item : value.get<std::vector<XmlElement>>()) {
          size += EstimateStringSize(item.value);
        }
        return size;
      }
      // Variable-size or nested element types: the header is exact and the
      // elements grow the buffer as they are appended. These are only reserve()
      // hints, so an underestimate costs a reallocation, not correctness.
      case Variant::LOCALIZED_TEXT:
      case Variant::EXTENSION_OBJECT:
      case Variant::DATA_VALUE:
      case Variant::VARIANT:
      case Variant::DIAGNOSTIC_INFO:
      case Variant::COUNT:
        return sizeof(std::uint8_t) + sizeof(std::int32_t);
    }
  }

  switch (value.type()) {
    case Variant::BOOL:
    case Variant::INT8:
    case Variant::UINT8:
      return sizeof(std::uint8_t) + 1;
    case Variant::FLOAT:
      return sizeof(std::uint8_t) + sizeof(float);
    case Variant::STATUS_CODE:
      return sizeof(std::uint8_t) + sizeof(std::uint32_t);
    case Variant::GUID:
      return sizeof(std::uint8_t) + kGuidWireSize;
    case Variant::XML_ELEMENT:
      return sizeof(std::uint8_t) +
             EstimateStringSize(value.get<XmlElement>().value);
    case Variant::DATA_VALUE:
    case Variant::VARIANT:
    case Variant::DIAGNOSTIC_INFO:
      return sizeof(std::uint8_t);
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

std::size_t EstimateReadRequestPayloadSize(const ua::ReadRequest& request) {
  std::size_t size = 96;
  for (const auto& input : request.nodes_to_read) {
    size += EstimateNodeIdSize(input.node_id) + sizeof(std::uint32_t) +
            EstimateStringSize({}) + EstimateQualifiedNameSize({});
  }
  return size;
}

std::size_t EstimateReadResponsePayloadSize(const ua::ReadResponse& response) {
  std::size_t size = 64;
  for (const auto& result : response.results) {
    size += EstimateDataValueSize(result);
  }
  return size;
}

// AdditionalParametersType Default Binary encoding id
// (AdditionalParametersType_Encoding_DefaultBinary, i=17537; the DataType is
// i=16313). Layout per Opc.Ua.Types.bsd.xml: Int32 NoOfParameters followed by
// inline KeyValuePair{QualifiedName key; Variant value} structures. Used as
// the RequestHeader.additionalHeader carrier for the W3C traceparent.
// OPC UA Part 4 §7.33 RequestHeader,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/7.33
constexpr std::uint32_t kAdditionalParametersTypeEncodingId = 17537;

constexpr std::string_view kTraceParentParameterName = "traceparent";

void AppendRequestHeader(Encoder& encoder, const ServiceRequestHeader& header) {
  encoder.Encode(header.authentication_token);
  encoder.Encode(std::int64_t{0});
  encoder.Encode(header.request_handle);
  encoder.Encode(std::uint32_t{0});
  encoder.Encode(std::string_view{""});
  encoder.Encode(std::uint32_t{0});
  if (header.trace_parent.empty()) {
    // Absent additionalHeader: null type id + no-body encoding byte.
    encoder.Encode(NodeId{});
    encoder.Encode(std::uint8_t{0x00});
  } else {
    // AdditionalParametersType {"traceparent": <String>}. A standard,
    // skippable ExtensionObject (length-prefixed body per OPC UA Part 6
    // §5.2.2.15,
    // https://reference.opcfoundation.org/Core/Part6/v105/docs/5.2.2.15), so
    // peers that don't understand it ignore it safely.
    std::vector<char> body;
    Encoder body_encoder{body};
    body_encoder.Encode(std::int32_t{1});  // NoOfParameters
    body_encoder.Encode(QualifiedName{std::string{kTraceParentParameterName}});
    body_encoder.Encode(Variant{String{header.trace_parent}});
    encoder.Encode(EncodedExtensionObject{kAdditionalParametersTypeEncodingId,
                                          std::move(body)});
  }
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

// True when an array element count is larger than the bytes left to decode.
// Every encoded element occupies at least one byte, so a larger count is
// malformed; rejecting it bounds the resize/reserve against a decode bomb.
// OPC UA Part 6 §5.1.2 Decoding Errors,
// https://reference.opcfoundation.org/Core/Part6/v105/docs/5.1.2
bool ArrayCountExceedsRemaining(const Decoder& decoder, std::int32_t count) {
  return count < 0 ||
         static_cast<std::size_t>(count) > decoder.remaining().size();
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

  const std::int32_t acked_clause_count =
      (filter.types & EventFilter::ACKED ? 1 : 0) +
      (filter.types & EventFilter::UNACKED ? 1 : 0);
  body_encoder.Encode(static_cast<std::int32_t>(
      filter.of_type.size() + filter.child_of.size() + acked_clause_count));
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
  // The bespoke ACKED/UNACKED bits travel as the standard where clause
  // `Equals(SimpleAttributeOperand("AckedState"), Literal(Boolean))` (OPC UA
  // Part 4 §7.7.3), so foreign servers see a conformant filter and foreign
  // clients can express the same selection.
  const auto append_acked_clause = [&](bool acked) {
    body_encoder.Encode(kFilterOperatorEquals);
    body_encoder.Encode(std::int32_t{2});
    AppendSimpleAttributeOperand(body_encoder,
                                 std::vector<std::string>{"AckedState"});
    AppendLiteralOperand(body_encoder, Variant{acked});
  };
  if (filter.types & EventFilter::ACKED) {
    append_acked_clause(true);
  }
  if (filter.types & EventFilter::UNACKED) {
    append_acked_clause(false);
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

// Extracts the "traceparent" entry from a decoded
// RequestHeader.additionalHeader. Tolerant by design: the additionalHeader is
// a vendor extension slot, so unknown extension types and any malformed body
// content yield an empty result instead of a decode failure — data from the
// wire must never fail the request over an optional header. Only the
// ExtensionObject envelope (already decoded by the caller) stays strict.
std::string ReadTraceParentFromAdditionalHeader(
    const DecodedExtensionObject& additional) {
  if (additional.type_id != kAdditionalParametersTypeEncodingId ||
      additional.encoding != 0x01) {
    return {};
  }

  Decoder decoder{additional.body};
  std::int32_t count = 0;
  if (!decoder.Decode(count) || count < 0 ||
      ArrayCountExceedsRemaining(decoder, count)) {
    return {};
  }

  for (std::int32_t i = 0; i < count; ++i) {
    QualifiedName key;
    Variant value;
    if (!decoder.Decode(key) || !decoder.Decode(value)) {
      return {};
    }
    if (key.namespace_index() == 0 && key.name() == kTraceParentParameterName) {
      if (const String* text = value.get_if<String>()) {
        return *text;
      }
      return {};  // A non-String "traceparent" value is dropped.
    }
  }
  return {};
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
  if (!decoder.Decode(additional)) {
    return false;
  }
  header.trace_parent = ReadTraceParentFromAdditionalHeader(additional);
  return true;
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

// -- Decode*Response helpers -------------------------------------------------

// Each returns a populated DecodedResponse on success. They operate
// on the payload inside the ExtensionObject wrapper (the caller strips the
// wrapper via ReadMessage).

template <class Wire>
std::optional<DecodedResponse> DecodeDiscoveryResponse(
    std::span<const char> body) {
  Decoder decoder{body};
  Wire wire;
  if (!ua::Decode(decoder, wire) || !decoder.consumed()) {
    return std::nullopt;
  }
  return DecodedResponse{.request_handle = wire.response_header.request_handle,
                         .body = discovery_conversion::ToManaged(wire)};
}

std::optional<DecodedResponse> DecodeFindServersResponse(
    std::span<const char> body) {
  return DecodeDiscoveryResponse<ua::FindServersResponse>(body);
}

std::optional<DecodedResponse> DecodeGetEndpointsResponse(
    std::span<const char> body) {
  return DecodeDiscoveryResponse<ua::GetEndpointsResponse>(body);
}

std::optional<DecodedResponse> DecodeRegisterServerResponse(
    std::span<const char> body) {
  return DecodeDiscoveryResponse<ua::RegisterServerResponse>(body);
}

std::optional<DecodedResponse> DecodeRegisterServer2Response(
    std::span<const char> body) {
  return DecodeDiscoveryResponse<ua::RegisterServer2Response>(body);
}

std::optional<DecodedResponse> DecodeCreateSessionResponse(
    std::span<const char> body) {
  Decoder decoder{body};
  ua::CreateSessionResponse wire;
  if (!ua::Decode(decoder, wire) || !decoder.consumed()) {
    return std::nullopt;
  }
  return DecodedResponse{.request_handle = wire.response_header.request_handle,
                         .body = session_conversion::ToManaged(wire)};
}

std::optional<DecodedResponse> DecodeActivateSessionResponse(
    std::span<const char> body) {
  Decoder decoder{body};
  ua::ActivateSessionResponse wire;
  if (!ua::Decode(decoder, wire) || !decoder.consumed()) {
    return std::nullopt;
  }
  return DecodedResponse{.request_handle = wire.response_header.request_handle,
                         .body = session_conversion::ToManaged(wire)};
}

std::optional<DecodedResponse> DecodeCloseSessionResponse(
    std::span<const char> body) {
  Decoder decoder{body};
  ua::CloseSessionResponse wire;
  if (!ua::Decode(decoder, wire) || !decoder.consumed()) {
    return std::nullopt;
  }
  return DecodedResponse{.request_handle = wire.response_header.request_handle,
                         .body = session_conversion::ToManaged(wire)};
}

// Client-side decode for a response migrated to the generated codec: decode
// the whole message (embedded ResponseHeader + body) and lift the
// request_handle back out for the envelope. The mirror of the migrated
// response-encode arm.
template <class Response>
std::optional<DecodedResponse> DecodeGeneratedResponse(
    std::span<const char> body) {
  Decoder decoder{body};
  Response response;
  if (!ua::Decode(decoder, response) || !decoder.consumed()) {
    return std::nullopt;
  }
  const std::uint32_t request_handle = response.response_header.request_handle;
  return DecodedResponse{.request_handle = request_handle,
                         .body = std::move(response)};
}

template <class Response>
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
  return DecodeGeneratedResponse<ua::AddNodesResponse>(body);
}

std::optional<DecodedResponse> DecodeCreateSubscriptionResponse(
    std::span<const char> body) {
  Decoder decoder{body};
  ua::CreateSubscriptionResponse response;
  if (!ua::Decode(decoder, response) || !decoder.consumed()) {
    return std::nullopt;
  }
  return DecodedResponse{
      .request_handle = response.response_header.request_handle,
      .body = subscription_conversion::ToManaged(response)};
}

std::optional<DecodedResponse> DecodeModifySubscriptionResponse(
    std::span<const char> body) {
  Decoder decoder{body};
  ua::ModifySubscriptionResponse response;
  if (!ua::Decode(decoder, response) || !decoder.consumed()) {
    return std::nullopt;
  }
  return DecodedResponse{
      .request_handle = response.response_header.request_handle,
      .body = subscription_conversion::ToManaged(response)};
}

std::optional<DecodedResponse> DecodeCreateMonitoredItemsResponse(
    std::span<const char> body) {
  Decoder decoder{body};
  ua::CreateMonitoredItemsResponse response;
  if (!ua::Decode(decoder, response) || !decoder.consumed()) {
    return std::nullopt;
  }
  return DecodedResponse{
      .request_handle = response.response_header.request_handle,
      .body = subscription_conversion::ToManaged(response)};
}

std::optional<DecodedResponse> DecodeModifyMonitoredItemsResponse(
    std::span<const char> body) {
  Decoder decoder{body};
  ua::ModifyMonitoredItemsResponse response;
  if (!ua::Decode(decoder, response) || !decoder.consumed()) {
    return std::nullopt;
  }
  return DecodedResponse{
      .request_handle = response.response_header.request_handle,
      .body = subscription_conversion::ToManaged(response)};
}

std::optional<DecodedResponse> DecodePublishResponse(
    std::span<const char> body) {
  Decoder decoder{body};
  ua::PublishResponse response;
  if (!ua::Decode(decoder, response) || !decoder.consumed()) {
    return std::nullopt;
  }
  return DecodedResponse{
      .request_handle = response.response_header.request_handle,
      .body = subscription_conversion::ToManaged(response)};
}

std::optional<DecodedResponse> DecodeRepublishResponse(
    std::span<const char> body) {
  Decoder decoder{body};
  ua::RepublishResponse response;
  if (!ua::Decode(decoder, response) || !decoder.consumed()) {
    return std::nullopt;
  }
  return DecodedResponse{
      .request_handle = response.response_header.request_handle,
      .body = subscription_conversion::ToManaged(response)};
}

std::optional<DecodedResponse> DecodeReadResponse(std::span<const char> body) {
  return DecodeGeneratedResponse<ua::ReadResponse>(body);
}

std::optional<DecodedResponse> DecodeWriteResponse(std::span<const char> body) {
  return DecodeGeneratedResponse<ua::WriteResponse>(body);
}

std::optional<DecodedResponse> DecodeBrowseResponseImpl(
    std::span<const char> body) {
  return DecodeGeneratedResponse<ua::BrowseResponse>(body);
}

std::optional<DecodedResponse> DecodeBrowseNextResponseImpl(
    std::span<const char> body) {
  return DecodeGeneratedResponse<ua::BrowseNextResponse>(body);
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
  return DecodeGeneratedResponse<ua::CallResponse>(body);
}

std::optional<DecodedRequest> DecodeCreateSessionRequest(
    std::span<const char> body) {
  Decoder decoder{body};
  ua::CreateSessionRequest wire;
  if (!ua::Decode(decoder, wire) || !decoder.consumed()) {
    return std::nullopt;
  }
  ServiceRequestHeader header{
      .authentication_token = wire.request_header.authentication_token,
      .request_handle = wire.request_header.request_handle,
      .trace_parent = ua::GetTraceParent(wire.request_header)};
  return DecodedRequest{.header = header,
                        .body = session_conversion::ToManaged(wire)};
}

template <class Wire>
std::optional<DecodedRequest> DecodeDiscoveryRequest(
    std::span<const char> body) {
  Decoder decoder{body};
  Wire wire;
  if (!ua::Decode(decoder, wire) || !decoder.consumed()) {
    return std::nullopt;
  }
  ServiceRequestHeader header{
      .authentication_token = wire.request_header.authentication_token,
      .request_handle = wire.request_header.request_handle,
      .trace_parent = ua::GetTraceParent(wire.request_header)};
  return DecodedRequest{.header = header,
                        .body = discovery_conversion::ToManaged(wire)};
}

std::optional<DecodedRequest> DecodeFindServersRequest(
    std::span<const char> body) {
  return DecodeDiscoveryRequest<ua::FindServersRequest>(body);
}

std::optional<DecodedRequest> DecodeGetEndpointsRequest(
    std::span<const char> body) {
  return DecodeDiscoveryRequest<ua::GetEndpointsRequest>(body);
}

std::optional<DecodedRequest> DecodeRegisterServerRequest(
    std::span<const char> body) {
  return DecodeDiscoveryRequest<ua::RegisterServerRequest>(body);
}

std::optional<DecodedRequest> DecodeRegisterServer2Request(
    std::span<const char> body) {
  return DecodeDiscoveryRequest<ua::RegisterServer2Request>(body);
}

std::optional<DecodedRequest> DecodeActivateSessionRequest(
    std::span<const char> body) {
  Decoder decoder{body};
  ua::ActivateSessionRequest wire;
  if (!ua::Decode(decoder, wire) || !decoder.consumed()) {
    return std::nullopt;
  }
  auto request = session_conversion::ToManaged(wire);
  if (!request) {
    return std::nullopt;
  }
  ServiceRequestHeader header{
      .authentication_token = wire.request_header.authentication_token,
      .request_handle = wire.request_header.request_handle,
      .trace_parent = ua::GetTraceParent(wire.request_header)};
  return DecodedRequest{.header = header, .body = std::move(*request)};
}

std::optional<DecodedRequest> DecodeCloseSessionRequest(
    std::span<const char> body) {
  Decoder decoder{body};
  ua::CloseSessionRequest wire;
  if (!ua::Decode(decoder, wire) || !decoder.consumed()) {
    return std::nullopt;
  }
  ServiceRequestHeader header{
      .authentication_token = wire.request_header.authentication_token,
      .request_handle = wire.request_header.request_handle,
      .trace_parent = ua::GetTraceParent(wire.request_header)};
  return DecodedRequest{.header = header,
                        .body = session_conversion::ToManaged(wire)};
}

std::optional<DecodedRequest> DecodeReadRequest(std::span<const char> body) {
  Decoder decoder{body};
  ua::ReadRequest request;
  if (!ua::Decode(decoder, request) || !decoder.consumed()) {
    return std::nullopt;
  }
  // MaxAge and TimestampsToReturn are range-validated by the service handler so
  // an out-of-range value yields a service-level fault rather than dropping the
  // connection (OPC UA Part 4 §7.40); a negative MaxAge is likewise rejected
  // there.
  ServiceRequestHeader header{
      .authentication_token = request.request_header.authentication_token,
      .request_handle = request.request_header.request_handle,
      .trace_parent = ua::GetTraceParent(request.request_header)};
  return DecodedRequest{
      .header = header,
      .body = std::move(request),
  };
}

std::optional<DecodedRequest> DecodeWriteRequest(std::span<const char> body) {
  Decoder decoder{body};
  ua::WriteRequest request;
  if (!ua::Decode(decoder, request) || !decoder.consumed()) {
    return std::nullopt;
  }
  ServiceRequestHeader header{
      .authentication_token = request.request_header.authentication_token,
      .request_handle = request.request_header.request_handle,
      .trace_parent = ua::GetTraceParent(request.request_header)};
  return DecodedRequest{
      .header = header,
      .body = std::move(request),
  };
}

std::optional<DecodedRequest> DecodeBrowseRequest(std::span<const char> body) {
  Decoder decoder{body};
  ua::BrowseRequest request;
  if (!ua::Decode(decoder, request) || !decoder.consumed()) {
    return std::nullopt;
  }
  ServiceRequestHeader header{
      .authentication_token = request.request_header.authentication_token,
      .request_handle = request.request_header.request_handle,
      .trace_parent = ua::GetTraceParent(request.request_header)};
  return DecodedRequest{
      .header = header,
      .body = std::move(request),
  };
}

std::optional<DecodedRequest> DecodeCallRequest(std::span<const char> body) {
  Decoder decoder{body};
  ua::CallRequest request;
  if (!ua::Decode(decoder, request) || !decoder.consumed()) {
    return std::nullopt;
  }
  ServiceRequestHeader header{
      .authentication_token = request.request_header.authentication_token,
      .request_handle = request.request_header.request_handle,
      .trace_parent = ua::GetTraceParent(request.request_header)};
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

// Decodes a LiteralOperand holding a Boolean (OPC UA Part 4 §7.7.4).
bool DecodeLiteralOperandBool(const DecodedExtensionObject& operand,
                              bool& value) {
  if (operand.type_id != kLiteralOperandEncodingId ||
      operand.encoding != 0x01) {
    return false;
  }

  Decoder decoder{operand.body};
  Variant variant;
  if (!decoder.Decode(variant) || !decoder.consumed()) {
    return false;
  }

  const auto* typed = variant.get_if<bool>();
  if (!typed) {
    return false;
  }
  value = *typed;
  return true;
}

// Decodes a SimpleAttributeOperand and yields the last browse-path segment
// name (the event-field leaf; OPC UA Part 4 §7.4.4.5).
bool DecodeSimpleAttributeOperandLeaf(const DecodedExtensionObject& operand,
                                      std::string& leaf) {
  if (operand.type_id != kSimpleAttributeOperandEncodingId ||
      operand.encoding != 0x01) {
    return false;
  }

  Decoder decoder{operand.body};
  NodeId ignored_type_definition_id;
  std::int32_t browse_path_count = 0;
  if (!decoder.Decode(ignored_type_definition_id) ||
      !decoder.Decode(browse_path_count) || browse_path_count <= 0 ||
      ArrayCountExceedsRemaining(decoder, browse_path_count)) {
    return false;
  }

  QualifiedName segment;
  for (std::int32_t i = 0; i < browse_path_count; ++i) {
    if (!decoder.Decode(segment)) {
      return false;
    }
  }

  std::uint32_t ignored_attribute_id = 0;
  std::string ignored_index_range;
  if (!decoder.Decode(ignored_attribute_id) ||
      !decoder.Decode(ignored_index_range) || !decoder.consumed()) {
    return false;
  }

  leaf = segment.name();
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
        !filter_decoder.Decode(operand_count) || operand_count < 0) {
      return false;
    }

    // Every ContentFilter operand is an extensible parameter (OPC UA Part 4
    // §7.7.4), so unknown operators can be skipped structurally: decode the
    // operand extension objects, then interpret only the shapes this server
    // evaluates. A foreign filter with unsupported operators degrades to
    // less server-side filtering instead of a rejected CreateMonitoredItems.
    // (Full conformance would report per-element EventFilterResult statuses —
    // future work.)
    if (ArrayCountExceedsRemaining(filter_decoder, operand_count))
      return false;
    std::vector<DecodedExtensionObject> operands;
    operands.reserve(static_cast<std::size_t>(operand_count));
    for (std::int32_t operand_index = 0; operand_index < operand_count;
         ++operand_index) {
      DecodedExtensionObject operand;
      if (!filter_decoder.Decode(operand)) {
        return false;
      }
      operands.push_back(std::move(operand));
    }

    NodeId node_id;
    if (filter_operator == kFilterOperatorOfType && operands.size() == 1 &&
        DecodeLiteralOperandNodeId(operands[0], node_id)) {
      filter.of_type.push_back(std::move(node_id));
    } else if (filter_operator == kFilterOperatorRelatedTo &&
               operands.size() == 1 &&
               DecodeLiteralOperandNodeId(operands[0], node_id)) {
      filter.child_of.push_back(std::move(node_id));
    } else if (filter_operator == kFilterOperatorEquals &&
               operands.size() == 2) {
      // `Equals(SimpleAttributeOperand("AckedState"), Literal(Boolean))`, in
      // either operand order, maps onto the ACKED/UNACKED selection.
      for (int attr_index = 0; attr_index < 2; ++attr_index) {
        std::string leaf;
        bool acked = false;
        if (DecodeSimpleAttributeOperandLeaf(operands[attr_index], leaf) &&
            leaf == "AckedState" &&
            DecodeLiteralOperandBool(operands[1 - attr_index], acked)) {
          filter.types |= acked ? EventFilter::ACKED : EventFilter::UNACKED;
          break;
        }
      }
    }
    // else: unsupported operator/shape — skipped (see above).
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

  if (details.encoding != 0x01) {
    return std::nullopt;
  }

  Decoder details_decoder{details.body};

  if (details.type_id == kUpdateDataDetailsEncodingId) {
    UpdateDataDetails data;
    std::int32_t perform_update_type = 0;
    std::int32_t values_count = 0;
    if (!details_decoder.Decode(data.node_id) ||
        !details_decoder.Decode(perform_update_type) ||
        !details_decoder.Decode(values_count) || values_count < 0 ||
        perform_update_type < 1 || perform_update_type > 4) {
      return std::nullopt;
    }
    data.perform_insert_replace =
        static_cast<PerformUpdateType>(perform_update_type);
    if (ArrayCountExceedsRemaining(details_decoder, values_count)) {
      return std::nullopt;
    }
    data.values.resize(static_cast<std::size_t>(values_count));
    for (auto& value : data.values) {
      if (!ReadDataValue(details_decoder, value)) {
        return std::nullopt;
      }
    }
    if (!details_decoder.consumed()) {
      return std::nullopt;
    }
    return DecodedRequest{
        .header = header,
        .body = HistoryUpdateRequest{.details = std::move(data)}};
  }

  if (details.type_id == kUpdateEventDetailsEncodingId) {
    // UpdateEventDetails body: node_id, performInsertReplace, then events as an
    // array of EventFieldList projected onto the default BaseEventType select
    // clauses (mirrors the event HistoryRead encoding). Reconstruct each Event.
    UpdateEventDetails event_details;
    std::int32_t perform_update_type = 0;
    std::int32_t events_count = 0;
    if (!details_decoder.Decode(event_details.node_id) ||
        !details_decoder.Decode(perform_update_type) ||
        !details_decoder.Decode(events_count) || events_count < 0 ||
        perform_update_type < 1 || perform_update_type > 4) {
      return std::nullopt;
    }
    event_details.perform_insert_replace =
        static_cast<PerformUpdateType>(perform_update_type);
    if (ArrayCountExceedsRemaining(details_decoder, events_count)) {
      return std::nullopt;
    }
    const auto& field_paths = DefaultEventFieldPaths();
    event_details.events.reserve(static_cast<std::size_t>(events_count));
    for (std::int32_t i = 0; i < events_count; ++i) {
      std::int32_t field_count = 0;
      if (!details_decoder.Decode(field_count) ||
          ArrayCountExceedsRemaining(details_decoder, field_count)) {
        return std::nullopt;
      }
      std::vector<Variant> fields(static_cast<std::size_t>(field_count));
      for (auto& field : fields) {
        if (!details_decoder.Decode(field)) {
          return std::nullopt;
        }
      }
      event_details.events.push_back(
          ReconstructEventFromFields(field_paths, fields));
    }
    if (!details_decoder.consumed()) {
      return std::nullopt;
    }
    return DecodedRequest{
        .header = header,
        .body = HistoryUpdateRequest{.details = std::move(event_details)}};
  }

  return std::nullopt;
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
  ua::BrowseNextRequest request;
  if (!ua::Decode(decoder, request) || !decoder.consumed()) {
    return std::nullopt;
  }
  ServiceRequestHeader header{
      .authentication_token = request.request_header.authentication_token,
      .request_handle = request.request_header.request_handle,
      .trace_parent = ua::GetTraceParent(request.request_header)};
  return DecodedRequest{
      .header = header,
      .body = std::move(request),
  };
}

std::optional<DecodedRequest> DecodeCreateSubscriptionRequest(
    std::span<const char> body) {
  Decoder decoder{body};
  ua::CreateSubscriptionRequest request;
  if (!ua::Decode(decoder, request) || !decoder.consumed()) {
    return std::nullopt;
  }
  ServiceRequestHeader header{
      .authentication_token = request.request_header.authentication_token,
      .request_handle = request.request_header.request_handle,
      .trace_parent = ua::GetTraceParent(request.request_header)};
  return DecodedRequest{.header = header,
                        .body = subscription_conversion::ToManaged(request)};
}

std::optional<DecodedRequest> DecodeDeleteSubscriptionsRequest(
    std::span<const char> body) {
  // Migrated to the generated codec: decode the whole message (embedded
  // RequestHeader + body), then lift the envelope fields back out of the
  // header for the dispatcher.
  Decoder decoder{body};
  ua::DeleteSubscriptionsRequest request;
  if (!ua::Decode(decoder, request) || !decoder.consumed()) {
    return std::nullopt;
  }
  ServiceRequestHeader header{
      .authentication_token = request.request_header.authentication_token,
      .request_handle = request.request_header.request_handle,
      .trace_parent = ua::GetTraceParent(request.request_header)};
  return DecodedRequest{
      .header = header,
      .body = std::move(request),
  };
}

std::optional<DecodedRequest> DecodeModifySubscriptionRequest(
    std::span<const char> body) {
  Decoder decoder{body};
  ua::ModifySubscriptionRequest request;
  if (!ua::Decode(decoder, request) || !decoder.consumed()) {
    return std::nullopt;
  }
  ServiceRequestHeader header{
      .authentication_token = request.request_header.authentication_token,
      .request_handle = request.request_header.request_handle,
      .trace_parent = ua::GetTraceParent(request.request_header)};
  return DecodedRequest{.header = header,
                        .body = subscription_conversion::ToManaged(request)};
}

std::optional<DecodedRequest> DecodeSetPublishingModeRequest(
    std::span<const char> body) {
  Decoder decoder{body};
  ua::SetPublishingModeRequest request;
  if (!ua::Decode(decoder, request) || !decoder.consumed()) {
    return std::nullopt;
  }
  ServiceRequestHeader header{
      .authentication_token = request.request_header.authentication_token,
      .request_handle = request.request_header.request_handle,
      .trace_parent = ua::GetTraceParent(request.request_header)};
  return DecodedRequest{
      .header = header,
      .body = std::move(request),
  };
}

// Rejects an out-of-range TimestampsToReturn / MonitoringMode, as the
// hand-written decoders did, so an invalid value faults at decode rather than
// reaching the handler (the generated decoder reads them as raw Int32).
bool ValidTimestampsToReturn(ua::TimestampsToReturn value) {
  return value == ua::TimestampsToReturn::Source ||
         value == ua::TimestampsToReturn::Server ||
         value == ua::TimestampsToReturn::Both ||
         value == ua::TimestampsToReturn::Neither;
}

bool ValidMonitoringMode(ua::MonitoringMode value) {
  return value == ua::MonitoringMode::Disabled ||
         value == ua::MonitoringMode::Sampling ||
         value == ua::MonitoringMode::Reporting;
}

std::optional<DecodedRequest> DecodeCreateMonitoredItemsRequest(
    std::span<const char> body) {
  Decoder decoder{body};
  ua::CreateMonitoredItemsRequest request;
  if (!ua::Decode(decoder, request) || !decoder.consumed() ||
      !ValidTimestampsToReturn(request.timestamps_to_return)) {
    return std::nullopt;
  }
  for (const auto& item : request.items_to_create) {
    if (!ValidMonitoringMode(item.monitoring_mode)) {
      return std::nullopt;
    }
  }
  ServiceRequestHeader header{
      .authentication_token = request.request_header.authentication_token,
      .request_handle = request.request_header.request_handle,
      .trace_parent = ua::GetTraceParent(request.request_header)};
  return DecodedRequest{.header = header,
                        .body = subscription_conversion::ToManaged(request)};
}

std::optional<DecodedRequest> DecodeModifyMonitoredItemsRequest(
    std::span<const char> body) {
  Decoder decoder{body};
  ua::ModifyMonitoredItemsRequest request;
  if (!ua::Decode(decoder, request) || !decoder.consumed() ||
      !ValidTimestampsToReturn(request.timestamps_to_return)) {
    return std::nullopt;
  }
  ServiceRequestHeader header{
      .authentication_token = request.request_header.authentication_token,
      .request_handle = request.request_header.request_handle,
      .trace_parent = ua::GetTraceParent(request.request_header)};
  return DecodedRequest{.header = header,
                        .body = subscription_conversion::ToManaged(request)};
}

std::optional<DecodedRequest> DecodePublishRequest(std::span<const char> body) {
  Decoder decoder{body};
  ua::PublishRequest request;
  if (!ua::Decode(decoder, request) || !decoder.consumed()) {
    return std::nullopt;
  }
  ServiceRequestHeader header{
      .authentication_token = request.request_header.authentication_token,
      .request_handle = request.request_header.request_handle,
      .trace_parent = ua::GetTraceParent(request.request_header)};
  return DecodedRequest{.header = header,
                        .body = subscription_conversion::ToManaged(request)};
}

std::optional<DecodedRequest> DecodeTransferSubscriptionsRequest(
    std::span<const char> body) {
  Decoder decoder{body};
  ua::TransferSubscriptionsRequest request;
  if (!ua::Decode(decoder, request) || !decoder.consumed()) {
    return std::nullopt;
  }
  ServiceRequestHeader header{
      .authentication_token = request.request_header.authentication_token,
      .request_handle = request.request_header.request_handle,
      .trace_parent = ua::GetTraceParent(request.request_header)};
  return DecodedRequest{
      .header = header,
      .body = std::move(request),
  };
}

std::optional<DecodedRequest> DecodeDeleteMonitoredItemsRequest(
    std::span<const char> body) {
  Decoder decoder{body};
  ua::DeleteMonitoredItemsRequest request;
  if (!ua::Decode(decoder, request) || !decoder.consumed()) {
    return std::nullopt;
  }
  ServiceRequestHeader header{
      .authentication_token = request.request_header.authentication_token,
      .request_handle = request.request_header.request_handle,
      .trace_parent = ua::GetTraceParent(request.request_header)};
  return DecodedRequest{
      .header = header,
      .body = std::move(request),
  };
}

std::optional<DecodedRequest> DecodeSetMonitoringModeRequest(
    std::span<const char> body) {
  Decoder decoder{body};
  ua::SetMonitoringModeRequest request;
  if (!ua::Decode(decoder, request) || !decoder.consumed()) {
    return std::nullopt;
  }
  // The generated decoder reads the MonitoringMode as a raw Int32; reject any
  // value outside the enumeration, as the hand-written decoder did (an
  // out-of-range mode would otherwise reach the handler).
  if (request.monitoring_mode != ua::MonitoringMode::Disabled &&
      request.monitoring_mode != ua::MonitoringMode::Sampling &&
      request.monitoring_mode != ua::MonitoringMode::Reporting) {
    return std::nullopt;
  }
  ServiceRequestHeader header{
      .authentication_token = request.request_header.authentication_token,
      .request_handle = request.request_header.request_handle,
      .trace_parent = ua::GetTraceParent(request.request_header)};
  return DecodedRequest{
      .header = header,
      .body = std::move(request),
  };
}

std::optional<DecodedRequest> DecodeRepublishRequest(
    std::span<const char> body) {
  Decoder decoder{body};
  ua::RepublishRequest request;
  if (!ua::Decode(decoder, request) || !decoder.consumed()) {
    return std::nullopt;
  }
  ServiceRequestHeader header{
      .authentication_token = request.request_header.authentication_token,
      .request_handle = request.request_header.request_handle,
      .trace_parent = ua::GetTraceParent(request.request_header)};
  return DecodedRequest{.header = header,
                        .body = subscription_conversion::ToManaged(request)};
}

std::optional<DecodedRequest> DecodeDeleteNodesRequest(
    std::span<const char> body) {
  Decoder decoder{body};
  ua::DeleteNodesRequest request;
  if (!ua::Decode(decoder, request) || !decoder.consumed()) {
    return std::nullopt;
  }
  ServiceRequestHeader header{
      .authentication_token = request.request_header.authentication_token,
      .request_handle = request.request_header.request_handle,
      .trace_parent = ua::GetTraceParent(request.request_header)};
  return DecodedRequest{
      .header = header,
      .body = std::move(request),
  };
}

std::optional<DecodedRequest> DecodeAddNodesRequest(
    std::span<const char> body) {
  Decoder decoder{body};
  ua::AddNodesRequest request;
  if (!ua::Decode(decoder, request) || !decoder.consumed()) {
    return std::nullopt;
  }
  ServiceRequestHeader header{
      .authentication_token = request.request_header.authentication_token,
      .request_handle = request.request_header.request_handle,
      .trace_parent = ua::GetTraceParent(request.request_header)};
  return DecodedRequest{
      .header = header,
      .body = std::move(request),
  };
}

std::optional<DecodedRequest> DecodeDeleteReferencesRequest(
    std::span<const char> body) {
  Decoder decoder{body};
  ua::DeleteReferencesRequest request;
  if (!ua::Decode(decoder, request) || !decoder.consumed()) {
    return std::nullopt;
  }
  ServiceRequestHeader header{
      .authentication_token = request.request_header.authentication_token,
      .request_handle = request.request_header.request_handle,
      .trace_parent = ua::GetTraceParent(request.request_header)};
  return DecodedRequest{
      .header = header,
      .body = std::move(request),
  };
}

std::optional<DecodedRequest> DecodeAddReferencesRequest(
    std::span<const char> body) {
  Decoder decoder{body};
  ua::AddReferencesRequest request;
  if (!ua::Decode(decoder, request) || !decoder.consumed()) {
    return std::nullopt;
  }
  ServiceRequestHeader header{
      .authentication_token = request.request_header.authentication_token,
      .request_handle = request.request_header.request_handle,
      .trace_parent = ua::GetTraceParent(request.request_header)};
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
        if constexpr (std::is_same_v<T, ua::ReadRequest>) {
          payload.reserve(EstimateReadRequestPayloadSize(typed_request));
        }
        Encoder payload_encoder{payload};
        Encoder body_encoder{body};

        if constexpr (std::is_same_v<T, FindServersRequest>) {
          ua::FindServersRequest message =
              discovery_conversion::ToWire(typed_request);
          message.request_header =
              ua::MakeRequestHeader(header.authentication_token,
                                    header.request_handle, header.trace_parent);
          ua::Encode(payload_encoder, message);
          AppendMessage(body_encoder, kFindServersRequestEncodingId, payload);
        } else if constexpr (std::is_same_v<T, GetEndpointsRequest>) {
          ua::GetEndpointsRequest message =
              discovery_conversion::ToWire(typed_request);
          message.request_header =
              ua::MakeRequestHeader(header.authentication_token,
                                    header.request_handle, header.trace_parent);
          ua::Encode(payload_encoder, message);
          AppendMessage(body_encoder, kGetEndpointsRequestEncodingId, payload);
        } else if constexpr (std::is_same_v<T, RegisterServerRequest>) {
          ua::RegisterServerRequest message =
              discovery_conversion::ToWire(typed_request);
          message.request_header =
              ua::MakeRequestHeader(header.authentication_token,
                                    header.request_handle, header.trace_parent);
          ua::Encode(payload_encoder, message);
          AppendMessage(body_encoder, kRegisterServerRequestEncodingId,
                        payload);
        } else if constexpr (std::is_same_v<T, RegisterServer2Request>) {
          ua::RegisterServer2Request message =
              discovery_conversion::ToWire(typed_request);
          message.request_header =
              ua::MakeRequestHeader(header.authentication_token,
                                    header.request_handle, header.trace_parent);
          ua::Encode(payload_encoder, message);
          AppendMessage(body_encoder, kRegisterServer2RequestEncodingId,
                        payload);
        } else if constexpr (std::is_same_v<T, CreateSessionRequest>) {
          ua::CreateSessionRequest message =
              session_conversion::ToWire(typed_request);
          message.request_header =
              ua::MakeRequestHeader(header.authentication_token,
                                    header.request_handle, header.trace_parent);
          ua::Encode(payload_encoder, message);
          AppendMessage(body_encoder, kCreateSessionRequestEncodingId, payload);
        } else if constexpr (std::is_same_v<T, ActivateSessionRequest>) {
          ua::ActivateSessionRequest message =
              session_conversion::ToWire(typed_request);
          message.request_header =
              ua::MakeRequestHeader(header.authentication_token,
                                    header.request_handle, header.trace_parent);
          ua::Encode(payload_encoder, message);
          AppendMessage(body_encoder, kActivateSessionRequestEncodingId,
                        payload);
        } else if constexpr (std::is_same_v<T, CloseSessionRequest>) {
          ua::CloseSessionRequest message =
              session_conversion::ToWire(typed_request);
          message.request_header =
              ua::MakeRequestHeader(header.authentication_token,
                                    header.request_handle, header.trace_parent);
          ua::Encode(payload_encoder, message);
          AppendMessage(body_encoder, kCloseSessionRequestEncodingId, payload);
        } else if constexpr (std::is_same_v<T, CreateSubscriptionRequest>) {
          ua::CreateSubscriptionRequest message =
              subscription_conversion::ToWire(typed_request);
          message.request_header =
              ua::MakeRequestHeader(header.authentication_token,
                                    header.request_handle, header.trace_parent);
          ua::Encode(payload_encoder, message);
          AppendMessage(body_encoder, kCreateSubscriptionRequestEncodingId,
                        payload);
        } else if constexpr (std::is_same_v<T, ModifySubscriptionRequest>) {
          ua::ModifySubscriptionRequest message =
              subscription_conversion::ToWire(typed_request);
          message.request_header =
              ua::MakeRequestHeader(header.authentication_token,
                                    header.request_handle, header.trace_parent);
          ua::Encode(payload_encoder, message);
          AppendMessage(body_encoder, kModifySubscriptionRequestEncodingId,
                        payload);
        } else if constexpr (std::is_same_v<T, ua::SetPublishingModeRequest>) {
          ua::SetPublishingModeRequest message = typed_request;
          message.request_header =
              ua::MakeRequestHeader(header.authentication_token,
                                    header.request_handle, header.trace_parent);
          ua::Encode(payload_encoder, message);
          AppendMessage(body_encoder, kSetPublishingModeRequestEncodingId,
                        payload);
        } else if constexpr (std::is_same_v<T,
                                            ua::DeleteSubscriptionsRequest>) {
          // Migrated to the generated codec: inject the envelope header into
          // the message's embedded RequestHeader, then let ua::Encode write
          // header + body as one spec message.
          ua::DeleteSubscriptionsRequest message = typed_request;
          message.request_header =
              ua::MakeRequestHeader(header.authentication_token,
                                    header.request_handle, header.trace_parent);
          ua::Encode(payload_encoder, message);
          AppendMessage(body_encoder, kDeleteSubscriptionsRequestEncodingId,
                        payload);
        } else if constexpr (std::is_same_v<T, CreateMonitoredItemsRequest>) {
          ua::CreateMonitoredItemsRequest message =
              subscription_conversion::ToWire(typed_request);
          message.request_header =
              ua::MakeRequestHeader(header.authentication_token,
                                    header.request_handle, header.trace_parent);
          ua::Encode(payload_encoder, message);
          AppendMessage(body_encoder, kCreateMonitoredItemsRequestEncodingId,
                        payload);
        } else if constexpr (std::is_same_v<T, ModifyMonitoredItemsRequest>) {
          ua::ModifyMonitoredItemsRequest message =
              subscription_conversion::ToWire(typed_request);
          message.request_header =
              ua::MakeRequestHeader(header.authentication_token,
                                    header.request_handle, header.trace_parent);
          ua::Encode(payload_encoder, message);
          AppendMessage(body_encoder, kModifyMonitoredItemsRequestEncodingId,
                        payload);
        } else if constexpr (std::is_same_v<T, PublishRequest>) {
          ua::PublishRequest message =
              subscription_conversion::ToWire(typed_request);
          message.request_header =
              ua::MakeRequestHeader(header.authentication_token,
                                    header.request_handle, header.trace_parent);
          ua::Encode(payload_encoder, message);
          AppendMessage(body_encoder, kPublishRequestEncodingId, payload);
        } else if constexpr (std::is_same_v<T, RepublishRequest>) {
          ua::RepublishRequest message =
              subscription_conversion::ToWire(typed_request);
          message.request_header =
              ua::MakeRequestHeader(header.authentication_token,
                                    header.request_handle, header.trace_parent);
          ua::Encode(payload_encoder, message);
          AppendMessage(body_encoder, kRepublishRequestEncodingId, payload);
        } else if constexpr (std::is_same_v<T,
                                            ua::TransferSubscriptionsRequest>) {
          ua::TransferSubscriptionsRequest message = typed_request;
          message.request_header =
              ua::MakeRequestHeader(header.authentication_token,
                                    header.request_handle, header.trace_parent);
          ua::Encode(payload_encoder, message);
          AppendMessage(body_encoder, kTransferSubscriptionsRequestEncodingId,
                        payload);
        } else if constexpr (std::is_same_v<T,
                                            ua::DeleteMonitoredItemsRequest>) {
          ua::DeleteMonitoredItemsRequest message = typed_request;
          message.request_header =
              ua::MakeRequestHeader(header.authentication_token,
                                    header.request_handle, header.trace_parent);
          ua::Encode(payload_encoder, message);
          AppendMessage(body_encoder, kDeleteMonitoredItemsRequestEncodingId,
                        payload);
        } else if constexpr (std::is_same_v<T, ua::SetMonitoringModeRequest>) {
          ua::SetMonitoringModeRequest message = typed_request;
          message.request_header =
              ua::MakeRequestHeader(header.authentication_token,
                                    header.request_handle, header.trace_parent);
          ua::Encode(payload_encoder, message);
          AppendMessage(body_encoder, kSetMonitoringModeRequestEncodingId,
                        payload);
        } else if constexpr (std::is_same_v<T, ua::ReadRequest>) {
          ua::ReadRequest message = typed_request;
          message.request_header =
              ua::MakeRequestHeader(header.authentication_token,
                                    header.request_handle, header.trace_parent);
          ua::Encode(payload_encoder, message);
          AppendMessage(body_encoder, kReadRequestEncodingId, payload);
        } else if constexpr (std::is_same_v<T, ua::WriteRequest>) {
          ua::WriteRequest message = typed_request;
          message.request_header =
              ua::MakeRequestHeader(header.authentication_token,
                                    header.request_handle, header.trace_parent);
          ua::Encode(payload_encoder, message);
          AppendMessage(body_encoder, kWriteRequestEncodingId, payload);
        } else if constexpr (std::is_same_v<T, ua::BrowseRequest>) {
          ua::BrowseRequest message = typed_request;
          message.request_header =
              ua::MakeRequestHeader(header.authentication_token,
                                    header.request_handle, header.trace_parent);
          ua::Encode(payload_encoder, message);
          AppendMessage(body_encoder, kBrowseRequestEncodingId, payload);
        } else if constexpr (std::is_same_v<T, ua::BrowseNextRequest>) {
          ua::BrowseNextRequest message = typed_request;
          message.request_header =
              ua::MakeRequestHeader(header.authentication_token,
                                    header.request_handle, header.trace_parent);
          ua::Encode(payload_encoder, message);
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
        } else if constexpr (std::is_same_v<T, ua::CallRequest>) {
          ua::CallRequest message = typed_request;
          message.request_header =
              ua::MakeRequestHeader(header.authentication_token,
                                    header.request_handle, header.trace_parent);
          ua::Encode(payload_encoder, message);
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
          std::uint32_t detail_type_id = kUpdateDataDetailsEncodingId;
          if (const auto* data =
                  std::get_if<UpdateDataDetails>(&typed_request.details)) {
            details_encoder.Encode(data->node_id);
            details_encoder.Encode(
                static_cast<std::int32_t>(data->perform_insert_replace));
            details_encoder.Encode(
                static_cast<std::int32_t>(data->values.size()));
            for (const auto& value : data->values) {
              AppendDataValue(details_encoder, value);
            }
          } else {
            const auto& event_details =
                std::get<UpdateEventDetails>(typed_request.details);
            detail_type_id = kUpdateEventDetailsEncodingId;
            const auto& field_paths = DefaultEventFieldPaths();
            details_encoder.Encode(event_details.node_id);
            details_encoder.Encode(static_cast<std::int32_t>(
                event_details.perform_insert_replace));
            details_encoder.Encode(
                static_cast<std::int32_t>(event_details.events.size()));
            for (const auto& event : event_details.events) {
              const auto event_fields =
                  ProjectEventFields(field_paths, std::any{event});
              details_encoder.Encode(
                  static_cast<std::int32_t>(event_fields.size()));
              for (const auto& field : event_fields) {
                details_encoder.Encode(field);
              }
            }
          }

          payload_encoder.Encode(std::int32_t{1});
          payload_encoder.Encode(EncodedExtensionObject{
              .type_id = detail_type_id,
              .body = std::move(details),
          });
          AppendMessage(body_encoder, kHistoryUpdateRequestEncodingId, payload);
        } else if constexpr (std::is_same_v<T, ua::AddNodesRequest>) {
          ua::AddNodesRequest message = typed_request;
          message.request_header =
              ua::MakeRequestHeader(header.authentication_token,
                                    header.request_handle, header.trace_parent);
          ua::Encode(payload_encoder, message);
          AppendMessage(body_encoder, kAddNodesRequestEncodingId, payload);
        } else if constexpr (std::is_same_v<T, ua::DeleteNodesRequest>) {
          ua::DeleteNodesRequest message = typed_request;
          message.request_header =
              ua::MakeRequestHeader(header.authentication_token,
                                    header.request_handle, header.trace_parent);
          ua::Encode(payload_encoder, message);
          AppendMessage(body_encoder, kDeleteNodesRequestEncodingId, payload);
        } else if constexpr (std::is_same_v<T, ua::DeleteReferencesRequest>) {
          ua::DeleteReferencesRequest message = typed_request;
          message.request_header =
              ua::MakeRequestHeader(header.authentication_token,
                                    header.request_handle, header.trace_parent);
          ua::Encode(payload_encoder, message);
          AppendMessage(body_encoder, kDeleteReferencesRequestEncodingId,
                        payload);
        } else if constexpr (std::is_same_v<T, ua::AddReferencesRequest>) {
          ua::AddReferencesRequest message = typed_request;
          message.request_header =
              ua::MakeRequestHeader(header.authentication_token,
                                    header.request_handle, header.trace_parent);
          ua::Encode(payload_encoder, message);
          AppendMessage(body_encoder, kAddReferencesRequestEncodingId, payload);
        } else if constexpr (std::is_same_v<T, ua::RegisterNodesRequest>) {
          ua::RegisterNodesRequest message = typed_request;
          message.request_header =
              ua::MakeRequestHeader(header.authentication_token,
                                    header.request_handle, header.trace_parent);
          ua::Encode(payload_encoder, message);
          AppendMessage(body_encoder, kRegisterNodesRequestEncodingId, payload);
        } else if constexpr (std::is_same_v<T, ua::UnregisterNodesRequest>) {
          ua::UnregisterNodesRequest message = typed_request;
          message.request_header =
              ua::MakeRequestHeader(header.authentication_token,
                                    header.request_handle, header.trace_parent);
          ua::Encode(payload_encoder, message);
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
  ua::RegisterNodesRequest request;
  if (!ua::Decode(decoder, request) || !decoder.consumed()) {
    return std::nullopt;
  }
  ServiceRequestHeader header{
      .authentication_token = request.request_header.authentication_token,
      .request_handle = request.request_header.request_handle,
      .trace_parent = ua::GetTraceParent(request.request_header)};
  return DecodedRequest{.header = header, .body = std::move(request)};
}

std::optional<DecodedRequest> DecodeUnregisterNodesRequest(
    std::span<const char> body) {
  Decoder decoder{body};
  ua::UnregisterNodesRequest request;
  if (!ua::Decode(decoder, request) || !decoder.consumed()) {
    return std::nullopt;
  }
  ServiceRequestHeader header{
      .authentication_token = request.request_header.authentication_token,
      .request_handle = request.request_header.request_handle,
      .trace_parent = ua::GetTraceParent(request.request_header)};
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
    case kRegisterServerRequestEncodingId:
      return DecodeRegisterServerRequest(message->second);
    case kRegisterServer2RequestEncodingId:
      return DecodeRegisterServer2Request(message->second);
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
    case kRegisterServerResponseEncodingId:
      return DecodeRegisterServerResponse(message->second);
    case kRegisterServer2ResponseEncodingId:
      return DecodeRegisterServer2Response(message->second);
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
      return DecodeGeneratedResponse<ua::SetPublishingModeResponse>(
          message->second);
    case kDeleteSubscriptionsResponseEncodingId:
      return DecodeGeneratedResponse<ua::DeleteSubscriptionsResponse>(
          message->second);
    case kCreateMonitoredItemsResponseEncodingId:
      return DecodeCreateMonitoredItemsResponse(message->second);
    case kModifyMonitoredItemsResponseEncodingId:
      return DecodeModifyMonitoredItemsResponse(message->second);
    case kDeleteMonitoredItemsResponseEncodingId:
      return DecodeGeneratedResponse<ua::DeleteMonitoredItemsResponse>(
          message->second);
    case kSetMonitoringModeResponseEncodingId:
      return DecodeGeneratedResponse<ua::SetMonitoringModeResponse>(
          message->second);
    case kPublishResponseEncodingId:
      return DecodePublishResponse(message->second);
    case kRepublishResponseEncodingId:
      return DecodeRepublishResponse(message->second);
    case kTransferSubscriptionsResponseEncodingId:
      return DecodeGeneratedResponse<ua::TransferSubscriptionsResponse>(
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
      return DecodeGeneratedResponse<ua::DeleteNodesResponse>(message->second);
    case kDeleteReferencesResponseEncodingId:
      return DecodeGeneratedResponse<ua::DeleteReferencesResponse>(
          message->second);
    case kAddReferencesResponseEncodingId:
      return DecodeGeneratedResponse<ua::AddReferencesResponse>(
          message->second);
    case kRegisterNodesResponseEncodingId:
      return DecodeGeneratedResponse<ua::RegisterNodesResponse>(
          message->second);
    case kUnregisterNodesResponseEncodingId:
      return DecodeGeneratedResponse<ua::UnregisterNodesResponse>(
          message->second);
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
        if constexpr (std::is_same_v<T, ua::ReadResponse>) {
          payload.reserve(EstimateReadResponsePayloadSize(typed_response));
        }
        Encoder payload_encoder{payload};
        Encoder body_encoder{body};

        if constexpr (std::is_same_v<T, FindServersResponse>) {
          ua::FindServersResponse message =
              discovery_conversion::ToWire(typed_response);
          message.response_header = ua::MakeResponseHeader(
              request_handle, message.response_header.service_result);
          ua::Encode(payload_encoder, message);
          AppendMessage(body_encoder, kFindServersResponseEncodingId, payload);
        } else if constexpr (std::is_same_v<T, GetEndpointsResponse>) {
          ua::GetEndpointsResponse message =
              discovery_conversion::ToWire(typed_response);
          message.response_header = ua::MakeResponseHeader(
              request_handle, message.response_header.service_result);
          ua::Encode(payload_encoder, message);
          AppendMessage(body_encoder, kGetEndpointsResponseEncodingId, payload);
        } else if constexpr (std::is_same_v<T, RegisterServerResponse>) {
          ua::RegisterServerResponse message =
              discovery_conversion::ToWire(typed_response);
          message.response_header = ua::MakeResponseHeader(
              request_handle, message.response_header.service_result);
          ua::Encode(payload_encoder, message);
          AppendMessage(body_encoder, kRegisterServerResponseEncodingId,
                        payload);
        } else if constexpr (std::is_same_v<T, RegisterServer2Response>) {
          ua::RegisterServer2Response message =
              discovery_conversion::ToWire(typed_response);
          message.response_header = ua::MakeResponseHeader(
              request_handle, message.response_header.service_result);
          ua::Encode(payload_encoder, message);
          AppendMessage(body_encoder, kRegisterServer2ResponseEncodingId,
                        payload);
        } else if constexpr (std::is_same_v<T, CreateSessionResponse>) {
          ua::CreateSessionResponse message =
              session_conversion::ToWire(typed_response);
          message.response_header = ua::MakeResponseHeader(
              request_handle, message.response_header.service_result);
          ua::Encode(payload_encoder, message);
          AppendMessage(body_encoder, kCreateSessionResponseEncodingId,
                        payload);
        } else if constexpr (std::is_same_v<T, ActivateSessionResponse>) {
          ua::ActivateSessionResponse message =
              session_conversion::ToWire(typed_response);
          message.response_header = ua::MakeResponseHeader(
              request_handle, message.response_header.service_result);
          ua::Encode(payload_encoder, message);
          AppendMessage(body_encoder, kActivateSessionResponseEncodingId,
                        payload);
        } else if constexpr (std::is_same_v<T, CloseSessionResponse>) {
          ua::CloseSessionResponse message =
              session_conversion::ToWire(typed_response);
          message.response_header = ua::MakeResponseHeader(
              request_handle, message.response_header.service_result);
          ua::Encode(payload_encoder, message);
          AppendMessage(body_encoder, kCloseSessionResponseEncodingId, payload);
        } else if constexpr (std::is_same_v<T, CreateSubscriptionResponse>) {
          ua::CreateSubscriptionResponse message =
              subscription_conversion::ToWire(typed_response);
          message.response_header = ua::MakeResponseHeader(
              request_handle, message.response_header.service_result);
          ua::Encode(payload_encoder, message);
          AppendMessage(body_encoder, kCreateSubscriptionResponseEncodingId,
                        payload);
        } else if constexpr (std::is_same_v<T, ModifySubscriptionResponse>) {
          ua::ModifySubscriptionResponse message =
              subscription_conversion::ToWire(typed_response);
          message.response_header = ua::MakeResponseHeader(
              request_handle, message.response_header.service_result);
          ua::Encode(payload_encoder, message);
          AppendMessage(body_encoder, kModifySubscriptionResponseEncodingId,
                        payload);
        } else if constexpr (std::is_same_v<T, ua::SetPublishingModeResponse>) {
          ua::SetPublishingModeResponse message = typed_response;
          message.response_header = ua::MakeResponseHeader(
              request_handle, message.response_header.service_result);
          ua::Encode(payload_encoder, message);
          AppendMessage(body_encoder, kSetPublishingModeResponseEncodingId,
                        payload);
        } else if constexpr (std::is_same_v<T,
                                            ua::DeleteSubscriptionsResponse>) {
          // Migrated to the generated codec. The envelope owns the
          // request_handle, so inject it into the embedded ResponseHeader
          // (keeping the handler-set service_result) before ua::Encode. The
          // trailing diagnostic_infos array goes out empty (0) rather than the
          // hand-written null (-1) — a spec-legal, non-breaking difference both
          // decoders accept.
          ua::DeleteSubscriptionsResponse message = typed_response;
          message.response_header = ua::MakeResponseHeader(
              request_handle, message.response_header.service_result);
          ua::Encode(payload_encoder, message);
          AppendMessage(body_encoder, kDeleteSubscriptionsResponseEncodingId,
                        payload);
        } else if constexpr (std::is_same_v<T, CreateMonitoredItemsResponse>) {
          ua::CreateMonitoredItemsResponse message =
              subscription_conversion::ToWire(typed_response);
          message.response_header = ua::MakeResponseHeader(
              request_handle, message.response_header.service_result);
          ua::Encode(payload_encoder, message);
          AppendMessage(body_encoder, kCreateMonitoredItemsResponseEncodingId,
                        payload);
        } else if constexpr (std::is_same_v<T, ModifyMonitoredItemsResponse>) {
          ua::ModifyMonitoredItemsResponse message =
              subscription_conversion::ToWire(typed_response);
          message.response_header = ua::MakeResponseHeader(
              request_handle, message.response_header.service_result);
          ua::Encode(payload_encoder, message);
          AppendMessage(body_encoder, kModifyMonitoredItemsResponseEncodingId,
                        payload);
        } else if constexpr (std::is_same_v<T, PublishResponse>) {
          ua::PublishResponse message =
              subscription_conversion::ToWire(typed_response);
          message.response_header = ua::MakeResponseHeader(
              request_handle, message.response_header.service_result);
          ua::Encode(payload_encoder, message);
          AppendMessage(body_encoder, kPublishResponseEncodingId, payload);
        } else if constexpr (std::is_same_v<T, RepublishResponse>) {
          ua::RepublishResponse message =
              subscription_conversion::ToWire(typed_response);
          message.response_header = ua::MakeResponseHeader(
              request_handle, message.response_header.service_result);
          ua::Encode(payload_encoder, message);
          AppendMessage(body_encoder, kRepublishResponseEncodingId, payload);
        } else if constexpr (std::is_same_v<
                                 T, ua::TransferSubscriptionsResponse>) {
          ua::TransferSubscriptionsResponse message = typed_response;
          message.response_header = ua::MakeResponseHeader(
              request_handle, message.response_header.service_result);
          ua::Encode(payload_encoder, message);
          AppendMessage(body_encoder, kTransferSubscriptionsResponseEncodingId,
                        payload);
        } else if constexpr (std::is_same_v<T,
                                            ua::DeleteMonitoredItemsResponse>) {
          ua::DeleteMonitoredItemsResponse message = typed_response;
          message.response_header = ua::MakeResponseHeader(
              request_handle, message.response_header.service_result);
          ua::Encode(payload_encoder, message);
          AppendMessage(body_encoder, kDeleteMonitoredItemsResponseEncodingId,
                        payload);
        } else if constexpr (std::is_same_v<T, ua::SetMonitoringModeResponse>) {
          ua::SetMonitoringModeResponse message = typed_response;
          message.response_header = ua::MakeResponseHeader(
              request_handle, message.response_header.service_result);
          ua::Encode(payload_encoder, message);
          AppendMessage(body_encoder, kSetMonitoringModeResponseEncodingId,
                        payload);
        } else if constexpr (std::is_same_v<T, ua::ReadResponse>) {
          ua::ReadResponse message = typed_response;
          message.response_header = ua::MakeResponseHeader(
              request_handle, message.response_header.service_result);
          ua::Encode(payload_encoder, message);
          AppendMessage(body_encoder, kReadResponseEncodingId, payload);
        } else if constexpr (std::is_same_v<T, ua::WriteResponse>) {
          ua::WriteResponse message = typed_response;
          message.response_header = ua::MakeResponseHeader(
              request_handle, message.response_header.service_result);
          ua::Encode(payload_encoder, message);
          AppendMessage(body_encoder, kWriteResponseEncodingId, payload);
        } else if constexpr (std::is_same_v<T, ua::BrowseResponse>) {
          ua::BrowseResponse message = typed_response;
          message.response_header = ua::MakeResponseHeader(
              request_handle, message.response_header.service_result);
          ua::Encode(payload_encoder, message);
          AppendMessage(body_encoder, kBrowseResponseEncodingId, payload);
        } else if constexpr (std::is_same_v<T, ua::BrowseNextResponse>) {
          ua::BrowseNextResponse message = typed_response;
          message.response_header = ua::MakeResponseHeader(
              request_handle, message.response_header.service_result);
          ua::Encode(payload_encoder, message);
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
        } else if constexpr (std::is_same_v<T, ua::CallResponse>) {
          ua::CallResponse message = typed_response;
          message.response_header = ua::MakeResponseHeader(
              request_handle, message.response_header.service_result);
          ua::Encode(payload_encoder, message);
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
        } else if constexpr (std::is_same_v<T, ua::AddNodesResponse>) {
          ua::AddNodesResponse message = typed_response;
          message.response_header = ua::MakeResponseHeader(
              request_handle, message.response_header.service_result);
          ua::Encode(payload_encoder, message);
          AppendMessage(body_encoder, kAddNodesResponseEncodingId, payload);
        } else if constexpr (std::is_same_v<T, ua::DeleteNodesResponse>) {
          ua::DeleteNodesResponse message = typed_response;
          message.response_header = ua::MakeResponseHeader(
              request_handle, message.response_header.service_result);
          ua::Encode(payload_encoder, message);
          AppendMessage(body_encoder, kDeleteNodesResponseEncodingId, payload);
        } else if constexpr (std::is_same_v<T, ua::DeleteReferencesResponse>) {
          ua::DeleteReferencesResponse message = typed_response;
          message.response_header = ua::MakeResponseHeader(
              request_handle, message.response_header.service_result);
          ua::Encode(payload_encoder, message);
          AppendMessage(body_encoder, kDeleteReferencesResponseEncodingId,
                        payload);
        } else if constexpr (std::is_same_v<T, ua::AddReferencesResponse>) {
          ua::AddReferencesResponse message = typed_response;
          message.response_header = ua::MakeResponseHeader(
              request_handle, message.response_header.service_result);
          ua::Encode(payload_encoder, message);
          AppendMessage(body_encoder, kAddReferencesResponseEncodingId,
                        payload);
        } else if constexpr (std::is_same_v<T, ua::RegisterNodesResponse>) {
          ua::RegisterNodesResponse message = typed_response;
          message.response_header = ua::MakeResponseHeader(
              request_handle, message.response_header.service_result);
          ua::Encode(payload_encoder, message);
          AppendMessage(body_encoder, kRegisterNodesResponseEncodingId,
                        payload);
        } else if constexpr (std::is_same_v<T, ua::UnregisterNodesResponse>) {
          ua::UnregisterNodesResponse message = typed_response;
          message.response_header = ua::MakeResponseHeader(
              request_handle, message.response_header.service_result);
          ua::Encode(payload_encoder, message);
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
