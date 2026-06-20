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
    std::optional<scada::LocalizedText> user_name;
    std::optional<scada::LocalizedText> password;
  };

  // Signature returned by a ClientSigner for ActivateSession.
  struct ClientSignatureData {
    std::string algorithm;
    scada::ByteString signature;
  };
  // Produces the ActivateSession clientSignature over
  // (server_certificate || server_nonce). Returns an empty signature for an
  // unsecured session.
  using ClientSigner = std::function<scada::StatusOr<ClientSignatureData>(
      const scada::ByteString& server_certificate,
      const scada::ByteString& server_nonce)>;
  // Client credentials sent during a secured CreateSession / ActivateSession.
  // Default-constructed (empty cert/nonce, null signer) for
  // SecurityPolicy=None.
  struct ClientCredentials {
    scada::ByteString certificate;  // client application instance cert (DER)
    scada::ByteString nonce;        // client nonce
    ClientSigner signer;
    // The server certificate (DER) the client expects, taken from the endpoint
    // selected during discovery. When non-empty, CreateSession is rejected if
    // the certificate the server returns does not match it (OPC UA Part 4
    // §5.6.2 — guards against a MITM swapping certificates between discovery
    // and session). Empty under SecurityPolicy=None.
    scada::ByteString expected_server_certificate;
  };

  [[nodiscard]] Awaitable<scada::Status> Create(
      base::TimeDelta requested_timeout = base::TimeDelta::FromMinutes(10),
      Identity identity = {},
      ClientCredentials credentials = {});

  // CloseSession + connection.Close(), best-effort.
  [[nodiscard]] Awaitable<scada::Status> Close();

  [[nodiscard]] bool is_active() const { return is_active_; }
  [[nodiscard]] const scada::NodeId& session_id() const { return session_id_; }
  [[nodiscard]] const scada::NodeId& authentication_token() const {
    return authentication_token_;
  }

  // -- Typed service helpers. Each one packages the request variant, calls
  // channel_.Call, then narrows the response variant. A bad Status is
  // returned if any step fails or the response type doesn't match.

  [[nodiscard]] Awaitable<scada::StatusOr<std::vector<scada::DataValue>>> Read(
      std::vector<scada::ReadValueId> inputs);

  [[nodiscard]] Awaitable<scada::StatusOr<std::vector<scada::StatusCode>>>
  Write(std::vector<scada::WriteValue> inputs);

  [[nodiscard]] Awaitable<scada::StatusOr<std::vector<scada::BrowseResult>>>
  Browse(std::vector<scada::BrowseDescription> inputs);

  [[nodiscard]] Awaitable<scada::StatusOr<std::vector<scada::BrowseResult>>>
  BrowseNext(std::vector<scada::ByteString> continuation_points,
             bool release_continuation_points = false);

  [[nodiscard]] Awaitable<scada::StatusOr<std::vector<scada::BrowsePathResult>>>
  TranslateBrowsePathsToNodeIds(std::vector<scada::BrowsePath> inputs);

  struct CallResult {
    scada::Status status{scada::StatusCode::Good};
    std::vector<scada::StatusCode> input_argument_results;
    std::vector<scada::Variant> output_arguments;
  };
  [[nodiscard]] Awaitable<scada::StatusOr<CallResult>> Call(
      scada::NodeId object_id,
      scada::NodeId method_id,
      std::vector<scada::Variant> arguments);

  [[nodiscard]] Awaitable<scada::StatusOr<std::vector<scada::AddNodesResult>>>
  AddNodes(std::vector<scada::AddNodesItem> inputs);

  [[nodiscard]] Awaitable<scada::StatusOr<std::vector<scada::StatusCode>>>
  DeleteNodes(std::vector<scada::DeleteNodesItem> inputs);

  [[nodiscard]] Awaitable<scada::StatusOr<std::vector<scada::StatusCode>>>
  AddReferences(std::vector<scada::AddReferencesItem> inputs);

  [[nodiscard]] Awaitable<scada::StatusOr<std::vector<scada::StatusCode>>>
  DeleteReferences(std::vector<scada::DeleteReferencesItem> inputs);

 private:
  // Helper that sends a typed request and extracts the typed response. On a
  // variant mismatch, decode error, or transport error it yields a bad
  // Status. On ServiceFault the fault status is propagated.
  template <typename Response>
  [[nodiscard]] Awaitable<scada::StatusOr<Response>> CallTyped(
      RequestBody request);

  ClientConnection& connection_;
  ClientChannel& channel_;

  bool is_active_ = false;
  scada::NodeId session_id_;
  scada::NodeId authentication_token_;
};

}  // namespace opcua
