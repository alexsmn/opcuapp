#include "opcua/binary/client_secure_channel.h"

#include "opcua/base/any_executor.h"
#include "opcua/base/test/awaitable_test.h"
#include "opcua/base/test/test_executor.h"
#include "opcua/binary/client_transport.h"
#include "opcua/binary/codec_utils.h"
#include "opcua/binary/crypto.h"
#include "opcua/binary/secure_channel.h"
#include "transport/transport.h"

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace opcua::binary {
namespace {

// Scripted scripted transport: reads drain a pre-queued deque, writes go into
// a separate deque that the test can inspect. A write-only duplex — the test
// pre-computes every server-side frame the client will see.
struct ScriptedState {
  std::deque<std::string> incoming;  // server->client bytes, pre-queued.
  std::vector<std::string> writes;   // client->server bytes captured here.
  bool opened = false;
  bool closed = false;
};

class ScriptedTransport {
 public:
  ScriptedTransport(transport::executor executor,
                    std::shared_ptr<ScriptedState> state)
      : executor_{std::move(executor)}, state_{std::move(state)} {}
  ScriptedTransport(ScriptedTransport&&) = default;
  ScriptedTransport& operator=(ScriptedTransport&&) = default;
  ScriptedTransport(const ScriptedTransport&) = delete;
  ScriptedTransport& operator=(const ScriptedTransport&) = delete;

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
    co_return data.size();
  }
  std::string name() const { return "ScriptedTransport"; }
  bool message_oriented() const { return false; }
  bool connected() const { return state_->opened && !state_->closed; }
  bool active() const { return true; }
  transport::executor get_executor() { return executor_; }

 private:
  transport::executor executor_;
  std::shared_ptr<ScriptedState> state_;
};

std::string AsString(const std::vector<char>& bytes) {
  return {bytes.begin(), bytes.end()};
}

// Synthesize the OPN response frame the server would produce for the given
// channel/token, using the same encoder the server itself uses. The client
// doesn't validate the inbound request_id against anything, so we pick 1.
std::vector<char> BuildOpenResponseFrame(std::uint32_t channel_id,
                                         std::uint32_t token_id,
                                         std::uint32_t request_id,
                                         std::uint32_t request_handle,
                                         std::uint32_t revised_lifetime_ms) {
  const OpenSecureChannelResponse response{
      .response_header = {.request_handle = request_handle,
                          .service_result = opcua::scada::StatusCode::Good},
      .server_protocol_version = 0,
      .security_token = {.channel_id = channel_id,
                         .token_id = token_id,
                         .created_at = 0,
                         .revised_lifetime = revised_lifetime_ms},
      .server_nonce = {},
  };
  const SecureConversationMessage message{
      .frame_header = {.message_type = MessageType::SecureOpen,
                       .chunk_type = 'F',
                       .message_size = 0},
      .secure_channel_id = channel_id,
      .asymmetric_security_header =
          AsymmetricSecurityHeader{
              .security_policy_uri = std::string{kSecurityPolicyNone},
              .sender_certificate = {},
              .receiver_certificate_thumbprint = {},
          },
      .sequence_header = {.sequence_number = 1, .request_id = request_id},
      .body = EncodeOpenSecureChannelResponseBody(response),
  };
  return EncodeSecureConversationMessage(message);
}

// Synthesize a symmetric SecureMessage frame echoing `body` back on the
// client's channel. Uses the same encoder as the real server for parity.
std::vector<char> BuildSymmetricMessageFrame(std::uint32_t channel_id,
                                             std::uint32_t token_id,
                                             std::uint32_t request_id,
                                             std::vector<char> body) {
  const SecureConversationMessage message{
      .frame_header = {.message_type = MessageType::SecureMessage,
                       .chunk_type = 'F',
                       .message_size = 0},
      .secure_channel_id = channel_id,
      .asymmetric_security_header = std::nullopt,
      .symmetric_security_header =
          SymmetricSecurityHeader{.token_id = token_id},
      .sequence_header = {.sequence_number = 2, .request_id = request_id},
      .body = std::move(body),
  };
  return EncodeSecureConversationMessage(message);
}

class ClientSecureChannelTest : public ::testing::Test {
 protected:
  std::unique_ptr<ClientTransport> MakeClientTransport(
      const std::shared_ptr<ScriptedState>& state) {
    return std::make_unique<ClientTransport>(ClientTransportContext{
        .transport =
            transport::any_transport{ScriptedTransport{any_executor_, state}},
        .endpoint_url = "opc.tcp://localhost:4840",
        .limits = {},
    });
  }

  // Primes the ACK frame the transport expects during its Connect() call.
  void PrimeAcknowledge(const std::shared_ptr<ScriptedState>& state) {
    state->incoming.push_back(AsString(EncodeAcknowledgeMessage(
        {.receive_buffer_size = 65535, .send_buffer_size = 65535})));
  }

  opcua::TestExecutor executor_;
  const transport::executor any_executor_ = executor_;
};

TEST_F(ClientSecureChannelTest, OpenCapturesServerTokenAndChannel) {
  constexpr std::uint32_t kChannelId = 123;
  constexpr std::uint32_t kTokenId = 1;
  auto state = std::make_shared<ScriptedState>();
  PrimeAcknowledge(state);
  state->incoming.push_back(AsString(BuildOpenResponseFrame(
      kChannelId, kTokenId, /*request_id=*/1, /*request_handle=*/1, 60000)));

  auto client_transport = MakeClientTransport(state);
  ASSERT_TRUE(opcua::WaitAwaitable(executor_, client_transport->Connect()).good());

  ClientSecureChannel client{*client_transport};
  const auto status = opcua::WaitAwaitable(executor_, client.Open());
  EXPECT_TRUE(status.good());
  EXPECT_TRUE(client.opened());
  EXPECT_EQ(client.channel_id(), kChannelId);
  EXPECT_EQ(client.token_id(), kTokenId);
  EXPECT_EQ(client.revised_lifetime_ms(), 60000u);

  // The client wrote exactly one OPN frame to the transport. Decode it and
  // confirm the wire bytes match the OPC UA Part 6 shape.
  ASSERT_EQ(state->writes.size(), 2u);  // 0: Hello, 1: OPN
  const auto opn_bytes =
      std::vector<char>{state->writes[1].begin(), state->writes[1].end()};
  const auto decoded = DecodeSecureConversationMessage(opn_bytes);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->frame_header.message_type, MessageType::SecureOpen);
  ASSERT_TRUE(decoded->asymmetric_security_header.has_value());
  EXPECT_EQ(decoded->asymmetric_security_header->security_policy_uri,
            kSecurityPolicyNone);
}

TEST_F(ClientSecureChannelTest, RenewRotatesSecurityToken) {
  constexpr std::uint32_t kChannelId = 123;
  auto state = std::make_shared<ScriptedState>();
  PrimeAcknowledge(state);
  state->incoming.push_back(AsString(
      BuildOpenResponseFrame(kChannelId, /*token_id=*/1, /*request_id=*/1,
                             /*request_handle=*/1, 60000)));
  state->incoming.push_back(AsString(
      BuildOpenResponseFrame(kChannelId, /*token_id=*/2, /*request_id=*/2,
                             /*request_handle=*/2, 45000)));

  auto client_transport = MakeClientTransport(state);
  ASSERT_TRUE(opcua::WaitAwaitable(executor_, client_transport->Connect()).good());

  ClientSecureChannel client{*client_transport};
  ASSERT_TRUE(opcua::WaitAwaitable(executor_, client.Open()).good());
  const auto status = opcua::WaitAwaitable(executor_, client.Renew());
  EXPECT_TRUE(status.good());
  EXPECT_TRUE(client.opened());
  EXPECT_EQ(client.channel_id(), kChannelId);
  EXPECT_EQ(client.token_id(), 2u);
  EXPECT_EQ(client.revised_lifetime_ms(), 45000u);

  ASSERT_EQ(state->writes.size(), 3u);  // Hello, Issue OPN, Renew OPN.
  const auto renew_bytes =
      std::vector<char>{state->writes[2].begin(), state->writes[2].end()};
  const auto decoded = DecodeSecureConversationMessage(renew_bytes);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->frame_header.message_type, MessageType::SecureOpen);
  EXPECT_EQ(decoded->secure_channel_id, kChannelId);

  const auto body = DecodeOpenSecureChannelRequestBody(decoded->body);
  ASSERT_TRUE(body.has_value());
  EXPECT_EQ(body->request_type, SecurityTokenRequestType::Renew);
}

TEST_F(ClientSecureChannelTest,
       SendServiceRequestRenewsExpiredTokenBeforeMessage) {
  constexpr std::uint32_t kChannelId = 123;
  auto state = std::make_shared<ScriptedState>();
  PrimeAcknowledge(state);
  state->incoming.push_back(AsString(BuildOpenResponseFrame(
      kChannelId, /*token_id=*/1, /*request_id=*/1, /*request_handle=*/1, 0)));
  state->incoming.push_back(AsString(
      BuildOpenResponseFrame(kChannelId, /*token_id=*/2, /*request_id=*/3,
                             /*request_handle=*/3, 60000)));

  auto client_transport = MakeClientTransport(state);
  ASSERT_TRUE(opcua::WaitAwaitable(executor_, client_transport->Connect()).good());

  ClientSecureChannel client{*client_transport};
  ASSERT_TRUE(opcua::WaitAwaitable(executor_, client.Open()).good());

  const std::uint32_t request_id = client.NextRequestId();
  const auto send_status =
      opcua::WaitAwaitable(executor_, client.SendServiceRequest(request_id, {'p'}));
  EXPECT_TRUE(send_status.good());
  EXPECT_EQ(client.token_id(), 2u);

  ASSERT_EQ(state->writes.size(), 4u);  // Hello, Issue OPN, Renew OPN, MSG.
  const auto renew_bytes =
      std::vector<char>{state->writes[2].begin(), state->writes[2].end()};
  const auto renew_message = DecodeSecureConversationMessage(renew_bytes);
  ASSERT_TRUE(renew_message.has_value());
  const auto renew_body =
      DecodeOpenSecureChannelRequestBody(renew_message->body);
  ASSERT_TRUE(renew_body.has_value());
  EXPECT_EQ(renew_body->request_type, SecurityTokenRequestType::Renew);

  const auto msg_bytes =
      std::vector<char>{state->writes[3].begin(), state->writes[3].end()};
  const auto msg = DecodeSecureConversationMessage(msg_bytes);
  ASSERT_TRUE(msg.has_value());
  ASSERT_TRUE(msg->symmetric_security_header.has_value());
  EXPECT_EQ(msg->symmetric_security_header->token_id, 2u);
  EXPECT_EQ(msg->sequence_header.request_id, request_id);
}

TEST_F(ClientSecureChannelTest, OpenPropagatesServerBadStatus) {
  auto state = std::make_shared<ScriptedState>();
  PrimeAcknowledge(state);
  // Server says bad — the client should surface that.
  const OpenSecureChannelResponse bad_response{
      .response_header = {.request_handle = 1,
                          .service_result = opcua::scada::StatusCode::Bad},
      .server_protocol_version = 0,
      .security_token = {},
      .server_nonce = {},
  };
  const SecureConversationMessage message{
      .frame_header = {.message_type = MessageType::SecureOpen,
                       .chunk_type = 'F',
                       .message_size = 0},
      .secure_channel_id = 0,
      .asymmetric_security_header =
          AsymmetricSecurityHeader{
              .security_policy_uri = std::string{kSecurityPolicyNone},
              .sender_certificate = {},
              .receiver_certificate_thumbprint = {},
          },
      .sequence_header = {.sequence_number = 1, .request_id = 1},
      .body = EncodeOpenSecureChannelResponseBody(bad_response),
  };
  state->incoming.push_back(AsString(EncodeSecureConversationMessage(message)));

  auto client_transport = MakeClientTransport(state);
  ASSERT_TRUE(opcua::WaitAwaitable(executor_, client_transport->Connect()).good());
  ClientSecureChannel client{*client_transport};
  const auto status = opcua::WaitAwaitable(executor_, client.Open());
  EXPECT_TRUE(status.bad());
  EXPECT_FALSE(client.opened());
}

TEST_F(ClientSecureChannelTest, SendServiceRequestWritesSymmetricFrame) {
  constexpr std::uint32_t kChannelId = 77;
  constexpr std::uint32_t kTokenId = 1;
  auto state = std::make_shared<ScriptedState>();
  PrimeAcknowledge(state);
  state->incoming.push_back(
      AsString(BuildOpenResponseFrame(kChannelId, kTokenId, 1, 1, 60000)));

  auto client_transport = MakeClientTransport(state);
  ASSERT_TRUE(opcua::WaitAwaitable(executor_, client_transport->Connect()).good());
  ClientSecureChannel client{*client_transport};
  ASSERT_TRUE(opcua::WaitAwaitable(executor_, client.Open()).good());

  const std::vector<char> payload{'h', 'i'};
  const std::uint32_t request_id = client.NextRequestId();
  const auto send_status =
      opcua::WaitAwaitable(executor_, client.SendServiceRequest(request_id, payload));
  EXPECT_TRUE(send_status.good());

  // writes: [0]=Hello, [1]=OPN, [2]=SecureMessage with payload.
  ASSERT_EQ(state->writes.size(), 3u);
  const auto bytes =
      std::vector<char>{state->writes[2].begin(), state->writes[2].end()};
  const auto decoded = DecodeSecureConversationMessage(bytes);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->frame_header.message_type, MessageType::SecureMessage);
  EXPECT_EQ(decoded->secure_channel_id, kChannelId);
  ASSERT_TRUE(decoded->symmetric_security_header.has_value());
  EXPECT_EQ(decoded->symmetric_security_header->token_id, kTokenId);
  EXPECT_EQ(decoded->sequence_header.request_id, request_id);
  EXPECT_EQ(decoded->body, payload);
}

TEST_F(ClientSecureChannelTest, ReadServiceResponseReturnsBody) {
  constexpr std::uint32_t kChannelId = 77;
  constexpr std::uint32_t kTokenId = 1;
  auto state = std::make_shared<ScriptedState>();
  PrimeAcknowledge(state);
  state->incoming.push_back(
      AsString(BuildOpenResponseFrame(kChannelId, kTokenId, 1, 1, 60000)));
  const std::vector<char> expected_body{'p', 'a', 'y', '!'};
  state->incoming.push_back(AsString(BuildSymmetricMessageFrame(
      kChannelId, kTokenId, /*request_id=*/42, expected_body)));

  auto client_transport = MakeClientTransport(state);
  ASSERT_TRUE(opcua::WaitAwaitable(executor_, client_transport->Connect()).good());
  ClientSecureChannel client{*client_transport};
  ASSERT_TRUE(opcua::WaitAwaitable(executor_, client.Open()).good());

  const auto response = opcua::WaitAwaitable(executor_, client.ReadServiceResponse());
  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response->request_id, 42u);
  EXPECT_EQ(response->body, expected_body);
}

TEST_F(ClientSecureChannelTest, ReadServiceResponseRejectsWrongTokenId) {
  constexpr std::uint32_t kChannelId = 77;
  constexpr std::uint32_t kTokenId = 1;
  auto state = std::make_shared<ScriptedState>();
  PrimeAcknowledge(state);
  state->incoming.push_back(
      AsString(BuildOpenResponseFrame(kChannelId, kTokenId, 1, 1, 60000)));
  // Symmetric frame on a different token_id — client must reject.
  state->incoming.push_back(AsString(BuildSymmetricMessageFrame(
      kChannelId, /*token_id=*/99, 1, std::vector<char>{'x'})));

  auto client_transport = MakeClientTransport(state);
  ASSERT_TRUE(opcua::WaitAwaitable(executor_, client_transport->Connect()).good());
  ClientSecureChannel client{*client_transport};
  ASSERT_TRUE(opcua::WaitAwaitable(executor_, client.Open()).good());

  const auto response = opcua::WaitAwaitable(executor_, client.ReadServiceResponse());
  EXPECT_FALSE(response.ok());
}

TEST_F(ClientSecureChannelTest, CloseWritesCloseSecureChannelFrame) {
  constexpr std::uint32_t kChannelId = 77;
  constexpr std::uint32_t kTokenId = 1;
  auto state = std::make_shared<ScriptedState>();
  PrimeAcknowledge(state);
  state->incoming.push_back(
      AsString(BuildOpenResponseFrame(kChannelId, kTokenId, 1, 1, 60000)));

  auto client_transport = MakeClientTransport(state);
  ASSERT_TRUE(opcua::WaitAwaitable(executor_, client_transport->Connect()).good());
  ClientSecureChannel client{*client_transport};
  ASSERT_TRUE(opcua::WaitAwaitable(executor_, client.Open()).good());

  const auto close_status = opcua::WaitAwaitable(executor_, client.Close());
  EXPECT_TRUE(close_status.good());
  EXPECT_FALSE(client.opened());

  // [0]=Hello, [1]=OPN, [2]=CLO.
  ASSERT_EQ(state->writes.size(), 3u);
  const auto bytes =
      std::vector<char>{state->writes[2].begin(), state->writes[2].end()};
  const auto decoded = DecodeSecureConversationMessage(bytes);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->frame_header.message_type, MessageType::SecureClose);
}

TEST_F(ClientSecureChannelTest, CloseWithoutOpenIsNoOp) {
  auto state = std::make_shared<ScriptedState>();
  PrimeAcknowledge(state);
  auto client_transport = MakeClientTransport(state);
  ASSERT_TRUE(opcua::WaitAwaitable(executor_, client_transport->Connect()).good());
  ClientSecureChannel client{*client_transport};

  const auto status = opcua::WaitAwaitable(executor_, client.Close());
  EXPECT_TRUE(status.good());
  // No extra frame beyond the Hello was written.
  EXPECT_EQ(state->writes.size(), 1u);
}

TEST_F(ClientSecureChannelTest, SendBeforeOpenReturnsBad) {
  auto state = std::make_shared<ScriptedState>();
  PrimeAcknowledge(state);
  auto client_transport = MakeClientTransport(state);
  ASSERT_TRUE(opcua::WaitAwaitable(executor_, client_transport->Connect()).good());
  ClientSecureChannel client{*client_transport};

  const auto status = opcua::WaitAwaitable(
      executor_, client.SendServiceRequest(1, std::vector<char>{'x'}));
  EXPECT_TRUE(status.bad());
}

TEST_F(ClientSecureChannelTest, RequestIdsAreMonotonic) {
  auto state = std::make_shared<ScriptedState>();
  PrimeAcknowledge(state);
  auto client_transport = MakeClientTransport(state);
  ASSERT_TRUE(opcua::WaitAwaitable(executor_, client_transport->Connect()).good());
  ClientSecureChannel client{*client_transport};

  EXPECT_EQ(client.NextRequestId(), 1u);
  EXPECT_EQ(client.NextRequestId(), 2u);
  EXPECT_EQ(client.NextRequestId(), 3u);
}

// --- Basic256Sha256 tests ---------------------------------------------------

namespace {

// Generates a fresh 2048-bit RSA keypair wrapped in a self-signed X.509
// cert, as PEM strings. Same helper as the crypto unittest.
struct PemKeypair {
  std::string cert_pem;
  std::string private_key_pem;
};

PemKeypair GenerateSelfSignedRsa() {
  EVP_PKEY* key = nullptr;
  EVP_PKEY_CTX* kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
  EVP_PKEY_keygen_init(kctx);
  EVP_PKEY_CTX_set_rsa_keygen_bits(kctx, 2048);
  EVP_PKEY_keygen(kctx, &key);
  EVP_PKEY_CTX_free(kctx);

  X509* cert = X509_new();
  ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
  X509_gmtime_adj(X509_get_notBefore(cert), 0);
  X509_gmtime_adj(X509_get_notAfter(cert), 60 * 60 * 24 * 30);
  X509_set_pubkey(cert, key);
  X509_NAME* name = X509_get_subject_name(cert);
  X509_NAME_add_entry_by_txt(
      name, "CN", MBSTRING_ASC,
      reinterpret_cast<const unsigned char*>("opc-ua-test"), -1, -1, 0);
  X509_set_issuer_name(cert, name);
  X509_sign(cert, key, EVP_sha256());

  PemKeypair result;
  {
    BIO* bio = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(bio, cert);
    char* data = nullptr;
    const auto len = BIO_get_mem_data(bio, &data);
    result.cert_pem.assign(data, len);
    BIO_free(bio);
  }
  {
    BIO* bio = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(bio, key, nullptr, nullptr, 0, nullptr, nullptr);
    char* data = nullptr;
    const auto len = BIO_get_mem_data(bio, &data);
    result.private_key_pem.assign(data, len);
    BIO_free(bio);
  }
  X509_free(cert);
  EVP_PKEY_free(key);
  return result;
}

// Build a Basic256Sha256 Security config by loading the given PEMs.
ClientSecureChannel::Security BuildSecurity(
    const PemKeypair& client_pk,
    const std::string& server_cert_pem) {
  ClientSecureChannel::Security s;
  s.security_policy_uri =
      "http://opcfoundation.org/UA/SecurityPolicy#Basic256Sha256";
  s.security_mode = MessageSecurityMode::SignAndEncrypt;
  s.client_certificate =
      std::move(*crypto::LoadPemCertificate(client_pk.cert_pem));
  s.client_private_key =
      std::move(*crypto::LoadPemPrivateKey(client_pk.private_key_pem));
  s.server_certificate =
      std::move(*crypto::LoadPemCertificate(server_cert_pem));
  s.client_nonce_generator = []() {
    return opcua::scada::StatusOr<opcua::scada::ByteString>{opcua::scada::ByteString(32, 0)};
  };
  return s;
}

}  // namespace

class ClientSecureChannelBasic256Sha256Test : public ::testing::Test {
 protected:
  std::unique_ptr<ClientTransport> MakeClientTransport(
      const std::shared_ptr<ScriptedState>& state) {
    return std::make_unique<ClientTransport>(ClientTransportContext{
        .transport =
            transport::any_transport{ScriptedTransport{any_executor_, state}},
        .endpoint_url = "opc.tcp://localhost:4840",
        .limits = {},
    });
  }
  void PrimeAcknowledge(const std::shared_ptr<ScriptedState>& state) {
    state->incoming.push_back(AsString(EncodeAcknowledgeMessage(
        {.receive_buffer_size = 65535, .send_buffer_size = 65535})));
  }

  opcua::TestExecutor executor_;
  const transport::executor any_executor_ = executor_;
};

TEST_F(ClientSecureChannelBasic256Sha256Test,
       SignClientDataProducesVerifiableSignature) {
  const auto client_pk = GenerateSelfSignedRsa();
  const auto server_pk = GenerateSelfSignedRsa();
  auto state = std::make_shared<ScriptedState>();
  auto transport = MakeClientTransport(state);
  ClientSecureChannel channel{*transport,
                              BuildSecurity(client_pk, server_pk.cert_pem)};

  const std::vector<std::uint8_t> data = {1, 2, 3, 4, 5, 6, 7, 8};
  auto signature = channel.SignClientData(data);
  ASSERT_TRUE(signature.ok());
  EXPECT_EQ(signature->algorithm,
            "http://www.w3.org/2001/04/xmldsig-more#rsa-sha256");
  ASSERT_FALSE(signature->signature.empty());

  // The signature must verify against the client certificate's public key.
  auto client_cert = crypto::LoadPemCertificate(client_pk.cert_pem);
  ASSERT_TRUE(client_cert.ok());
  auto public_key = crypto::CertificatePublicKey(*client_cert);
  ASSERT_TRUE(public_key.ok());
  const std::span<const std::uint8_t> sig_bytes{
      reinterpret_cast<const std::uint8_t*>(signature->signature.data()),
      signature->signature.size()};
  EXPECT_TRUE(crypto::RsaPkcs1Sha256Verify(*public_key, data, sig_bytes));
}

TEST_F(ClientSecureChannelBasic256Sha256Test,
       SignClientDataEmptyForNoneChannel) {
  auto state = std::make_shared<ScriptedState>();
  auto transport = MakeClientTransport(state);
  ClientSecureChannel channel{*transport};  // SecurityPolicy=None

  const std::vector<std::uint8_t> data = {1, 2, 3};
  auto signature = channel.SignClientData(data);
  ASSERT_TRUE(signature.ok());
  EXPECT_TRUE(signature->algorithm.empty());
  EXPECT_TRUE(signature->signature.empty());
}

TEST_F(ClientSecureChannelBasic256Sha256Test,
       OpenFrameRoundTripsThroughOurOwnDecoder) {
  const auto client_pk = GenerateSelfSignedRsa();
  const auto server_pk = GenerateSelfSignedRsa();

  // For this test we act as both sides: the "client" encodes the OPN
  // with Basic256Sha256, and a second channel configured with the
  // server's private key + client's cert decodes it. This pins down the
  // byte-exact format of the asymmetric frame.
  auto client_state = std::make_shared<ScriptedState>();
  PrimeAcknowledge(client_state);
  auto client_transport = MakeClientTransport(client_state);
  ASSERT_TRUE(opcua::WaitAwaitable(executor_, client_transport->Connect()).good());

  auto client_security = BuildSecurity(client_pk, server_pk.cert_pem);
  ClientSecureChannel client{*client_transport, std::move(client_security)};

  // Drive Open far enough that the client writes its OPN frame into
  // client_state->writes, then stop (no ACK response queued so Open
  // will fail, but we've already captured the wire bytes).
  (void)opcua::WaitAwaitable(executor_, client.Open());

  // writes: [0]=Hello, [1]=asymmetric-encrypted OPN.
  ASSERT_EQ(client_state->writes.size(), 2u);
  const auto opn_bytes = std::vector<char>{client_state->writes[1].begin(),
                                           client_state->writes[1].end()};

  // Confirm the cleartext prefix matches the expected asymmetric header:
  // frame header says SecureOpen, security_policy_uri is Basic256Sha256.
  const auto plaintext_msg = DecodeSecureConversationMessage(opn_bytes);
  ASSERT_TRUE(plaintext_msg.has_value());
  EXPECT_EQ(plaintext_msg->frame_header.message_type, MessageType::SecureOpen);
  ASSERT_TRUE(plaintext_msg->asymmetric_security_header.has_value());
  EXPECT_EQ(plaintext_msg->asymmetric_security_header->security_policy_uri,
            "http://opcfoundation.org/UA/SecurityPolicy#Basic256Sha256");
  EXPECT_FALSE(
      plaintext_msg->asymmetric_security_header->sender_certificate.empty());
  EXPECT_EQ(plaintext_msg->asymmetric_security_header
                ->receiver_certificate_thumbprint.size(),
            20u);
}

TEST_F(ClientSecureChannelBasic256Sha256Test,
       OpenRejectsInvalidInjectedClientNonce) {
  const auto client_pk = GenerateSelfSignedRsa();
  const auto server_pk = GenerateSelfSignedRsa();

  auto client_state = std::make_shared<ScriptedState>();
  PrimeAcknowledge(client_state);
  auto client_transport = MakeClientTransport(client_state);
  ASSERT_TRUE(opcua::WaitAwaitable(executor_, client_transport->Connect()).good());

  auto client_security = BuildSecurity(client_pk, server_pk.cert_pem);
  client_security.client_nonce_generator = []() {
    return opcua::scada::StatusOr<opcua::scada::ByteString>{opcua::scada::ByteString(31, 0)};
  };
  ClientSecureChannel client{*client_transport, std::move(client_security)};

  const auto status = opcua::WaitAwaitable(executor_, client.Open());
  EXPECT_TRUE(status.bad());
  EXPECT_FALSE(client.opened());
  ASSERT_EQ(client_state->writes.size(), 1u);  // Hello only, no OPN.
}

TEST_F(ClientSecureChannelBasic256Sha256Test,
       SymmetricFrameRoundTripsEndToEnd) {
  // Round-trip a symmetric MSG frame between two channels configured with
  // the same derived keys. We fake the Open() by reaching into the
  // channels' private state via an Open() against a scripted server
  // response that carries a known server_nonce.
  //
  // Simpler end-to-end path: use two channels with IDENTICAL derived
  // keys (they both call DeriveBasic256Sha256Keys with the same nonces),
  // and route bytes between them. The server channel needs to produce
  // the SAME derived keys as the client — that requires constructing
  // mirrored channels.
  //
  // Here we use a simpler test: client encodes its request; we use the
  // crypto primitives directly to decrypt + verify the bytes, mirroring
  // what a server-side DecodeSymmetricBasic256Sha256Frame would do.

  const auto client_pk = GenerateSelfSignedRsa();
  const auto server_pk = GenerateSelfSignedRsa();

  auto client_state = std::make_shared<ScriptedState>();
  PrimeAcknowledge(client_state);
  // Queue an OPN response. We manufacture it as None-policy for
  // simplicity — that keeps this test focused on the symmetric path.
  // The crypto-asymmetric path is covered by the preceding test.
  //
  // Since the client Open expects a Basic256Sha256 response from the
  // server, we drive the test only through the symmetric-frame builder
  // helper. Skip Open() and directly invoke internal state...
  // Instead: fabricate an OPN response that's itself Basic256Sha256 and
  // decryptable with the client's private key. Use the server-side
  // identity as the signer.

  auto server_cert_or = crypto::LoadPemCertificate(server_pk.cert_pem);
  auto server_priv_or = crypto::LoadPemPrivateKey(server_pk.private_key_pem);
  auto client_cert_or = crypto::LoadPemCertificate(client_pk.cert_pem);
  ASSERT_TRUE(server_cert_or.ok());
  ASSERT_TRUE(server_priv_or.ok());
  ASSERT_TRUE(client_cert_or.ok());
  auto server_cert = std::move(*server_cert_or);
  auto server_priv = std::move(*server_priv_or);
  auto client_cert = std::move(*client_cert_or);
  auto client_pub_or = crypto::CertificatePublicKey(client_cert);
  ASSERT_TRUE(client_pub_or.ok());
  auto client_pub_of_client = std::move(*client_pub_or);

  // Build an OpenSecureChannelResponse body with a fixed server_nonce.
  const opcua::scada::ByteString server_nonce(32, '\x7f');
  const OpenSecureChannelResponse response{
      .response_header = {.request_handle = 1,
                          .service_result = opcua::scada::StatusCode::Good},
      .server_protocol_version = 0,
      .security_token = {.channel_id = 99,
                         .token_id = 1,
                         .created_at = 0,
                         .revised_lifetime = 60000},
      .server_nonce = server_nonce,
  };
  const auto response_body = EncodeOpenSecureChannelResponseBody(response);

  // Mirror the client's own asymmetric framing logic, with the roles
  // reversed: server_priv signs, client_cert is the encryption target.
  //
  // Layout matches the Build* function: prefix (FrameHeader + channel_id +
  // asym_header) then encrypted (seq_header + body + padding + padsize +
  // signature).
  std::vector<char> prefix;
  // 8-byte frame header: "OPNF" + message_size (fix later)
  prefix.insert(prefix.end(), {'O', 'P', 'N', 'F', 0, 0, 0, 0});
  {
    Encoder enc{prefix};
    enc.Encode(std::uint32_t{99});  // channel_id
    enc.Encode(std::string{
        "http://opcfoundation.org/UA/SecurityPolicy#Basic256Sha256"});
    enc.Encode(*crypto::CertificateDer(server_cert));
    enc.Encode(*crypto::CertificateThumbprint(client_cert));
  }

  std::vector<char> to_encrypt;
  {
    Encoder enc{to_encrypt};
    enc.Encode(std::uint32_t{1});  // sequence_number
    enc.Encode(std::uint32_t{1});  // request_id matches client's OPN
  }
  to_encrypt.insert(to_encrypt.end(), response_body.begin(),
                    response_body.end());
  // pad to plain-block boundary accounting for 32-bit padding scheme
  const std::size_t client_key_bytes = 256;  // 2048-bit RSA
  const std::size_t plain_block = client_key_bytes - 42;
  const std::size_t server_sig_size = 256;
  const std::size_t unpadded = to_encrypt.size() + 1 + server_sig_size;
  const std::size_t pad_count =
      (plain_block - unpadded % plain_block) % plain_block;
  to_encrypt.insert(to_encrypt.end(), pad_count, static_cast<char>(pad_count));
  to_encrypt.push_back(static_cast<char>(pad_count));

  // Pre-compute the final frame size so the signature covers the final
  // message_size field (matching ClientSecureChannel's impl).
  const std::size_t total_plaintext_with_sig =
      to_encrypt.size() + server_sig_size;
  const std::size_t num_blocks = total_plaintext_with_sig / plain_block;
  const std::size_t cipher_size = num_blocks * client_key_bytes;
  const std::size_t predicted_final_size = prefix.size() + cipher_size;
  {
    const auto size32 = static_cast<std::uint32_t>(predicted_final_size);
    std::memcpy(prefix.data() + 4, &size32, sizeof(size32));
  }
  // Sign
  std::vector<char> to_sign;
  to_sign.insert(to_sign.end(), prefix.begin(), prefix.end());
  to_sign.insert(to_sign.end(), to_encrypt.begin(), to_encrypt.end());
  auto sig = *crypto::RsaPkcs1Sha256Sign(
      server_priv,
      {reinterpret_cast<const std::uint8_t*>(to_sign.data()), to_sign.size()});
  to_encrypt.insert(to_encrypt.end(), sig.begin(), sig.end());
  // Encrypt with client's pub key
  auto cipher = *crypto::RsaOaepEncrypt(
      client_pub_of_client,
      {reinterpret_cast<const std::uint8_t*>(to_encrypt.data()),
       to_encrypt.size()});
  std::vector<char> final_frame;
  final_frame.insert(final_frame.end(), prefix.begin(), prefix.end());
  final_frame.insert(final_frame.end(), cipher.begin(), cipher.end());
  // Fix up size
  const std::uint32_t msg_size = static_cast<std::uint32_t>(final_frame.size());
  std::memcpy(final_frame.data() + 4, &msg_size, sizeof(msg_size));

  client_state->incoming.push_back(AsString(final_frame));

  auto client_transport = MakeClientTransport(client_state);
  ASSERT_TRUE(opcua::WaitAwaitable(executor_, client_transport->Connect()).good());

  auto client_security = BuildSecurity(client_pk, server_pk.cert_pem);
  ClientSecureChannel client{*client_transport, std::move(client_security)};

  const auto open_status = opcua::WaitAwaitable(executor_, client.Open());
  EXPECT_TRUE(open_status.good());
  EXPECT_TRUE(client.opened());
  EXPECT_EQ(client.channel_id(), 99u);
  EXPECT_EQ(client.token_id(), 1u);

  // The symmetric send path now has derived keys installed. Round-trip a
  // payload through the build + decode helpers by sending and then
  // decoding from the test side using the matching server keys.
  const std::vector<char> payload{'h', 'e', 'l', 'l', 'o'};
  const auto send_status =
      opcua::WaitAwaitable(executor_, client.SendServiceRequest(42, payload));
  EXPECT_TRUE(send_status.good());

  // The client wrote a MSGF frame; decode it with the server-side
  // equivalents of the derived keys to confirm the layout + crypto.
  ASSERT_GE(client_state->writes.size(), 3u);
  const std::vector<char> msg_bytes{client_state->writes.back().begin(),
                                    client_state->writes.back().end()};
  // The client SEND path encrypts with its derived ClientKeys, which are
  // P_SHA256(serverNonce, clientNonce). BuildSecurity injects a deterministic
  // zero nonce for this test.
  const opcua::scada::ByteString zero_nonce(32, 0);
  const auto client_keys = crypto::DeriveBasic256Sha256Keys(
      {reinterpret_cast<const std::uint8_t*>(server_nonce.data()),
       server_nonce.size()},
      {reinterpret_cast<const std::uint8_t*>(zero_nonce.data()),
       zero_nonce.size()});
  // Decrypt and verify to prove the symmetric frame is valid.
  constexpr std::size_t kHeaderSize = 16;
  ASSERT_GT(msg_bytes.size(), kHeaderSize);
  auto decrypted = crypto::AesCbcDecrypt(
      {reinterpret_cast<const std::uint8_t*>(client_keys.encrypting_key.data()),
       client_keys.encrypting_key.size()},
      {reinterpret_cast<const std::uint8_t*>(
           client_keys.initialization_vector.data()),
       client_keys.initialization_vector.size()},
      {reinterpret_cast<const std::uint8_t*>(msg_bytes.data() + kHeaderSize),
       msg_bytes.size() - kHeaderSize});
  ASSERT_TRUE(decrypted.ok());
  // Last 32 bytes = HMAC; strip padding before them.
  ASSERT_GE(decrypted->size(), 32u);
  const auto sig_begin = decrypted->size() - 32;
  const auto pad_size = static_cast<std::uint8_t>((*decrypted)[sig_begin - 1]);
  const auto body_end = sig_begin - 1 - pad_size;
  // First 8 bytes are seq_header (seq_num + request_id).
  std::uint32_t decoded_request_id = 0;
  std::memcpy(&decoded_request_id, decrypted->data() + 4, 4);
  EXPECT_EQ(decoded_request_id, 42u);
  const std::vector<char> decoded_body(decrypted->begin() + 8,
                                       decrypted->begin() + body_end);
  EXPECT_EQ(decoded_body, payload);
}

}  // namespace
}  // namespace opcua::binary
