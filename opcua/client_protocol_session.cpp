#include "opcua/client_protocol_session.h"

#include "opcua/base/boost_log.h"
#include "opcua/base/debug_util.h"

#include <utility>
#include <variant>

namespace opcua {
namespace {

BoostLogger logger_{LOG_NAME("OpcUaClientProtocolSession")};

std::size_t CountReferences(const std::vector<scada::BrowseResult>& results) {
  std::size_t count = 0;
  for (const auto& result : results)
    count += result.references.size();
  return count;
}

}  // namespace

ClientProtocolSession::ClientProtocolSession(Context context)
    : connection_{context.connection}, channel_{context.channel} {}

template <typename Response>
Awaitable<scada::StatusOr<Response>> ClientProtocolSession::CallTyped(
    RequestBody request) {
  const std::uint32_t request_handle = channel_.NextRequestHandle();
  auto result = co_await channel_.Call(request_handle, std::move(request));
  if (!result.ok()) {
    co_return scada::StatusOr<Response>{result.status()};
  }
  if (auto* fault = std::get_if<ServiceFault>(&result.value())) {
    co_return scada::StatusOr<Response>{fault->status};
  }
  if (auto* typed = std::get_if<Response>(&result.value())) {
    co_return scada::StatusOr<Response>{std::move(*typed)};
  }
  co_return scada::StatusOr<Response>{scada::Status{scada::StatusCode::Bad}};
}

Awaitable<scada::Status> ClientProtocolSession::Create(
    base::TimeDelta requested_timeout,
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
    co_return scada::Status{scada::StatusCode::Bad};
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
  co_return scada::Status{scada::StatusCode::Good};
}

Awaitable<scada::Status> ClientProtocolSession::Close() {
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
  co_return scada::Status{scada::StatusCode::Good};
}

Awaitable<scada::StatusOr<std::vector<scada::DataValue>>>
ClientProtocolSession::Read(std::vector<scada::ReadValueId> inputs) {
  const auto input_count = inputs.size();
  const auto start_ticks = base::TimeTicks::Now();
  auto result = co_await CallTyped<ReadResponse>(
      RequestBody{ReadRequest{.inputs = std::move(inputs)}});
  const auto duration = base::TimeTicks::Now() - start_ticks;
  if (!result.ok()) {
    LOG_INFO(logger_) << "OPC UA client Read completed"
                      << LOG_TAG("InputCount", input_count)
                      << LOG_TAG("ResultCount", 0)
                      << LOG_TAG("DurationMs", duration.InMilliseconds())
                      << LOG_TAG("Status", ToString(result.status()));
    co_return scada::StatusOr<std::vector<scada::DataValue>>{result.status()};
  }
  if (result->status.bad()) {
    LOG_INFO(logger_) << "OPC UA client Read completed"
                      << LOG_TAG("InputCount", input_count)
                      << LOG_TAG("ResultCount", result->results.size())
                      << LOG_TAG("DurationMs", duration.InMilliseconds())
                      << LOG_TAG("Status", ToString(result->status));
    co_return scada::StatusOr<std::vector<scada::DataValue>>{result->status};
  }
  LOG_INFO(logger_) << "OPC UA client Read completed"
                    << LOG_TAG("InputCount", input_count)
                    << LOG_TAG("ResultCount", result->results.size())
                    << LOG_TAG("DurationMs", duration.InMilliseconds())
                    << LOG_TAG("Status", ToString(result->status));
  co_return scada::StatusOr<std::vector<scada::DataValue>>{
      std::move(result->results)};
}

Awaitable<scada::StatusOr<std::vector<scada::StatusCode>>>
ClientProtocolSession::Write(std::vector<scada::WriteValue> inputs) {
  auto result = co_await CallTyped<WriteResponse>(
      RequestBody{WriteRequest{.inputs = std::move(inputs)}});
  if (!result.ok()) {
    co_return scada::StatusOr<std::vector<scada::StatusCode>>{result.status()};
  }
  if (result->status.bad()) {
    co_return scada::StatusOr<std::vector<scada::StatusCode>>{result->status};
  }
  co_return scada::StatusOr<std::vector<scada::StatusCode>>{
      std::move(result->results)};
}

Awaitable<scada::StatusOr<std::vector<scada::AddNodesResult>>>
ClientProtocolSession::AddNodes(std::vector<scada::AddNodesItem> inputs) {
  auto result = co_await CallTyped<AddNodesResponse>(
      RequestBody{AddNodesRequest{.items = std::move(inputs)}});
  if (!result.ok()) {
    co_return scada::StatusOr<std::vector<scada::AddNodesResult>>{
        result.status()};
  }
  if (result->status.bad()) {
    co_return scada::StatusOr<std::vector<scada::AddNodesResult>>{
        result->status};
  }
  co_return scada::StatusOr<std::vector<scada::AddNodesResult>>{
      std::move(result->results)};
}

Awaitable<scada::StatusOr<std::vector<scada::StatusCode>>>
ClientProtocolSession::DeleteNodes(std::vector<scada::DeleteNodesItem> inputs) {
  auto result = co_await CallTyped<DeleteNodesResponse>(
      RequestBody{DeleteNodesRequest{.items = std::move(inputs)}});
  if (!result.ok()) {
    co_return scada::StatusOr<std::vector<scada::StatusCode>>{result.status()};
  }
  if (result->status.bad()) {
    co_return scada::StatusOr<std::vector<scada::StatusCode>>{result->status};
  }
  co_return scada::StatusOr<std::vector<scada::StatusCode>>{
      std::move(result->results)};
}

Awaitable<scada::StatusOr<std::vector<scada::StatusCode>>>
ClientProtocolSession::AddReferences(
    std::vector<scada::AddReferencesItem> inputs) {
  auto result = co_await CallTyped<AddReferencesResponse>(
      RequestBody{AddReferencesRequest{.items = std::move(inputs)}});
  if (!result.ok()) {
    co_return scada::StatusOr<std::vector<scada::StatusCode>>{result.status()};
  }
  if (result->status.bad()) {
    co_return scada::StatusOr<std::vector<scada::StatusCode>>{result->status};
  }
  co_return scada::StatusOr<std::vector<scada::StatusCode>>{
      std::move(result->results)};
}

Awaitable<scada::StatusOr<std::vector<scada::StatusCode>>>
ClientProtocolSession::DeleteReferences(
    std::vector<scada::DeleteReferencesItem> inputs) {
  auto result = co_await CallTyped<DeleteReferencesResponse>(
      RequestBody{DeleteReferencesRequest{.items = std::move(inputs)}});
  if (!result.ok()) {
    co_return scada::StatusOr<std::vector<scada::StatusCode>>{result.status()};
  }
  if (result->status.bad()) {
    co_return scada::StatusOr<std::vector<scada::StatusCode>>{result->status};
  }
  co_return scada::StatusOr<std::vector<scada::StatusCode>>{
      std::move(result->results)};
}

Awaitable<scada::StatusOr<std::vector<scada::BrowseResult>>>
ClientProtocolSession::Browse(std::vector<scada::BrowseDescription> inputs) {
  const auto input_count = inputs.size();
  const auto start_ticks = base::TimeTicks::Now();
  auto result = co_await CallTyped<BrowseResponse>(
      RequestBody{BrowseRequest{.inputs = std::move(inputs)}});
  const auto duration = base::TimeTicks::Now() - start_ticks;
  if (!result.ok()) {
    LOG_INFO(logger_) << "OPC UA client Browse completed"
                      << LOG_TAG("InputCount", input_count)
                      << LOG_TAG("ResultCount", 0)
                      << LOG_TAG("ReferenceCount", 0)
                      << LOG_TAG("DurationMs", duration.InMilliseconds())
                      << LOG_TAG("Status", ToString(result.status()));
    co_return scada::StatusOr<std::vector<scada::BrowseResult>>{
        result.status()};
  }
  if (result->status.bad()) {
    LOG_INFO(logger_) << "OPC UA client Browse completed"
                      << LOG_TAG("InputCount", input_count)
                      << LOG_TAG("ResultCount", result->results.size())
                      << LOG_TAG("ReferenceCount",
                                 CountReferences(result->results))
                      << LOG_TAG("DurationMs", duration.InMilliseconds())
                      << LOG_TAG("Status", ToString(result->status));
    co_return scada::StatusOr<std::vector<scada::BrowseResult>>{result->status};
  }
  LOG_INFO(logger_) << "OPC UA client Browse completed"
                    << LOG_TAG("InputCount", input_count)
                    << LOG_TAG("ResultCount", result->results.size())
                    << LOG_TAG("ReferenceCount",
                               CountReferences(result->results))
                    << LOG_TAG("DurationMs", duration.InMilliseconds())
                    << LOG_TAG("Status", ToString(result->status));
  co_return scada::StatusOr<std::vector<scada::BrowseResult>>{
      std::move(result->results)};
}

Awaitable<scada::StatusOr<std::vector<scada::BrowseResult>>>
ClientProtocolSession::BrowseNext(
    std::vector<scada::ByteString> continuation_points,
    bool release_continuation_points) {
  auto result =
      co_await CallTyped<BrowseNextResponse>(RequestBody{BrowseNextRequest{
          .release_continuation_points = release_continuation_points,
          .continuation_points = std::move(continuation_points),
      }});
  if (!result.ok()) {
    co_return scada::StatusOr<std::vector<scada::BrowseResult>>{
        result.status()};
  }
  if (result->status.bad()) {
    co_return scada::StatusOr<std::vector<scada::BrowseResult>>{result->status};
  }
  co_return scada::StatusOr<std::vector<scada::BrowseResult>>{
      std::move(result->results)};
}

Awaitable<scada::StatusOr<std::vector<scada::BrowsePathResult>>>
ClientProtocolSession::TranslateBrowsePathsToNodeIds(
    std::vector<scada::BrowsePath> inputs) {
  auto result = co_await CallTyped<TranslateBrowsePathsResponse>(
      RequestBody{TranslateBrowsePathsRequest{.inputs = std::move(inputs)}});
  if (!result.ok()) {
    co_return scada::StatusOr<std::vector<scada::BrowsePathResult>>{
        result.status()};
  }
  if (result->status.bad()) {
    co_return scada::StatusOr<std::vector<scada::BrowsePathResult>>{
        result->status};
  }
  co_return scada::StatusOr<std::vector<scada::BrowsePathResult>>{
      std::move(result->results)};
}

Awaitable<scada::StatusOr<ClientProtocolSession::CallResult>>
ClientProtocolSession::Call(scada::NodeId object_id,
                            scada::NodeId method_id,
                            std::vector<scada::Variant> arguments) {
  auto result = co_await CallTyped<CallResponse>(
      RequestBody{CallRequest{.methods = {MethodCallRequest{
                                  .object_id = std::move(object_id),
                                  .method_id = std::move(method_id),
                                  .arguments = std::move(arguments),
                              }}}});
  if (!result.ok()) {
    co_return scada::StatusOr<CallResult>{result.status()};
  }
  if (result->results.empty()) {
    co_return scada::StatusOr<CallResult>{
        scada::Status{scada::StatusCode::Bad}};
  }
  auto& first = result->results.front();
  co_return scada::StatusOr<CallResult>{CallResult{
      .status = first.status,
      .input_argument_results = std::move(first.input_argument_results),
      .output_arguments = std::move(first.output_arguments),
  }};
}

}  // namespace opcua
