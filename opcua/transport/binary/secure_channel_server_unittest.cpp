#include "opcua/transport/binary/secure_channel.h"
#include "opcua/transport/binary/test/test_certificate.h"

#include "opcua/base/any_executor.h"
#include "opcua/base/test/awaitable_test.h"
#include "opcua/base/test/test_executor.h"
#include "opcua/transport/binary/client_secure_channel.h"
#include "opcua/transport/binary/client_transport.h"
#include "opcua/transport/binary/codec_utils.h"
#include "opcua/transport/binary/crypto.h"
#include "opcua/transport/binary/protocol.h"
#include "transport/transport.h"

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <vector>

// End-to-end coverage for the server-side Basic256Sha256 SignAndEncrypt
// SecureChannel. Rather than mock the crypto, these tests drive the real
// ClientSecureChannel against the real server SecureChannel through a loopback
// transport: the client encrypts/signs an OpenSecureChannel and service
// messages, the server decrypts/verifies them and replies in kind, proving the
// two sides agree on the OPC UA Part 6 §6.7 wire format and key derivation.
namespace opcua::binary {
namespace {

using binary::test::GenerateSelfSignedRsa;
using binary::test::PemKeypair;

std::string AsString(const std::vector<char>& bytes) {
  return {bytes.begin(), bytes.end()};
}

std::vector<char> AsVector(const std::string& bytes) {
  return {bytes.begin(), bytes.end()};
}

// Loopback transport that feeds every client frame into a real server
// SecureChannel and queues the server's reply back for the client to read.
// Hello is answered with an Acknowledge; SecureMessage payloads are echoed
// back through the server's symmetric response path.
struct LoopbackState {
  SecureChannel* server = nullptr;
  std::deque<std::string> incoming;  // server->client bytes
  std::vector<std::string> writes;   // client->server bytes
  bool opened = false;
  bool closed = false;
};

class LoopbackTransport {
 public:
  LoopbackTransport(transport::executor executor,
                    std::shared_ptr<LoopbackState> state)
      : executor_{std::move(executor)}, state_{std::move(state)} {}
  LoopbackTransport(LoopbackTransport&&) = default;
  LoopbackTransport& operator=(LoopbackTransport&&) = default;
  LoopbackTransport(const LoopbackTransport&) = delete;
  LoopbackTransport& operator=(const LoopbackTransport&) = delete;

  transport::awaitable<transport::error_code> open() {
    state_->opened = true;
    co_return transport::OK;
  }
  transport::awaitable<transport::error_code> close() {
    state_->closed = true;
    co_return transport::OK;
  }
  transport::awaitable<transport::expected<transport::any_transport>> accept() {
    co_return transport::ERR_NOT_IMPLEMENTED;
  }
  transport::awaitable<transport::expected<size_t>> read(std::span<char> data) {
    if (state_->incoming.empty()) {
      co_return size_t{0};
    }
    auto chunk = std::move(state_->incoming.front());
    state_->incoming.pop_front();
    if (chunk.size() > data.size()) {
      co_return transport::ERR_INVALID_ARGUMENT;
    }
    std::ranges::copy(chunk, data.begin());
    co_return chunk.size();
  }
  transport::awaitable<transport::expected<size_t>> write(
      std::span<const char> data) {
    state_->writes.emplace_back(data.begin(), data.end());
    const std::vector<char> frame{data.begin(), data.end()};
    if (frame.size() >= 3 && frame[0] == 'H' && frame[1] == 'E' &&
        frame[2] == 'L') {
      state_->incoming.push_back(AsString(EncodeAcknowledgeMessage(
          {.receive_buffer_size = 65535, .send_buffer_size = 65535})));
      co_return data.size();
    }

    auto result = co_await state_->server->HandleFrame(frame);
    if (result.outbound_frame.has_value() && !result.outbound_frame->empty()) {
      state_->incoming.push_back(AsString(*result.outbound_frame));
    }
    if (result.service_payload.has_value() && result.request_id.has_value()) {
      // Echo the request body back through the server's response path.
      auto response = state_->server->BuildServiceResponse(
          *result.request_id, std::move(*result.service_payload));
      if (!response.empty()) {
        state_->incoming.push_back(AsString(response));
      }
    }
    co_return data.size();
  }
  std::string name() const { return "LoopbackTransport"; }
  bool message_oriented() const { return false; }
  bool connected() const { return state_->opened && !state_->closed; }
  bool active() const { return true; }
  transport::executor get_executor() { return executor_; }

 private:
  transport::executor executor_;
  std::shared_ptr<LoopbackState> state_;
};

ClientSecureChannel::Security BuildClientSecurity(
    const PemKeypair& client_pk,
    const std::string& server_cert_pem,
    std::string_view policy_uri = kSecurityPolicyBasic256Sha256) {
  ClientSecureChannel::Security s;
  s.security_policy_uri = std::string{policy_uri};
  s.security_mode = MessageSecurityMode::SignAndEncrypt;
  s.client_certificate =
      std::move(*crypto::LoadPemCertificate(client_pk.cert_pem));
  s.client_private_key =
      std::move(*crypto::LoadPemPrivateKey(client_pk.private_key_pem));
  s.server_certificate =
      std::move(*crypto::LoadPemCertificate(server_cert_pem));
  return s;
}

std::shared_ptr<const SecureChannelServerConfig> BuildServerConfig(
    const PemKeypair& server_pk,
    std::function<opcua::Status(std::span<const std::uint8_t>)> validator =
        {}) {
  auto certificate = crypto::LoadPemCertificate(server_pk.cert_pem);
  auto private_key = crypto::LoadPemPrivateKey(server_pk.private_key_pem);
  EXPECT_TRUE(certificate.ok());
  EXPECT_TRUE(private_key.ok());
  auto config = MakeSecureChannelServerConfig(
      std::move(*certificate), std::move(*private_key), /*allow_none=*/true,
      std::move(validator));
  EXPECT_TRUE(config.ok());
  return std::move(*config);
}

class SecureChannelServerBasic256Sha256Test : public ::testing::Test {
 protected:
  std::unique_ptr<ClientTransport> MakeClientTransport(
      const std::shared_ptr<LoopbackState>& state) {
    return std::make_unique<ClientTransport>(ClientTransportContext{
        .transport =
            transport::any_transport{LoopbackTransport{any_executor_, state}},
        .endpoint_url = "opc.tcp://localhost:4840",
        .limits = {},
    });
  }

  opcua::TestExecutor executor_;
  const transport::executor any_executor_ = executor_;
};

TEST_F(SecureChannelServerBasic256Sha256Test, OpenAndServiceRoundTrip) {
  const auto client_pk = GenerateSelfSignedRsa();
  const auto server_pk = GenerateSelfSignedRsa();

  auto config = BuildServerConfig(server_pk);
  SecureChannel server{config, /*channel_id=*/77};

  auto state = std::make_shared<LoopbackState>();
  state->server = &server;
  auto client_transport = MakeClientTransport(state);
  ASSERT_TRUE(
      opcua::WaitAwaitable(executor_, client_transport->Connect()).good());

  ClientSecureChannel client{
      *client_transport, BuildClientSecurity(client_pk, server_pk.cert_pem)};
  const auto open_status = opcua::WaitAwaitable(executor_, client.Open());
  ASSERT_TRUE(open_status.good());
  EXPECT_TRUE(client.opened());
  EXPECT_TRUE(server.opened());
  EXPECT_TRUE(server.secure());
  EXPECT_EQ(client.channel_id(), 77u);
  EXPECT_EQ(client.token_id(), server.token_id());

  // The server captured the client application instance certificate (DER).
  EXPECT_FALSE(server.client_certificate().empty());

  // A symmetric service request is encrypted+signed by the client, decrypted
  // and verified by the server, echoed, and decrypted back by the client.
  const std::uint32_t request_id = client.NextRequestId();
  const std::vector<char> payload{'p', 'i', 'n', 'g'};
  ASSERT_TRUE(opcua::WaitAwaitable(
                  executor_, client.SendServiceRequest(request_id, payload))
                  .good());
  auto response = opcua::WaitAwaitable(executor_, client.ReadServiceResponse());
  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response->request_id, request_id);
  EXPECT_EQ(response->body, payload);
}

TEST_F(SecureChannelServerBasic256Sha256Test,
       RejectsUntrustedClientCertificate) {
  const auto client_pk = GenerateSelfSignedRsa();
  const auto server_pk = GenerateSelfSignedRsa();

  // First capture a well-formed encrypted OPN frame from a real client.
  auto capture_config = BuildServerConfig(server_pk);
  SecureChannel capture_server{capture_config, /*channel_id=*/5};
  auto state = std::make_shared<LoopbackState>();
  state->server = &capture_server;
  auto client_transport = MakeClientTransport(state);
  ASSERT_TRUE(
      opcua::WaitAwaitable(executor_, client_transport->Connect()).good());
  ClientSecureChannel client{
      *client_transport, BuildClientSecurity(client_pk, server_pk.cert_pem)};
  (void)opcua::WaitAwaitable(executor_, client.Open());
  ASSERT_GE(state->writes.size(), 2u);  // [0]=Hello, [1]=encrypted OPN
  const auto opn_frame = AsVector(state->writes[1]);

  // A server that rejects every client certificate must drop the channel.
  bool validator_called = false;
  auto reject_config = BuildServerConfig(
      server_pk, [&validator_called](std::span<const std::uint8_t> der) {
        validator_called = !der.empty();
        return opcua::Status{opcua::StatusCode::Bad};
      });
  SecureChannel rejecting_server{reject_config, /*channel_id=*/5};
  const auto result =
      opcua::WaitAwaitable(executor_, rejecting_server.HandleFrame(opn_frame));
  EXPECT_TRUE(validator_called);
  EXPECT_TRUE(result.close_transport);
  EXPECT_FALSE(result.outbound_frame.has_value());
  EXPECT_FALSE(rejecting_server.opened());
}

TEST_F(SecureChannelServerBasic256Sha256Test,
       RejectsSecureOpenWhenNotConfigured) {
  const auto client_pk = GenerateSelfSignedRsa();
  const auto server_pk = GenerateSelfSignedRsa();

  // Capture a Basic256Sha256 OPN frame.
  auto capture_config = BuildServerConfig(server_pk);
  SecureChannel capture_server{capture_config, /*channel_id=*/9};
  auto state = std::make_shared<LoopbackState>();
  state->server = &capture_server;
  auto client_transport = MakeClientTransport(state);
  ASSERT_TRUE(
      opcua::WaitAwaitable(executor_, client_transport->Connect()).good());
  ClientSecureChannel client{
      *client_transport, BuildClientSecurity(client_pk, server_pk.cert_pem)};
  (void)opcua::WaitAwaitable(executor_, client.Open());
  ASSERT_GE(state->writes.size(), 2u);
  const auto opn_frame = AsVector(state->writes[1]);

  // A None-only channel (no config) must refuse the secure OPN.
  SecureChannel none_only{/*channel_id=*/9};
  const auto result =
      opcua::WaitAwaitable(executor_, none_only.HandleFrame(opn_frame));
  EXPECT_TRUE(result.close_transport);
  EXPECT_FALSE(result.outbound_frame.has_value());
}

// Aes128_Sha256_RsaOaep, the [A] RSA policy: the same handshake as
// Basic256Sha256 with a 16-byte derived encrypting key and AES-128-CBC
// (https://profiles.opcfoundation.org/profile/2058). A key-length mismatch
// between the two ends would fail the first symmetric message, so a round trip
// over both directions is what pins it.
TEST_F(SecureChannelServerBasic256Sha256Test, Aes128Sha256RsaOaepRoundTrip) {
  const auto client_pk = GenerateSelfSignedRsa();
  const auto server_pk = GenerateSelfSignedRsa();

  auto config = BuildServerConfig(server_pk);
  SecureChannel server{config, /*channel_id=*/78};

  auto state = std::make_shared<LoopbackState>();
  state->server = &server;
  auto client_transport = MakeClientTransport(state);
  ASSERT_TRUE(
      opcua::WaitAwaitable(executor_, client_transport->Connect()).good());

  ClientSecureChannel client{
      *client_transport,
      BuildClientSecurity(client_pk, server_pk.cert_pem,
                          kSecurityPolicyAes128Sha256RsaOaep)};
  ASSERT_TRUE(opcua::WaitAwaitable(executor_, client.Open()).good());
  EXPECT_TRUE(server.secure());

  for (int i = 0; i < 3; ++i) {
    const std::uint32_t request_id = client.NextRequestId();
    const std::vector<char> payload(100 + i * 37, static_cast<char>('a' + i));
    ASSERT_TRUE(opcua::WaitAwaitable(
                    executor_, client.SendServiceRequest(request_id, payload))
                    .good());
    auto response =
        opcua::WaitAwaitable(executor_, client.ReadServiceResponse());
    ASSERT_TRUE(response.ok());
    EXPECT_EQ(response->request_id, request_id);
    EXPECT_EQ(response->body, payload);
  }
}

TEST_F(SecureChannelServerBasic256Sha256Test, RefusesAPolicyItDoesNotAllow) {
  const auto client_pk = GenerateSelfSignedRsa();
  const auto server_pk = GenerateSelfSignedRsa();

  auto capture_config = BuildServerConfig(server_pk);
  SecureChannel capture_server{capture_config, /*channel_id=*/11};
  auto state = std::make_shared<LoopbackState>();
  state->server = &capture_server;
  auto client_transport = MakeClientTransport(state);
  ASSERT_TRUE(
      opcua::WaitAwaitable(executor_, client_transport->Connect()).good());
  ClientSecureChannel client{
      *client_transport,
      BuildClientSecurity(client_pk, server_pk.cert_pem,
                          kSecurityPolicyAes128Sha256RsaOaep)};
  (void)opcua::WaitAwaitable(executor_, client.Open());
  ASSERT_GE(state->writes.size(), 2u);
  const auto opn_frame = AsVector(state->writes[1]);

  auto certificate = crypto::LoadPemCertificate(server_pk.cert_pem);
  auto private_key = crypto::LoadPemPrivateKey(server_pk.private_key_pem);
  auto made = MakeSecureChannelServerConfig(std::move(*certificate),
                                            std::move(*private_key),
                                            /*allow_none=*/true, {});
  ASSERT_TRUE(made.ok());
  // The config was just made and has no other owner, so adjusting it before
  // any channel sees it is safe; the pointer is const only for sharing.
  auto basic256_only =
      std::const_pointer_cast<SecureChannelServerConfig>(*made);
  basic256_only->allow_aes128_sha256_rsaoaep = false;
  SecureChannel server{basic256_only, /*channel_id=*/11};
  const auto result =
      opcua::WaitAwaitable(executor_, server.HandleFrame(opn_frame));
  EXPECT_TRUE(result.close_transport);
  EXPECT_FALSE(result.outbound_frame.has_value());
}

// After a Renew the server keeps securing its messages with the old token
// until the client sends under the new one (OPC UA Part 4 §5.6.2
// OpenSecureChannel), and "the Client shall continue to accept the old
// SecurityToken until it expires or it receives a Message from the Server
// secured with the new SecurityToken" (Part 6 §6.7.4 Establishing a
// SecureChannel). The None path has honoured the overlap since a deployment
// lost every inter-tier session at each renewal; the encrypted path did not
// until 2026-11-01. The server's acceptance of an old-token *request* that
// follows the Renew on the wire is not exercised here: this client renews
// only while idle, so it never writes one.
TEST_F(SecureChannelServerBasic256Sha256Test,
       RenewOverlapKeepsTheSupersededTokenUntilTheClientSwitches) {
  const auto client_pk = GenerateSelfSignedRsa();
  const auto server_pk = GenerateSelfSignedRsa();
  SecureChannel server{BuildServerConfig(server_pk), /*channel_id=*/9};
  auto state = std::make_shared<LoopbackState>();
  state->server = &server;
  auto client_transport = MakeClientTransport(state);
  ASSERT_TRUE(
      opcua::WaitAwaitable(executor_, client_transport->Connect()).good());
  ClientSecureChannel client{
      *client_transport, BuildClientSecurity(client_pk, server_pk.cert_pem)};
  ASSERT_TRUE(opcua::WaitAwaitable(executor_, client.Open()).good());
  const std::uint32_t old_token = client.token_id();

  ASSERT_TRUE(opcua::WaitAwaitable(executor_, client.Renew()).good());
  ASSERT_NE(client.token_id(), old_token);
  EXPECT_EQ(client.token_id(), server.token_id());

  // A message the server sends now (a Publish response, say) still uses the
  // old token, and the client must accept it.
  const std::vector<char> pushed{'o', 'l', 'd'};
  const auto old_token_message =
      server.BuildServiceResponse(/*request_id=*/42, pushed);
  std::uint32_t message_token = 0;
  std::memcpy(&message_token, old_token_message.data() + 12, 4);
  EXPECT_EQ(message_token, old_token);
  state->incoming.push_back(AsString(old_token_message));
  auto response = opcua::WaitAwaitable(executor_, client.ReadServiceResponse());
  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response->request_id, 42u);
  EXPECT_EQ(response->body, pushed);

  // The client's next request carries the new token; the server's answer
  // moves to it too.
  const std::uint32_t next_id = client.NextRequestId();
  const std::vector<char> next{'n', 'e', 'w'};
  ASSERT_TRUE(
      opcua::WaitAwaitable(executor_, client.SendServiceRequest(next_id, next))
          .good());
  ASSERT_FALSE(state->incoming.empty());
  std::uint32_t new_response_token = 0;
  std::memcpy(&new_response_token, state->incoming.back().data() + 12, 4);
  EXPECT_EQ(new_response_token, client.token_id());
  response = opcua::WaitAwaitable(executor_, client.ReadServiceResponse());
  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response->body, next);
}

// A recorded, still-valid encrypted request sent again is a replay (OPC UA
// Part 2 §4.3.6 Message replay). Its sequence number does not follow the last
// one, which the receiver treats as a transport error (Part 6 §6.7.2.4).
TEST_F(SecureChannelServerBasic256Sha256Test, ReplayedRequestClosesTheChannel) {
  const auto client_pk = GenerateSelfSignedRsa();
  const auto server_pk = GenerateSelfSignedRsa();
  SecureChannel server{BuildServerConfig(server_pk), /*channel_id=*/4};
  auto state = std::make_shared<LoopbackState>();
  state->server = &server;
  auto client_transport = MakeClientTransport(state);
  ASSERT_TRUE(
      opcua::WaitAwaitable(executor_, client_transport->Connect()).good());
  ClientSecureChannel client{
      *client_transport, BuildClientSecurity(client_pk, server_pk.cert_pem)};
  ASSERT_TRUE(opcua::WaitAwaitable(executor_, client.Open()).good());

  const std::vector<char> payload{'x'};
  ASSERT_TRUE(
      opcua::WaitAwaitable(
          executor_, client.SendServiceRequest(client.NextRequestId(), payload))
          .good());
  const auto recorded = AsVector(state->writes.back());
  ASSERT_TRUE(
      opcua::WaitAwaitable(executor_, client.ReadServiceResponse()).ok());

  const auto result =
      opcua::WaitAwaitable(executor_, server.HandleFrame(recorded));
  EXPECT_TRUE(result.close_transport);
  EXPECT_EQ(result.close_reason, "message sequence number out of order");
}

// A server-side response replayed to the client is refused the same way.
TEST_F(SecureChannelServerBasic256Sha256Test, ReplayedResponseIsRefused) {
  const auto client_pk = GenerateSelfSignedRsa();
  const auto server_pk = GenerateSelfSignedRsa();
  SecureChannel server{BuildServerConfig(server_pk), /*channel_id=*/6};
  auto state = std::make_shared<LoopbackState>();
  state->server = &server;
  auto client_transport = MakeClientTransport(state);
  ASSERT_TRUE(
      opcua::WaitAwaitable(executor_, client_transport->Connect()).good());
  ClientSecureChannel client{
      *client_transport, BuildClientSecurity(client_pk, server_pk.cert_pem)};
  ASSERT_TRUE(opcua::WaitAwaitable(executor_, client.Open()).good());

  ASSERT_TRUE(
      opcua::WaitAwaitable(
          executor_, client.SendServiceRequest(client.NextRequestId(), {'y'}))
          .good());
  const std::string response_frame = state->incoming.back();
  ASSERT_TRUE(
      opcua::WaitAwaitable(executor_, client.ReadServiceResponse()).ok());

  state->incoming.push_back(response_frame);
  const auto replayed =
      opcua::WaitAwaitable(executor_, client.ReadServiceResponse());
  ASSERT_FALSE(replayed.ok());
  EXPECT_EQ(replayed.status().code(), StatusCode::Bad_SequenceNumberInvalid);
}

// Part 6 §6.7.4: "The Server shall reject renew requests if the
// SenderCertificate is not the same as the one used to create the
// SecureChannel."
TEST_F(SecureChannelServerBasic256Sha256Test,
       RenewWithAnotherClientCertificateIsRejected) {
  const auto client_pk = GenerateSelfSignedRsa();
  const auto other_pk = GenerateSelfSignedRsa();
  const auto server_pk = GenerateSelfSignedRsa();
  const auto config = BuildServerConfig(server_pk);

  SecureChannel server{config, /*channel_id=*/12};
  auto state = std::make_shared<LoopbackState>();
  state->server = &server;
  auto client_transport = MakeClientTransport(state);
  ASSERT_TRUE(
      opcua::WaitAwaitable(executor_, client_transport->Connect()).good());
  ClientSecureChannel client{
      *client_transport, BuildClientSecurity(client_pk, server_pk.cert_pem)};
  ASSERT_TRUE(opcua::WaitAwaitable(executor_, client.Open()).good());

  // A second application opens the same channel id elsewhere and renews; its
  // Renew frame is then presented to the first server.
  SecureChannel other_server{config, /*channel_id=*/12};
  auto other_state = std::make_shared<LoopbackState>();
  other_state->server = &other_server;
  auto other_transport = MakeClientTransport(other_state);
  ASSERT_TRUE(
      opcua::WaitAwaitable(executor_, other_transport->Connect()).good());
  ClientSecureChannel other{*other_transport,
                            BuildClientSecurity(other_pk, server_pk.cert_pem)};
  ASSERT_TRUE(opcua::WaitAwaitable(executor_, other.Open()).good());
  ASSERT_TRUE(opcua::WaitAwaitable(executor_, other.Renew()).good());
  const auto renew_frame = AsVector(other_state->writes.back());

  const auto result =
      opcua::WaitAwaitable(executor_, server.HandleFrame(renew_frame));
  EXPECT_TRUE(result.close_transport);
  EXPECT_EQ(result.close_reason,
            "Renew presented a different client certificate");
}

TEST(SequenceNumberTest, FollowsByOneAndWrapsOnlyNearTheTop) {
  EXPECT_TRUE(IsNextSequenceNumber(1, 2));
  EXPECT_FALSE(IsNextSequenceNumber(1, 1));
  EXPECT_FALSE(IsNextSequenceNumber(1, 3));
  EXPECT_FALSE(IsNextSequenceNumber(5, 0));
  // Legacy wrap: past UInt32.MaxValue - 1024 the next may restart below 1024.
  EXPECT_TRUE(IsNextSequenceNumber(4'294'966'272u, 4'294'966'273u));
  EXPECT_TRUE(IsNextSequenceNumber(4'294'966'272u, 0));
  EXPECT_TRUE(IsNextSequenceNumber(4'294'967'295u, 1023));
  EXPECT_FALSE(IsNextSequenceNumber(4'294'967'295u, 1024));
  EXPECT_FALSE(IsNextSequenceNumber(4'294'966'271u, 0));
}

}  // namespace
}  // namespace opcua::binary
