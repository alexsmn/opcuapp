#pragma once

#include "opcua/base/awaitable.h"
#include "opcua/base/time/time.h"
#include "opcua/client_channel.h"
#include "opcua/client_connection.h"
#include "opcua/message.h"
#include "opcua/scada/attribute_service.h"
#include "opcua/scada/basic_types.h"
#include "opcua/scada/localized_text.h"
#include "opcua/scada/method_service.h"
#include "opcua/scada/node_management_service.h"
#include "opcua/scada/status.h"
#include "opcua/scada/status_or.h"
#include "opcua/scada/variant.h"
#include "opcua/scada/view_service.h"

#include <functional>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace opcua {

// Thin typed facade over ClientChannel. Drives the Session
// lifecycle (CreateSession -> ActivateSession -> ... -> CloseSession) and
// packages each OPC UA service call into an RequestBody /
// ResponseBody round-trip.
//
// All service methods return Awaitable<StatusOr<Result>> so the caller can
// compose them with other coroutines. Errors at any layer (connection,
// codec, service fault, wrong response type) surface as a bad Status.
class ClientProtocolSession {
 public:
  struct Context {
    ClientConnection& connection;
    ClientChannel& channel;
  };

  explicit ClientProtocolSession(Context context);

  // Runs the connection-establishment dance:
  //   1. connection.Open()
  //   2. CreateSession
  //   3. ActivateSession (with the given identity)
  struct Identity {
    // Empty user_name selects anonymous.
    std::optional<LocalizedText> user_name;
    std::optional<LocalizedText> password;
  };

  // Signature returned by a ClientSigner for ActivateSession.
  struct ClientSignatureData {
    std::string algorithm;
    ByteString signature;
  };
  // Produces the ActivateSession clientSignature over
  // (server_certificate || server_nonce). Returns an empty signature for an
  // unsecured session.
  using ClientSigner = std::function<StatusOr<ClientSignatureData>(
      const ByteString& server_certificate,
      const ByteString& server_nonce)>;
  // Client credentials sent during a secured CreateSession / ActivateSession.
  // Default-constructed (empty cert/nonce, null signer) for
  // SecurityPolicy=None.
  struct ClientCredentials {
    ByteString certificate;  // client application instance cert (DER)
    ByteString nonce;        // client nonce
    ClientSigner signer;
    // The server certificate (DER) the client expects, taken from the endpoint
    // selected during discovery. When non-empty, CreateSession is rejected if
    // the certificate the server returns does not match it (OPC UA Part 4
    // §5.6.2 — guards against a MITM swapping certificates between discovery
    // and session). Empty under SecurityPolicy=None.
    ByteString expected_server_certificate;
  };

  [[nodiscard]] Awaitable<Status> Create(
      base::TimeDelta requested_timeout = base::TimeDelta::FromMinutes(10),
      Identity identity = {},
      ClientCredentials credentials = {});

  // CloseSession + connection.Close(), best-effort.
  [[nodiscard]] Awaitable<Status> Close();

  [[nodiscard]] bool is_active() const { return is_active_; }
  [[nodiscard]] const NodeId& session_id() const { return session_id_; }
  [[nodiscard]] const NodeId& authentication_token() const {
    return authentication_token_;
  }

  // -- Typed service helpers. Each one packages the request variant, calls
  // channel_.Call, then narrows the response variant. A bad Status is
  // returned if any step fails or the response type doesn't match.

  [[nodiscard]] Awaitable<StatusOr<std::vector<DataValue>>> Read(
      std::vector<ReadValueId> inputs);

  [[nodiscard]] Awaitable<StatusOr<std::vector<StatusCode>>>
  Write(std::vector<WriteValue> inputs);

  [[nodiscard]] Awaitable<StatusOr<std::vector<BrowseResult>>>
  Browse(std::vector<BrowseDescription> inputs);

  [[nodiscard]] Awaitable<StatusOr<std::vector<BrowseResult>>>
  BrowseNext(std::vector<ByteString> continuation_points,
             bool release_continuation_points = false);

  [[nodiscard]] Awaitable<StatusOr<std::vector<BrowsePathResult>>>
  TranslateBrowsePathsToNodeIds(std::vector<BrowsePath> inputs);

  struct CallResult {
    Status status{StatusCode::Good};
    std::vector<StatusCode> input_argument_results;
    std::vector<Variant> output_arguments;
  };
  [[nodiscard]] Awaitable<StatusOr<CallResult>> Call(
      NodeId object_id,
      NodeId method_id,
      std::vector<Variant> arguments);

  [[nodiscard]] Awaitable<StatusOr<std::vector<AddNodesResult>>>
  AddNodes(std::vector<AddNodesItem> inputs);

  [[nodiscard]] Awaitable<StatusOr<std::vector<StatusCode>>>
  DeleteNodes(std::vector<DeleteNodesItem> inputs);

  [[nodiscard]] Awaitable<StatusOr<std::vector<StatusCode>>>
  AddReferences(std::vector<AddReferencesItem> inputs);

  [[nodiscard]] Awaitable<StatusOr<std::vector<StatusCode>>>
  DeleteReferences(std::vector<DeleteReferencesItem> inputs);

 private:
  // Helper that sends a typed request and extracts the typed response. On a
  // variant mismatch, decode error, or transport error it yields a bad
  // Status. On ServiceFault the fault status is propagated.
  template <typename Response>
  [[nodiscard]] Awaitable<StatusOr<Response>> CallTyped(
      RequestBody request);

  ClientConnection& connection_;
  ClientChannel& channel_;

  bool is_active_ = false;
  NodeId session_id_;
  NodeId authentication_token_;
};

}  // namespace opcua
