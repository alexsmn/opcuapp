#include "opcua/client/client_protocol_session.h"

#include "opcua/base/boost_log.h"
#include "opcua/base/debug_util.h"
#include "opcua/base/time_ticks.h"
#include "opcua/services/browse_conversion.h"
#include "opcua/services/node_attributes_conversion.h"

#include <utility>
#include <variant>

namespace opcua {
namespace {

BoostLogger logger_{LOG_NAME("OpcUaClientProtocolSession")};

std::size_t CountReferences(const std::vector<BrowseResult>& results) {
  std::size_t count = 0;
  for (const auto& result : results)
    count += result.references.size();
  return count;
}

}  // namespace

ClientProtocolSession::ClientProtocolSession(Context context)
    : connection_{context.connection}, channel_{context.channel} {}

template <typename Response>
Awaitable<StatusOr<Response>> ClientProtocolSession::CallTyped(
    RequestBody request,
    std::string trace_parent) {
  const std::uint32_t request_handle = channel_.NextRequestHandle();
  auto result = co_await channel_.Call(request_handle, std::move(request),
                                       std::move(trace_parent));
  if (!result.ok()) {
    co_return StatusOr<Response>{result.status()};
  }
  if (auto* fault = std::get_if<ServiceFault>(&result.value())) {
    co_return StatusOr<Response>{fault->status};
  }
  if (auto* typed = std::get_if<Response>(&result.value())) {
    co_return StatusOr<Response>{std::move(*typed)};
  }
  co_return StatusOr<Response>{Status{StatusCode::Bad}};
}

Awaitable<Status> ClientProtocolSession::Create(Duration requested_timeout,
                                                Identity identity,
                                                ClientCredentials credentials) {
  auto open_status = co_await connection_.Open();
  if (open_status.bad()) {
    co_return open_status;
  }

  // CreateSession: pre-authentication, so channel's authentication_token is
  // empty (already the default). The client certificate/nonce are sent so the
  // server can verify the ActivateSession signature; both are empty under None.
  auto create_result = co_await CallTyped<CreateSessionResponse>(RequestBody{
      CreateSessionRequest{.requested_timeout = requested_timeout,
                           .client_certificate = credentials.certificate,
                           .client_nonce = credentials.nonce}});
  if (!create_result.ok()) {
    co_return create_result.status();
  }
  if (create_result->status.bad()) {
    co_return create_result->status;
  }
  // Verify the server returned the same certificate the client selected during
  // discovery (OPC UA Part 4 §5.6.2). A mismatch means the secured channel is
  // not talking to the endpoint we vetted, so reject before activating.
  if (!credentials.expected_server_certificate.empty() &&
      credentials.expected_server_certificate !=
          create_result->server_certificate) {
    co_return Status{StatusCode::Bad};
  }

  session_id_ = create_result->session_id;
  authentication_token_ = create_result->authentication_token;

  // Subsequent requests (including ActivateSession) need the session's
  // authentication token in the header.
  channel_.set_authentication_token(authentication_token_);

  ActivateSessionRequest activate_request{
      .session_id = session_id_,
      .authentication_token = authentication_token_,
      .user_name = identity.user_name,
      .password = identity.password,
      .delete_existing = false,
      .allow_anonymous = !identity.user_name.has_value(),
  };
  // Sign (serverCertificate || serverNonce) when a secured channel provided a
  // signer (OPC UA Part 4 §5.6.3). Under None the signer is null and the
  // signature stays empty.
  if (credentials.signer) {
    auto signature = credentials.signer(create_result->server_certificate,
                                        create_result->server_nonce);
    if (!signature.ok()) {
      co_return signature.status();
    }
    activate_request.client_signature_algorithm =
        std::move(signature->algorithm);
    activate_request.client_signature = std::move(signature->signature);
  }

  auto activate_result = co_await CallTyped<ActivateSessionResponse>(
      RequestBody{std::move(activate_request)});
  if (!activate_result.ok()) {
    co_return activate_result.status();
  }
  if (activate_result->status.bad()) {
    co_return activate_result->status;
  }

  is_active_ = true;
  channel_.MarkLoginComplete();
  co_return Status{StatusCode::Good};
}

Awaitable<Status> ClientProtocolSession::Close() {
  if (is_active_) {
    auto close_result = co_await CallTyped<CloseSessionResponse>(
        RequestBody{CloseSessionRequest{
            .session_id = session_id_,
            .authentication_token = authentication_token_,
        }});
    is_active_ = false;
    // Swallow the close status; the connection shutdown below is more
    // important to run than to report.
    (void)close_result;
  }
  (void)(co_await connection_.Close());
  co_return Status{StatusCode::Good};
}

Awaitable<StatusOr<std::vector<DataValue>>> ClientProtocolSession::Read(
    std::vector<ReadValueId> inputs,
    std::string trace_parent) {
  const auto input_count = inputs.size();
  const auto start_ticks = base::TimeTicks::Now();
  // The public API speaks the hand-written ReadValueId; widen to the generated
  // request. TimestampsToReturn is Both (the previous encoder hardcoded it),
  // and IndexRange/DataEncoding stay empty.
  ua::ReadRequest request;
  request.timestamps_to_return = ua::TimestampsToReturn::Both;
  request.nodes_to_read.reserve(inputs.size());
  for (const auto& input : inputs) {
    request.nodes_to_read.push_back(
        {.node_id = input.node_id,
         .attribute_id = static_cast<UInt32>(input.attribute_id)});
  }
  auto result = co_await CallTyped<ua::ReadResponse>(
      RequestBody{std::move(request)}, trace_parent);
  const auto duration = base::TimeTicks::Now() - start_ticks;
  if (!result.ok()) {
    LOG_INFO(logger_) << "OPC UA client Read completed"
                      << LOG_TAG("InputCount", input_count)
                      << LOG_TAG("ResultCount", 0)
                      << LOG_TAG("DurationMs", duration.InMilliseconds())
                      << LOG_TAG("Status", ToString(result.status()))
                      << LOG_TAG(kTraceParentLogAttribute, trace_parent);
    co_return StatusOr<std::vector<DataValue>>{result.status()};
  }
  if (result->response_header.service_result.bad()) {
    LOG_INFO(logger_) << "OPC UA client Read completed"
                      << LOG_TAG("InputCount", input_count)
                      << LOG_TAG("ResultCount", result->results.size())
                      << LOG_TAG("DurationMs", duration.InMilliseconds())
                      << LOG_TAG(
                             "Status",
                             ToString(result->response_header.service_result))
                      << LOG_TAG(kTraceParentLogAttribute, trace_parent);
    co_return StatusOr<std::vector<DataValue>>{
        result->response_header.service_result};
  }
  LOG_INFO(logger_) << "OPC UA client Read completed"
                    << LOG_TAG("InputCount", input_count)
                    << LOG_TAG("ResultCount", result->results.size())
                    << LOG_TAG("DurationMs", duration.InMilliseconds())
                    << LOG_TAG("Status",
                               ToString(result->response_header.service_result))
                    << LOG_TAG(kTraceParentLogAttribute, trace_parent);
  co_return StatusOr<std::vector<DataValue>>{std::move(result->results)};
}

Awaitable<StatusOr<std::vector<StatusCode>>> ClientProtocolSession::Write(
    std::vector<WriteValue> inputs,
    std::string trace_parent) {
  const auto input_count = inputs.size();
  const auto start_ticks = base::TimeTicks::Now();
  // The public API speaks the hand-written WriteValue (value:Variant + flags);
  // wrap each Variant into the generated WriteValue's DataValue. flags is an
  // opcuapp-internal detail never carried on the wire, so it is dropped, and
  // index_range is left empty — matching the previous hand-written encode.
  ua::WriteRequest request;
  request.nodes_to_write.reserve(inputs.size());
  for (auto& input : inputs) {
    ua::WriteValue value;
    value.node_id = std::move(input.node_id);
    value.attribute_id = static_cast<UInt32>(input.attribute_id);
    value.value.value = std::move(input.value);
    request.nodes_to_write.push_back(std::move(value));
  }
  auto result = co_await CallTyped<ua::WriteResponse>(
      RequestBody{std::move(request)}, trace_parent);
  const auto duration = base::TimeTicks::Now() - start_ticks;
  if (!result.ok()) {
    LOG_INFO(logger_) << "OPC UA client Write completed"
                      << LOG_TAG("InputCount", input_count)
                      << LOG_TAG("ResultCount", 0)
                      << LOG_TAG("DurationMs", duration.InMilliseconds())
                      << LOG_TAG("Status", ToString(result.status()))
                      << LOG_TAG(kTraceParentLogAttribute, trace_parent);
    co_return StatusOr<std::vector<StatusCode>>{result.status()};
  }
  LOG_INFO(logger_) << "OPC UA client Write completed"
                    << LOG_TAG("InputCount", input_count)
                    << LOG_TAG("ResultCount", result->results.size())
                    << LOG_TAG("DurationMs", duration.InMilliseconds())
                    << LOG_TAG("Status",
                               ToString(result->response_header.service_result))
                    << LOG_TAG(kTraceParentLogAttribute, trace_parent);
  if (result->response_header.service_result.bad()) {
    co_return StatusOr<std::vector<StatusCode>>{
        result->response_header.service_result};
  }
  std::vector<StatusCode> results;
  results.reserve(result->results.size());
  for (const auto status : result->results)
    results.push_back(status.code());
  co_return StatusOr<std::vector<StatusCode>>{std::move(results)};
}

Awaitable<StatusOr<std::vector<AddNodesResult>>>
ClientProtocolSession::AddNodes(std::vector<AddNodesItem> inputs,
                                std::string trace_parent) {
  // The public API speaks the hand-written AddNodesItem; build the generated
  // request: NodeIds widen to ExpandedNodeIds, BrowseName is lifted out of the
  // flat NodeAttributes onto the item, and the attributes become the spec
  // ExtensionObject body. The hand-written item has no reference_type_id, so it
  // stays null (matching the previous encoder).
  ua::AddNodesRequest request;
  request.nodes_to_add.reserve(inputs.size());
  for (auto& item : inputs) {
    request.nodes_to_add.push_back(ua::AddNodesItem{
        .parent_node_id = ExpandedNodeId{item.parent_id},
        .requested_new_node_id = ExpandedNodeId{item.requested_id},
        .browse_name = item.attributes.browse_name,
        .node_class = static_cast<ua::NodeClass>(item.node_class),
        .node_attributes =
            NodeAttributesToExtensionObject(item.node_class, item.attributes),
        .type_definition = ExpandedNodeId{item.type_definition_id}});
  }
  auto result = co_await CallTyped<ua::AddNodesResponse>(
      RequestBody{std::move(request)}, std::move(trace_parent));
  if (!result.ok()) {
    co_return StatusOr<std::vector<AddNodesResult>>{result.status()};
  }
  if (result->response_header.service_result.bad()) {
    co_return StatusOr<std::vector<AddNodesResult>>{
        result->response_header.service_result};
  }
  std::vector<AddNodesResult> results;
  results.reserve(result->results.size());
  for (const auto& add_result : result->results) {
    results.push_back(
        AddNodesResult{.status_code = add_result.status_code.code(),
                       .added_node_id = add_result.added_node_id});
  }
  co_return StatusOr<std::vector<AddNodesResult>>{std::move(results)};
}

Awaitable<StatusOr<std::vector<StatusCode>>> ClientProtocolSession::DeleteNodes(
    std::vector<DeleteNodesItem> inputs,
    std::string trace_parent) {
  // The public API speaks the hand-written DeleteNodesItem; convert to the
  // generated request's items (identical fields).
  ua::DeleteNodesRequest request;
  request.nodes_to_delete.reserve(inputs.size());
  for (const auto& item : inputs) {
    request.nodes_to_delete.push_back(
        {.node_id = item.node_id,
         .delete_target_references = item.delete_target_references});
  }
  auto result = co_await CallTyped<ua::DeleteNodesResponse>(
      RequestBody{std::move(request)}, std::move(trace_parent));
  if (!result.ok()) {
    co_return StatusOr<std::vector<StatusCode>>{result.status()};
  }
  if (result->response_header.service_result.bad()) {
    co_return StatusOr<std::vector<StatusCode>>{
        result->response_header.service_result};
  }
  std::vector<StatusCode> results;
  results.reserve(result->results.size());
  for (const auto status : result->results)
    results.push_back(status.code());
  co_return StatusOr<std::vector<StatusCode>>{std::move(results)};
}

Awaitable<StatusOr<std::vector<StatusCode>>>
ClientProtocolSession::AddReferences(std::vector<AddReferencesItem> inputs,
                                     std::string trace_parent) {
  // The public API speaks the hand-written AddReferencesItem; convert to the
  // generated request's items (identical fields).
  ua::AddReferencesRequest request;
  request.references_to_add.reserve(inputs.size());
  for (const auto& item : inputs) {
    request.references_to_add.push_back(
        {.source_node_id = item.source_node_id,
         .reference_type_id = item.reference_type_id,
         .is_forward = item.forward,
         .target_server_uri = item.target_server_uri,
         .target_node_id = item.target_node_id,
         // The hand-written opcua::NodeClass and the generated
         // opcua::ua::NodeClass share the standard integer values.
         .target_node_class =
             static_cast<ua::NodeClass>(item.target_node_class)});
  }
  auto result = co_await CallTyped<ua::AddReferencesResponse>(
      RequestBody{std::move(request)}, std::move(trace_parent));
  if (!result.ok()) {
    co_return StatusOr<std::vector<StatusCode>>{result.status()};
  }
  if (result->response_header.service_result.bad()) {
    co_return StatusOr<std::vector<StatusCode>>{
        result->response_header.service_result};
  }
  std::vector<StatusCode> results;
  results.reserve(result->results.size());
  for (const auto status : result->results)
    results.push_back(status.code());
  co_return StatusOr<std::vector<StatusCode>>{std::move(results)};
}

Awaitable<StatusOr<std::vector<StatusCode>>>
ClientProtocolSession::DeleteReferences(
    std::vector<DeleteReferencesItem> inputs,
    std::string trace_parent) {
  // The public API speaks the hand-written DeleteReferencesItem; convert to the
  // generated request's items (identical fields).
  ua::DeleteReferencesRequest request;
  request.references_to_delete.reserve(inputs.size());
  for (const auto& item : inputs) {
    request.references_to_delete.push_back(
        {.source_node_id = item.source_node_id,
         .reference_type_id = item.reference_type_id,
         .is_forward = item.forward,
         .target_node_id = item.target_node_id,
         .delete_bidirectional = item.delete_bidirectional});
  }
  auto result = co_await CallTyped<ua::DeleteReferencesResponse>(
      RequestBody{std::move(request)}, std::move(trace_parent));
  if (!result.ok()) {
    co_return StatusOr<std::vector<StatusCode>>{result.status()};
  }
  if (result->response_header.service_result.bad()) {
    co_return StatusOr<std::vector<StatusCode>>{
        result->response_header.service_result};
  }
  std::vector<StatusCode> results;
  results.reserve(result->results.size());
  for (const auto status : result->results)
    results.push_back(status.code());
  co_return StatusOr<std::vector<StatusCode>>{std::move(results)};
}

Awaitable<StatusOr<HistoryReadRawResult>> ClientProtocolSession::HistoryReadRaw(
    HistoryReadRawDetails details,
    std::string trace_parent) {
  // The HistoryReadRawResult carries its own per-node status, so transport
  // failure is the only thing folded into the StatusOr; callers inspect
  // result.status for the service-level outcome.
  auto result = co_await CallTyped<HistoryReadRawResponse>(
      RequestBody{HistoryReadRawRequest{.details = std::move(details)}},
      std::move(trace_parent));
  if (!result.ok()) {
    co_return StatusOr<HistoryReadRawResult>{result.status()};
  }
  co_return StatusOr<HistoryReadRawResult>{std::move(result->result)};
}

Awaitable<StatusOr<HistoryReadEventsResult>>
ClientProtocolSession::HistoryReadEvents(HistoryReadEventsDetails details,
                                         std::string trace_parent) {
  auto result = co_await CallTyped<HistoryReadEventsResponse>(
      RequestBody{HistoryReadEventsRequest{.details = std::move(details)}},
      std::move(trace_parent));
  if (!result.ok()) {
    co_return StatusOr<HistoryReadEventsResult>{result.status()};
  }
  co_return StatusOr<HistoryReadEventsResult>{std::move(result->result)};
}

Awaitable<StatusOr<HistoryUpdateResult>>
ClientProtocolSession::HistoryUpdateData(UpdateDataDetails details,
                                         std::string trace_parent) {
  auto result = co_await CallTyped<HistoryUpdateResponse>(
      RequestBody{HistoryUpdateRequest{.details = std::move(details)}},
      std::move(trace_parent));
  if (!result.ok()) {
    co_return StatusOr<HistoryUpdateResult>{result.status()};
  }
  co_return StatusOr<HistoryUpdateResult>{std::move(result->result)};
}

Awaitable<StatusOr<HistoryUpdateResult>>
ClientProtocolSession::HistoryUpdateEvent(UpdateEventDetails details,
                                          std::string trace_parent) {
  auto result = co_await CallTyped<HistoryUpdateResponse>(
      RequestBody{HistoryUpdateRequest{.details = std::move(details)}},
      std::move(trace_parent));
  if (!result.ok()) {
    co_return StatusOr<HistoryUpdateResult>{result.status()};
  }
  co_return StatusOr<HistoryUpdateResult>{std::move(result->result)};
}

Awaitable<StatusOr<std::vector<BrowseResult>>> ClientProtocolSession::Browse(
    std::vector<BrowseDescription> inputs,
    std::string trace_parent) {
  const auto input_count = inputs.size();
  const auto start_ticks = base::TimeTicks::Now();
  // The public API speaks the hand-written Browse types; convert to and from
  // the generated request/response.
  ua::BrowseRequest request;
  request.nodes_to_browse.reserve(inputs.size());
  for (const auto& description : inputs)
    request.nodes_to_browse.push_back(ToGenerated(description));
  auto result = co_await CallTyped<ua::BrowseResponse>(
      RequestBody{std::move(request)}, trace_parent);
  const auto duration = base::TimeTicks::Now() - start_ticks;
  if (!result.ok()) {
    LOG_INFO(logger_) << "OPC UA client Browse completed"
                      << LOG_TAG("InputCount", input_count)
                      << LOG_TAG("ResultCount", 0)
                      << LOG_TAG("ReferenceCount", 0)
                      << LOG_TAG("DurationMs", duration.InMilliseconds())
                      << LOG_TAG("Status", ToString(result.status()))
                      << LOG_TAG(kTraceParentLogAttribute, trace_parent);
    co_return StatusOr<std::vector<BrowseResult>>{result.status()};
  }
  std::vector<BrowseResult> results;
  results.reserve(result->results.size());
  for (const auto& browse_result : result->results)
    results.push_back(ToHandWritten(browse_result));
  const auto& service_result = result->response_header.service_result;
  LOG_INFO(logger_) << "OPC UA client Browse completed"
                    << LOG_TAG("InputCount", input_count)
                    << LOG_TAG("ResultCount", results.size())
                    << LOG_TAG("ReferenceCount", CountReferences(results))
                    << LOG_TAG("DurationMs", duration.InMilliseconds())
                    << LOG_TAG("Status", ToString(service_result))
                    << LOG_TAG(kTraceParentLogAttribute, trace_parent);
  if (service_result.bad()) {
    co_return StatusOr<std::vector<BrowseResult>>{service_result};
  }
  co_return StatusOr<std::vector<BrowseResult>>{std::move(results)};
}

Awaitable<StatusOr<std::vector<BrowseResult>>>
ClientProtocolSession::BrowseNext(std::vector<ByteString> continuation_points,
                                  bool release_continuation_points) {
  auto result = co_await CallTyped<ua::BrowseNextResponse>(
      RequestBody{ua::BrowseNextRequest{
          .release_continuation_points = release_continuation_points,
          .continuation_points = std::move(continuation_points),
      }});
  if (!result.ok()) {
    co_return StatusOr<std::vector<BrowseResult>>{result.status()};
  }
  if (result->response_header.service_result.bad()) {
    co_return StatusOr<std::vector<BrowseResult>>{
        result->response_header.service_result};
  }
  std::vector<BrowseResult> results;
  results.reserve(result->results.size());
  for (const auto& browse_result : result->results)
    results.push_back(ToHandWritten(browse_result));
  co_return StatusOr<std::vector<BrowseResult>>{std::move(results)};
}

Awaitable<StatusOr<std::vector<BrowsePathResult>>>
ClientProtocolSession::TranslateBrowsePathsToNodeIds(
    std::vector<BrowsePath> inputs,
    std::string trace_parent) {
  auto result = co_await CallTyped<TranslateBrowsePathsResponse>(
      RequestBody{TranslateBrowsePathsRequest{.inputs = std::move(inputs)}},
      std::move(trace_parent));
  if (!result.ok()) {
    co_return StatusOr<std::vector<BrowsePathResult>>{result.status()};
  }
  if (result->status.bad()) {
    co_return StatusOr<std::vector<BrowsePathResult>>{result->status};
  }
  co_return StatusOr<std::vector<BrowsePathResult>>{std::move(result->results)};
}

Awaitable<StatusOr<ClientProtocolSession::CallResult>>
ClientProtocolSession::Call(NodeId object_id,
                            NodeId method_id,
                            std::vector<Variant> arguments,
                            std::string trace_parent) {
  const auto start_ticks = base::TimeTicks::Now();
  auto result = co_await CallTyped<CallResponse>(
      RequestBody{CallRequest{.methods = {MethodCallRequest{
                                  .object_id = std::move(object_id),
                                  .method_id = std::move(method_id),
                                  .arguments = std::move(arguments),
                              }}}},
      trace_parent);
  const auto duration = base::TimeTicks::Now() - start_ticks;
  if (!result.ok()) {
    LOG_INFO(logger_) << "OPC UA client Call completed"
                      << LOG_TAG("DurationMs", duration.InMilliseconds())
                      << LOG_TAG("Status", ToString(result.status()))
                      << LOG_TAG(kTraceParentLogAttribute, trace_parent);
    co_return StatusOr<CallResult>{result.status()};
  }
  if (result->results.empty()) {
    LOG_INFO(logger_) << "OPC UA client Call completed"
                      << LOG_TAG("DurationMs", duration.InMilliseconds())
                      << LOG_TAG("Status", ToString(Status{StatusCode::Bad}))
                      << LOG_TAG(kTraceParentLogAttribute, trace_parent);
    co_return StatusOr<CallResult>{Status{StatusCode::Bad}};
  }
  auto& first = result->results.front();
  LOG_INFO(logger_) << "OPC UA client Call completed"
                    << LOG_TAG("DurationMs", duration.InMilliseconds())
                    << LOG_TAG("Status", ToString(first.status))
                    << LOG_TAG(kTraceParentLogAttribute, trace_parent);
  co_return StatusOr<CallResult>{CallResult{
      .status = first.status,
      .input_argument_results = std::move(first.input_argument_results),
      .output_arguments = std::move(first.output_arguments),
  }};
}

}  // namespace opcua
